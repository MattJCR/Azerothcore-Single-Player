#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  01_dependencies.sh — Instala dependencias del sistema y MySQL 8.4 LTS
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

header "FASE 1 — Dependencias del sistema"

# --- Zona horaria (04/09/2026) ---
# Los logs del worldserver, las fechas DATETIME de las tablas adaptive_* (se
# guardan como texto en hora local) y el reinicio diario del cron van en la
# hora del sistema. Se fija aquí, antes de que arranque nada que la cachee
# (cron y mysqld leen la zona al arrancar). Vacío = no tocar.
if [ -n "${TIMEZONE:-}" ] && command -v timedatectl >/dev/null 2>&1; then
    if [ "$(timedatectl show -p Timezone --value 2>/dev/null)" != "$TIMEZONE" ]; then
        sudo timedatectl set-timezone "$TIMEZONE" && log "Zona horaria: $TIMEZONE" || warn "No se pudo fijar la zona horaria $TIMEZONE"
    else
        log "Zona horaria ya es $TIMEZONE."
    fi
fi

# --- Actualizar repositorios ---
run "Actualizar lista de paquetes" sudo apt-get update -y

# --- Dependencias base de compilación ---
run "Instalar dependencias de compilación" sudo apt-get install -y \
    git cmake make gcc g++ clang \
    libssl-dev libbz2-dev libreadline-dev libncurses-dev \
    libboost-all-dev lsb-release gnupg wget curl screen unzip p7zip-full

# --- Instalar MySQL 8.4 LTS ---
header "Instalando MySQL 8.4 LTS"

MYSQL_APT_CONFIG_VERSION="0.8.36-1"
MYSQL_DEB="mysql-apt-config_${MYSQL_APT_CONFIG_VERSION}_all.deb"

if systemctl is-active --quiet mysql 2>/dev/null; then
    warn "MySQL ya está activo. Saltando instalación."
else
    # El wget iba suelto: si fallaba (404 cuando Oracle retira la version, o
    # VM sin salida a internet) el script continuaba y reventaba mas tarde en
    # dpkg con un error incomprensible. Ahora aborta aqui con un mensaje claro.
    run "Descargar paquete de configuracion MySQL" \
        wget -q "https://dev.mysql.com/get/${MYSQL_DEB}" -O "/tmp/${MYSQL_DEB}"
    run "Configurar repositorio MySQL" sudo DEBIAN_FRONTEND="noninteractive" dpkg -i "/tmp/${MYSQL_DEB}"
    rm -f "/tmp/${MYSQL_DEB}"

    run "Actualizar repositorios tras añadir MySQL" sudo apt-get update -y
    run "Instalar MySQL Server" sudo DEBIAN_FRONTEND="noninteractive" apt-get install -y \
        mysql-server

    run "Habilitar MySQL al arranque" sudo systemctl enable mysql
    run "Iniciar MySQL" sudo systemctl start mysql
fi

# --- Cabeceras de desarrollo de MySQL ---------------------------------------
# SIEMPRE, aunque el servidor ya estuviera instalado. Antes esto vivia dentro
# del bloque de arriba, asi que en una VM que ya traia MySQL/MariaDB la fase 1
# terminaba con un OK y era la fase 4 la que moria al compilar sin cabeceras.
if dpkg -s libmysqlclient-dev &>/dev/null || dpkg -s libmariadb-dev-compat &>/dev/null; then
    log "Cabeceras de desarrollo de MySQL ya presentes."
else
    run "Instalar cabeceras de desarrollo de MySQL" \
        sudo DEBIAN_FRONTEND="noninteractive" apt-get install -y libmysqlclient-dev
fi

# --- Verificar versiones mínimas ---
header "Verificando versiones"

# El `|| true` evita que un binario ausente (o el SIGPIPE de head) aborte la
# fase por culpa de `set -o pipefail`: esto es solo informativo.
CLANG_VER=$(clang --version 2>/dev/null | head -1 || true)
CMAKE_VER=$(cmake --version 2>/dev/null | head -1 || true)
OPENSSL_VER=$(openssl version 2>/dev/null || true)
MYSQL_VER=$(mysql --version 2>/dev/null || true)

info "Clang:   $CLANG_VER"
info "CMake:   $CMAKE_VER"
info "OpenSSL: $OPENSSL_VER"
info "MySQL:   $MYSQL_VER"

log "Dependencias instaladas correctamente."
