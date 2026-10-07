#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Crea las bases de datos y comprueba el primer arranque con config.sh.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$SCRIPT_DIR/config.sh"
export TERM="${TERM:-xterm-256color}"
BIN="$AC_DIR/env/dist/bin"
OUT="$HOME/primer-arranque.log"
[ -x "$BIN/worldserver" ] || { echo "NO EXISTE $BIN/worldserver"; exit 1; }

if systemctl list-unit-files ac-worldserver.socket >/dev/null 2>&1; then
    sudo -n systemctl stop ac-worldserver.socket 2>/dev/null || sudo systemctl stop ac-worldserver.socket
fi
if ss -ltn | grep -q ':8085 '; then
    echo "Algo ya escucha en 8085. Detén ac-worldserver y su socket antes del primer arranque."
    exit 1
fi

umask 077
WORK=$(mktemp -d)
IN="$WORK/stdin"
WS_PID=""
cleanup() {
    local status=$?
    trap - EXIT INT TERM HUP
    if [ -n "$WS_PID" ] && kill -0 "$WS_PID" 2>/dev/null; then
        echo "Apagando el worldserver del primer arranque..."
        kill -TERM "$WS_PID" 2>/dev/null || true
        for ((attempt=0; attempt<60; attempt++)); do
            kill -0 "$WS_PID" 2>/dev/null || break
            sleep 2
        done
        if kill -0 "$WS_PID" 2>/dev/null; then
            # Visto el 15/09/2026, reproducible: lanzado así (stdin en una
            # fifo, sin systemd) se puede quedar colgado sin usar CPU justo
            # tras "Loading TalentSpecs..." y no responde a SIGTERM. Las
            # bases ya están creadas a esas alturas (es lo que importa aquí),
            # así que se remata con SIGKILL en vez de dejar el proceso
            # zombi ocupando RAM y bloqueando la reanudación.
            echo "SIGTERM no ha parado el proceso en 120s; forzando con SIGKILL."
            kill -KILL "$WS_PID" 2>/dev/null || true
            sleep 2
        fi
        if kill -0 "$WS_PID" 2>/dev/null; then
            echo "El proceso $WS_PID no se ha detenido ni con SIGKILL. Revisa $OUT antes de reanudar."
            status=1
        else
            wait "$WS_PID" 2>/dev/null || true
        fi
    fi
    exec 3>&- || true
    rm -f "$IN"
    rmdir "$WORK"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
mkfifo "$IN"
# Abrir lectura/escritura evita bloquear si el binario falla antes de abrir stdin.
exec 3<> "$IN"
: > "$OUT"
setsid bash -c 'cd "$1"; exec ./worldserver < "$2"' bash "$BIN" "$IN" >> "$OUT" 2>&1 &
WS_PID=$!
for ((answer=0; answer<6; answer++)); do printf 'yes\n' >&3; done

echo "Esperando al primer arranque (hasta 15 minutos; registro: $OUT)..."
READY=false
DB_POPULATED=false
for ((attempt=0; attempt<180; attempt++)); do
    if ! kill -0 "$WS_PID" 2>/dev/null; then
        echo "El worldserver terminó antes de estar listo."
        break
    fi
    if grep -aqE '\(worldserver-daemon\) ready' "$OUT"; then
        READY=true
        break
    fi
    if grep -qaE 'FATAL|error while loading shared|Aborted' "$OUT"; then
        break
    fi
    # Comprobar cada 30s si las bases YA están creadas y pobladas: es el
    # objetivo real de este script (su nombre y su comentario lo dicen). Si
    # el proceso se cuelga más adelante cargando playerbots (visto el
    # 15/09/2026 con esta fifo, nunca bajo systemd) ya no hace falta esperar
    # a "ready" aquí: el paso siguiente de instalar-todo.sh arranca el
    # worldserver de verdad por systemd y confirma "ready" contra Server.log
    # con su propio timeout.
    if [ $((attempt % 6)) -eq 0 ] && [ "$attempt" -gt 0 ]; then
        COUNT=$(mysql -u "$AC_DB_USER" -p"$AC_DB_PASS" -N -e \
            "SELECT COUNT(*) FROM acore_world.creature_template;" 2>/dev/null || true)
        if [[ "$COUNT" =~ ^[1-9] ]]; then
            DB_POPULATED=true
            break
        fi
    fi
    sleep 5
done
tail -12 "$OUT" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
if [ "$READY" != true ] && [ "$DB_POPULATED" != true ]; then
    echo "Primer arranque incompleto. Revisa $OUT y retoma con ./install.sh --reanudar."
    exit 1
fi
mysql -u "$AC_DB_USER" -p"$AC_DB_PASS" -N -e \
    "SELECT table_schema, COUNT(*) FROM information_schema.tables WHERE table_schema LIKE 'acore%' GROUP BY table_schema;"
if [ "$READY" = true ]; then
    echo "Bases creadas y mundo listo."
else
    echo "Bases creadas y pobladas. El mundo no llegó a 'ready' en este arranque de prueba"
    echo "(revisa $OUT); lo confirma el arranque real por systemd en el siguiente paso."
fi
