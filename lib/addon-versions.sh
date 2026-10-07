#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  lib/addon-versions.sh — ¿hay addons de cliente con actualización?
#
#  Equivalente de check_upstream_updates() (lib/versions.sh) pero para
#  addons.lock y web-panel/addons/catalog.json en vez de versions.lock. No
#  hace git fetch sobre clones locales porque no los hay: los addons de
#  cliente viven como carpetas ya extraídas en cliente/Interface/AddOns/, sin
#  .git. Por eso delega en tools/addon-updates-report.mjs, que reutiliza tal
#  cual la misma comprobación de la pestaña Actualizaciones del panel web
#  (git ls-remote + la API compare de GitHub, sin clonar nada).
#
#  Deja el resultado en ADDON_UPDATE_COUNT / ADDON_UPDATE_UNREACHABLE /
#  ADDON_UPDATE_DETAILS para que lib/check-updates.sh los fusione con los de
#  check_upstream_updates() antes de escribir el informe único que lee
#  mod-update-notice (antes sólo cubría core/módulos, nunca addons).
# =============================================================================

check_addon_updates() {
    ADDON_UPDATE_COUNT=0
    ADDON_UPDATE_UNREACHABLE=0
    ADDON_UPDATE_DETAILS=()

    if ! command -v node >/dev/null 2>&1; then
        warn "node no está disponible: no se comprueban actualizaciones de addons."
        return 1
    fi
    if [ ! -f "$INSTALLER_DIR/addons.lock" ] || [ ! -f "$INSTALLER_DIR/web-panel/addons/catalog.json" ]; then
        return 1
    fi

    local TAG VALUE
    while IFS=$'\t' read -r TAG VALUE; do
        case "$TAG" in
            COUNT=*)       ADDON_UPDATE_COUNT="${TAG#COUNT=}" ;;
            UNREACHABLE=*) ADDON_UPDATE_UNREACHABLE="${TAG#UNREACHABLE=}" ;;
            DETAIL)        ADDON_UPDATE_DETAILS+=("$VALUE") ;;
        esac
    done < <(node "$INSTALLER_DIR/tools/addon-updates-report.mjs" "$INSTALLER_DIR" 2>/dev/null)

    [ "${ADDON_UPDATE_COUNT:-0}" -gt 0 ]
}
