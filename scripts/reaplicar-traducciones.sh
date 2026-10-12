#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  reaplicar-traducciones.sh — vuelve a aplicar patches/locales-es/ a acore_world
#
#  Cuándo: tras el primer arranque del worldserver que sigue a actualizar un
#  módulo cuyo SQL se reejecuta. Ejemplo (12/10/2026): mod-transmog renombró sus
#  ficheros SQL y su `DELETE FROM module_string_locale WHERE module='mod-transmog'`
#  borró nuestras filas esES/esMX 18 y 19, que la fase 5 había aplicado ANTES de
#  ese arranque. `doctor` ("traducciones-es") avisa cuando faltan.
#
#  Es idempotente (cada SQL borra sus propias filas antes de insertar) y no toca
#  nada más: ni confs, ni servicios, ni cuentas.
#
#  Uso (en la VM, como el usuario del servidor):  bash scripts/reaplicar-traducciones.sh
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

header "Reaplicar traducciones al español"

if ! db_exists acore_world; then
    error "acore_world no existe todavía: arranca el worldserver una vez antes."
    exit 1
fi

apply_sql_dir "traducciones es" acore_world "$PATCHES_DIR/locales-es"

if [ "${INSTALL_MOD_PLAYERBOTS:-false}" = true ] && table_exists acore_playerbots ai_playerbot_texts; then
    apply_sql_dir "bots en español" acore_playerbots "$PATCHES_DIR/locales-es-playerbots"
fi
