#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  preparar-recursos.sh — lo que el panel necesita ANTES de desplegarse (PUB04-I)
#
#  La edición pública no lleva los 309 addons, ni los iconos, mapas y MPQ que
#  salen del cliente de Blizzard. Este paso los deja listos por vías verificables:
#
#    1. dependencias: Node, bsdtar (para los RAR de los addons) y Pillow;
#    2. addons: web-panel/tools/build-addons.mjs reconstruye cliente/Interface/AddOns
#       desde las fuentes fijadas por SHA-256 (o comprueba lo que ya hay: el
#       repositorio de trabajo los trae y entonces no se descarga nada);
#    3. datos ya preparados: si el repositorio los trae, se ofrecen al panel como
#       «semilla»; los mapas se descargan con hash o se toman de ahí;
#    4. los tres DBC de ARAC, verificados desde su snapshot, para generar los MPQ.
#
#  Los iconos y los parches de idioma NO se generan aquí: salen del cliente del
#  jugador, desde el propio panel (Addons -> elegir carpeta de WoW).
#
#  Idempotente. Cualquier fallo detiene la instalación: un recurso ausente o una
#  construcción fallida no se da por buena.
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"
source "$SCRIPT_DIR/lib/versions.sh"
source "$SCRIPT_DIR/lib/mirrors.sh"

header "Preparar recursos del panel"

PANEL_SRC="$SCRIPT_DIR/web-panel"
STAGE="$SCRIPT_DIR/.instalacion"
mkdir -p "$STAGE"

# --- 1. Dependencias ----------------------------------------------------------
NEED=()
command -v node >/dev/null 2>&1 || NEED+=(nodejs)
command -v npm >/dev/null 2>&1 || NEED+=(npm)
command -v bsdtar >/dev/null 2>&1 || NEED+=(libarchive-tools)
python3 -c 'import PIL' >/dev/null 2>&1 || NEED+=(python3-pil)
command -v rsync >/dev/null 2>&1 || NEED+=(rsync)
if [ "${#NEED[@]}" -gt 0 ]; then
    run "Instalar ${NEED[*]}" sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${NEED[@]}"
fi
NODE_MAJOR="$(node --version | sed -E 's/^v([0-9]+).*/\1/')"
[ "$NODE_MAJOR" -ge 18 ] || { error "Hace falta Node.js 18.18 o posterior (hay $(node --version))."; exit 1; }

# --- 2. Dependencias del panel y addons ---------------------------------------
run "Instalar las dependencias de Node del panel" bash -c "cd '$PANEL_SRC' && npm ci --omit=dev --no-audit --no-fund"

ADDONS_DIR="$SCRIPT_DIR/cliente/Interface/AddOns"
BUILD=(node "$PANEL_SRC/tools/build-addons.mjs")
if "${BUILD[@]}" verificar --destino "$ADDONS_DIR" >/dev/null 2>&1; then
    log "Los 309 addons de cliente ya coinciden con arbol.tsv; no se descarga nada."
else
    info "Construyendo los addons de cliente desde las fuentes fijadas (descarga con SHA-256)…"
    "${BUILD[@]}" build --destino "$ADDONS_DIR" --cache "$STAGE/cache/addons" >> "$INSTALL_LOG" 2>&1 \
        || { error "La construcción de los addons falló (ver $INSTALL_LOG)."; exit 1; }
    "${BUILD[@]}" verificar --destino "$ADDONS_DIR" || { error "Los addons construidos no coinciden con arbol.tsv."; exit 1; }
fi

# --- 3. Datos preparados y mapas ----------------------------------------------
SEED="$STAGE/semilla"
rm -rf "$SEED"
mkdir -p "$SEED/iconos" "$SEED/parches" "$SEED/mapas"
if [ -f "$PANEL_SRC/public/assets/item-icons/map.json" ]; then
    cp -r "$PANEL_SRC/public/assets/item-icons/." "$SEED/iconos/"
    info "Iconos preparados del repositorio ofrecidos al panel."
fi
for LANG_DIR in esES enUS; do
    MPQ="$SCRIPT_DIR/cliente/Data/$LANG_DIR/patch-${LANG_DIR}-4.MPQ"
    if [ -f "$MPQ" ]; then
        mkdir -p "$SEED/parches/$LANG_DIR"
        cp "$MPQ" "$SEED/parches/$LANG_DIR/"
        info "patch-${LANG_DIR}-4.MPQ preparado del repositorio ofrecido al panel."
    fi
done
if node "$PANEL_SRC/tools/fetch-maps.mjs" --salida "$SEED/mapas" --desde "$PANEL_SRC/public/assets/maps" >> "$INSTALL_LOG" 2>&1; then
    log "Mapas del panel listos y verificados."
else
    warn "Algún mapa no se consiguió con el hash fijado: el panel mostrará un fondo neutro en ese continente (ver $INSTALL_LOG)."
fi

# --- 4. DBC de ARAC para generar los MPQ --------------------------------------
if [ "${INSTALL_MOD_ARAC:-true}" = true ]; then
    extract_arac_inputs "$STAGE/insumos/arac" || { error "No se pudieron preparar los DBC de ARAC."; exit 1; }
fi

log "Recursos del panel preparados. Los iconos y los parches de idioma se generan desde tu cliente en el panel (Addons)."
