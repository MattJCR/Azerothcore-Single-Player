#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Primer arranque real con binario y servicios simulados, sin tocar la máquina.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_PARENT=$(cd "${TMPDIR:-/tmp}" && pwd -P)
TEST_DIR=$(mktemp -d "$TEST_PARENT/azeroth-test.XXXXXX")
cleanup() {
    case "$TEST_DIR" in "$TEST_PARENT"/azeroth-test.*) rm -rf -- "$TEST_DIR" ;; esac
}
trap cleanup EXIT
mkdir -p "$TEST_DIR/repo/tools" "$TEST_DIR/bin" "$TEST_DIR/home" "$TEST_DIR/server con espacios/env/dist/bin"
cp "$ROOT/tools/primer-arranque.sh" "$TEST_DIR/repo/tools/"
cat > "$TEST_DIR/repo/config.sh" <<'EOF'
AC_DIR="$TEST_SERVER"
AC_DB_USER=custom_user
AC_DB_PASS=custom_password
EOF
printf '#!/bin/bash\nexit 1\n' > "$TEST_DIR/bin/systemctl"
printf '#!/bin/bash\nexit 0\n' > "$TEST_DIR/bin/ss"
printf '#!/bin/bash\nexec "$@"\n' > "$TEST_DIR/bin/setsid"
REAL_SLEEP=$(command -v sleep)
printf '#!/bin/bash\nexec "%s" 0.02\n' "$REAL_SLEEP" > "$TEST_DIR/bin/sleep"
cat > "$TEST_DIR/bin/mysql" <<'EOF'
#!/bin/bash
printf '%s\n' "$@" >> "$TEST_TRACE"
case "$*" in
    *creature_template*)
        if [ "${TEST_DB_POPULATED:-}" = true ]; then echo 5; else echo 0; fi
        ;;
esac
EOF
cat > "$TEST_DIR/server con espacios/env/dist/bin/worldserver" <<'EOF'
#!/bin/bash
trap 'exit 0' TERM
case "$TEST_MODE" in
    fail) echo FATAL; exit 1 ;;
    ready) echo '(worldserver-daemon) ready' ;;
    timeout) : ;;
esac
while :; do sleep 1; done
EOF
chmod +x "$TEST_DIR/bin/"* "$TEST_DIR/server con espacios/env/dist/bin/worldserver"
export PATH="$TEST_DIR/bin:$PATH" HOME="$TEST_DIR/home"
export TEST_SERVER="$TEST_DIR/server con espacios" TEST_TRACE="$TEST_DIR/mysql-args"
for TEST_MODE in fail timeout; do
    export TEST_MODE
    rm -f "$TEST_TRACE"
    if bash "$TEST_DIR/repo/tools/primer-arranque.sh" > "$TEST_DIR/output" 2>&1; then
        echo "Debía fallar: $TEST_MODE"; cat "$TEST_DIR/output"; exit 1
    fi
done
unset TEST_MODE
rm -f "$TEST_TRACE"
export TEST_MODE=ready
bash "$TEST_DIR/repo/tools/primer-arranque.sh" > "$TEST_DIR/output" 2>&1 || { cat "$TEST_DIR/output"; exit 1; }
grep -qx custom_user "$TEST_TRACE"
grep -qx -- -pcustom_password "$TEST_TRACE"

# El mundo nunca dice "ready" (se cuelga, como el 15/09/2026 cargando
# playerbots), pero las bases ya están pobladas: debe darse por bueno igual,
# porque el arranque real lo confirma después el paso de systemd.
rm -f "$TEST_TRACE"
export TEST_MODE=timeout TEST_DB_POPULATED=true
bash "$TEST_DIR/repo/tools/primer-arranque.sh" > "$TEST_DIR/output" 2>&1 || { cat "$TEST_DIR/output"; exit 1; }
grep -qi "no lleg" "$TEST_DIR/output"
grep -qx custom_user "$TEST_TRACE"
unset TEST_DB_POPULATED

echo 'OK: primer arranque, fallo, timeout, bases pobladas sin ready, ruta y credenciales personalizadas.'
