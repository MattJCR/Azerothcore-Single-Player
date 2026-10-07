#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  lib/reapply-patches.sh — Reaplica los parches locales del core y los módulos
#
#  Lo llama weekly-update.sh después de cada `git reset --hard`: el reset borra
#  los parches (para eso está, para que el pull no falle), así que hay que
#  volver a ponerlos antes de recompilar.
#
#  También se puede ejecutar a mano:
#      bash ~/azerothcore-installer/lib/reapply-patches.sh
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../config.sh"
source "$SCRIPT_DIR/utils.sh"

header "Reaplicando parches locales sobre el core y los módulos"

FAILED=0
apply_all_source_patches || FAILED=1

if [ "$FAILED" -eq 0 ]; then
    log "Todos los parches están aplicados."
    exit 0
else
    error "Algún parche no se pudo aplicar — revisa los avisos de arriba."
    error "La compilación seguirá adelante con el código sin parchear."
    exit 1
fi
