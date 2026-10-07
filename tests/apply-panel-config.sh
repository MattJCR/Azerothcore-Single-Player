#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# apply-panel-config.sh (generado por scripts/phases/06_systemd.sh) en
# aislado, con mysql/systemctl/sudo sustituidos por stubs.
#
# Cubre el modelo de lote inmutable y publicación atómica: éxito con dos
# ficheros, segunda clave inválida (aborta sin tocar los originales), fallo
# de lectura SQL (se detecta explícito, no se confunde con "lote vacío") y
# fallo de reinicio (los .conf ya quedan publicados, las revisiones NO se
# borran para reintentarse en la siguiente pasada).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_PARENT=$(cd "${TMPDIR:-/tmp}" && pwd -P)
TEST_DIR=$(mktemp -d "$TEST_PARENT/azeroth-apc-test.XXXXXX")
cleanup() {
    case "$TEST_DIR" in "$TEST_PARENT"/azeroth-apc-test.*) rm -rf -- "$TEST_DIR" ;; esac
}
trap cleanup EXIT

# Extrae el cuerpo real del script generado (entre "cat << 'APPLYSCRIPT'" y
# el delimitador de cierre) del propio fichero fuente: si alguien lo edita,
# esta prueba corre contra el texto de verdad, no contra una copia aparte
# que pudiera desincronizarse.
awk '/^cat << .APPLYSCRIPT.$/{flag=1; next} /^APPLYSCRIPT$/{flag=0} flag' \
    "$ROOT/scripts/phases/06_systemd.sh" > "$TEST_DIR/apply-body.sh"
[ -s "$TEST_DIR/apply-body.sh" ] || { echo "FALLO: no se pudo extraer el cuerpo de apply-panel-config.sh de 06_systemd.sh"; exit 1; }
bash -n "$TEST_DIR/apply-body.sh" || { echo "FALLO: el cuerpo extraído no es Bash válido"; exit 1; }

mkdir -p "$TEST_DIR/bin" "$TEST_DIR/work/etc/modules"

cat > "$TEST_DIR/bin/mysql" <<'EOF'
#!/bin/bash
SQL=""
while [ $# -gt 0 ]; do
    case "$1" in
        -e) SQL="$2"; shift 2 ;;
        *) shift ;;
    esac
done
echo "MYSQL-CALL: $SQL" >> "$LOGFILE"
case "$SQL" in
    *"SELECT id FROM panel_config_apply_request WHERE status = 'pending'"*)
        echo "1"
        ;;
    *"SELECT revision, key_name, file_name, risk, new_value FROM panel_config_pending"*)
        if [ "$SCENARIO" = "sql-read-fail" ]; then
            echo "error simulado de lectura" >&2
            exit 1
        fi
        if [ "$SCENARIO" = "validation-fail" ]; then
            printf '1\tRate.Health\tworldserver.conf\tlow\t2.0\n'
            printf '2\tNo.Existe\tworldserver.conf\tlow\t9\n'
        else
            printf '1\tRate.Health\tworldserver.conf\tlow\t2.0\n'
            printf '2\tX.Enable\tmod_x.conf\tlow\t0\n'
        fi
        ;;
    *) ;;
esac
exit 0
EOF

cat > "$TEST_DIR/bin/systemctl" <<'EOF'
#!/bin/bash
echo "SYSTEMCTL-CALL: $*" >> "$LOGFILE"
if [ "$1" = "is-active" ]; then
    [ "${WS_ACTIVE:-1}" = "1" ] && exit 0 || exit 3
fi
if [ "$1" = "restart" ]; then
    [ "$SCENARIO" = "restart-fail" ] && { echo "fallo simulado de reinicio" >&2; exit 1; }
    exit 0
fi
exit 0
EOF

cat > "$TEST_DIR/bin/sudo" <<'EOF'
#!/bin/bash
bin="$1"; shift
if [ "$bin" = "/usr/bin/systemctl" ]; then exec systemctl "$@"; fi
exec "$bin" "$@"
EOF
chmod +x "$TEST_DIR/bin/mysql" "$TEST_DIR/bin/systemctl" "$TEST_DIR/bin/sudo"

run_scenario() {
    local SCENARIO_NAME="$1"
    cat > "$TEST_DIR/work/etc/worldserver.conf" <<'EOF'
Rate.Health = 1.0
GM.StartLevel = 3
EOF
    cat > "$TEST_DIR/work/etc/modules/mod_x.conf" <<'EOF'
X.Enable = 1
EOF
    : > "$TEST_DIR/apply.log"
    : > "$TEST_DIR/mysql-calls.log"
    PATH="$TEST_DIR/bin:$PATH" \
    LOGFILE="$TEST_DIR/mysql-calls.log" \
    AC_DB_USER=acore AC_DB_PASS=acore PANEL_DB=acore_panel \
    ETC_DIR="$TEST_DIR/work/etc" MOD_CONF_DIR="$TEST_DIR/work/etc/modules" \
    LOG="$TEST_DIR/apply.log" SCENARIO="$SCENARIO_NAME" WS_ACTIVE="${WS_ACTIVE:-1}" \
    bash "$TEST_DIR/apply-body.sh"
}

# ── Éxito: dos ficheros publicados, sólo esas revisiones se borran ─────────
RC=0; run_scenario happy > "$TEST_DIR/happy.out" 2>&1 || RC=$?
[ "$RC" -eq 0 ] || { echo "FALLO happy: código de salida $RC"; cat "$TEST_DIR/happy.out"; exit 1; }
grep -qx 'Rate.Health = 2.0' "$TEST_DIR/work/etc/worldserver.conf" || { echo "FALLO happy: worldserver.conf no se publicó"; exit 1; }
grep -qx 'X.Enable = 0' "$TEST_DIR/work/etc/modules/mod_x.conf" || { echo "FALLO happy: mod_x.conf no se publicó"; exit 1; }
grep -q "DELETE FROM panel_config_pending WHERE revision IN (1,2);" "$TEST_DIR/mysql-calls.log" || { echo "FALLO happy: no se borraron las revisiones capturadas"; exit 1; }
[ -f "$TEST_DIR/work/etc/worldserver.conf.bak.req1" ] || { echo "FALLO happy: falta la copia de seguridad de worldserver.conf"; exit 1; }

# ── Clave inválida: aborta, originales intactos, sin ningún DELETE ─────────
RC=0; run_scenario validation-fail > "$TEST_DIR/valfail.out" 2>&1 || RC=$?
[ "$RC" -ne 0 ] || { echo "FALLO validation-fail: debía abortar"; exit 1; }
grep -qx 'Rate.Health = 1.0' "$TEST_DIR/work/etc/worldserver.conf" || { echo "FALLO validation-fail: el original se tocó"; exit 1; }
grep -q "DELETE FROM panel_config_pending" "$TEST_DIR/mysql-calls.log" && { echo "FALLO validation-fail: se borró algo pese al aborto"; exit 1; }
grep -q "clave 'No.Existe' no declarada" "$TEST_DIR/apply.log" || { echo "FALLO validation-fail: falta el mensaje de error esperado"; exit 1; }

# ── Fallo de lectura SQL: se detecta explícito, no como "lote vacío" ───────
RC=0; run_scenario sql-read-fail > "$TEST_DIR/sqlfail.out" 2>&1 || RC=$?
[ "$RC" -ne 0 ] || { echo "FALLO sql-read-fail: debía abortar"; exit 1; }
grep -q "no se pudo leer panel_config_pending" "$TEST_DIR/apply.log" || { echo "FALLO sql-read-fail: no se detectó el fallo de lectura explícitamente"; exit 1; }

# ── Fallo de reinicio: .conf publicados, revisiones SIN borrar (reintento) ─
RC=0; run_scenario restart-fail > "$TEST_DIR/restartfail.out" 2>&1 || RC=$?
[ "$RC" -ne 0 ] || { echo "FALLO restart-fail: debía terminar en error"; exit 1; }
grep -qx 'Rate.Health = 2.0' "$TEST_DIR/work/etc/worldserver.conf" || { echo "FALLO restart-fail: el .conf no quedó publicado"; exit 1; }
grep -q "DELETE FROM panel_config_pending" "$TEST_DIR/mysql-calls.log" && { echo "FALLO restart-fail: se borraron revisiones pese al fallo de reinicio"; exit 1; }
grep -q "status = 'failed'" "$TEST_DIR/mysql-calls.log" || { echo "FALLO restart-fail: la solicitud no se marcó 'failed'"; exit 1; }

echo 'OK: apply-panel-config.sh — éxito con dos ficheros, clave inválida sin tocar originales, fallo de lectura SQL explícito, fallo de reinicio con revisiones intactas para reintentar.'
