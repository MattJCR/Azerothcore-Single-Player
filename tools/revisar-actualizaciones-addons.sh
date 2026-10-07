#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  tools/revisar-actualizaciones-addons.sh — ¿hay commits nuevos en los addons
#  de cliente (WoW Lua) de terceros que llevamos vendorizados y traducidos en
#  cliente/Interface/AddOns/<Nombre>/?
#
#  A diferencia de tools/revisar-actualizaciones.sh (módulos de servidor, que
#  viven clonados en la VM), estos addons viven copiados dentro de este mismo
#  repo. No hace falta SSH ni clonar nada: sólo `git ls-remote` contra GitHub
#  (y, para el catálogo de abajo, la API pública de GitHub por HTTPS).
#  NO TOCA NADA localmente ni en la VM.
#
#  También comprueba el catálogo grande de web-panel/addons/catalog.json
#  (~150 addons de terceros mirroreados en bloque desde un único repositorio,
#  NoM0Re/WoW-3.3.5a-Addons; ver web-panel/addons/README.md). MultiBot y
#  ServerHelp son propios (sourceUrl null) y quedan fuera de esa comprobación;
#  MultiBot en particular NO se actualiza nunca sin tarea previa.
#
#  Este script es para leer con detalle a mano (tabla + --log/--log-catalog).
#  El resumen sin detalle también llega dentro del juego: lib/addon-versions.sh
#  fusiona el mismo dato en el aviso de mod-update-notice (ver
#  tools/revisar-actualizaciones.sh y lib/check-updates.sh).
#
#  Uso:
#      bash tools/revisar-actualizaciones-addons.sh
#      bash tools/revisar-actualizaciones-addons.sh --log PlayerBotManager
#      bash tools/revisar-actualizaciones-addons.sh --log-catalog
#
#  Para actualizar de verdad un addon tras revisar los cambios, ver
#  patches-cliente/README.md (clonar en el commit nuevo, reaplicar la
#  traducción guardada en patches-cliente/<Nombre>/, regenerar el .patch,
#  actualizar el commit en addons.lock, y desplegar). Para el catálogo grande,
#  ver web-panel/addons/README.md (npm run sync:addons).
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOCK="$ROOT/addons.lock"
CATALOG="$ROOT/web-panel/addons/catalog.json"

if [ ! -f "$LOCK" ]; then
    echo "No existe $LOCK"
    exit 1
fi

if [ "${1:-}" = "--log-catalog" ]; then
    [ -f "$CATALOG" ] || { echo "No existe $CATALOG"; exit 1; }
    CATALOG_URL="$(grep -m1 '"sourceUrl"' "$CATALOG" | sed -E 's/.*"sourceUrl": *"([^"]*)".*/\1/')"
    CATALOG_COMMIT="$(grep -m1 '"sourceCommit"' "$CATALOG" | sed -E 's/.*"sourceCommit": *"([^"]*)".*/\1/')"
    REPO_PATH="${CATALOG_URL#https://github.com/}"
    REMOTE="$(git ls-remote "$CATALOG_URL" HEAD 2>/dev/null | awk '{print $1}')"
    [ -n "$REMOTE" ] || { echo "Sin acceso a $CATALOG_URL"; exit 1; }
    if [ "$REMOTE" = "$CATALOG_COMMIT" ]; then
        echo "El catálogo sigue en $CATALOG_COMMIT: sin novedades en $CATALOG_URL."
        exit 0
    fi
    echo "Cambios en $REPO_PATH desde ${CATALOG_COMMIT:0:12} hasta ${REMOTE:0:12} (vía API de GitHub, sin clonar):"
    echo ""
    COMPARE_URL="https://api.github.com/repos/$REPO_PATH/compare/${CATALOG_COMMIT}...${REMOTE}"
    if command -v node >/dev/null 2>&1; then
        # Se usa fetch() de Node directamente (en vez de curl a un fichero temporal)
        # porque en Windows las rutas /tmp de bash y de Node no son la misma ruta.
        node -e "
            fetch('$COMPARE_URL').then((r) => r.json()).then((d) => {
                const files = (d.files || []).filter((f) => f.filename.startsWith('src/Addons/'));
                console.log('  ' + (d.total_commits ?? '?') + ' commit(s) nuevos en el repositorio; ' + files.length + ' archivo(s) tocados dentro de src/Addons/ (lo que de verdad afecta a los addons empaquetados):');
                console.log('');
                for (const f of files) console.log('    ' + f.status.padEnd(10) + f.filename);
                if (!files.length) console.log('    (ninguno: los commits nuevos sólo tocan metadatos/documentación del repositorio, no paquetes de addons)');
            }).catch((e) => { console.error('  No se pudo interpretar la respuesta de la API:', e.message); process.exit(1); });
        " || echo "  Revisar a mano: https://github.com/$REPO_PATH/compare/${CATALOG_COMMIT}...${REMOTE}"
    else
        echo "  (node no disponible para listar los archivos; revisar a mano: https://github.com/$REPO_PATH/compare/${CATALOG_COMMIT}...${REMOTE})"
    fi
    echo ""
    echo "  Antes de re-sincronizar: web-panel/addons/README.md (npm run sync:addons -- /ruta/repo-clonado)."
    exit 0
fi

LOG_OF="${2:-}"
if [ "${1:-}" = "--log" ]; then
    LOG_OF="${2:?'--log requiere el nombre de un addon'}"
fi

echo "Comprobando ${LOCK##*/} contra GitHub (sólo git ls-remote, no se toca nada)..."
printf '\n  %-20s %-10s   %-10s   %s\n' "addon" "fijado" "remoto" "estado"
printf '  %-20s %-10s   %-10s   %s\n' "--------------------" "----------" "----------" "------"

HAS_UPDATES=0
while IFS=$'\t' read -r NAME BRANCH COMMIT DATE URL; do
    [ -z "${NAME:-}" ] && continue
    case "$NAME" in \#*) continue ;; esac

    REMOTE="$(git ls-remote "$URL" "refs/heads/$BRANCH" 2>/dev/null | awk '{print $1}')"
    if [ -z "$REMOTE" ]; then
        printf '  %-20s %-10s   %-10s   %s\n' "$NAME" "${COMMIT:0:10}" "?" "sin acceso a $URL"
        continue
    fi

    if [ "$REMOTE" = "$COMMIT" ]; then
        printf '  %-20s %-10s   %-10s   %s\n' "$NAME" "${COMMIT:0:10}" "${REMOTE:0:10}" "al dia"
    else
        HAS_UPDATES=$((HAS_UPDATES + 1))
        printf '  %-20s %-10s   %-10s   %s\n' "$NAME" "${COMMIT:0:10}" "${REMOTE:0:10}" "ACTUALIZACION DISPONIBLE"

        if [ "$NAME" = "$LOG_OF" ]; then
            echo ""
            echo "  Commits nuevos en $NAME (clonando superficialmente para listar el log):"
            TMP="$(mktemp -d)"
            if git clone --quiet --branch "$BRANCH" "$URL" "$TMP" 2>/dev/null; then
                git -C "$TMP" log --oneline --date=short --format='    %ad %h %s' "${COMMIT}..HEAD" 2>/dev/null \
                    || echo "    (el commit fijado ya no existe en el historial remoto; puede que se haya reescrito la rama)"
            else
                echo "    No se pudo clonar $URL"
            fi
            rm -rf "$TMP"
            echo ""
        fi
    fi
done < <(grep -v '^\s*#' "$LOCK" | grep -v '^\s*$')

# ── Catálogo grande (~150 addons mirroreados en bloque desde un solo repo) ───
if [ -f "$CATALOG" ]; then
    CATALOG_URL="$(grep -m1 '"sourceUrl"' "$CATALOG" | sed -E 's/.*"sourceUrl": *"([^"]*)".*/\1/')"
    CATALOG_COMMIT="$(grep -m1 '"sourceCommit"' "$CATALOG" | sed -E 's/.*"sourceCommit": *"([^"]*)".*/\1/')"
    CATALOG_REMOTE="$(git ls-remote "$CATALOG_URL" HEAD 2>/dev/null | awk '{print $1}')"
    if [ -z "$CATALOG_REMOTE" ]; then
        printf '  %-20s %-10s   %-10s   %s\n' "(catálogo NoM0Re)" "${CATALOG_COMMIT:0:10}" "?" "sin acceso a $CATALOG_URL"
    elif [ "$CATALOG_REMOTE" = "$CATALOG_COMMIT" ]; then
        printf '  %-20s %-10s   %-10s   %s\n' "(catálogo NoM0Re)" "${CATALOG_COMMIT:0:10}" "${CATALOG_REMOTE:0:10}" "al dia"
    else
        HAS_UPDATES=$((HAS_UPDATES + 1))
        printf '  %-20s %-10s   %-10s   %s\n' "(catálogo NoM0Re)" "${CATALOG_COMMIT:0:10}" "${CATALOG_REMOTE:0:10}" "ACTUALIZACION DISPONIBLE"
    fi
fi

echo ""
if [ "$HAS_UPDATES" -gt 0 ]; then
    echo "  $HAS_UPDATES addon(s)/catálogo con commits nuevos."
    echo "  Ver el log de uno:       bash tools/revisar-actualizaciones-addons.sh --log <nombre>"
    echo "  Ver qué cambió del catálogo: bash tools/revisar-actualizaciones-addons.sh --log-catalog"
    echo "  Antes de actualizar, leer patches-cliente/README.md (hay traducción propia que reaplicar)."
else
    echo "  Todo al día."
fi
