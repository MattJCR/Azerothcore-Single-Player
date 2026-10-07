#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  lib/check-updates.sh — ¿Hay versiones nuevas río arriba?
#
#  MIRA, pero NO TOCA NADA. Compara cada repositorio contra el commit fijado en
#  versions.lock, y cada addon de cliente contra addons.lock/catalog.json
#  (lib/addon-versions.sh), y si hay novedades en cualquiera de los dos avisa
#  a las cuentas GM con un correo dentro del juego.
#
#  Es lo que ejecuta la tarea semanal cuando WEEKLY_UPDATE_MODE="check".
#  El comportamiento antiguo (descargar, recompilar y reiniciar solo a las 3 de
#  la madrugada) sigue disponible con WEEKLY_UPDATE_MODE="apply", pero por
#  defecto actualizar pasa a ser una decisión tuya y no una sorpresa del lunes.
#
#  También se puede lanzar a mano:
#      bash ~/azerothcore-installer/lib/check-updates.sh
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../config.sh"
source "$SCRIPT_DIR/utils.sh"
source "$SCRIPT_DIR/versions.sh"
source "$SCRIPT_DIR/addon-versions.sh"

header "Comprobando si hay versiones nuevas"

if [ ! -f "$VERSIONS_LOCK" ]; then
    warn "No existe versions.lock, así que no hay con qué comparar."
    warn "  Congela las versiones actuales con: ./install.sh --freeze"
    exit 0
fi

check_upstream_updates || true

# Se fusiona con lo de arriba (versions.lock) para que mod-update-notice
# enseñe un único aviso con módulos Y addons, en vez de que los addons se
# queden fuera del todo (antes de esto, sólo se veían a mano en la pestaña
# Actualizaciones del panel web o con tools/revisar-actualizaciones-addons.sh).
check_addon_updates || true
UPDATE_COUNT=$((UPDATE_COUNT + ADDON_UPDATE_COUNT))
UPDATE_DETAILS+=("${ADDON_UPDATE_DETAILS[@]}")
if [ "${ADDON_UPDATE_COUNT:-0}" -gt 0 ]; then
    UPDATE_REPORT="${UPDATE_REPORT}addons de cliente: ${ADDON_UPDATE_COUNT} con actualización; "
fi
if [ "${ADDON_UPDATE_UNREACHABLE:-0}" -gt 0 ]; then
    UPDATE_UNREACHABLE="${UPDATE_UNREACHABLE}${UPDATE_UNREACHABLE:+ }addons-de-cliente(${ADDON_UPDATE_UNREACHABLE})"
fi

# El fichero que enseña mod-update-notice a los GM al conectarse. Se escribe
# siempre (con novedades o sin ellas: ver el comentario de write_update_report).
write_update_report

if [ "$UPDATE_COUNT" -eq 0 ]; then
    log "Todo al día: ningún repositorio ni addon tiene commits nuevos sobre la versión fijada."
    exit 0
fi

info "Informe escrito en $UPDATE_REPORT_FILE (lo enseña mod-update-notice al conectarse un GM)."

warn "Hay actualizaciones disponibles (${UPDATE_COUNT} en total, módulos del servidor y addons de cliente):"
for LINE in "${UPDATE_DETAILS[@]}"; do
    warn "  - $LINE"
done

info ""
info "NO se ha tocado nada. Para actualizar cuando te venga bien:"
info "  cd ~/azerothcore-installer"
info "  ./install.sh --only 3      # baja el código nuevo y reaplica los parches"
info "  sudo systemctl stop ac-worldserver"
info "  ./install.sh --from 4      # recompila y reconfigura"
info "  ./install.sh --freeze      # si todo va bien, fija estas versiones"
info ""
info "Y si algo se rompe, se vuelve atrás con el versions.lock anterior."

# ── Aviso dentro del juego ───────────────────────────────────────────────────
if [ "${NOTIFY_GM_ON_UPDATE:-true}" = true ]; then
    BODY="Hay ${UPDATE_COUNT} actualizacion(es) disponible(s) (modulos y addons): ${UPDATE_REPORT}"
    BODY="${BODY} El servidor NO se ha tocado. Para actualizar: ./install.sh --only 3, luego --from 4, y --freeze si todo va bien."
    bash "$SCRIPT_DIR/notify-gm.sh" "AzerothCore: hay actualizaciones" "$BODY" || \
        warn "No se pudo enviar el aviso a los GM (¿worldserver parado?)."
fi

exit 0
