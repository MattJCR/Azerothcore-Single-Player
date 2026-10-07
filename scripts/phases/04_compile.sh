#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  04_compile.sh — Configura CMake, compila e instala AzerothCore
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

header "FASE 4 — Compilar AzerothCore"

BUILD_DIR="$AC_DIR/build"
DIST_DIR="$AC_DIR/env/dist"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR" || { error "No se puede entrar en $BUILD_DIR"; exit 1; }

info "Cores de compilación: $BUILD_CORES"
info "Destino de instalación: $DIST_DIR"

info "Módulos de compilación detectados en $AC_DIR/modules:"
if [ -d "$AC_DIR/modules" ]; then
    for MOD in "$AC_DIR/modules"/*/; do
        [ -d "$MOD" ] && info "  - $(basename "$MOD")"
    done
fi
if [ -d "$AC_DIR/modules-disabled" ] && [ -n "$(ls -A "$AC_DIR/modules-disabled" 2>/dev/null)" ]; then
    info "Modulos apartados (no se compilan), en modules-disabled/:"
    for MOD in "$AC_DIR/modules-disabled"/*/; do
        [ -d "$MOD" ] && info "  - $(basename "$MOD")"
    done
fi
[ "$INSTALL_MOD_ARAC" = true ] && \
    info "  (mod-arac NO se compila: vive en extras/ y solo aporta SQL/parche)"

# --- Configurar CMake ---
# BUILD_TESTING=OFF: la batería de unit_tests del core (src/test/) no se
# despliega y ha ido por detrás de lo que la rama Playerbot añade al core
# (hasta el 18/09/2026 su WorldMock no implementaba GetPlayerbotsDBRevision,
# ya retirado al pasar la BD de playerbots al módulo), así que `make` llegó a
# abortar al compilar los tests aunque worldserver/scripts/modules estuvieran
# bien. No los necesitamos para producción.
run "Configurar CMake" cmake "$AC_DIR" \
    -DCMAKE_INSTALL_PREFIX="$DIST_DIR" \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DWITH_WARNINGS=1 \
    -DTOOLS_BUILD=all \
    -DBUILD_TESTING=OFF \
    -DSCRIPTS=static \
    -DMODULES=static

# --- Compilar ---
info "Compilando con $BUILD_CORES cores (esto puede tardar 10-20 minutos)..."
if make -j"$BUILD_CORES" >> "$INSTALL_LOG" 2>&1; then
    log "Compilación exitosa."
else
    error "La compilación falló. Revisa $INSTALL_LOG"
    exit 1
fi

# --- Instalar ---
run "Instalar binarios" make install

# --- Crear script de recompilación reutilizable ---
cat > "$BUILD_DIR/recompile.sh" << RECOMPILE
#!/bin/bash
# Script de recompilación — generado automáticamente por el instalador
set -e
cd "$BUILD_DIR"
BUILD_CORES=\$(nproc | awk '{print \$1 - 1}')
cmake "$AC_DIR" \\
    -DCMAKE_INSTALL_PREFIX="$DIST_DIR" \\
    -DCMAKE_C_COMPILER=/usr/bin/clang \\
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \\
    -DWITH_WARNINGS=1 \\
    -DTOOLS_BUILD=all \\
    -DBUILD_TESTING=OFF \\
    -DSCRIPTS=static \\
    -DMODULES=static && \\
make -j"\$BUILD_CORES" && \\
make install
echo "Recompilación completada: \$(date)"
RECOMPILE
chmod +x "$BUILD_DIR/recompile.sh"
log "Script de recompilación creado en $BUILD_DIR/recompile.sh"

log "Compilación e instalación completadas."
