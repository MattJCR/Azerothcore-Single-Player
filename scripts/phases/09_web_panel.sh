#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  09_web_panel.sh — Instala o actualiza el panel web después de crear las BBDD
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

header "FASE 9 — Panel web"

if [ "${INSTALL_WEB_PANEL:-true}" != true ]; then
    info "INSTALL_WEB_PANEL=false: se omite el panel web."
    exit 0
fi

if ! db_exists "${PANEL_AUTH_DATABASE:-acore_auth}" || ! db_exists "${PANEL_CHARACTERS_DATABASE:-acore_characters}"; then
    error "Las bases de datos del panel todavía no existen."
    error "Ejecuta primero el primer arranque y después: ./install.sh --panel"
    exit 1
fi

if [ ! -f "$SCRIPT_DIR/web-panel/deploy/install.sh" ]; then
    error "No existe web-panel/deploy/install.sh en el instalador."
    exit 1
fi

# Antes de desplegar: addons reconstruidos y comprobados, datos preparados, mapas y
# entradas de ARAC (scripts/preparar-recursos.sh). Si falla, el panel no se despliega.
bash "$SCRIPT_DIR/scripts/preparar-recursos.sh"

info "Instalando/actualizando el panel desde $SCRIPT_DIR/web-panel…"

# El deploy del panel instala paquetes (apt), escribe en /etc y toca systemd:
# necesita root. En la VM el NOPASSWD solo cubre start/stop/restart, así que
# 'sudo bash ...' pediría contraseña y una ejecución no interactiva se
# quedaría colgada. Se comprueba antes y se avisa con instrucciones claras.
if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    error "La fase 9 necesita sudo sin contraseña o ejecutarse como root."
    error "  - Interactivo:      sudo ./install.sh --panel"
    error "  - No interactivo:   ejecuta este paso a mano con una sesión con tty,"
    error "                      o añade un NOPASSWD para 'bash $SCRIPT_DIR/web-panel/deploy/install.sh'."
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    bash "$SCRIPT_DIR/web-panel/deploy/install.sh"
else
    sudo bash "$SCRIPT_DIR/web-panel/deploy/install.sh"
fi
log "Panel web instalado o actualizado en el puerto ${PANEL_HTTP_PORT:-80}."
