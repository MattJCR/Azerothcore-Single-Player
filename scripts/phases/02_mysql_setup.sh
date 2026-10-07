#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  02_mysql_setup.sh — Configura MySQL: usuario, bases de datos y optimizaciones
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

header "FASE 2 — Configurar MySQL"

# Construir el comando de acceso root
if [ -n "$MYSQL_ROOT_PASS" ]; then
    MYSQL_ROOT="mysql -u root -p${MYSQL_ROOT_PASS}"
else
    MYSQL_ROOT="sudo mysql -u root"
fi

# --- Verificar que MySQL está activo ---
if ! systemctl is-active --quiet mysql 2>/dev/null; then
    run "Iniciar MySQL" sudo systemctl start mysql
fi

# --- Verificar que el motor es el soportado ---
require_mysql8 $MYSQL_ROOT

# --- Verificar si el usuario ya existe y funciona ---
if check_mysql_connection "$AC_DB_USER" "$AC_DB_PASS"; then
    warn "El usuario MySQL '${AC_DB_USER}' ya existe y tiene acceso. Saltando creación."
else
    info "Creando usuario MySQL '${AC_DB_USER}'..."
    # El `if [ $? -ne 0 ]` de antes no llegaba a evaluarse con `set -e`:
    # el heredoc va ahora dentro del propio `if`.
    #
    # Privilegios acotados a acore_*: antes era GRANT ALL ON *.* WITH GRANT
    # OPTION, es decir, el usuario del juego podia leer y reescribir mysql.user.
    if $MYSQL_ROOT >> "$INSTALL_LOG" 2>&1 << SQL
DROP USER IF EXISTS '${AC_DB_USER}'@'localhost';
DROP USER IF EXISTS '${AC_DB_USER}'@'127.0.0.1';
CREATE USER '${AC_DB_USER}'@'localhost' IDENTIFIED BY '${AC_DB_PASS}';
CREATE USER '${AC_DB_USER}'@'127.0.0.1' IDENTIFIED BY '${AC_DB_PASS}';
GRANT ALL PRIVILEGES ON \`acore\_%\`.* TO '${AC_DB_USER}'@'localhost';
GRANT ALL PRIVILEGES ON \`acore\_%\`.* TO '${AC_DB_USER}'@'127.0.0.1';
GRANT PROCESS ON *.* TO '${AC_DB_USER}'@'localhost';
GRANT PROCESS ON *.* TO '${AC_DB_USER}'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
    then
        log "Usuario MySQL '${AC_DB_USER}' creado (privilegios acotados a acore_*)."
    else
        error "Error creando el usuario MySQL. Revisa: $INSTALL_LOG"
        exit 1
    fi
fi

# --- Verificar acceso tras creación ---
if ! check_mysql_connection "$AC_DB_USER" "$AC_DB_PASS"; then
    error "No se puede conectar a MySQL con el usuario '${AC_DB_USER}'."
    error "Comprueba la contraseña en config.sh (AC_DB_PASS) o los permisos de MySQL."
    exit 1
fi
log "Conexión MySQL verificada correctamente."

# --- Base de datos de Playerbots ---
if [ "$INSTALL_MOD_PLAYERBOTS" = true ]; then
    if db_exists acore_playerbots; then
        warn "La base de datos 'acore_playerbots' ya existe. Saltando."
    else
        info "Creando base de datos 'acore_playerbots'..."
        $MYSQL_ROOT << SQL >> "$INSTALL_LOG" 2>&1
CREATE DATABASE IF NOT EXISTS acore_playerbots CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
GRANT ALL PRIVILEGES ON acore_playerbots.* TO '${AC_DB_USER}'@'localhost';
GRANT ALL PRIVILEGES ON acore_playerbots.* TO '${AC_DB_USER}'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
        log "Base de datos 'acore_playerbots' creada."
    fi
fi

# --- Optimizaciones de MySQL ---
header "Optimizando MySQL"

MYCNF="/etc/mysql/mysql.conf.d/mysqld.cnf"
if [ ! -f "$MYCNF" ]; then
    # Debian/Ubuntu alternativo
    MYCNF="/etc/mysql/conf.d/azerothcore.cnf"
fi

if grep -q "AzerothCore optimizations" "$MYCNF" 2>/dev/null; then
    warn "Optimizaciones de MySQL ya aplicadas. Saltando."
else
    sudo tee -a "$MYCNF" > /dev/null << MYCNF_BLOCK

# ── AzerothCore optimizations ─────────────────────────────────────────────────
skip-log-bin
innodb_buffer_pool_size         = ${MYSQL_BUFFER_POOL_GB}G
innodb_buffer_pool_instances    = 4
innodb_io_capacity              = 2000
innodb_io_capacity_max          = 4000
innodb_log_buffer_size          = 32M
innodb_flush_log_at_trx_commit  = 2
transaction_isolation           = READ-COMMITTED
# ──────────────────────────────────────────────────────────────────────────────
MYCNF_BLOCK
    run "Reiniciar MySQL con nueva configuración" sudo systemctl restart mysql

    # Verificar que MySQL volvió a arrancar correctamente
    sleep 2
    if ! check_mysql_connection "$AC_DB_USER" "$AC_DB_PASS"; then
        error "MySQL no arrancó correctamente tras las optimizaciones."
        error "Revisa $MYCNF y $INSTALL_LOG"
        exit 1
    fi
    log "Optimizaciones MySQL aplicadas y verificadas."
fi

log "MySQL configurado correctamente."
