#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Estado de la instalación completa. Nunca se ejecuta el contenido del fichero.
save_install_step() {
    [ -n "${AC_INSTALL_STATE_FILE:-}" ] || return 0
    local next="$1" temporary
    [[ "$next" =~ ^([1-9]|1[0-2])$ ]] || return 1
    temporary=$(mktemp "${AC_INSTALL_STATE_FILE}.XXXXXX") || return 1
    printf '%s %s\n' "$AC_INSTALL_FINGERPRINT" "$next" > "$temporary"
    mv -f "$temporary" "$AC_INSTALL_STATE_FILE"
}
