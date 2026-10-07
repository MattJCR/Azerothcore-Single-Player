#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Asistente local de instalación. Sólo requiere la biblioteca estándar."""
import argparse
from datetime import datetime
import getpass
import ipaddress
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
LOCAL = "config.local.sh"
LOCAL_HEADER = (
    "# Valores de esta instalación, escritos por ./install.sh --guiado.\n"
    "# Sustituyen a los de config.sh. No se versiona: contiene contraseñas.\n"
)
FIELDS = (
    "AC_DIR", "REALM_IP", "REALM_NAME", "ADMIN_ACCOUNT_NAME",
    "ADMIN_ACCOUNT_PASS", "AC_DB_PASS", "CREATE_ADMIN_ACCOUNT", "INSTALL_WEB_PANEL",
    "PANEL_HTTP_PORT", "TIMEZONE",
)


def load_config(root):
    # Se evalúa la configuración real con Bash, incluyendo ajustes calculados.
    script = 'source "$1"; shift; for key; do printf "%s\\0" "${!key}"; done'
    result = subprocess.run(
        ["bash", "-eu", "-c", script, "asistente", (root / "config.sh").as_posix(), *FIELDS],
        check=True, stdout=subprocess.PIPE,
    )
    values = result.stdout.decode().split("\0")
    if len(values) != len(FIELDS) + 1:
        raise ValueError("config.sh imprime contenido inesperado; usa el instalador avanzado.")
    return dict(zip(FIELDS, values[:-1]))


def detected_ip():
    try:
        # No envía datos: consulta la dirección de origen de la ruta por defecto.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
            connection.connect(("192.0.2.1", 80))
            return connection.getsockname()[0]
    except OSError:
        return ""


def validate(key, value):
    if key == "REALM_IP":
        try:
            address = ipaddress.IPv4Address(value)
        except ValueError:
            return False
        return not (address.is_unspecified or address.is_multicast or address.is_loopback)
    if key == "REALM_NAME":
        # Se usa también en SQL y ficheros .conf de las fases existentes.
        return bool(re.fullmatch(r"[\w .-]{1,32}", value))
    if key == "ADMIN_ACCOUNT_NAME":
        return bool(re.fullmatch(r"[A-Za-z0-9_]{1,16}", value))
    if key == "ADMIN_ACCOUNT_PASS":
        return bool(re.fullmatch(r"[A-Za-z0-9@!_+.-]{8,16}", value))
    return False


def ask(label, default, key):
    while True:
        value = input(f"{label}" + (f" [{default}]" if default else "") + ": ").strip() or default
        if validate(key, value):
            return value
        print("Valor no válido. " + {
            "REALM_IP": "Escribe la IPv4 del servidor en tu red, por ejemplo 192.168.1.100.",
            "REALM_NAME": "Usa entre 1 y 32 letras, números, espacios, puntos, guiones o _.",
            "ADMIN_ACCOUNT_NAME": "Usa entre 1 y 16 letras sin acentos, números o _.",
        }[key])


def ask_password():
    while True:
        value = getpass.getpass("Contraseña de tu cuenta (8-16 caracteres): ")
        if not validate("ADMIN_ACCOUNT_PASS", value):
            print("Usa letras sin acentos, números o @ ! _ + . - (entre 8 y 16).")
            continue
        if value == getpass.getpass("Repite la contraseña: "):
            return value
        print("Las contraseñas no coinciden.")


def render_local(original, changes):
    """Aplica `changes` sobre el texto de config.local.sh (vacío si aún no existe)."""
    result = original or LOCAL_HEADER
    for key, value in changes.items():
        if key not in FIELDS:
            raise ValueError(f"Opción fuera del asistente: {key}")
        pattern = re.compile(r"^(" + re.escape(key) + r")=.*$", re.MULTILINE)
        found = len(pattern.findall(result))
        line = f"{key}={shlex.quote(value)}"
        if found > 1:
            raise ValueError(f"{key} tiene una definición personalizada. Usa {LOCAL} manualmente.")
        if found:
            result = pattern.sub(lambda _: line, result)
        else:
            result = result + ("" if result.endswith("\n") else "\n") + line + "\n"
    return result


def read_local(root):
    path = root / LOCAL
    return path.read_text(encoding="utf-8") if path.exists() else ""


def save_config(root, original, changes):
    """Guarda los ajustes en config.local.sh; config.sh no se toca. Devuelve la copia previa o None."""
    path = root / LOCAL
    updated = render_local(original, changes)
    # Validar antes de tocar el original y detectar ediciones simultáneas.
    subprocess.run(["bash", "-n"], input=updated.encode(), check=True)
    if read_local(root) != original:
        raise ValueError(f"{LOCAL} cambió mientras usabas el asistente. Vuelve a abrirlo.")
    backup = None
    if path.exists():
        backups = root / ".instalacion" / "copias"
        backups.mkdir(parents=True, exist_ok=True, mode=0o700)
        backup = backups / (datetime.now().strftime("config-local-%Y%m%d-%H%M%S-") + secrets.token_hex(4) + ".sh")
        with backup.open("x", encoding="utf-8", newline="\n") as stream:
            os.chmod(backup, 0o600)
            stream.write(original)
    descriptor, temporary = tempfile.mkstemp(prefix=".config-", dir=root)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(updated)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return backup


def preflight(config):
    errors = []
    if sys.platform != "linux":
        return ["Ejecuta el asistente en el servidor Ubuntu 24.04, no en el PC Windows."]
    release = dict(line.split("=", 1) for line in Path("/etc/os-release").read_text().splitlines() if "=" in line)
    if release.get("ID", "").strip('"') != "ubuntu" or release.get("VERSION_ID", "").strip('"') != "24.04":
        errors.append("El modo sencillo requiere Ubuntu 24.04. Para otro sistema, revisa INSTALL_ES.md.")
    if os.geteuid() == 0:
        errors.append("Ejecuta ./install.sh --guiado con tu usuario normal, sin sudo.")
    for command in ("sudo", "bash", "systemctl", "flock", "sha256sum"):
        if not shutil.which(command):
            errors.append(f"Falta el programa {command}.")
    if not Path("/run/systemd/system").is_dir():
        errors.append("El servidor debe arrancar con systemd (Ubuntu Server normal).")
    target = Path(config["AC_DIR"]).expanduser()
    while not target.exists():
        target = target.parent
    free_gib = shutil.disk_usage(target).free / 1024**3
    memory = re.search(r"^MemTotal:\s+(\d+)", Path("/proc/meminfo").read_text(), re.MULTILINE)
    ram_gib = int(memory[1]) / 1024**2 if memory else 0
    print(f"Equipo: {os.cpu_count()} CPU, {ram_gib:.1f} GiB RAM, {free_gib:.1f} GiB libres.")
    print("Equipo de pruebas del proyecto: 8 CPU, 16 GB RAM y disco de 120 GB; la compilación tarda decenas de minutos.")
    if ram_gib < 14:
        errors.append("Asigna 16 GB de RAM para el perfil actual de módulos y bots.")
    if free_gib < 60:
        errors.append("Deja al menos 60 GiB libres para código, compilación, bases y datos del juego.")
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description="Instalación guiada de Azeroth SP en Ubuntu 24.04.")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--comprobar", action="store_true", help="comprobar el equipo sin cambiar nada")
    mode.add_argument("--configurar", action="store_true", help="guardar los datos sin instalar")
    mode.add_argument("--reanudar", action="store_true", help="retomar el último paso pendiente sin cambiar ajustes")
    args = parser.parse_args(argv)
    if sys.platform != "linux":
        print("Ejecuta este asistente en Ubuntu: ./install.sh --guiado", file=sys.stderr)
        return 1
    config = load_config(ROOT)
    if args.reanudar:
        return subprocess.call(["bash", str(ROOT / "scripts" / "instalar-todo.sh"), "--reanudar"])
    print("Azeroth SP — Instalación guiada\n")
    errors = preflight(config)
    for error in errors:
        print(f"  Pendiente: {error}")
    if errors or args.comprobar:
        return 1 if errors else 0
    if not sys.stdin.isatty():
        print("Abre una terminal interactiva para responder al asistente.")
        return 1
    if (ROOT / ".instalacion" / "estado").exists():
        print("Ya hay una instalación registrada. Retómala con: ./install.sh --reanudar")
        print("Para ajustes posteriores usa config.local.sh o config.sh y los modos de install.sh (INSTALL_ES.md).")
        return 1
    if Path(config["AC_DIR"]).exists():
        print("Ya existe el directorio del servidor. Conservamos su configuración.")
        print("Para mantenerlo o retomar una instalación anterior, sigue INSTALL_ES.md (modo avanzado).")
        return 1
    original = read_local(ROOT)
    print("Se conservan los módulos, bots, tasas y demás ajustes actuales de config.sh.")
    changes = {
        "REALM_IP": ask("Dirección del servidor", detected_ip(), "REALM_IP"),
        "REALM_NAME": ask("Nombre del reino", config["REALM_NAME"], "REALM_NAME"),
        "ADMIN_ACCOUNT_NAME": ask("Nombre de tu cuenta administradora", config["ADMIN_ACCOUNT_NAME"], "ADMIN_ACCOUNT_NAME"),
        "ADMIN_ACCOUNT_PASS": ask_password(),
        "CREATE_ADMIN_ACCOUNT": "true",
    }
    # Sólo sustituir la clave de ejemplo; respetar una clave preparada a mano.
    if config["AC_DB_PASS"] == "acore":
        changes["AC_DB_PASS"] = secrets.token_hex(16)
    print(f"\nReino: {changes['REALM_NAME']} — {changes['REALM_IP']}")
    print(f"Cuenta: {changes['ADMIN_ACCOUNT_NAME']} (contraseña oculta)")
    print(f"Directorio: {config['AC_DIR']}\nZona horaria: {config['TIMEZONE']}")
    print("Panel web: " + (f"http://{changes['REALM_IP']}:{config['PANEL_HTTP_PORT']}" if config["INSTALL_WEB_PANEL"] == "true" else "desactivado en config.sh"))
    print(f"Estos datos se guardan en {LOCAL} (sólo en este equipo); config.sh no se modifica.")
    action = "Guardar configuración" if args.configurar else "Guardar e instalar todo (pedirá sudo)"
    if input(f"{action} [s/N]: ").strip().lower() != "s":
        print("Cancelado. No se ha modificado nada.")
        return 0
    backup = save_config(ROOT, original, changes)
    print(f"Configuración guardada en {LOCAL}." + (f" Copia anterior: {backup}" if backup else ""), flush=True)
    if args.configurar:
        print("Cuando quieras instalar: ./scripts/instalar-todo.sh")
        return 0
    print("Mantén esta terminal abierta. Para retomar un fallo: ./install.sh --reanudar", flush=True)
    return subprocess.call(["bash", str(ROOT / "scripts" / "instalar-todo.sh")])


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyboardInterrupt, EOFError):
        print("\nAsistente interrumpido. Si la instalación había empezado, usa --reanudar.")
        sys.exit(130)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        # CalledProcessError sólo muestra argumentos fijos, nunca las claves.
        print(f"No se pudo continuar: {exc}", file=sys.stderr)
        sys.exit(1)
