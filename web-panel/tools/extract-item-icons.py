#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Extrae los iconos de objetos del cliente 3.3.5a para la armería web."""

import argparse
import hashlib
import io
import json
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Falta Pillow. Ejecuta: pip install -r tools/requirements-icons.txt (o apt install python3-pil)")

try:
    import mpyq
except ImportError:
    mpyq = None  # sólo hace falta para leer un cliente (--client); con --insumos no


DBC_CANDIDATES = (
    "patch-G.mpq",
    "esES/patch-esES-3.MPQ",
    "enUS/patch-enUS-3.MPQ",
)
ICON_ARCHIVES = (
    "patch-I.mpq",
    "patch-3.MPQ",
    "patch-2.MPQ",
    "patch.MPQ",
    "lichking.MPQ",
    "expansion.MPQ",
    "common-2.MPQ",
    "common.MPQ",
)
DBC_PATH = r"DBFilesClient\ItemDisplayInfo.dbc"
FALLBACK_ICON = "inv_misc_questionmark"


def find_file(directory, relative_name):
    current = directory
    for part in Path(relative_name).parts:
        matches = {child.name.lower(): child for child in current.iterdir()}
        current = matches.get(part.lower())
        if current is None:
            return None
    return current if current.is_file() else None


def read_dbc(data_directory):
    if mpyq is None:
        sys.exit("Falta mpyq para leer un cliente. Ejecuta: pip install -r tools/requirements-icons.txt")
    errors = []
    for relative_name in DBC_CANDIDATES:
        mpq_path = find_file(data_directory, relative_name)
        if not mpq_path:
            continue
        try:
            contents = mpyq.MPQArchive(str(mpq_path)).read_file(DBC_PATH)
        except Exception as error:  # mpyq presenta errores distintos según el MPQ
            errors.append(f"{mpq_path.name}: {error}")
            continue
        if contents:
            return mpq_path, contents
    detail = f" ({'; '.join(errors)})" if errors else ""
    raise RuntimeError(f"No se encontró {DBC_PATH} en el cliente{detail}")


def dbc_rows(contents):
    if len(contents) < 20:
        raise RuntimeError("ItemDisplayInfo.dbc está truncado")
    magic, record_count, field_count, record_size, _ = struct.unpack("<4siiii", contents[:20])
    if magic != b"WDBC" or field_count != 25 or record_size != field_count * 4:
        raise RuntimeError("ItemDisplayInfo.dbc no tiene el formato esperado de WotLK 3.3.5a")
    records_end = 20 + record_count * record_size
    strings = contents[records_end:]

    def text(offset):
        if offset <= 0 or offset >= len(strings):
            return ""
        end = strings.find(b"\0", offset)
        if end < 0:
            raise RuntimeError("ItemDisplayInfo.dbc contiene una cadena sin terminar")
        return strings[offset:end].decode("latin-1")

    for index in range(record_count):
        start = 20 + index * record_size
        values = struct.unpack(f"<{field_count}i", contents[start:start + record_size])
        # En 3.3.5a los campos 5 y 6 son InventoryIcon[2].
        yield values[0], tuple(text(values[field]) for field in (5, 6) if values[field])


def icon_key(name):
    value = name.lower()
    if value.endswith((".blp", ".tga")):
        value = value[:-4]
    return value


def safe_name(name, used):
    base = re.sub(r"[^a-z0-9_.-]+", "_", name.strip().lower()).strip("._") or "icon"
    candidate = base
    if candidate in used and used[candidate] != name:
        candidate = f"{base}-{hashlib.sha1(name.encode('latin-1')).hexdigest()[:8]}"
    used[candidate] = name
    return candidate


def open_icon_archives(data_directory):
    if mpyq is None:
        sys.exit("Falta mpyq para leer un cliente. Ejecuta: pip install -r tools/requirements-icons.txt")
    archives = []
    for relative_name in ICON_ARCHIVES:
        mpq_path = find_file(data_directory, relative_name)
        if not mpq_path:
            continue
        try:
            archive = mpyq.MPQArchive(str(mpq_path))
        except Exception:
            continue
        listed = {}
        for member in archive.files or []:
            decoded = member.decode("latin-1") if isinstance(member, bytes) else member
            listed[decoded.replace("/", "\\").lower()] = member
        archives.append((mpq_path, archive, listed))
    if not archives:
        raise RuntimeError("No se encontró ningún MPQ de arte compatible")
    return archives


def read_icon(name, archives):
    key = icon_key(name)
    candidates = (f"interface\\icons\\{key}.blp", f"interface\\icons\\{name.lower()}")
    for _, archive, listed in archives:
        for candidate in candidates:
            member = listed.get(candidate, candidate)
            try:
                contents = archive.read_file(member)
            except Exception:
                contents = None
            if contents:
                return contents
    return None


def convert_webp(contents, output_path):
    with Image.open(io.BytesIO(contents)) as image:
        image.load()
        image.save(output_path, "WEBP", lossless=True, method=4)


RECIPE_VERSION = 1


def generate(rows, read_icon_for, final_directory, source_lines):
    """Genera los WebP y map.json en `final_directory` (se sustituye entera al final).

    `rows` son (displayId, nombres de icono); `read_icon_for(nombre)` devuelve los
    bytes del BLP o None. Devuelve un resumen con las cifras y el contenido de map.json.
    """
    icon_names = {icon_key(name): name for _, names in rows for name in names}
    icon_names.setdefault(FALLBACK_ICON, "INV_Misc_QuestionMark")
    final_directory.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="item-icons-", dir=final_directory.parent))
    used_names = {}
    generated = {}
    missing = []
    try:
        for index, (key, original_name) in enumerate(sorted(icon_names.items()), 1):
            contents = read_icon_for(original_name)
            if not contents:
                missing.append(original_name)
                continue
            filename = f"{safe_name(key, used_names)}-{hashlib.sha256(contents).hexdigest()[:10]}"
            convert_webp(contents, temporary / f"{filename}.webp")
            generated[key] = filename
            if index % 500 == 0:
                print(f"Convertidos {index}/{len(icon_names)} iconos...", flush=True)

        fallback = generated.get(FALLBACK_ICON)
        if not fallback:
            raise RuntimeError("No se pudo extraer el icono genérico de objeto")

        maximum_id = max(display_id for display_id, _ in rows)
        display_icons = [fallback] * (maximum_id + 1)
        mapped_to_fallback = 0
        for display_id, names in rows:
            resolved = next((generated.get(icon_key(name)) for name in names if generated.get(icon_key(name))), None)
            if resolved:
                display_icons[display_id] = resolved
            else:
                mapped_to_fallback += 1

        manifest = {"version": 1, "fallback": fallback, "icons": display_icons}
        (temporary / "map.json").write_text(
            json.dumps(manifest, ensure_ascii=True, separators=(",", ":")), encoding="utf-8"
        )
        source_text = f"""# Iconos de objetos de WotLK 3.3.5a

Recursos generados; no deben editarse a mano.

{source_lines}
- Apariencias: {len(rows)}
- Iconos WebP: {len(generated)}
- Referencias de icono sin arte: {len(missing)}
- Apariencias con fallback: {mapped_to_fallback}

Referencias sin arte: {', '.join(sorted(missing)) if missing else 'ninguna'}.
"""
        (temporary / "SOURCE.md").write_text(source_text, encoding="utf-8")

        if final_directory.exists():
            shutil.rmtree(final_directory)
        temporary.rename(final_directory)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    return {
        "recipe": RECIPE_VERSION, "appearances": len(rows), "icons": len(generated),
        "missing": len(missing), "fallback": mapped_to_fallback,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--client", type=Path, help="Carpeta que contiene Wow.exe y Data/")
    source.add_argument("--insumos", type=Path,
                        help="Carpeta con los ficheros ya extraídos: dbc/ItemDisplayInfo.dbc e iconos/<nombre>.blp")
    parser.add_argument("--salida", type=Path, help="Carpeta de destino (por defecto public/assets/item-icons)")
    args = parser.parse_args()

    project_root = Path(__file__).resolve().parents[1]
    final_directory = (args.salida or project_root / "public" / "assets" / "item-icons").resolve()

    if args.insumos:
        base = args.insumos.resolve()
        dbc_path = base / "dbc" / "ItemDisplayInfo.dbc"
        if not dbc_path.is_file():
            parser.error(f"Falta {dbc_path}")
        dbc = dbc_path.read_bytes()
        rows = list(dbc_rows(dbc))
        icon_directory = base / "iconos"
        # El navegador envía cada arte como iconos/<número>.blp y un índice
        # {clave de icono: número}; así los nombres del cliente (con espacios o
        # símbolos) nunca son nombres de fichero del servidor.
        index_path = icon_directory / "indice.json"
        icon_index = json.loads(index_path.read_text(encoding="utf-8")) if index_path.is_file() else {}

        def read_icon_for(name):
            identifier = icon_index.get(icon_key(name))
            if not identifier or not re.fullmatch(r"[0-9]{1,6}", str(identifier)):
                return None
            path = icon_directory / f"{identifier}.blp"
            return path.read_bytes() if path.is_file() else None

        source_lines = (f"- DBC: `ItemDisplayInfo.dbc` (`sha256:{hashlib.sha256(dbc).hexdigest()}`), extraído por el panel\n"
                        "- Arte: iconos BLP recibidos del cliente")
    else:
        data_directory = args.client.resolve() / "Data"
        if not data_directory.is_dir():
            parser.error(f"No existe la carpeta del cliente: {data_directory}")
        dbc_mpq, dbc = read_dbc(data_directory)
        rows = list(dbc_rows(dbc))
        archives = open_icon_archives(data_directory)

        def read_icon_for(name):
            return read_icon(name, archives)

        source_names = ", ".join(path.name for path, _, _ in archives)
        source_lines = (f"- DBC: `{dbc_mpq.name}` (`sha256:{hashlib.sha256(dbc).hexdigest()}`)\n"
                        f"- MPQ de arte consultados: {source_names}")

    summary = generate(rows, read_icon_for, final_directory, source_lines)
    total_bytes = sum(path.stat().st_size for path in final_directory.iterdir())
    print(f"Generados {summary['icons']} iconos ({total_bytes / 1024 / 1024:.1f} MiB) en {final_directory}")
    print(f"Referencias sin arte: {summary['missing']}; apariencias con fallback: {summary['fallback']}")


if __name__ == "__main__":
    main()
