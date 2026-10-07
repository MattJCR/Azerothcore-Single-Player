#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  tools/revisar-actualizaciones.sh — ¿qué repositorio remoto tiene novedades?
#
#  Compara cada repositorio instalado (core, módulos, mod-arac) contra el
#  commit fijado en versions.lock y enseña una tabla: cuántos commits nuevos
#  hay río arriba, de qué fecha es lo fijado y de qué fecha lo último. NO
#  TOCA NADA en el servidor: sólo hace `git fetch`.
#
#  Además escribe el fichero que lee mod-update-notice, el módulo que enseña
#  el aviso a las cuentas GM al conectarse (env/dist/bin/updates-pending.txt),
#  fusionado con las novedades del catálogo de addons de cliente (addons.lock +
#  web-panel/addons/catalog.json, vía lib/addon-versions.sh; para verlas con
#  detalle propio: tools/revisar-actualizaciones-addons.sh). La tarea semanal
#  (lib/check-updates.sh) hace lo mismo y manda un correo.
#
#  Uso:
#      bash tools/revisar-actualizaciones.sh            # tabla + fichero
#      bash tools/revisar-actualizaciones.sh --solo-novedades
#      bash tools/revisar-actualizaciones.sh --sin-fichero
#      bash tools/revisar-actualizaciones.sh --log NOMBRE   # los commits nuevos de ese repo
#
#  Para actualizar de verdad, después de leer los cambios:
#      ./install.sh --only 3      # baja el código nuevo y reaplica los parches
#      sudo systemctl stop ac-worldserver
#      ./install.sh --from 4      # recompila y reconfigura
#      ./install.sh --freeze      # si todo va bien, fija estas versiones
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALLER="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$INSTALLER/config.sh"
source "$INSTALLER/lib/utils.sh"
source "$INSTALLER/lib/versions.sh"
source "$INSTALLER/lib/addon-versions.sh"

ONLY_NEW=false
WRITE=true
LOG_OF=""
while [ $# -gt 0 ]; do
    case "$1" in
        --solo-novedades) ONLY_NEW=true ;;
        --sin-fichero)    WRITE=false ;;
        --log)            LOG_OF="${2:?'--log requiere el nombre de un repositorio'}"; shift ;;
        -h|--help)        sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "Opción desconocida: $1"; exit 1 ;;
    esac
    shift
done

if [ ! -f "$VERSIONS_LOCK" ]; then
    echo "No existe versions.lock: no hay con qué comparar. Genera uno con ./install.sh --freeze"
    exit 1
fi

# ── Los commits nuevos de un repositorio concreto ────────────────────────────
if [ -n "$LOG_OF" ]; then
    ROW=$(awk -F'\t' -v n="$LOG_OF" '$1==n {print; exit}' "$VERSIONS_LOCK")
    [ -n "$ROW" ] || { echo "'$LOG_OF' no está en versions.lock"; exit 1; }
    IFS=$'\t' read -r NAME BRANCH COMMIT DATE URL <<< "$ROW"
    if [ "$NAME" = core ]; then DIR="$AC_DIR"
    elif [ -d "$AC_DIR/modules/$NAME/.git" ]; then DIR="$AC_DIR/modules/$NAME"
    elif [ -d "$AC_DIR/extras/$NAME/.git" ]; then DIR="$AC_DIR/extras/$NAME"
    else echo "'$NAME' no está instalado"; exit 1; fi
    git -C "$DIR" fetch --quiet origin "$BRANCH"
    echo "Commits en $NAME desde ${COMMIT:0:12} ($DATE) hasta origin/$BRANCH:"
    git -C "$DIR" log --oneline --date=short --format='  %ad %h %s' "${COMMIT}..origin/${BRANCH}"
    echo ""
    echo "Ficheros tocados (los parches propios viven en patches/$NAME/, si los hay):"
    git -C "$DIR" diff --stat "${COMMIT}..origin/${BRANCH}" | tail -25
    exit 0
fi

# ── La tabla ────────────────────────────────────────────────────────────────
echo "Comprobando ${VERSIONS_LOCK##*/} contra GitHub (sólo git fetch, no se toca nada)..."
HAS_UPDATES=0
check_upstream_updates || HAS_UPDATES=$?

printf '\n  %-28s %8s   %-10s   %-10s   %s\n' "repositorio" "nuevos" "fijado" "upstream" "estado"
printf '  %-28s %8s   %-10s   %-10s   %s\n' "----------------------------" "------" "----------" "----------" "------"
for ROW in "${UPDATE_ROWS[@]}"; do
    IFS=$'\t' read -r NAME N DATE UP_DATE UP_COMMIT <<< "$ROW"
    if [ "${N:-0}" -gt 0 ]; then
        STATE="ACTUALIZACION DISPONIBLE"
    else
        STATE="al dia"
        [ "$ONLY_NEW" = true ] && continue
    fi
    printf '  %-28s %8s   %-10s   %-10s   %s\n' "$NAME" "$N" "$DATE" "$UP_DATE" "$STATE"
done
echo ""

if [ -n "${UPDATE_UNREACHABLE:-}" ]; then
    echo "  Sin acceso a: ${UPDATE_UNREACHABLE} (sin red, o repositorio movido/borrado: mirar mirrors/)."
fi

if [ "${UPDATE_COUNT:-0}" -gt 0 ]; then
    echo "  ${UPDATE_COUNT} de ${UPDATE_CHECKED} repositorios tienen commits nuevos."
    echo "  Ver qué cambió:  bash tools/revisar-actualizaciones.sh --log <nombre>"
    echo "  Antes de actualizar un módulo con parche propio (patches/<nombre>/), leer INSTALL_ES.md parte 4 §3."
else
    echo "  Todo al día: ${UPDATE_CHECKED} repositorios en el commit fijado o sin novedades."
fi

if [ "$WRITE" = true ]; then
    # Se fusiona con el catálogo de addons de cliente (tools/revisar-actualizaciones-addons.sh
    # las enseña por separado, con más detalle) para que el fichero que lee
    # mod-update-notice tenga siempre la misma forma, la corra quien la corra.
    check_addon_updates || true
    UPDATE_COUNT=$((UPDATE_COUNT + ADDON_UPDATE_COUNT))
    UPDATE_DETAILS+=("${ADDON_UPDATE_DETAILS[@]}")
    write_update_report
    echo "  Informe para mod-update-notice: $UPDATE_REPORT_FILE"
fi

exit 0
