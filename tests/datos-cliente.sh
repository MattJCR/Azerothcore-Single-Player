#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# install_client_data() en aislado, con wget y 7z sustituidos por stubs (sin red):
# éxito, SHA-256 incorrecto, descarga cortada y reanudada, reutilización de un
# data.zip verificado, extracción interrumpida y ZIP corrupto. Lo que se prueba es
# la lógica del instalador; el wget -c real se comprueba en una instalación completa.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_PARENT=$(cd "${TMPDIR:-/tmp}" && pwd -P)
TEST_DIR=$(mktemp -d "$TEST_PARENT/azeroth-datos-test.XXXXXX")
cleanup() {
    case "$TEST_DIR" in "$TEST_PARENT"/azeroth-datos-test.*) rm -rf -- "$TEST_DIR" ;; esac
}
trap cleanup EXIT
FAILED=0
fail() { echo "ERROR: $*" >&2; FAILED=1; }

export AC_DIR="$TEST_DIR/no-existe"
source "$ROOT/lib/utils.sh"

# --- "Servidor": un fichero local que el stub de wget copia (con soporte de -c) ---
ORIGEN="$TEST_DIR/origen.zip"
PY=python3; "$PY" -c 0 2>/dev/null || PY=python
"$PY" - "$ORIGEN" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("data-version", "v20.0\n")
    z.writestr("maps/000.map", "m" * 4096)
    z.writestr("mmaps/000.mmap", "n" * 4096)
PY
SHA=$(sha256sum "$ORIGEN" | cut -d' ' -f1)
MALO=$(printf '%064d' 0)

STUBS="$TEST_DIR/stubs"; mkdir -p "$STUBS"
cat > "$STUBS/wget" <<'EOF'
#!/usr/bin/env bash
# Soporta: wget [-c] [--progress=..] -O <fichero> <url>; la url es una ruta local.
cont=0; out=""; url=""
while [ $# -gt 0 ]; do
    case "$1" in -c) cont=1 ;; --progress=*) ;; -O) shift; out="$1" ;; *) url="$1" ;; esac
    shift
done
total=$(stat -c %s "$url")
have=0; [ "$cont" = 1 ] && [ -f "$out" ] && have=$(stat -c %s "$out")
[ "$cont" = 1 ] || : > "$out"
if [ -n "${STUB_WGET_CUT:-}" ] && [ "$have" -eq 0 ]; then
    head -c "$STUB_WGET_CUT" "$url" > "$out"; exit 4          # corte a mitad
fi
tail -c +"$((have + 1))" "$url" >> "$out"
echo "$have" > "$out.reanudado-desde"
EOF
printf '#!/usr/bin/env %s
' "$PY" > "$STUBS/7z"
cat >> "$STUBS/7z" <<'EOF'
import os, sys, zipfile
a = sys.argv[1:]
try:
    if a[0] == "t":
        zipfile.ZipFile(a[1]).testzip(); sys.exit(0)
    dest = [x[2:] for x in a if x.startswith("-o")][0]
    if os.environ.get("STUB_7Z_FALLA"):
        zipfile.ZipFile(a[1]).extract("maps/000.map", dest); sys.exit(2)   # a medias
    zipfile.ZipFile(a[1]).extractall(dest)
except zipfile.BadZipFile:
    sys.exit(2)
EOF
chmod +x "$STUBS/wget" "$STUBS/7z"
export PATH="$STUBS:$PATH"

nuevo() { rm -rf "$TEST_DIR/bin"; mkdir -p "$TEST_DIR/bin"; }
ok()    { ( install_client_data "$1" "$2" "$TEST_DIR/bin" ) >"$TEST_DIR/salida.txt" 2>&1; }

# 1. Éxito: extrae, deja data-version y limpia temporales y marca.
nuevo
ok "$ORIGEN" "$SHA" || { fail "la descarga correcta falló"; cat "$TEST_DIR/salida.txt"; }
[ -f "$TEST_DIR/bin/data-version" ] && [ -f "$TEST_DIR/bin/mmaps/000.mmap" ] || fail "1: no extrajo los datos"
grep -q "sha256:$SHA" "$TEST_DIR/bin/data-version" || fail "1: data-version no deja constancia del hash"
[ ! -e "$TEST_DIR/bin/data.zip" ] && [ ! -e "$TEST_DIR/bin/data.zip.part" ] && [ ! -e "$TEST_DIR/bin/.data-extracting" ] \
    || fail "1: quedaron temporales"

# 2. SHA-256 incorrecto: falla, no extrae nada y no deja la descarga.
nuevo
if ok "$ORIGEN" "$MALO"; then fail "2: aceptó un SHA-256 incorrecto"; fi
[ ! -e "$TEST_DIR/bin/data-version" ] && [ ! -e "$TEST_DIR/bin/maps" ] || fail "2: extrajo con un hash incorrecto"
[ ! -e "$TEST_DIR/bin/data.zip" ] && [ ! -e "$TEST_DIR/bin/data.zip.part" ] || fail "2: conservó la descarga corrupta"
grep -q "no coincide" "$TEST_DIR/salida.txt" || fail "2: el mensaje no explica el hash"

# 3. Descarga cortada: falla conservando la parte; al repetir se reanuda y termina.
nuevo
if STUB_WGET_CUT=1000 ok "$ORIGEN" "$SHA"; then fail "3: no detectó la descarga cortada"; fi
[ -s "$TEST_DIR/bin/data.zip.part" ] || fail "3: no conservó data.zip.part"
[ ! -e "$TEST_DIR/bin/data-version" ] || fail "3: extrajo una descarga incompleta"
ok "$ORIGEN" "$SHA" || { fail "3: no reanudó"; cat "$TEST_DIR/salida.txt"; }
[ "$(cat "$TEST_DIR/bin/data.zip.part.reanudado-desde" 2>/dev/null || cat "$TEST_DIR/bin/data.zip.reanudado-desde" 2>/dev/null || echo 0)" != "0" ] \
    || fail "3: no continuó desde donde se cortó"
[ -f "$TEST_DIR/bin/data-version" ] || fail "3: tras reanudar no hay datos"

# 4. data.zip entero y verificado de antes: se reutiliza sin descargar.
nuevo
cp "$ORIGEN" "$TEST_DIR/bin/data.zip"
ok "/no/existe/no-se-descarga.zip" "$SHA" || { fail "4: no reutilizó el data.zip verificado"; cat "$TEST_DIR/salida.txt"; }
[ -f "$TEST_DIR/bin/data-version" ] || fail "4: no extrajo"

# 5. Extracción interrumpida: deja la marca; al repetir se rehace entera.
nuevo
if STUB_7Z_FALLA=1 ok "$ORIGEN" "$SHA"; then fail "5: no detectó el fallo de extracción"; fi
[ -e "$TEST_DIR/bin/.data-extracting" ] || fail "5: no dejó la marca de extracción"
[ ! -e "$TEST_DIR/bin/data-version" ] || fail "5: data-version presente tras extracción a medias"
ok "$ORIGEN" "$SHA" || { fail "5: no rehízo la extracción"; cat "$TEST_DIR/salida.txt"; }
[ -f "$TEST_DIR/bin/data-version" ] && [ ! -e "$TEST_DIR/bin/.data-extracting" ] || fail "5: no completó o dejó la marca"

# 6. ZIP corrupto con el hash "esperado" (manifiesto erróneo): falla y se borra.
nuevo
head -c 2000 /dev/urandom > "$TEST_DIR/roto.zip"
RSHA=$(sha256sum "$TEST_DIR/roto.zip" | cut -d' ' -f1)
if ok "$TEST_DIR/roto.zip" "$RSHA"; then fail "6: aceptó un ZIP inválido"; fi
[ ! -e "$TEST_DIR/bin/data.zip" ] && [ ! -e "$TEST_DIR/bin/data-version" ] || fail "6: dejó restos de un ZIP inválido"

[ "$FAILED" -eq 0 ] && echo "OK: datos del cliente — hash, reanudación, reutilización, extracción interrumpida y ZIP inválido" || exit 1
