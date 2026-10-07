#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  instalar-todo.sh — instalación completa y desatendida, de una tacada
#
#  Entrada normal: ./install.sh --guiado (o --reanudar). Este script es el
#  motor que usa ese modo por dentro; se documenta aquí para quien prefiera
#  el control manual sin pasar por el asistente. Para un administrador que
#  quiere el servidor funcionando sin ir fase a fase. Hace, en orden y
#  parándose en el primer error:
#
#    1. comprobaciones previas (Ubuntu, sudo, red, config.sh, TERM)
#    2. ./install.sh          fases 1-7: dependencias, MySQL, código, compilar,
#                             configurar, systemd, automatización (~35 min)
#    3. tools/primer-arranque.sh   primer arranque del worldserver: crea las BD
#    4. ./install.sh --post   realmlist, cuenta admin, SQL pendiente y panel web
#    5. arranca los servicios y espera al puerto 8085
#    6. tools/verificar-instalacion.sh   y el resumen de qué hacer en el cliente
#
#  Uso manual (desde la raíz del proyecto):
#      nano config.sh                              # REALM_IP, contraseñas, módulos
#      chmod +x scripts/instalar-todo.sh
#      ./scripts/instalar-todo.sh                   # pide la contraseña de sudo UNA vez
#      ./scripts/instalar-todo.sh --sin-sudo-temporal   # si ya tienes sudo sin contraseña
#      ./scripts/instalar-todo.sh --desde 4         # retomar desde la fase 4
#      ./scripts/instalar-todo.sh --reanudar       # retomar automáticamente el paso pendiente
#
#  Deja el registro en ~/instalacion-completa.log. Mantén abierta la sesión;
#  si se interrumpe, ./install.sh --reanudar continúa desde el último paso pendiente.
#
#  Lo que NO hace: instalar la carpeta cliente/ (patch-<idioma>-4.MPQ y los
#  addons) en cada PC que juegue: cliente/instalar-cliente.ps1 o README.md,
#  "Después de instalar".
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

LOG="$HOME/instalacion-completa.log"
# Proteger el registro sin cambiar la umask que heredan las fases (algunos
# directorios del panel deben poder leerlos sus usuarios de servicio).
(umask 077; touch "$LOG")
chmod 600 "$LOG"
FROM_PHASE=1
RESUME=false
EXPLICIT_FROM=false
TEMP_SUDO=true
SUDOERS_TMP="/etc/sudoers.d/99-instalar-todo"
SUDO_CREATED=false

while [ $# -gt 0 ]; do
    case "$1" in
        --desde)             FROM_PHASE="${2:?'--desde requiere un número de fase'}"; EXPLICIT_FROM=true; shift ;;
        --reanudar)          RESUME=true ;;
        --sin-sudo-temporal) TEMP_SUDO=false ;;
        -h|--help)           sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "Opción desconocida: $1"; exit 1 ;;
    esac
    shift
done

[[ "$FROM_PHASE" =~ ^[1-7]$ ]] || { echo "--desde acepta una fase del 1 al 7."; exit 1; }
if [ "$RESUME" = true ] && [ "$EXPLICIT_FROM" = true ]; then
    echo "Elige --reanudar o --desde, no ambos."; exit 1
fi

# El instalador usa tput: sin TERM aborta en la primera línea (INSTALL_ES.md parte 4 §4).
export TERM="${TERM:-xterm-256color}"
[ "$TERM" = dumb ] && export TERM=xterm-256color

ts() { date '+%Y-%m-%d %H:%M:%S'; }
say() { echo "[$(ts)] $*" | tee -a "$LOG"; }
die() { say "ERROR: $*"; say "Registro completo: $LOG"; exit 1; }

say "=== instalar-todo.sh: instalación completa desatendida ==="

# ── 1. Comprobaciones previas ────────────────────────────────────────────────
[ -f config.sh ] || die "No existe config.sh en $SCRIPT_DIR."
[ -f install.sh ] || die "No existe install.sh en $SCRIPT_DIR."
# shellcheck disable=SC1091
source ./config.sh
source ./lib/install-state.sh

[ "$(id -u)" -ne 0 ] || die "No lo lances como root: usa el usuario que va a correr el servidor (por ejemplo 'acore')."
command -v sudo >/dev/null || die "Falta sudo."

# Un solo instalador completo por usuario. La huella impide saltarse pasos si
# se cambia de directorio, de configuración o de versiones entre intentos.
mkdir -p -m 700 "$SCRIPT_DIR/.instalacion"
exec 9> "$HOME/.azerothcore-install.lock"
flock -n 9 || die "Ya hay una instalación completa en curso con este usuario."
export AC_INSTALL_STATE_FILE="$SCRIPT_DIR/.instalacion/estado"
AC_INSTALL_FINGERPRINT=$( { sha256sum "$SCRIPT_DIR/config.sh" "$SCRIPT_DIR/versions.lock"; [ ! -f "${AC_CONFIG_LOCAL:-}" ] || sha256sum "$AC_CONFIG_LOCAL"; printf '%s\n' "$AC_DIR"; } | sha256sum | cut -d' ' -f1)
export AC_INSTALL_FINGERPRINT
NEXT_STEP="$FROM_PHASE"
if [ "$RESUME" = true ]; then
    [ -f "$AC_INSTALL_STATE_FILE" ] || die "No hay instalación guardada. Ejecuta ./install.sh --guiado, o scripts/instalar-todo.sh --desde N para una instalación anterior."
    read -r SAVED_FINGERPRINT NEXT_STEP EXTRA < "$AC_INSTALL_STATE_FILE" || die "Estado de instalación ilegible."
    [[ "$NEXT_STEP" =~ ^([1-9]|1[0-2])$ ]] && [ -z "$EXTRA" ] || die "Estado de instalación no válido."
    [ "$SAVED_FINGERPRINT" = "$AC_INSTALL_FINGERPRINT" ] || die "Han cambiado config.sh, config.local.sh, versions.lock o el destino. Revisa los cambios y usa --desde N para elegir qué reaplicar (INSTALL_ES.md)."
    if [ "$NEXT_STEP" -eq 12 ]; then
        say "La instalación ya terminó. Reino: $REALM_NAME @ $REALM_IP. Mantenimiento: INSTALL_ES.md."
        exit 0
    fi
    FROM_PHASE="$NEXT_STEP"
    say "Retomando el paso $NEXT_STEP/11; se conservan los pasos completados."
elif [ -f "$AC_INSTALL_STATE_FILE" ] && [ "$EXPLICIT_FROM" = false ]; then
    die "Ya existe una instalación registrada. Usa --reanudar o el modo avanzado --desde N."
fi
grep -qi ubuntu /etc/os-release 2>/dev/null || say "AVISO: esto se ha probado en Ubuntu 24.04; en otra distribución la fase 1 puede fallar."

if [ -z "${REALM_IP:-}" ] || [ "${REALM_IP}" = "127.0.0.1" ]; then
    say "AVISO: REALM_IP es '${REALM_IP:-}'. Sólo podrás conectar desde esta misma máquina. Cámbialo en config.local.sh (o en config.sh) si no es lo que quieres."
fi

if ! getent hosts github.com >/dev/null 2>&1; then
    say "AVISO: no se resuelve github.com. Sin red, la fase 3 tirará de mirrors/ si existen."
fi

if [ "$FROM_PHASE" -le 1 ] && [ -d "${AC_DIR:-$HOME/azerothcore}" ]; then
    die "Ya existe ${AC_DIR:-$HOME/azerothcore}. Para reinstalar desde cero sigue INSTALL_ES.md parte 4 §4 (copia de seguridad, borrar BD y carpeta), o usa --desde N para retomar."
fi
save_install_step "$NEXT_STEP"

# ── 2. sudo sin contraseña mientras dura la instalación ──────────────────────
# Las fases 1, 2, 6 y 7 usan sudo. Escrito así y sólo así (INSTALL_ES.md parte 4 §4):
# con `sudo -S tee` el fichero queda vacío.
cleanup_sudo() {
    if [ "$SUDO_CREATED" = true ] && sudo -n test -f "$SUDOERS_TMP" 2>/dev/null; then
        sudo -n rm -f "$SUDOERS_TMP" && say "sudo temporal retirado ($SUDOERS_TMP)."
    fi
}
trap cleanup_sudo EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

if [ "$TEMP_SUDO" = true ]; then
    if sudo -n true 2>/dev/null; then
        say "sudo ya funciona sin contraseña: no hace falta el temporal."
        TEMP_SUDO=false
    else
        say "Hace falta tu contraseña de sudo una vez, para las fases con apt, MySQL y systemd."
        read -r -s -p "Contraseña de sudo para $(whoami): " SUDO_PASS; echo
        echo "$SUDO_PASS" | sudo -S bash -c "printf '%s\n' '$(whoami) ALL=(ALL) NOPASSWD: ALL' > '$SUDOERS_TMP' && chmod 440 '$SUDOERS_TMP'" \
            || die "No se pudo escribir $SUDOERS_TMP (¿contraseña incorrecta?)."
        unset SUDO_PASS
        SUDO_CREATED=true
        sudo -n visudo -c -f "$SUDOERS_TMP" >/dev/null || die "$SUDOERS_TMP no pasa visudo -c."
        say "sudo temporal escrito: se retira solo al terminar."
    fi
fi

chmod +x install.sh scripts/*.sh scripts/phases/*.sh tools/*.sh 2>/dev/null || true

# Las fases envían mensajes breves a la terminal aunque su salida detallada
# esté redirigida al registro.
exec 8>&1
export AC_INSTALL_PROGRESS_FD=8

# ── 3. Fases 1-7 ─────────────────────────────────────────────────────────────
if [ "$NEXT_STEP" -le 7 ]; then
    say "Fases ${FROM_PHASE}-7 del instalador (sigue el progreso con: tail -f $LOG)"
    if [ "$FROM_PHASE" -le 1 ]; then
        echo s | ./install.sh >> "$LOG" 2>&1 || die "install.sh falló. Retoma con: ./install.sh --reanudar"
    else
        echo s | ./install.sh --from "$FROM_PHASE" >> "$LOG" 2>&1 || die "install.sh --from $FROM_PHASE falló. Retoma con: ./install.sh --reanudar"
    fi
    say "Fases del instalador completadas."
fi

# ── 4. Primer arranque: crea las bases de datos ──────────────────────────────
if [ "$NEXT_STEP" -le 8 ]; then
    say "Paso 8/11: primer arranque."
    if [ -f "${AC_DIR:-$HOME/azerothcore}/env/dist/bin/worldserver" ]; then
        if mysql -u "${AC_DB_USER:-acore}" -p"${AC_DB_PASS:-acore}" -e "USE acore_world;" >/dev/null 2>&1 \
           && mysql -u "${AC_DB_USER:-acore}" -p"${AC_DB_PASS:-acore}" -N -e "SELECT COUNT(*) FROM acore_world.creature_template;" 2>/dev/null | grep -qE '^[1-9]'; then
            say "acore_world ya está poblada: se salta el primer arranque."
        else
            say "Primer arranque del worldserver (crea e importa las bases de datos; varios minutos)..."
            bash tools/primer-arranque.sh >> "$LOG" 2>&1 || die "El primer arranque no terminó bien. Mira ~/primer-arranque.log y retoma con: ./install.sh --reanudar"
            say "Primer arranque hecho."
        fi
    else
        die "No existe el binario worldserver: la compilación no terminó."
    fi
    save_install_step 9
fi

# ── 5. Post-instalación: realmlist, cuenta admin, NPC, SQL pendiente ─────────
if [ "$NEXT_STEP" -le 9 ]; then
    say "Paso 9/11: post-instalación y panel web..."
    echo s | ./install.sh --post >> "$LOG" 2>&1 || die "install.sh --post falló. Mira $LOG"
    save_install_step 10
fi

# ── 6. Servicios ─────────────────────────────────────────────────────────────
if [ "$NEXT_STEP" -le 10 ]; then
    say "Paso 10/11: arrancando ac-authserver y ac-worldserver..."
    sudo -n systemctl enable --now ac-authserver >> "$LOG" 2>&1 || die "No se pudo arrancar ac-authserver."
    if [ "${WORLDSERVER_STANDBY:-false}" = true ]; then
        # El .socket posee el 8085 y arranca el servicio a la primera conexión; se
        # arranca también el servicio para dejar el mundo listo al terminar la
        # instalación (se dormirá solo tras STANDBY_IDLE_MINUTES sin jugadores).
        sudo -n systemctl start ac-worldserver.socket >> "$LOG" 2>&1 || die "No se pudo arrancar ac-worldserver.socket."
        sudo -n systemctl start ac-worldserver        >> "$LOG" 2>&1 || die "No se pudo arrancar ac-worldserver."
    else
        sudo -n systemctl enable --now ac-worldserver >> "$LOG" 2>&1 || die "No se pudo arrancar ac-worldserver."
    fi

    say "Esperando a que el mundo esté listo (hasta 10 minutos)..."
    WS_LOG="${AC_DIR:-$HOME/azerothcore}/env/dist/bin/Server.log"
    READY=false
    for i in $(seq 1 120); do
        if systemctl is-active --quiet ac-worldserver \
           && grep -aqE '\(worldserver-daemon\) ready' "$WS_LOG" 2>/dev/null; then
            READY=true; break
        fi
        sleep 5
    done
    [ "$READY" = true ] || die "El worldserver no llegó a 'ready'. Mira: journalctl -u ac-worldserver -n 50, y $WS_LOG"
    say "Mundo listo (auth en 3724, world en 8085)."
    save_install_step 11
fi

# ── 7. Verificación y resumen ────────────────────────────────────────────────
say "Paso 11/11: verificación del servidor (doctor)."
if ! bash tools/doctor.sh 2>&1 | tee -a "$LOG"; then
    die "La comprobación detectó fallos. Consulta el registro, corrígelos y ejecuta ./install.sh --reanudar."
fi
save_install_step 12

# Con panel, la instalación sólo está completa cuando los recursos que salen del
# cliente (iconos de la armería, parches de idioma) existen: se generan desde el
# propio panel con la carpeta de WoW del jugador (Addons).
RES_DIR="${PANEL_RESOURCES_DIRECTORY:-/var/lib/azerothcore-panel/recursos}"
CLIENT_PENDING=false
if [ "${INSTALL_WEB_PANEL:-true}" = true ] && { [ ! -f "$RES_DIR/iconos/map.json" ] || [ ! -f "$RES_DIR/parches/esES/patch-esES-4.MPQ" ]; }; then
    CLIENT_PENDING=true
fi
PANEL_URL="https://${REALM_IP:-?}"
[ "${PANEL_HTTPS_PORT:-443}" = 443 ] || PANEL_URL="https://${REALM_IP:-?}:${PANEL_HTTPS_PORT}"

if [ "$CLIENT_PENDING" = true ]; then
cat <<EOF | tee -a "$LOG"

  ╔════════════════════════════════════════════════════════════╗
  ║   SERVIDOR Y PANEL LISTOS — FALTA EL PASO DEL CLIENTE      ║
  ║   La instalación NO está completa hasta hacerlo            ║
  ╚════════════════════════════════════════════════════════════╝

  Realm:   ${REALM_NAME:-?} @ ${REALM_IP:-?}
  Cuenta:  ${ADMIN_ACCOUNT_NAME:-admin} / ${ADMIN_ACCOUNT_PASS:-admin} (GM ${ADMIN_ACCOUNT_GMLEVEL:-3}; cámbiala con .account password)

  Paso que falta (en tu PC, con tu cliente de WoW 3.3.5a):
    1. Abre ${PANEL_URL} (primera vez: instala el certificado que ofrece la página)
       e inicia sesión con la cuenta de arriba.
    2. Menú «Addons»: pulsa «Elegir carpeta de WoW». Como administrador, el panel
       lee de tu cliente lo que necesita, genera los iconos de la armería y los
       parches patch-<idioma>-4.MPQ y los deja listos. No hay que instalar nada más.
    3. Pulsa «Instalar parches» y, en el paso 3, «Instalar» los addons obligatorios.
    - Data/realmlist.wtf  ->  set realmlist ${REALM_IP:-?}

  Mientras no lo hagas, el doctor avisa («recursos-cliente») y el panel muestra
  los parches como pendientes; los jugadores no podrán instalarlos.

  Operar:  INSTALL_ES.md   ·   Registro: $LOG
EOF
else
cat <<EOF | tee -a "$LOG"

  ╔════════════════════════════════════════════════════════════╗
  ║   SERVIDOR INSTALADO Y ARRANCADO                           ║
  ╚════════════════════════════════════════════════════════════╝

  Realm:   ${REALM_NAME:-?} @ ${REALM_IP:-?}
  Cuenta:  ${ADMIN_ACCOUNT_NAME:-admin} / ${ADMIN_ACCOUNT_PASS:-admin} (GM ${ADMIN_ACCOUNT_GMLEVEL:-3}; cámbiala con .account password)

  En cada PC que juegue:
    - Data/realmlist.wtf  ->  set realmlist ${REALM_IP:-?}
    - parches y addons: el panel (${PANEL_URL}, menú «Addons») los instala en el
      cliente; o, en Windows, cliente/instalar-cliente.ps1 -Cliente "C:\ruta\WoW"
    - opcional: patch-V.mpq de individual-progression

  Los bots tardan unos minutos en entrar tras el primer login; la subasta,
  horas (acelerar con '.ahbot update' con el GM). Comandos dentro del juego:
  .grupo (grupo de bots), .actualizaciones (GM), .dc on (mazmorra con tanque bot).

  Operar:  INSTALL_ES.md   ·   Registro: $LOG
EOF
fi
