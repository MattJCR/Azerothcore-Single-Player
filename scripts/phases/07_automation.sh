#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  07_automation.sh — Crea los scripts de automatización y configura crontab
#  Incluye: reinicio diario, actualización semanal, arranque automático
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

header "FASE 7 — Scripts de automatización"

mkdir -p "$AC_SCRIPTS_DIR"
mkdir -p "$AC_LOGS_DIR"

# Marca compartida con safe-stop.sh: le dice que la cuenta atrás con los
# jugadores ya se ha hecho y que no la repita al pararse el servicio.
RESTART_FLAG="$AC_LOGS_DIR/.restart-announced"

# =============================================================================
# Script: notify-restart.sh
# Avisa a los jugadores con cuenta regresiva de 5 minutos y para los servicios
#
# ⚠️  Antes este script mandaba "server shutdown 1" por la sesión screen y solo
#     después hacía `systemctl stop`. El worldserver moría en ~1 segundo, screen
#     terminaba, y systemd — que tiene Restart=always — lo relanzaba a los 5
#     segundos, justo cuando llegaba el `stop`. Resultado: carrera, doble tanda
#     de avisos a los jugadores y, en el peor caso, un `screen -X quit` sobre un
#     worldserver a medio cargar.
#
#     Ahora el único que apaga es systemd: `systemctl stop` dispara ExecStop
#     (safe-stop.sh), que hace el saveall y el shutdown ordenado. La marca
#     RESTART_FLAG evita que safe-stop.sh repita la cuenta atrás.
# =============================================================================
{
cat << PREAMBLE
#!/bin/bash
# Avisa a los jugadores y para los servicios de AzerothCore de forma ordenada.
# Generado automáticamente por el instalador (fase 7). No editar a mano.
# Uso: notify-restart.sh [motivo]
WS_SCREEN="${WS_SCREEN}"
RESTART_FLAG="${RESTART_FLAG}"
WS_CONSOLE="${AC_SCRIPTS_DIR}/ws-console.sh"
PREAMBLE
cat << 'SCRIPT'
MOTIVO="${1:-reinicio programado}"

# Canal a la consola: screen (clásico) o SOAP (modo en espera, sin screen).
send_msg() {
    local TXT="El servidor se reiniciará en $1 ($MOTIVO)."
    if screen -list 2>/dev/null | grep -q "\.${WS_SCREEN}[[:space:]]"; then
        screen -S "$WS_SCREEN" -X stuff "announce ${TXT}$(printf '\r')" 2>/dev/null || true
    elif [ -x "$WS_CONSOLE" ]; then
        "$WS_CONSOLE" "announce ${TXT}" || true
    fi
}

echo "[$(date '+%Y-%m-%d %H:%M:%S')] Enviando avisos de reinicio (motivo: $MOTIVO)"
send_msg "5 minutos"
sleep 240
send_msg "1 minuto"
sleep 50
send_msg "10 segundos"
sleep 10

# Avisar a safe-stop.sh de que la cuenta atrás ya está hecha
touch "$RESTART_FLAG"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] Parando servicios..."
# Solo systemd apaga el worldserver: su ExecStop (safe-stop.sh) hace el
# saveall y el "server shutdown" ordenado. Mandarlo también por screen desde
# aquí provocaba una carrera con Restart=always.
sudo systemctl stop ac-worldserver || true
sudo systemctl stop ac-authserver  || true
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Servicios parados."
SCRIPT
} > "$AC_SCRIPTS_DIR/notify-restart.sh"
chmod +x "$AC_SCRIPTS_DIR/notify-restart.sh"
log "Script notify-restart.sh creado."

# =============================================================================
# Script: daily-restart.sh
# Reinicia los servicios WoW cada día a las 00:00
# =============================================================================
{
cat << PREAMBLE
#!/bin/bash
# Reinicio diario del servidor WoW
# Generado automáticamente por el instalador (fase 7). No editar a mano.
LOG="${AC_LOGS_DIR}/daily-restart.log"
SCRIPTS_DIR="${AC_SCRIPTS_DIR}"
BIN_DIR="${AC_DIR}/env/dist/bin"
mkdir -p "${AC_LOGS_DIR}"
PREAMBLE
cat << 'SCRIPT'

ts() { date '+%Y-%m-%d %H:%M:%S'; }

echo "==============================" >> "$LOG"
echo "[$(ts)] Iniciando reinicio diario..." >> "$LOG"

# Modo en espera: si el worldserver está dormido no hay nada que reiniciar —
# la próxima sesión de juego ya arranca con un proceso recién iniciado. Sólo
# se reinicia si estaba despierto (alguien jugando pasada la medianoche).
if systemctl list-unit-files ac-worldserver.socket >/dev/null 2>&1 \
   && systemctl is-enabled ac-worldserver.socket >/dev/null 2>&1 \
   && ! systemctl is-active --quiet ac-worldserver; then
    echo "[$(ts)] Worldserver dormido (modo en espera): no se reinicia nada." >> "$LOG"
    find "$BIN_DIR" -maxdepth 1 -name 'Server.log.*' -mtime +14 -delete 2>/dev/null
    exit 0
fi

bash "$SCRIPTS_DIR/notify-restart.sh" "reinicio diario" >> "$LOG" 2>&1
sleep 10

sudo systemctl start ac-authserver >> "$LOG" 2>&1
sleep 5
sudo systemctl start ac-worldserver >> "$LOG" 2>&1

echo "[$(ts)] Reinicio diario completado." >> "$LOG"

# Copias del Server.log que deja el appender al arrancar (flag 16): 14 dias
find "$BIN_DIR" -maxdepth 1 -name 'Server.log.*' -mtime +14 -delete 2>/dev/null
SCRIPT
} > "$AC_SCRIPTS_DIR/daily-restart.sh"
chmod +x "$AC_SCRIPTS_DIR/daily-restart.sh"
log "Script daily-restart.sh creado."

# =============================================================================
# Script: weekly-update.sh
# Domingo 03:00. Su comportamiento depende de WEEKLY_UPDATE_MODE en config.sh:
#   "check" (por defecto) — solo mira si hay versiones nuevas y avisa a los GM
#                           por correo dentro del juego. No toca nada.
#   "apply"               — descarga, recompila, reinicia y refresca el lock.
#
# ⚠️  Los módulos que llevan parches locales (mod-dungeon-master,
#     mod-random-enchants y mod-congrats-on-level) quedan con el
#     árbol de git sucio, y `git pull` se niega a seguir con un "your local
#     changes would be overwritten". El módulo dejaba de actualizarse para
#     siempre, en silencio. Ahora se hace `fetch` + `reset --hard` y el parche
#     se reaplica después, que es justo para lo que está pensado.
# =============================================================================
{
cat << PREAMBLE
#!/bin/bash
# Actualización semanal: git pull core + módulos + recompilación
# Generado automáticamente por el instalador (fase 7). No editar a mano.
AC_DIR="${AC_DIR}"
LOG="${AC_LOGS_DIR}/weekly-update.log"
BUILD_DIR="${AC_DIR}/build"
SCRIPTS_DIR="${AC_SCRIPTS_DIR}"
INSTALLER_DIR="${SCRIPT_DIR}"
MODE="${WEEKLY_UPDATE_MODE}"
mkdir -p "${AC_LOGS_DIR}"
PREAMBLE
cat << 'SCRIPT'

ts() { date '+%Y-%m-%d %H:%M:%S'; }

echo "==============================" >> "$LOG"
echo "[$(ts)] === REVISIÓN SEMANAL ===" >> "$LOG"

# ── Modo "check": mirar y avisar, sin tocar nada ─────────────────────────────
# Es el modo por defecto. Actualizar a ciegas de madrugada es la forma más
# rápida de encontrarte el servidor roto un lunes sin saber qué cambió.
if [ "$MODE" != "apply" ]; then
    if [ -f "$INSTALLER_DIR/lib/check-updates.sh" ]; then
        bash "$INSTALLER_DIR/lib/check-updates.sh" >> "$LOG" 2>&1
        echo "[$(ts)] Revisión terminada (modo '$MODE': no se ha modificado nada)." >> "$LOG"
    else
        echo "[$(ts)] AVISO: falta $INSTALLER_DIR/lib/check-updates.sh" >> "$LOG"
    fi
    exit 0
fi

echo "[$(ts)] MODO 'apply': se actualizará y recompilará automáticamente." >> "$LOG"

# ── Reaplica los parches locales sobre módulos de terceros ───────────────────
# El `git reset --hard` de más abajo los borra a propósito (si no, el pull
# fallaría cada semana), así que hay que volver a ponerlos antes de compilar.
# La lógica vive en el instalador para no tener dos copias que se desincronicen.
reapply_patches() {
    if [ -f "$INSTALLER_DIR/lib/reapply-patches.sh" ]; then
        bash "$INSTALLER_DIR/lib/reapply-patches.sh" >> "$LOG" 2>&1 \
            && echo "[$(ts)] Parches locales reaplicados." >> "$LOG" \
            || echo "[$(ts)] AVISO: algun parche local no se pudo reaplicar." >> "$LOG"
    else
        echo "[$(ts)] AVISO: no se encuentra $INSTALLER_DIR/lib/reapply-patches.sh" >> "$LOG"
        echo "[$(ts)]        Los modulos se compilaran SIN los parches locales." >> "$LOG"
    fi
}

# ── 1. Comprobar cambios en el core ──────────────────────────────────────────
cd "$AC_DIR"
BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "master")
git fetch origin "$BRANCH" >> "$LOG" 2>&1
CAMBIOS_CORE=$(git rev-list --count "HEAD..origin/$BRANCH" 2>/dev/null || echo 0)

# ── 2. Comprobar cambios en cada módulo ──────────────────────────────────────
CAMBIOS_MODULOS=0
if [ -d "$AC_DIR/modules" ]; then
    for mod_dir in "$AC_DIR/modules"/*/; do
        if [ -d "$mod_dir/.git" ]; then
            cd "$mod_dir"
            MOD_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "master")
            git fetch origin "$MOD_BRANCH" >> "$LOG" 2>&1
            N=$(git rev-list --count "HEAD..origin/$MOD_BRANCH" 2>/dev/null || echo 0)
            CAMBIOS_MODULOS=$((CAMBIOS_MODULOS + N))
        fi
    done
fi

TOTAL=$((CAMBIOS_CORE + CAMBIOS_MODULOS))
echo "[$(ts)] Cambios: core=$CAMBIOS_CORE módulos=$CAMBIOS_MODULOS" >> "$LOG"

# ── 3. Sin cambios → salir sin reiniciar ─────────────────────────────────────
if [ "$TOTAL" -eq 0 ]; then
    echo "[$(ts)] Sin cambios. No se recompila." >> "$LOG"
    exit 0
fi

# ── 4. Avisar y parar servidores ─────────────────────────────────────────────
# Modo en espera: recordar si el worldserver estaba despierto. Si dormía, tras
# recompilar NO se arranca a la fuerza — el .socket carga el binario nuevo en
# la próxima conexión de un cliente.
WS_WAS_ACTIVE=no
if systemctl is-active --quiet ac-worldserver; then WS_WAS_ACTIVE=yes; fi
STANDBY_MODE=no
if systemctl list-unit-files ac-worldserver.socket >/dev/null 2>&1 \
   && systemctl is-enabled ac-worldserver.socket >/dev/null 2>&1; then STANDBY_MODE=yes; fi

start_worldserver_if_wanted() {
    if [ "$STANDBY_MODE" = yes ] && [ "$WS_WAS_ACTIVE" = no ]; then
        echo "[$(ts)] Modo en espera: el worldserver dormía; se deja dormido (arranca en la próxima conexión)." >> "$LOG"
        return 0
    fi
    sudo systemctl start ac-worldserver >> "$LOG" 2>&1
}

bash "$SCRIPTS_DIR/notify-restart.sh" "actualización semanal" >> "$LOG" 2>&1
sleep 10

# ── 5. Actualizar core ───────────────────────────────────────────────────────
# --ff-only: no queremos merges automáticos en el core (además, un merge commit
# exigiría user.name/user.email configurados en la VM, que normalmente no lo están).
cd "$AC_DIR"
if ! git merge --ff-only "origin/$BRANCH" >> "$LOG" 2>&1; then
    echo "[$(ts)] AVISO: el core no avanza en fast-forward (¿cambios locales?). Se deja como está." >> "$LOG"
fi

# ── 6. Actualizar módulos ────────────────────────────────────────────────────
# reset --hard en vez de pull: los parches locales (ver reapply_patches) dejan
# el árbol sucio y harían fallar el merge cada semana.
if [ -d "$AC_DIR/modules" ]; then
    for mod_dir in "$AC_DIR/modules"/*/; do
        if [ -d "$mod_dir/.git" ]; then
            MOD_NAME=$(basename "$mod_dir")
            echo "[$(ts)] Actualizando módulo: $MOD_NAME" >> "$LOG"
            cd "$mod_dir"
            MOD_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "master")
            git fetch origin "$MOD_BRANCH" >> "$LOG" 2>&1
            git reset --hard "origin/$MOD_BRANCH" >> "$LOG" 2>&1 \
                || echo "[$(ts)] AVISO: no se pudo actualizar $MOD_NAME" >> "$LOG"
        fi
    done
fi

# ── 6b. Reaplicar parches conocidos sobre módulos de terceros ───────────────
reapply_patches

# ── 7. Recompilar ────────────────────────────────────────────────────────────
echo "[$(ts)] Iniciando compilación..." >> "$LOG"
cd "$BUILD_DIR"
if bash recompile.sh >> "$LOG" 2>&1; then
    echo "[$(ts)] Compilación exitosa." >> "$LOG"
else
    echo "[$(ts)] ERROR: compilación fallida. Revisa $LOG" >> "$LOG"
    # Intentar arrancar con el binario anterior
    sudo systemctl start ac-authserver  2>/dev/null || true
    start_worldserver_if_wanted
    exit 1
fi

# ── 8. Arrancar con el nuevo binario ─────────────────────────────────────────
sudo systemctl start ac-authserver >> "$LOG" 2>&1
sleep 5
start_worldserver_if_wanted

# Si la actualización automática ha ido bien, estas pasan a ser las versiones
# buenas conocidas: se refresca el lock para poder volver aquí más adelante.
if [ -f "$INSTALLER_DIR/install.sh" ]; then
    bash "$INSTALLER_DIR/install.sh" --freeze >> "$LOG" 2>&1         && echo "[$(ts)] versions.lock actualizado con las versiones nuevas." >> "$LOG"
fi

echo "[$(ts)] Actualización semanal completada." >> "$LOG"
SCRIPT
} > "$AC_SCRIPTS_DIR/weekly-update.sh"
chmod +x "$AC_SCRIPTS_DIR/weekly-update.sh"
log "Script weekly-update.sh creado."

# =============================================================================
# Script: startup.sh
# Arranque manual de los servicios esperando a que MySQL esté listo.
#
# NOTA: ya NO se engancha a @reboot. Las unidades systemd están habilitadas
# (WantedBy=multi-user.target) y ahora traen su propio ExecStartPre que espera
# a MySQL, así que el arranque en el boot lo gobierna systemd. Este script se
# mantiene como atajo manual: bash ~/azerothcore/scripts/startup.sh
# =============================================================================
{
cat << PREAMBLE
#!/bin/bash
# Arranque manual de AzerothCore (espera a que MySQL acepte conexiones).
# Generado automáticamente por el instalador (fase 7). No editar a mano.
LOG="${AC_LOGS_DIR}/startup.log"
WAIT_SCRIPT="${AC_SCRIPTS_DIR}/wait-for-mysql.sh"
mkdir -p "${AC_LOGS_DIR}"
PREAMBLE
cat << 'SCRIPT'

ts() { date '+%Y-%m-%d %H:%M:%S'; }

echo "==============================" >> "$LOG"
echo "[$(ts)] Arranque manual solicitado." >> "$LOG"

if ! bash "$WAIT_SCRIPT" >> "$LOG" 2>&1; then
    echo "[$(ts)] ERROR: MySQL no respondió. Abortando." >> "$LOG"
    exit 1
fi

echo "[$(ts)] MySQL listo. Arrancando servicios..." >> "$LOG"
sudo systemctl start ac-authserver  >> "$LOG" 2>&1
sleep 5
sudo systemctl start ac-worldserver >> "$LOG" 2>&1
echo "[$(ts)] Servicios arrancados." >> "$LOG"
SCRIPT
} > "$AC_SCRIPTS_DIR/startup.sh"
chmod +x "$AC_SCRIPTS_DIR/startup.sh"
log "Script startup.sh creado."

# =============================================================================
# Configurar crontab
#
# Se borran primero las entradas anteriores del instalador (van marcadas con
# CRON_TAG). Antes solo se comprobaba "¿existe esta línea exacta?", así que
# cambiar una hora en config.sh y relanzar esta fase dejaba DOS reinicios
# diarios programados, uno con la hora vieja y otro con la nueva.
# =============================================================================
header "Configurando crontab"

clear_installer_cron

# Reinicio diario. Dos modos:
#  - CRON_DAILY_RESTART_FULL_REBOOT=true: "sudo reboot" de la VM entera a la
#    hora exacta configurada (mismo patrón sin aviso propio que el reinicio
#    semanal de abajo: lo dispara safe-stop.sh, que ya avisa 60s y hace
#    saveall como ExecStop de ac-worldserver.service). No se salta aunque el
#    worldserver esté dormido -libera memoria de MySQL/panel/cache del
#    sistema igualmente, que un reinicio de servicio no toca-.
#  - false: reinicio de servicios de siempre (daily-restart.sh, con su propio
#    aviso de 5 min, por eso el cron se dispara 5 min antes de la hora
#    objetivo; ese script se salta solo si el worldserver ya está dormido).
if [ "${CRON_DAILY_RESTART_FULL_REBOOT:-false}" = "true" ]; then
    add_cron "$CRON_DAILY_RESTART_MIN $CRON_DAILY_RESTART_HOUR * * * sudo /usr/sbin/reboot"
else
    DAILY_NOTIFY_MIN=$(( CRON_DAILY_RESTART_MIN - 5 ))
    DAILY_NOTIFY_HOUR=$CRON_DAILY_RESTART_HOUR
    if [ "$DAILY_NOTIFY_MIN" -lt 0 ]; then
        DAILY_NOTIFY_MIN=$(( 60 + DAILY_NOTIFY_MIN ))
        DAILY_NOTIFY_HOUR=$(( DAILY_NOTIFY_HOUR - 1 ))
        [ "$DAILY_NOTIFY_HOUR" -lt 0 ] && DAILY_NOTIFY_HOUR=23
    fi
    add_cron "$DAILY_NOTIFY_MIN $DAILY_NOTIFY_HOUR * * * /bin/bash $AC_SCRIPTS_DIR/daily-restart.sh >> $AC_LOGS_DIR/cron.log 2>&1"
fi
add_cron "$CRON_WEEKLY_UPDATE_MIN $CRON_WEEKLY_UPDATE_HOUR * * 0 /bin/bash $AC_SCRIPTS_DIR/weekly-update.sh >> $AC_LOGS_DIR/cron.log 2>&1"
add_cron "$CRON_WEEKLY_REBOOT_MIN $CRON_WEEKLY_REBOOT_HOUR * * 0 sudo /usr/sbin/reboot"

# El antiguo "@reboot startup.sh" se ha eliminado a propósito: duplicaba el
# arranque que ya hace systemd (unidades habilitadas) y llegaba tarde, porque
# systemd ya había intentado arrancar el worldserver sin esperar a MySQL.
# La espera vive ahora en ExecStartPre=wait-for-mysql.sh (fase 6).

# =============================================================================
# Configurar logrotate
# =============================================================================
header "Configurando logrotate"

sudo tee /etc/logrotate.d/azerothcore > /dev/null << LOGROTATE
${AC_LOGS_DIR}/*.log {
    # Sin "su", logrotate se NIEGA a rotar y deja este aviso en su log:
    #   "skipping ... because parent directory has insecure permissions"
    # Es su protección contra rotar directorios escribibles por otros; como
    # estos logs viven en el home del usuario del servidor, hay que decirle
    # explícitamente con qué usuario debe rotarlos.
    su ${AC_SYSTEM_USER} ${AC_SYSTEM_USER}
    weekly
    rotate 4
    compress
    missingok
    notifempty
    create 0644 ${AC_SYSTEM_USER} ${AC_SYSTEM_USER}
}

# Los logs del propio worldserver/authserver son los que de verdad crecen
# (Server.log, Errors.log, Playerbots.log...). Van a bin/ y no a logs/ porque
# worldserver.conf trae LogsDir = "" y el binario escribe en su directorio.
${AC_DIR}/env/dist/bin/*.log {
    su ${AC_SYSTEM_USER} ${AC_SYSTEM_USER}
    weekly
    rotate 4
    compress
    missingok
    notifempty
    copytruncate
    create 0644 ${AC_SYSTEM_USER} ${AC_SYSTEM_USER}
}
LOGROTATE
log "Logrotate configurado."

log "Scripts de automatización creados y crontab configurado."
info "Resumen del crontab:"
crontab -l 2>/dev/null | grep -v "^#" | grep -v "^$" || true
