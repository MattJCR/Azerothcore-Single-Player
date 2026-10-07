#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# set_conf_value()/flush_conf_files() en aislado:
# la cola diferida por fichero debe producir el MISMO resultado que la
# reescritura inmediata anterior, clave por clave.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_PARENT=$(cd "${TMPDIR:-/tmp}" && pwd -P)
TEST_DIR=$(mktemp -d "$TEST_PARENT/azeroth-conf-test.XXXXXX")
cleanup() {
    case "$TEST_DIR" in "$TEST_PARENT"/azeroth-conf-test.*) rm -rf -- "$TEST_DIR" ;; esac
}
trap cleanup EXIT

# AC_DIR inexistente a propósito: lib/utils.sh cae a /tmp para el log, que es
# justo lo que se quiere en un test aislado.
export AC_DIR="$TEST_DIR/no-existe"
source "$ROOT/lib/utils.sh"

FILE_A="$TEST_DIR/a.conf"
FILE_B="$TEST_DIR/b.conf"

cat > "$FILE_A" <<'EOF'
# Comentario de cabecera
Rate.XP.Kill = 1.0
Rate.XP.Quest = 1.0

# Clave duplicada por error histórico: ambas líneas deben actualizarse igual
Rate.Reputation.Gain = 1.0
Rate.Reputation.Gain = 1.0

GM.StartLevel = 3
Password.Test = old|value\with&chars
EOF

cat > "$FILE_B" <<'EOF'
X.Enable = 0
EOF

# 1. Clave existente, sustitución simple.
set_conf_value "$FILE_A" "Rate.XP.Kill" "2.5"
# 2. La misma clave dos veces antes del flush: gana el último valor, igual
#    que con sed -i secuencial.
set_conf_value "$FILE_A" "GM.StartLevel" "1"
set_conf_value "$FILE_A" "GM.StartLevel" "2"
# 3. Clave duplicada en el fichero: las DOS líneas deben quedar iguales.
set_conf_value "$FILE_A" "Rate.Reputation.Gain" "9.0"
# 4. Valor con metacaracteres de sed (| \ &) sin corromper el fichero.
set_conf_value "$FILE_A" 'Password.Test' 'new|val\ue&x'
# 5. Clave que no existe en el fichero: se añade y avisa.
set_conf_value "$FILE_A" "No.Existe.Clave" "9"
# 6. Clave de la lista blanca (mod-playerbots): se añade SIN aviso de "no declarada".
set_conf_value "$FILE_A" "PlayerbotsDatabaseInfo" "127.0.0.1;3306;acore;acore;acore_playerbots"
# 7. Un segundo fichero en la misma cola: sólo debe tocarse a sí mismo.
set_conf_value "$FILE_B" "X.Enable" "1"

BEFORE_MTIME_B=$(stat -c %Y "$FILE_B" 2>/dev/null || stat -f %m "$FILE_B")

flush_conf_files

# ── Aserciones sobre el contenido final ──────────────────────────────────
grep -qx 'Rate.XP.Kill = 2.5' "$FILE_A" || { echo "FALLO: Rate.XP.Kill no se sustituyó"; exit 1; }
grep -qx 'GM.StartLevel = 2' "$FILE_A" || { echo "FALLO: GM.StartLevel no se quedó con el último valor"; exit 1; }
[ "$(grep -c '^GM.StartLevel = ' "$FILE_A")" = 1 ] || { echo "FALLO: GM.StartLevel se duplicó"; exit 1; }
[ "$(grep -c '^Rate.Reputation.Gain = 9.0$' "$FILE_A")" = 2 ] || { echo "FALLO: las dos líneas duplicadas no se actualizaron igual"; exit 1; }
grep -qxF 'Password.Test = new|val\ue&x' "$FILE_A" || { echo "FALLO: metacaracteres de sed corrompieron el valor"; exit 1; }
grep -qx 'No.Existe.Clave = 9' "$FILE_A" || { echo "FALLO: la clave desconocida no se añadió"; exit 1; }
grep -qx 'PlayerbotsDatabaseInfo = 127.0.0.1;3306;acore;acore;acore_playerbots' "$FILE_A" || { echo "FALLO: la clave de la lista blanca no se añadió"; exit 1; }
grep -qx '# Comentario de cabecera' "$FILE_A" || { echo "FALLO: se perdió un comentario"; exit 1; }
grep -qx 'X.Enable = 1' "$FILE_B" || { echo "FALLO: el segundo fichero no se aplicó"; exit 1; }
[ "$(grep -c '^Rate.XP.Kill' "$FILE_B" 2>/dev/null || true)" = 0 ] || { echo "FALLO: b.conf se contaminó con claves de a.conf"; exit 1; }

# ── El resumen de claves no declaradas sólo lleva la de verdad desconocida ──
[ "$CONF_KEYS_DESCONOCIDAS" = "a.conf:No.Existe.Clave" ] || { echo "FALLO: CONF_KEYS_DESCONOCIDAS = '$CONF_KEYS_DESCONOCIDAS'"; exit 1; }

# ── set_conf_value() no debe tocar disco hasta flush_conf_files() ──────────
cat > "$FILE_B" <<'EOF'
X.Enable = 1
EOF
BEFORE_MTIME_B=$(stat -c %Y "$FILE_B" 2>/dev/null || stat -f %m "$FILE_B")
set_conf_value "$FILE_A" "GM.StartLevel" "5"
sleep 1.1   # margen para que un mtime con resolución de un segundo pudiera cambiar
AFTER_MTIME_B=$(stat -c %Y "$FILE_B" 2>/dev/null || stat -f %m "$FILE_B")
[ "$BEFORE_MTIME_B" = "$AFTER_MTIME_B" ] || { echo "FALLO: set_conf_value() escribió en disco antes de flush_conf_files()"; exit 1; }
flush_conf_files
grep -qx 'GM.StartLevel = 5' "$FILE_A" || { echo "FALLO: la segunda tanda de cambios no se aplicó"; exit 1; }

# ── Un fichero sin cambios pendientes no debe tocarse en el flush ──────────
cp "$FILE_B" "$TEST_DIR/b.conf.before-noop-flush"
flush_conf_files
cmp -s "$FILE_B" "$TEST_DIR/b.conf.before-noop-flush" || { echo "FALLO: flush_conf_files() sin cambios pendientes tocó un fichero"; exit 1; }

echo 'OK: set_conf_value()/flush_conf_files() — sustitución, última-gana, duplicados, metacaracteres, claves desconocidas/lista blanca, comentarios, ficheros aislados y sin escritura antes del flush.'
