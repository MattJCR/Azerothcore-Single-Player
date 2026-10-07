#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  apply-safe-stop.sh — (Re)instala el apagado seguro de AzerothCore
#  Ejecutar en la VM directamente: bash scripts/apply-safe-stop.sh
#
#  Antes este script llevaba su propia copia de safe-stop.sh y de la unidad
#  ac-worldserver.service, con rutas y nombre de sesión screen escritos a mano.
#  Las dos copias acabaron divergiendo de la fase 6 (mensajes distintos, orden
#  del saveall distinto) y, peor, ejecutar este script deshacía los arreglos de
#  la fase 6 reescribiendo la unidad con KillMode=none y DefaultDependencies=no.
#
#  Ahora es un envoltorio fino sobre la fase 6, que es la única dueña de la
#  unidad systemd y de safe-stop.sh. La configuración sale de config.sh.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

# =============================================================================
# Comprobaciones previas
# =============================================================================
if [ ! -d "$AC_DIR" ]; then
    error "No se encontró $AC_DIR. ¿Está AzerothCore instalado en este usuario?"
    error "Revisa AC_DIR en config.sh."
    exit 1
fi

UNIT="/etc/systemd/system/ac-worldserver.service"

clear
echo -e "${BOLD}${CYAN}"
echo "  ╔══════════════════════════════════════════════════╗"
echo "  ║     AzerothCore — Instalador de parada segura    ║"
echo "  ╚══════════════════════════════════════════════════╝"
echo -e "${NC}"
echo -e "  Usuario           : ${CYAN}$AC_SYSTEM_USER${NC}"
echo -e "  Directorio AC     : ${CYAN}$AC_DIR${NC}"
echo -e "  Sesión screen     : ${CYAN}$WS_SCREEN${NC}"
echo -e "  Script de parada  : ${CYAN}$AC_SCRIPTS_DIR/safe-stop.sh${NC}"
echo -e "  Log de parada     : ${CYAN}$AC_LOGS_DIR/safe-stop.log${NC}"
echo ""
echo -e "  Se regenerarán los servicios systemd (fase 6) con esta configuración."
echo ""
echo -e "${BOLD}¿Continuar? [s/N]${NC} "
read -r CONFIRM
[[ "$CONFIRM" =~ ^[sSyY]$ ]] || { echo "Cancelado."; exit 0; }
echo ""

# =============================================================================
# Backup de la unidad actual (si existe)
# =============================================================================
if [ -f "$UNIT" ]; then
    BACKUP="/root/ac-worldserver.service.bak.$(date +%s)"
    # En /root y no junto a la unidad: systemd ignora los ficheros que no
    # terminan en un sufijo de unidad conocido, pero dejarlos en
    # /etc/systemd/system/ solo sirve para acumular basura.
    sudo cp "$UNIT" "$BACKUP"
    log "Backup de la unidad actual en $BACKUP"
else
    warn "No existe $UNIT todavía — se creará desde cero."
fi

# =============================================================================
# Delegar en la fase 6 (única dueña de las unidades y de safe-stop.sh)
# =============================================================================
info "Ejecutando la fase 6 (servicios systemd)..."
bash "$SCRIPT_DIR/scripts/phases/06_systemd.sh"

# =============================================================================
# Verificación final
# =============================================================================
echo ""
echo -e "${BOLD}${CYAN}══════════════════════════════════════════${NC}"
echo -e "${BOLD}${CYAN}  Verificación${NC}"
echo -e "${BOLD}${CYAN}══════════════════════════════════════════${NC}"
echo ""

echo -e "${BOLD}Servicio ac-worldserver:${NC}"
sudo systemctl cat ac-worldserver \
    | grep -E "ExecStartPre|ExecStart=|ExecStop|TimeoutStop|KillMode|Restart=" \
    | while read -r LINE; do echo -e "  ${CYAN}$LINE${NC}"; done || true

echo ""
echo -e "${BOLD}Timeout del sistema:${NC}"
grep "DefaultTimeout" /etc/systemd/system.conf.d/azerothcore-shutdown.conf 2>/dev/null \
    | while read -r LINE; do echo -e "  ${CYAN}$LINE${NC}"; done || true

echo ""
echo -e "${BOLD}Scripts generados:${NC}"
for S in safe-stop.sh wait-for-mysql.sh; do
    if [ -x "$AC_SCRIPTS_DIR/$S" ]; then
        echo -e "  ${GREEN}✔${NC} $AC_SCRIPTS_DIR/$S"
    else
        echo -e "  ${RED}✘${NC} $AC_SCRIPTS_DIR/$S (no encontrado)"
    fi
done

echo ""
echo -e "${GREEN}${BOLD}✔ Instalación completada.${NC}"
echo ""
echo -e "${BOLD}Para probar sin apagar la VM:${NC}"
echo -e "  ${CYAN}sudo systemctl stop ac-worldserver${NC}"
echo -e "  ${CYAN}tail -f $AC_LOGS_DIR/safe-stop.log${NC}"
echo ""
echo -e "${BOLD}Flujo al apagar desde Proxmox:${NC}"
echo -e "  Proxmox ACPI shutdown → systemd → ExecStop → safe-stop.sh"
echo -e "  → aviso 60s → saveall → shutdown limpio → servicios parados"
echo ""
