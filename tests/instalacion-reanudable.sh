#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Integración aislada: instaladores reales, fases y comandos del sistema falsos.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_PARENT=$(cd "${TMPDIR:-/tmp}" && pwd -P)
TEST_DIR=$(mktemp -d "$TEST_PARENT/azeroth-test.XXXXXX")
cleanup() {
    case "$TEST_DIR" in "$TEST_PARENT"/azeroth-test.*) rm -rf -- "$TEST_DIR" ;; esac
}
trap cleanup EXIT
mkdir -p "$TEST_DIR/repo/lib" "$TEST_DIR/repo/tools" "$TEST_DIR/repo/scripts/phases" "$TEST_DIR/bin" "$TEST_DIR/home"
cp "$ROOT/scripts/instalar-todo.sh" "$TEST_DIR/repo/scripts/"
cp "$ROOT/install.sh" "$ROOT/config.sh" "$ROOT/versions.lock" "$TEST_DIR/repo/"
cp "$ROOT/lib/install-state.sh" "$ROOT/lib/utils.sh" "$TEST_DIR/repo/lib/"
printf 'run_doctor() { return 0; }\n' > "$TEST_DIR/repo/lib/doctor.sh"
touch "$TEST_DIR/repo/lib/versions.sh" "$TEST_DIR/repo/lib/mirrors.sh"
for command in sudo systemctl getent clear flock; do
    printf '#!/bin/bash\nexit 0\n' > "$TEST_DIR/bin/$command"
done
printf '#!/bin/bash\necho 1000\n' > "$TEST_DIR/bin/id"
printf '#!/bin/bash\nexit 1\n' > "$TEST_DIR/bin/mysql"
for file in "$ROOT"/scripts/phases/0[1-9]_*.sh; do
    name=$(basename "$file")
    number=${name:1:1}
    cat > "$TEST_DIR/repo/scripts/phases/$name" <<EOF
#!/bin/bash
echo "$number" >> "\$TEST_TRACE"
[ "\${TEST_FAIL:-}" != "$number" ] || exit 1
if [ "$number" = 4 ]; then
    mkdir -p "\$HOME/azerothcore/env/dist/bin"
    touch "\$HOME/azerothcore/env/dist/bin/worldserver"
    echo '(worldserver-daemon) ready' > "\$HOME/azerothcore/env/dist/bin/Server.log"
fi
EOF
done
cat > "$TEST_DIR/repo/tools/primer-arranque.sh" <<'EOF'
#!/bin/bash
echo first >> "$TEST_TRACE"
[ "${TEST_FAIL:-}" != first ]
EOF
cat > "$TEST_DIR/repo/tools/doctor.sh" <<'EOF'
#!/bin/bash
echo doctor >> "$TEST_TRACE"
[ "${TEST_FAIL:-}" != doctor ]
EOF
chmod +x "$TEST_DIR/bin/"* "$TEST_DIR/repo/"*.sh "$TEST_DIR/repo/tools/"*.sh "$TEST_DIR/repo/scripts/"*.sh "$TEST_DIR/repo/scripts/phases/"*.sh
export PATH="$TEST_DIR/bin:$PATH" HOME="$TEST_DIR/home" TEST_TRACE="$TEST_DIR/trace"
cd "$TEST_DIR/repo"
expect_failure() {
    if bash scripts/instalar-todo.sh "$@" > "$TEST_DIR/output" 2>&1; then
        echo "Debía fallar: $*"; cat "$TEST_DIR/output"; exit 1
    fi
}
expect_step() {
    local fingerprint step
    [ -f .instalacion/estado ] || { cat "$TEST_DIR/output"; exit 1; }
    read -r fingerprint step < .instalacion/estado
    [ "$step" = "$1" ] || { echo "Paso esperado $1, recibido $step"; exit 1; }
}

# Argumentos inválidos no crean estado ni ejecutan fases.
expect_failure --desde 9
[ ! -e .instalacion/estado ]
expect_failure --desde 2 --reanudar
expect_failure --reanudar

export TEST_FAIL=4
expect_failure
expect_step 4
[ "$(tr '\n' ',' < "$TEST_TRACE")" = '1,2,3,4,' ]

# Un cambio de configuración impide reutilizar los checkpoints anteriores.
cp config.sh config.saved
echo '# cambio manual' >> config.sh
expect_failure --reanudar
expect_step 4
mv config.saved config.sh

# Lo mismo con la configuración local: crearla o cambiarla invalida la huella.
echo 'REALM_NAME="Otro reino"' > config.local.sh
expect_failure --reanudar
expect_step 4
rm config.local.sh

# No repetir las fases 1-3; el primer arranque fallido sigue pendiente.
export TEST_FAIL=first
expect_failure --reanudar
expect_step 8
[ "$(tr '\n' ',' < "$TEST_TRACE")" = '1,2,3,4,4,5,6,7,first,' ]

# Fallar en post-instalación conserva el primer arranque completado.
export TEST_FAIL=9
expect_failure --reanudar
expect_step 9
[ "$(tr '\n' ',' < "$TEST_TRACE")" = '1,2,3,4,4,5,6,7,first,first,8,9,' ]

# Doctor debe bloquear la declaración de éxito.
export TEST_FAIL=doctor
expect_failure --reanudar
expect_step 11
unset TEST_FAIL
bash scripts/instalar-todo.sh --reanudar > "$TEST_DIR/output" 2>&1
expect_step 12
before=$(cat "$TEST_TRACE")
bash scripts/instalar-todo.sh --reanudar > "$TEST_DIR/output" 2>&1
[ "$(cat "$TEST_TRACE")" = "$before" ]

# Los modos manuales no alteran el estado guardado.
cp .instalacion/estado "$TEST_DIR/saved-state"
echo s | bash install.sh --only 5 > "$TEST_DIR/output" 2>&1
cmp .instalacion/estado "$TEST_DIR/saved-state"
echo 'OK: fases, fallos, reanudación, huella, doctor y modos manuales.'
