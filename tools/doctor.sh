#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  tools/doctor.sh — punto de entrada independiente para lib/doctor.sh
#
#  Pensado para dispararse solo, sin pasar por install.sh: lo llama
#  ExecStartPost de ac-worldserver.service (fase 6) en cada arranque o
#  reinicio del worldserver, con el evento "restart". También sirve para
#  lanzarlo a mano: bash tools/doctor.sh [evento]
#
#  './install.sh --doctor' es el otro punto de entrada (evento "manual");
#  ambos llaman a la misma run_doctor().
# =============================================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"
source "$SCRIPT_DIR/lib/versions.sh"
source "$SCRIPT_DIR/lib/mirrors.sh"
source "$SCRIPT_DIR/lib/doctor.sh"

run_doctor "${1:-manual}"
