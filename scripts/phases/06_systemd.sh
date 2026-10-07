#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  06_systemd.sh — Crea y habilita los servicios systemd para AC
#  Incluye apagado seguro: el worldserver guarda la BD antes de cerrarse
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

header "FASE 6 — Servicios systemd"

mkdir -p "$AC_SCRIPTS_DIR" "$AC_LOGS_DIR"

# Marca que deja notify-restart.sh para avisar a safe-stop.sh de que la cuenta
# atrás con los jugadores YA se ha hecho, y que no la repita.
RESTART_FLAG="$AC_LOGS_DIR/.restart-announced"

# =============================================================================
# Script de espera a MySQL
#
# `After=mysql.service` solo garantiza que la UNIDAD de MySQL ha arrancado,
# no que el servidor esté aceptando conexiones. En un reinicio de la VM el
# worldserver llegaba antes que MySQL, fallaba al conectar y entraba en bucle
# de reinicios (o dependía del `@reboot startup.sh` del crontab, que corría
# demasiado tarde). Con esto la espera vive donde toca: en la propia unidad.
# =============================================================================
WAIT_SCRIPT="$AC_SCRIPTS_DIR/wait-for-mysql.sh"

cat > "$WAIT_SCRIPT" << 'WAITSCRIPT'
#!/bin/bash
# Espera a que MySQL acepte conexiones TCP (máx. 120 segundos).
# Generado automáticamente por el instalador (fase 6).
HOST=127.0.0.1
PORT=3306
for _ in $(seq 1 40); do
    if timeout 2 bash -c "echo > /dev/tcp/$HOST/$PORT" 2>/dev/null; then
        exit 0
    fi
    sleep 3
done
echo "MySQL no responde en $HOST:$PORT tras 120 segundos." >&2
exit 1
WAITSCRIPT
chmod +x "$WAIT_SCRIPT"
log "Script wait-for-mysql.sh creado en $WAIT_SCRIPT"

# =============================================================================
# ws-console.sh — mandar un comando a la consola del worldserver por SOAP
#
# En modo en espera (WORLDSERVER_STANDBY=true) el worldserver corre como binario
# directo bajo systemd, sin screen: `screen -X stuff` ya no vale para el aviso
# de parada ni para el saveall. SOAP (127.0.0.1:7878, lo activa la fase 5 con el
# panel) sí. Las credenciales salen de /etc/azerothcore-panel.env, que el
# instalador del panel deja legible para el grupo del usuario de AzerothCore.
#
# Es siempre "best effort": si SOAP no responde (worldserver aún cargando, o
# panel sin instalar) devuelve 1 en silencio y quien llama sigue adelante — el
# SIGTERM de systemd hace un apagado limpio igualmente (el core lo captura y
# guarda la BD).
# =============================================================================
WS_CONSOLE_SCRIPT="$AC_SCRIPTS_DIR/ws-console.sh"

cat > "$WS_CONSOLE_SCRIPT" << 'WSCONSOLE'
#!/bin/bash
# Manda un comando al worldserver por SOAP. Uso: ws-console.sh "saveall"
# Generado automáticamente por el instalador (fase 6). No editar a mano.
set -euo pipefail
CMD="${1:-}"
[ -n "$CMD" ] || { echo "uso: ws-console.sh <comando>" >&2; exit 2; }

ENV_FILE="${WS_CONSOLE_ENV:-/etc/azerothcore-panel.env}"
[ -r "$ENV_FILE" ] || exit 1

SOAP_HOST="$(sed -n 's/^SOAP_HOST=//p' "$ENV_FILE" | head -n1)"
SOAP_PORT="$(sed -n 's/^SOAP_PORT=//p' "$ENV_FILE" | head -n1)"
SOAP_USER="$(sed -n 's/^SOAP_USERNAME=//p' "$ENV_FILE" | head -n1)"
SOAP_PASS="$(sed -n 's/^SOAP_PASSWORD=//p' "$ENV_FILE" | head -n1)"
SOAP_HOST="${SOAP_HOST:-127.0.0.1}"
SOAP_PORT="${SOAP_PORT:-7878}"
[ -n "$SOAP_USER" ] && [ -n "$SOAP_PASS" ] || exit 1

# Escapar XML del comando (&, <, >).
CMD_XML="$(printf '%s' "$CMD" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g')"

BODY="<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://schemas.xmlsoap.org/soap/envelope/\">
<SOAP-ENV:Body><ns1:executeCommand xmlns:ns1=\"urn:AC\"><command>${CMD_XML}</command></ns1:executeCommand></SOAP-ENV:Body>
</SOAP-ENV:Envelope>"

curl -sS --max-time 5 -u "${SOAP_USER}:${SOAP_PASS}" \
    -H 'Content-Type: text/xml; charset=utf-8' -H 'SOAPAction: ""' \
    --data "$BODY" "http://${SOAP_HOST}:${SOAP_PORT}/" >/dev/null 2>&1
WSCONSOLE
chmod +x "$WS_CONSOLE_SCRIPT"
log "Script ws-console.sh creado en $WS_CONSOLE_SCRIPT"

# =============================================================================
# Script de parada limpia del worldserver
# Usado por ExecStop del servicio y por el apagado de Proxmox
# =============================================================================
STOP_SCRIPT="$AC_SCRIPTS_DIR/safe-stop.sh"

{
# Preámbulo con los valores de config.sh (el cuerpo va entrecomillado para no
# tener que escapar cada '$' del script).
cat << PREAMBLE
#!/bin/bash
# =============================================================================
#  safe-stop.sh — Parada limpia de AzerothCore
#
#  Avisa a los jugadores, espera a que el worldserver guarde la BD
#  y cierra el worldserver de forma ordenada.
#
#  Generado automáticamente por el instalador (fase 6). No editar a mano:
#  se regenera con ./install.sh --only 6
#
#  Llamado automáticamente por systemd al hacer:
#    sudo systemctl stop ac-worldserver
#    sudo shutdown / sudo reboot
#    Apagado/reinicio desde Proxmox (ACPI shutdown)
# =============================================================================
WS_SCREEN="${WS_SCREEN}"
LOG="${AC_LOGS_DIR}/safe-stop.log"
RESTART_FLAG="${RESTART_FLAG}"
WS_CONSOLE="${WS_CONSOLE_SCRIPT}"
mkdir -p "${AC_LOGS_DIR}"
PREAMBLE
cat << 'STOPSCRIPT'

ts() { date '+%Y-%m-%d %H:%M:%S'; }

echo "" >> "$LOG"
echo "[$(ts)] ══ Iniciando parada segura ══" >> "$LOG"

# Canal a la consola: screen (instalación clásica) o SOAP (modo en espera, sin
# screen). ws_cmd prueba screen y, si no hay sesión, cae a SOAP (best effort).
ws_cmd() {
    if screen -list 2>/dev/null | grep -q "\.${WS_SCREEN}[[:space:]]"; then
        screen -S "$WS_SCREEN" -X stuff "$1$(printf '\r')" 2>/dev/null || true
    elif [ -x "$WS_CONSOLE" ]; then
        "$WS_CONSOLE" "$1" || true
    fi
}

send_msg() { ws_cmd "announce $1"; }

# ── ¿Hace falta avisar? ───────────────────────────────────────────────────────
# Si notify-restart.sh acaba de hacer su cuenta atrás de 5 minutos (reinicio
# diario o actualización semanal), repetir aquí otros 60 segundos de avisos
# sería redundante y alargaría la parada sin motivo. La marca caduca a los
# 10 minutos para que un apagado normal sí avise.
ANNOUNCE=true
if [ -f "$RESTART_FLAG" ]; then
    FLAG_AGE=$(( $(date +%s) - $(stat -c %Y "$RESTART_FLAG" 2>/dev/null || echo 0) ))
    if [ "$FLAG_AGE" -lt 600 ]; then
        ANNOUNCE=false
        echo "[$(ts)] Cuenta atrás ya realizada por notify-restart.sh hace ${FLAG_AGE}s. No repito avisos." >> "$LOG"
    fi
fi
rm -f "$RESTART_FLAG"

if screen -list 2>/dev/null | grep -q "\.${WS_SCREEN}[[:space:]]"; then
    if [ "$ANNOUNCE" = true ]; then
        echo "[$(ts)] Avisando a jugadores..." >> "$LOG"
        send_msg "El servidor se apagará en 60 segundos. Por favor, poneos a salvo."
        sleep 30
        send_msg "El servidor se apagará en 30 segundos."
        sleep 20
        send_msg "El servidor se apagará en 10 segundos."
        sleep 10
    fi

    # ── Ordenar al worldserver que guarde y cierre ────────────────────────────
    echo "[$(ts)] Enviando saveall y shutdown al worldserver..." >> "$LOG"
    screen -S "$WS_SCREEN" -X stuff "saveall$(printf '\r')" 2>/dev/null || true
    sleep 3
    screen -S "$WS_SCREEN" -X stuff "server shutdown 1$(printf '\r')" 2>/dev/null || true

    # ── Esperar a que el proceso cierre solo (máx. 60 seg) ────────────────────
    echo "[$(ts)] Esperando cierre limpio del worldserver..." >> "$LOG"
    WAIT=0
    while screen -list 2>/dev/null | grep -q "\.${WS_SCREEN}[[:space:]]" && [ "$WAIT" -lt 60 ]; do
        sleep 2
        WAIT=$((WAIT + 2))
    done

    if screen -list 2>/dev/null | grep -q "\.${WS_SCREEN}[[:space:]]"; then
        echo "[$(ts)] Worldserver no cerró a tiempo. Forzando..." >> "$LOG"
        screen -S "$WS_SCREEN" -X quit 2>/dev/null || true
        sleep 2
    else
        echo "[$(ts)] Worldserver cerrado limpiamente en ${WAIT}s." >> "$LOG"
    fi
elif pgrep -x worldserver >/dev/null 2>&1; then
    # Modo en espera: el worldserver corre como binario directo bajo systemd
    # (sin screen; `systemctl is-active` ya diría "deactivating" cuando este
    # ExecStop corre, así que miramos el proceso). Aviso + saveall por SOAP
    # (best effort) y dejamos que systemd
    # mande el SIGTERM al terminar este ExecStop: el core lo captura y hace un
    # apagado limpio con guardado de BD. Un apagado por inactividad de
    # mod-standby ya llega aquí con el servidor vacío, así que el aviso rara vez
    # tiene destinatario, pero un `systemctl stop` a mano sí puede tener a alguien.
    if [ "$ANNOUNCE" = true ]; then
        echo "[$(ts)] (modo en espera) Avisando a jugadores por SOAP..." >> "$LOG"
        send_msg "El servidor se apagará en 60 segundos. Por favor, poneos a salvo."
        sleep 30
        send_msg "El servidor se apagará en 30 segundos."
        sleep 20
        send_msg "El servidor se apagará en 10 segundos."
        sleep 10
    fi
    echo "[$(ts)] (modo en espera) saveall por SOAP; systemd enviará SIGTERM para el cierre limpio." >> "$LOG"
    ws_cmd "saveall"
    sleep 3
else
    echo "[$(ts)] Worldserver no encontrado (ni screen ni servicio activo). Ya estaba parado." >> "$LOG"
fi

# NO llamamos aquí a "systemctl stop ac-worldserver": este script YA ES el
# ExecStop de ac-worldserver.service. Si systemd nos invocó (stop/shutdown/
# reboot), basta con que este script termine para que la unidad se marque
# como detenida — pedirle a systemd que pare una unidad que ya se está
# parando (ejecutando este mismo script) puede colgarse hasta agotar
# TimeoutStopSec, ya que el job de parada estaría esperando a sí mismo.
echo "[$(ts)] ══ Parada segura completada ══" >> "$LOG"
STOPSCRIPT
} > "$STOP_SCRIPT"
chmod +x "$STOP_SCRIPT"
log "Script safe-stop.sh creado en $STOP_SCRIPT"

# =============================================================================
# Script de aplicación de la configuración pendiente del panel web
#
# El proceso del panel (systemd, DynamicUser, sandbox estricto) NUNCA escribe
# en disco ni llama a systemctl: sólo encola filas en la tabla
# panel_config_pending de acore_panel. Este script — ejecutado periódicamente
# por ac-panel-config-apply.timer como AC_SYSTEM_USER, nunca como root — es el
# único que escribe en los .conf reales y el único que reinicia el
# worldserver, reutilizando el NOPASSWD ya concedido más abajo.
# =============================================================================
APPLY_SCRIPT="$AC_SCRIPTS_DIR/apply-panel-config.sh"

if [ "${INSTALL_WEB_PANEL:-true}" = true ]; then
{
cat << PREAMBLE
#!/bin/bash
# =============================================================================
#  apply-panel-config.sh — Aplica los cambios de worldserver.conf/módulos que
#  un administrador dejó encolados desde el panel web y reinicia el worldserver.
#
#  Generado automáticamente por el instalador (fase 6). No editar a mano:
#  se regenera con ./install.sh --only 6
#
#  Llamado periódicamente por ac-panel-config-apply.timer. No hace nada si no
#  hay ninguna solicitud pendiente (panel_config_apply_request).
# =============================================================================
AC_DB_USER="${AC_DB_USER}"
AC_DB_PASS="${AC_DB_PASS}"
PANEL_DB="${PANEL_PANEL_DATABASE:-acore_panel}"
ETC_DIR="${AC_DIR}/env/dist/etc"
MOD_CONF_DIR="\${ETC_DIR}/modules"
LOG="${AC_LOGS_DIR}/apply-panel-config.log"
PREAMBLE
cat << 'APPLYSCRIPT'
set -euo pipefail
export MYSQL_PWD="${AC_DB_PASS}"
MYSQL="mysql -u ${AC_DB_USER} -N -B ${PANEL_DB}"

ts() { date '+%Y-%m-%d %H:%M:%S'; }

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# En una instalación desde cero, esta fase (6) crea y habilita el timer antes
# de que la fase 9 cree la base acore_panel — el hueco entre medias puede ser
# de decenas de minutos (compilar, primer arranque...). El "|| true" evita que
# esa consulta contra una base inexistente tumbe el script bajo "set -e": se
# trata igual que "no hay nada pendiente" en vez de como un fallo.
REQUEST_ID="$($MYSQL -e "SELECT id FROM panel_config_apply_request WHERE status = 'pending' ORDER BY id ASC LIMIT 1;" 2>>"$LOG" || true)"
[ -n "$REQUEST_ID" ] || exit 0

echo "[$(ts)] ══ Solicitud #$REQUEST_ID: aplicando configuración pendiente del panel ══" >> "$LOG"

fail() {
    local MSG_ESCAPED
    MSG_ESCAPED="$(printf '%s' "$1" | sed "s/'/''/g" | cut -c1-500)"
    echo "[$(ts)] ERROR: $1" >> "$LOG"
    $MYSQL -e "UPDATE panel_config_apply_request SET status = 'failed', applied_at = NOW(), error = '${MSG_ESCAPED}' WHERE id = ${REQUEST_ID};"
    exit 1
}

# Igual de estricto que set_conf_value de lib/utils.sh, pero SIN su fallback
# de "añadir al final si no existe": aquí un parámetro que no está ya en el
# fichero real es señal de que el catálogo del panel se desincronizó de los
# .conf.dist reales, y hay que abortar el lote entero, no escribir a medias.
# Recibe una COPIA DE TRABAJO, nunca el .conf real: ver el bucle de
# validación más abajo.
set_conf_value_strict() {
    local FILE="$1" KEY="$2" VALUE="$3" KEY_RE VALUE_ESC
    [ -f "$FILE" ] || { echo "fichero no encontrado: $FILE"; return 1; }
    KEY_RE=$(printf '%s' "$KEY" | sed 's/[][\.*^$]/\\&/g')
    VALUE_ESC=$(printf '%s' "$VALUE" | sed 's/[\\&|]/\\&/g')
    grep -qE "^[[:space:]]*${KEY_RE}[[:space:]]*=" "$FILE" || { echo "clave '$KEY' no declarada en $(basename "$FILE" .work)"; return 1; }
    sed -i "s|^[[:space:]]*${KEY_RE}[[:space:]]*=.*|${KEY} = ${VALUE_ESC}|" "$FILE"
}

# ── Captura inmutable del lote (A1) ─────────────────────────────────────────
# La lista de revisiones se fija AQUÍ, antes de tocar ningún .conf. Un stage()
# o discard() posterior del panel queda fuera de este lote (REPLACE/UPDATE le
# da una revisión nueva a esa clave) y se recoge en la siguiente pasada del
# timer, en vez de desaparecer sin haberse aplicado ni conservarse.
#
# El SELECT corre fuera de una sustitución de proceso y se comprueba su
# código de salida explícitamente (A2): antes, un fallo de lectura SQL caía en
# el bucle como "sin filas" y se confundía con un lote vacío.
BATCH_FILE="$WORKDIR/batch.tsv"
if ! $MYSQL -e "SELECT revision, key_name, file_name, risk, new_value FROM panel_config_pending WHERE status = 'pending' ORDER BY revision;" > "$BATCH_FILE" 2>>"$LOG"; then
    fail "no se pudo leer panel_config_pending (ver $LOG)"
fi

if [ ! -s "$BATCH_FILE" ]; then
    echo "[$(ts)] Solicitud #$REQUEST_ID sin cambios pendientes reales; no se reinicia nada." >> "$LOG"
    $MYSQL -e "UPDATE panel_config_apply_request SET status = 'applied', applied_at = NOW() WHERE id = ${REQUEST_ID};"
    exit 0
fi

# ── Preparar y validar TODO el lote sobre copias de trabajo (A2) ───────────
# Nada toca los .conf reales todavía: una clave inválida en cualquier punto
# del lote aborta sin haber escrito nada, así que fail() no necesita
# restaurar nada en este tramo — los originales ni se rozan.
declare -A TEMP_OF   # ruta real del .conf → su copia de trabajo en WORKDIR
REVISIONS=()
while IFS=$'\t' read -r REVISION KEY_NAME FILE_NAME RISK NEW_VALUE; do
    [ -n "$REVISION" ] || continue
    REVISIONS+=("$REVISION")
    # Defensa en profundidad: el panel ya rechaza riesgo alto al encolar, pero
    # si por lo que sea llegó una fila así, se aborta todo el lote sin tocar
    # nada más.
    if [ "$RISK" = "high" ]; then
        fail "clave de riesgo alto en la cola ('${KEY_NAME}'): el panel no debería haber dejado encolar esto"
    fi
    if [ "$FILE_NAME" = "worldserver.conf" ]; then
        TARGET="$ETC_DIR/worldserver.conf"
    else
        TARGET="$MOD_CONF_DIR/$FILE_NAME"
    fi
    if [ -z "${TEMP_OF[$TARGET]:-}" ]; then
        [ -f "$TARGET" ] || fail "fichero no encontrado: $TARGET"
        WORK="$WORKDIR/$(basename "$TARGET").work"
        cp "$TARGET" "$WORK" || fail "no se pudo preparar una copia de trabajo de $TARGET"
        TEMP_OF["$TARGET"]="$WORK"
    fi
    if ! ERR="$(set_conf_value_strict "${TEMP_OF[$TARGET]}" "$KEY_NAME" "$NEW_VALUE" 2>&1)"; then
        fail "$ERR"
    fi
    echo "[$(ts)] (validado) ${FILE_NAME}: ${KEY_NAME} = ${NEW_VALUE}" >> "$LOG"
done < "$BATCH_FILE"
CAPTURED_REVISIONS="$(IFS=,; echo "${REVISIONS[*]}")"

# ── Publicar (A2) ───────────────────────────────────────────────────────────
# Una única copia de seguridad por fichero (antes se repetía por CLAVE con
# resolución de un segundo: varias claves del mismo fichero podían pisar la
# copia inicial) y reemplazo atómico con `mv` dentro del mismo directorio. Si
# falla la publicación de un fichero a mitad de lote, se restauran los ya
# publicados desde su copia y se aborta sin marcar éxito parcial.
PUBLISHED=()
for TARGET in "${!TEMP_OF[@]}"; do
    BACKUP="${TARGET}.bak.req${REQUEST_ID}"
    if ! cp "$TARGET" "$BACKUP" 2>>"$LOG"; then
        for DONE in "${PUBLISHED[@]}"; do cp "${DONE}.bak.req${REQUEST_ID}" "$DONE" 2>>"$LOG" || true; done
        fail "no se pudo respaldar $TARGET antes de publicar"
    fi
    if ! mv "${TEMP_OF[$TARGET]}" "$TARGET" 2>>"$LOG"; then
        for DONE in "${PUBLISHED[@]}"; do cp "${DONE}.bak.req${REQUEST_ID}" "$DONE" 2>>"$LOG" || true; done
        fail "no se pudo publicar $TARGET (copia de recuperación: ${BACKUP})"
    fi
    PUBLISHED+=("$TARGET")
done

APPLIED_COUNT="${#REVISIONS[@]}"
echo "[$(ts)] $APPLIED_COUNT cambio(s) publicado(s) en ${#TEMP_OF[@]} fichero(s)." >> "$LOG"

# Si el worldserver está dormido (modo en espera), NO lo despertamos sólo para
# aplicar la config: los .conf ya están escritos y se leen en el próximo
# arranque, que hace cualquier conexión de cliente. En instalación clásica el
# worldserver siempre está activo, así que esto siempre reinicia.
#
# Un fallo de reinicio aquí deja los .conf ya publicados (correctos) pero no
# retira las revisiones capturadas ni marca la solicitud como aplicada: la
# siguiente pasada del timer vuelve a intentar este mismo lote (reescribir
# los mismos valores es idempotente) y a reiniciar, en vez de darlo por hecho
# a medias.
if systemctl is-active --quiet ac-worldserver; then
    echo "[$(ts)] Reiniciando el worldserver…" >> "$LOG"
    if ! sudo /usr/bin/systemctl restart ac-worldserver >> "$LOG" 2>&1; then
        fail "el reinicio del worldserver falló (ver $LOG); los .conf ya quedaron publicados, se reintentará en la próxima pasada"
    fi
else
    echo "[$(ts)] Worldserver dormido (modo en espera): se aplicará al próximo arranque." >> "$LOG"
fi

# ── Confirmar sólo las revisiones capturadas al principio (A1) ─────────────
# Una reescritura de la misma clave llegada DESPUÉS de la captura tiene una
# revisión distinta y no aparece en CAPTURED_REVISIONS, así que sobrevive a
# este DELETE para la siguiente pasada. Las filas 'discarded' son terminales
# (un restage las sustituye por completo) y se limpian aparte, sin relación
# con este lote.
$MYSQL -e "DELETE FROM panel_config_pending WHERE revision IN (${CAPTURED_REVISIONS});"
$MYSQL -e "DELETE FROM panel_config_pending WHERE status = 'discarded';"
$MYSQL -e "UPDATE panel_config_apply_request SET status = 'applied', applied_at = NOW() WHERE id = ${REQUEST_ID};"
echo "[$(ts)] ══ Solicitud #$REQUEST_ID aplicada. ══" >> "$LOG"
APPLYSCRIPT
} > "$APPLY_SCRIPT"
chmod +x "$APPLY_SCRIPT"
log "Script apply-panel-config.sh creado en $APPLY_SCRIPT"
fi

# =============================================================================
# Servicio ac-authserver
# =============================================================================
sudo tee /etc/systemd/system/ac-authserver.service > /dev/null << SVCEOF
[Unit]
Description=AzerothCore Authserver
After=network.target mysql.service
StartLimitIntervalSec=0

[Service]
Type=simple
Restart=always
RestartSec=5
User=${AC_SYSTEM_USER}
WorkingDirectory=${AC_DIR}
# Esperar a que MySQL acepte conexiones, no solo a que la unidad haya arrancado
ExecStartPre=/bin/bash ${AC_SCRIPTS_DIR}/wait-for-mysql.sh
ExecStart=${AC_DIR}/acore.sh run-authserver
# Tiempo máximo de espera para el apagado (segundos)
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
SVCEOF
log "Servicio ac-authserver creado."

# =============================================================================
# Servicio ac-worldserver
# Con ExecStop que invoca safe-stop.sh para guardar la BD antes de cerrar
#
# NOTA sobre el orden de parada: NO hace falta declarar Before=shutdown.target
# ni Conflicts=shutdown.target. systemd ya los añade solo a todos los servicios
# normales — eso es exactamente lo que hace DefaultDependencies (por defecto,
# "yes"). La versión anterior ponía DefaultDependencies=no y luego los volvía a
# escribir a mano: además de redundante, quitaba las dependencias implícitas
# con sysinit.target y basic.target, con lo que la unidad podía intentar
# arrancar antes de que el sistema base estuviera listo.
# =============================================================================
if [ "${WORLDSERVER_STANDBY:-false}" = true ]; then
    # ─────────────────────────────────────────────────────────────────────────
    # MODO EN ESPERA
    #
    # ac-worldserver.socket posee el puerto 8085 desde el arranque de la VM y
    # NO se para nunca. A la primera conexión de un cliente, systemd arranca
    # ac-worldserver.service y le entrega el socket por herencia de descriptor
    # (el core lo usa porque Network.UseSocketActivation=1, fase 5).
    #
    # El servicio ejecuta el BINARIO directamente (sin screen ni restarter):
    # la activación de socket exige que el proceso que hereda el descriptor sea
    # hijo directo de systemd (LISTEN_PID == getpid, ver src/common/Utilities/
    # Systemd.cpp del core). Restart=on-failure: un crash (código 1) o un
    # ".server restart" (código 2) se relanzan; el apagado por inactividad de
    # mod-standby sale con código 0 y systemd lo deja dormido.
    #
    # Sin [Install] en el servicio: al boot sólo se habilita el .socket.
    # ─────────────────────────────────────────────────────────────────────────
    sudo tee /etc/systemd/system/ac-worldserver.socket > /dev/null << SOCKEOF
[Unit]
Description=AzerothCore Worldserver (activación de socket, puerto 8085)

[Socket]
ListenStream=0.0.0.0:8085
# AzerothCore acepta y reparte TODAS las conexiones: una sola instancia
# compartida, systemd sólo hereda el socket de escucha.
Accept=no
# El core fuerza IPv4 al asignar el descriptor (AsyncAcceptor::assign con
# tcp::v4). Con BindIPv6Only=both un ListenStream=0.0.0.0 basta.
BindIPv6Only=both

[Install]
WantedBy=sockets.target
SOCKEOF
    log "Unidad ac-worldserver.socket creada (activación de socket en 8085)."

    sudo tee /etc/systemd/system/ac-worldserver.service > /dev/null << SVCEOF
[Unit]
Description=AzerothCore Worldserver
After=network.target mysql.service ac-authserver.service ac-worldserver.socket
Requires=ac-worldserver.socket
# Corta bucles de crash: sin el restarter de AzerothCore, este es el único
# freno. 6 arranques en 120 s y systemd deja la unidad en 'failed'.
StartLimitIntervalSec=120
StartLimitBurst=6

[Service]
Type=simple
Restart=on-failure
RestartSec=5
# 'success' incluye el código 0 del apagado por inactividad (mod-standby):
# con esto ExecStopPost puede distinguir "dormido" de "caído" para el panel.
User=${AC_SYSTEM_USER}
WorkingDirectory=${AC_DIR}/env/dist/bin
ExecStartPre=/bin/bash ${AC_SCRIPTS_DIR}/wait-for-mysql.sh
ExecStart=${AC_DIR}/env/dist/bin/worldserver -c ${AC_DIR}/env/dist/etc/worldserver.conf
# Aviso + saveall (por SOAP, sin screen); luego systemd manda SIGTERM y el core
# hace el apagado limpio con guardado de BD.
ExecStop=/bin/bash ${AC_SCRIPTS_DIR}/safe-stop.sh
TimeoutStopSec=150
KillMode=mixed
# Estado para el panel web: /run/azerothcore/worldserver.state
RuntimeDirectory=azerothcore
RuntimeDirectoryPreserve=yes
ExecStartPost=/bin/sh -c 'echo running > /run/azerothcore/worldserver.state'
ExecStopPost=/bin/sh -c 'if [ "\$SERVICE_RESULT" = success ]; then echo standby; else echo down; fi > /run/azerothcore/worldserver.state'
# Comprobación de salud tras cada arranque (manual, tras un crash, o al
# despertar del modo en espera). El '-' inicial hace que systemd ignore su
# código de salida: nunca puede dejar la unidad en 'failed' ni retrasar el
# arranque más allá del margen de 'timeout'. Solo lee y guarda su resultado
# en acore_world para el panel (lib/doctor.sh).
ExecStartPost=-/bin/timeout 30 /bin/bash ${SCRIPT_DIR}/tools/doctor.sh restart
SVCEOF
    log "Servicio ac-worldserver creado (modo en espera: binario directo + activación de socket)."
else
    sudo rm -f /etc/systemd/system/ac-worldserver.socket
    sudo tee /etc/systemd/system/ac-worldserver.service > /dev/null << SVCEOF
[Unit]
Description=AzerothCore Worldserver
After=network.target mysql.service ac-authserver.service
StartLimitIntervalSec=0

[Service]
Type=simple
Restart=always
RestartSec=5
User=${AC_SYSTEM_USER}
WorkingDirectory=${AC_DIR}
# Esperar a que MySQL acepte conexiones, no solo a que la unidad haya arrancado
ExecStartPre=/bin/bash ${AC_SCRIPTS_DIR}/wait-for-mysql.sh
ExecStart=/bin/screen -S ${WS_SCREEN} -D -m ${AC_DIR}/acore.sh run-worldserver
# Parada limpia: avisa jugadores, guarda BD, cierra ordenadamente
ExecStop=/bin/bash ${AC_SCRIPTS_DIR}/safe-stop.sh
# Dar tiempo suficiente para que safe-stop.sh complete (60s aviso + 60s cierre + margen)
TimeoutStopSec=150
# KillMode=mixed: si safe-stop.sh falla o agota el timeout, systemd todavía
# puede limpiar el grupo de procesos. Con el KillMode=none anterior no mataba
# nada y quedaban worldserver huérfanos ocupando el puerto en el siguiente
# arranque; además systemd lo marca como obsoleto desde la v250 y lo avisa en
# el journal en cada arranque.
KillMode=mixed
# Comprobación de salud tras cada arranque; ver la nota igual en la rama de
# modo en espera de arriba.
ExecStartPost=-/bin/timeout 30 /bin/bash ${SCRIPT_DIR}/tools/doctor.sh restart

[Install]
WantedBy=multi-user.target
SVCEOF
    log "Servicio ac-worldserver creado (con parada segura)."
fi

# =============================================================================
# Servicio + temporizador ac-panel-config-apply
#
# Corre como AC_SYSTEM_USER (nunca root) cada 20s. Es la única pieza que
# escribe en los .conf reales y reinicia el worldserver a partir de lo que un
# administrador dejó encolado desde el panel web; el proceso del panel en sí
# no tiene ni permiso de escritura en disco ni acceso a systemctl.
# =============================================================================
if [ "${INSTALL_WEB_PANEL:-true}" = true ]; then
sudo tee /etc/systemd/system/ac-panel-config-apply.service > /dev/null << SVCEOF
[Unit]
Description=Aplica la configuración pendiente del panel web de AzerothCore
After=network.target mysql.service ac-worldserver.service

[Service]
Type=oneshot
User=${AC_SYSTEM_USER}
ExecStart=/bin/bash ${APPLY_SCRIPT}
SVCEOF

sudo tee /etc/systemd/system/ac-panel-config-apply.timer > /dev/null << TIMEREOF
[Unit]
Description=Comprueba cada 20s si hay configuración pendiente del panel web

[Timer]
OnBootSec=20s
OnUnitActiveSec=20s
AccuracySec=5s

[Install]
WantedBy=timers.target
TIMEREOF
log "Servicio y temporizador ac-panel-config-apply creados."
fi

# =============================================================================
# Systemd override para aumentar el timeout global de apagado
# Por defecto systemd mata los procesos tras 90s — lo subimos a 180s
# para dar tiempo a safe-stop.sh cuando hay muchos jugadores conectados
# =============================================================================
sudo mkdir -p /etc/systemd/system.conf.d/
sudo tee /etc/systemd/system.conf.d/azerothcore-shutdown.conf > /dev/null << TIMEOUTEOF
[Manager]
# Tiempo máximo de apagado del sistema antes de forzar kill
# Necesario para que safe-stop.sh complete el guardado de BD
DefaultTimeoutStopSec=180
TIMEOUTEOF
log "Timeout de apagado del sistema configurado a 180s."

# =============================================================================
# Recargar systemd y habilitar servicios
# =============================================================================
run "Recargar systemd"          sudo systemctl daemon-reload
run "Habilitar ac-authserver"   sudo systemctl enable ac-authserver
if [ "${WORLDSERVER_STANDBY:-false}" = true ]; then
    # Al boot se habilita el .socket (WantedBy=sockets.target): él posee el 8085
    # y arranca el servicio a la primera conexión. El servicio NO se habilita
    # (no tiene [Install]); si estaba habilitado de una instalación clásica, se
    # quita.
    #
    # ¿Se arranca el .socket AHORA?  Sólo si ya había algo sirviendo (worldserver
    # clásico o el propio .socket): es un redespliegue en caliente y hay que
    # mantener el 8085 vivo. En una instalación limpia NO se arranca: el primer
    # arranque (tools/primer-arranque.sh) necesita bindear el 8085 él mismo para
    # crear las BD, y instalar-todo.sh arranca el .socket al final.
    WAS_SERVING=no
    if systemctl is-active --quiet ac-worldserver || systemctl is-active --quiet ac-worldserver.socket; then
        WAS_SERVING=yes
    fi
    sudo systemctl disable ac-worldserver 2>/dev/null || true
    if systemctl is-active --quiet ac-worldserver; then
        warn "Parando el worldserver clásico para ceder el puerto 8085 al .socket…"
        sudo systemctl stop ac-worldserver || true
    fi
    run "Habilitar ac-worldserver.socket" sudo systemctl enable ac-worldserver.socket
    if [ "$WAS_SERVING" = yes ]; then
        run "Arrancar ac-worldserver.socket (redespliegue en caliente)" sudo systemctl restart ac-worldserver.socket
    else
        info "ac-worldserver.socket habilitado pero sin arrancar (instalación limpia): lo arranca instalar-todo.sh tras el primer arranque."
    fi
else
    # Vuelta a modo clásico: si venía de modo en espera, retirar el .socket.
    sudo systemctl disable --now ac-worldserver.socket 2>/dev/null || true
    run "Habilitar ac-worldserver"  sudo systemctl enable ac-worldserver
fi
if [ "${INSTALL_WEB_PANEL:-true}" = true ]; then
    run "Habilitar ac-panel-config-apply.timer" sudo systemctl enable --now ac-panel-config-apply.timer
fi

# =============================================================================
# Permisos sudo sin contraseña para scripts de automatización
#
# Se valida con `visudo -c` ANTES de instalarlo: un fichero corrupto en
# /etc/sudoers.d/ deja la máquina entera sin sudo, y recuperarla desde ahí
# exige consola de rescate.
# =============================================================================
header "Configurando permisos sudo"

SUDOERS_FILE="/etc/sudoers.d/azerothcore"
SUDOERS_TMP="$(mktemp)"

cat > "$SUDOERS_TMP" << SUDOEOF
# AzerothCore — permisos sin contraseña para scripts de automatización
# Generado por el instalador (fase 6). No editar a mano.
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl start ac-authserver
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl stop ac-authserver
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl restart ac-authserver
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl start ac-worldserver
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl stop ac-worldserver
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl restart ac-worldserver
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl start ac-worldserver.socket
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl stop ac-worldserver.socket
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/bin/systemctl restart ac-worldserver.socket
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/sbin/reboot
${AC_SYSTEM_USER} ALL=(ALL) NOPASSWD: /usr/sbin/shutdown
SUDOEOF

if sudo visudo -cqf "$SUDOERS_TMP"; then
    sudo install -m 0440 -o root -g root "$SUDOERS_TMP" "$SUDOERS_FILE"
    rm -f "$SUDOERS_TMP"
    log "Sudoers configurado y validado en $SUDOERS_FILE"
else
    rm -f "$SUDOERS_TMP"
    error "El fichero de sudoers generado no es válido. NO se ha instalado."
    error "La automatización (cron) necesitará contraseña hasta que se arregle."
    exit 1
fi

# =============================================================================
# Deshabilitar actualizaciones automáticas del SO
#
# ⚠️  Esto también desactiva los parches de seguridad desatendidos: la VM deja
#     de actualizarse sola. Es deliberado (evita que apt reinicie servicios a
#     media partida), pero implica que las actualizaciones del SO pasan a ser
#     tarea manual — `sudo apt update && sudo apt upgrade` cada cierto tiempo.
# =============================================================================
header "Deshabilitando actualizaciones automáticas del SO"

AUTO_UPGRADES="/etc/apt/apt.conf.d/20auto-upgrades"
if [ -f "$AUTO_UPGRADES" ]; then
    sudo sed -i 's|^APT::Periodic|//APT::Periodic|g' "$AUTO_UPGRADES"
    log "Actualizaciones automáticas deshabilitadas."
    warn "La VM ya no recibirá parches de seguridad sola: actualízala a mano"
    warn "  de vez en cuando con 'sudo apt update && sudo apt upgrade'."
else
    warn "$AUTO_UPGRADES no encontrado — puede que ya esté deshabilitado."
fi

log "Servicios systemd configurados y habilitados."
info ""
info "Apagado seguro desde Proxmox:"
info "  Proxmox envía ACPI shutdown → systemd para los servicios en orden"
info "  → ExecStop llama a safe-stop.sh → avisa jugadores → saveall → shutdown limpio"
info "  El log de cada parada queda en: ${AC_LOGS_DIR}/safe-stop.log"
