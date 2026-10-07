#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# apply_server_dbc_overrides() y la comprobación server-dbc del doctor en
# aislado (PLAN AR01, 24/09/2026): los DBC de ARAC sólo se pueden copiar
# cuando existe env/dist/bin/dbc; una vez copiados, el original queda en
# backup-pre-arac/ y una segunda pasada no cambia nada.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_PARENT=$(cd "${TMPDIR:-/tmp}" && pwd -P)
TEST_DIR=$(mktemp -d "$TEST_PARENT/azeroth-dbc-test.XXXXXX")
cleanup() {
    case "$TEST_DIR" in "$TEST_PARENT"/azeroth-dbc-test.*) rm -rf -- "$TEST_DIR" ;; esac
}
trap cleanup EXIT

export AC_DIR="$TEST_DIR/ac"
source "$ROOT/lib/utils.sh"
source "$ROOT/lib/doctor.sh"
INSTALL_LOG="$TEST_DIR/install.log"
INSTALL_MOD_ARAC=true
INSTALL_MOD_INDIVIDUAL_PROGRESSION=false

fail() { echo "FALLO: $*" >&2; exit 1; }
last_doctor() { local E="${DOCTOR_CHECKS[${#DOCTOR_CHECKS[@]}-1]}"; E="${E#*$'\t'}"; echo "${E%%$'\t'*}"; }

SRC="$AC_DIR/extras/mod-arac/patch-contents/DBFilesContent"
DST="$AC_DIR/env/dist/bin/dbc"
mkdir -p "$SRC"
for N in CharBaseInfo CharStartOutfit SkillRaceClassInfo; do
    printf 'arac-%s' "$N" > "$SRC/$N.dbc"
done

# 1. Sin datos del cliente (el caso de la instalación limpia antes del arreglo):
#    falla de forma explícita, no en silencio.
if apply_server_dbc_overrides >/dev/null 2>&1; then fail "copió sin existir $DST"; fi
DOCTOR_CHECKS=(); _doctor_check_server_dbc >/dev/null 2>&1
[ "$(last_doctor)" = warn ] || fail "doctor sin $DST debería avisar"

# 2. Datos del cliente presentes: copia, respalda y el doctor lo da por bueno.
mkdir -p "$DST"
for N in CharBaseInfo CharStartOutfit SkillRaceClassInfo Spell; do
    printf 'original-%s' "$N" > "$DST/$N.dbc"
done
DOCTOR_CHECKS=(); _doctor_check_server_dbc >/dev/null 2>&1
[ "$(last_doctor)" = fail ] || fail "doctor con DBC originales debería fallar"
apply_server_dbc_overrides >/dev/null 2>&1 || fail "no copió con $DST presente"
[ "$SERVER_DBC_CHANGED" -eq 3 ] || fail "esperaba 3 cambios, hubo $SERVER_DBC_CHANGED"
for N in CharBaseInfo CharStartOutfit SkillRaceClassInfo; do
    cmp -s "$SRC/$N.dbc" "$DST/$N.dbc" || fail "$N.dbc no es el de ARAC"
    [ "$(cat "$DST/backup-pre-arac/$N.dbc")" = "original-$N" ] || fail "$N.dbc sin respaldo del original"
done
[ "$(cat "$DST/Spell.dbc")" = "original-Spell" ] || fail "tocó un DBC ajeno a ARAC"
DOCTOR_CHECKS=(); _doctor_check_server_dbc >/dev/null 2>&1
[ "$(last_doctor)" = ok ] || fail "doctor tras copiar debería dar ok"

# 3. Idempotente: segunda pasada sin cambios y el respaldo sigue siendo el original.
apply_server_dbc_overrides >/dev/null 2>&1 || fail "segunda pasada falló"
[ "$SERVER_DBC_CHANGED" -eq 0 ] || fail "segunda pasada cambió $SERVER_DBC_CHANGED ficheros"
[ "$(cat "$DST/backup-pre-arac/CharBaseInfo.dbc")" = "original-CharBaseInfo" ] || fail "respaldo pisado"

# 4. IP con DBC opcionales activados y nunca copiados: el doctor lo detecta.
INSTALL_MOD_INDIVIDUAL_PROGRESSION=true IP_OPTIONAL_DBC=true
DOCTOR_CHECKS=(); _doctor_check_server_dbc >/dev/null 2>&1
[ "$(last_doctor)" = fail ] || fail "doctor sin backup-pre-ip debería fallar"

# 5. Piedra de la sede (mod-guildhouse): el hechizo 600001 se añade al Spell.dbc efectivo y
#    sobrevive a que los DBC opcionales de IP vuelvan a copiar su propio Spell.dbc (orden que la
#    fase 8 tenía al revés hasta el 07/10/2026: una instalación limpia arrancaba sin el hechizo).
INSTALL_MOD_INDIVIDUAL_PROGRESSION=false IP_OPTIONAL_DBC=false
INSTALL_MOD_GUILDHOUSE=true
STONE="$INSTALLER_DIR/tools/piedra_sede_dbc.py"
make_spell() {  # Spell.dbc mínimo con el hechizo base 8690 y 234 campos
    python3 - "$1" <<'PY'
import struct, sys
fields = 234
row = bytearray(fields * 4)
struct.pack_into("<I", row, 0, 8690)
open(sys.argv[1], "wb").write(struct.pack("<4s4I", b"WDBC", 1, fields, fields * 4, 1) + bytes(row) + b"\0")
PY
}
make_spell "$DST/Spell.dbc"
python3 "$STONE" --comprobar "$DST/Spell.dbc" && fail "el Spell.dbc base ya llevaba la piedra"
DOCTOR_CHECKS=(); _doctor_check_server_dbc >/dev/null 2>&1
[ "$(last_doctor)" = fail ] || fail "doctor sin la piedra debería fallar"
apply_server_dbc_overrides >/dev/null 2>&1 || fail "no añadió la piedra"
python3 "$STONE" --comprobar "$DST/Spell.dbc" || fail "tras aplicar, Spell.dbc no lleva 600001"
[ "$SERVER_DBC_CHANGED" -eq 1 ] || fail "esperaba 1 cambio (Spell.dbc), hubo $SERVER_DBC_CHANGED"
DOCTOR_CHECKS=(); _doctor_check_server_dbc >/dev/null 2>&1
[ "$(last_doctor)" = ok ] || fail "doctor con la piedra debería dar ok"
apply_server_dbc_overrides >/dev/null 2>&1 || fail "segunda pasada falló"
[ "$SERVER_DBC_CHANGED" -eq 0 ] || fail "segunda pasada con la piedra cambió $SERVER_DBC_CHANGED ficheros"
# Un Spell.dbc de IP pisa el parcheado: la siguiente pasada vuelve a dejar la piedra.
make_spell "$DST/Spell.dbc"
apply_server_dbc_overrides >/dev/null 2>&1 || fail "no repuso la piedra tras pisarla"
python3 "$STONE" --comprobar "$DST/Spell.dbc" || fail "la piedra no sobrevivió al Spell.dbc de IP"

echo "OK: server-dbc — sin datos falla explícito, copia con respaldo único, idempotente, no toca otros DBC y el doctor distingue los casos."
