#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Comprobaciones tras instalar o actualizar. Uso: bash tools/verificar-instalacion.sh
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$SCRIPT_DIR/config.sh"
AC="$AC_DIR"
WS=$AC/env/dist/etc/worldserver.conf
PB=$AC/env/dist/etc/modules/playerbots.conf
LOCK="$SCRIPT_DIR/versions.lock"

echo "=== 1. commits instalados vs versions.lock ==="
while IFS=$'\t' read -r NAME BRANCH COMMIT DATE URL; do
    case "$NAME" in ''|\#*) continue ;; esac
    if [ "$NAME" = core ]; then D=$AC
    elif [ -d "$AC/modules/$NAME/.git" ]; then D=$AC/modules/$NAME
    elif [ -d "$AC/extras/$NAME/.git" ]; then D=$AC/extras/$NAME
    elif [ -d "$AC/modules-disabled/$NAME/.git" ]; then D=$AC/modules-disabled/$NAME
    else printf "  %-28s NO INSTALADO\n" "$NAME"; continue; fi
    CUR=$(git -C "$D" rev-parse HEAD 2>/dev/null)
    if [ "$CUR" = "$COMMIT" ]; then printf "  %-28s OK   %s\n" "$NAME" "${CUR:0:12}"
    else printf "  %-28s ¡DIFIERE!  lock=%s  real=%s\n" "$NAME" "${COMMIT:0:12}" "${CUR:0:12}"; fi
done < "$LOCK"

echo "=== 2. las claves que estaban rotas ==="
for k in "MapUpdate.Threads" "AllowTwoSide.Interaction.Chat" "AllowTwoSide.Interaction.Channel" "AllowTwoSide.Interaction.Auction"; do
    printf "  %-36s %s\n" "$k" "$(grep -E "^${k//./\.} *=" "$WS" 2>/dev/null | tail -1)"
done
echo "  claves fantasma que ya NO deben aparecer:"
grep -nE "^(MapUpdateThreadCount|AllowTwoSide\.(Interaction\.(Trade|Mail)|WhoList|AddFriend))" "$WS" 2>/dev/null \
    && echo "    ¡SIGUEN AHI!" || echo "    ninguna, correcto"

echo "=== 3. level brackets ==="
grep -E "^AiPlayerbot\.(LevelBrackets\.(Enabled|Dynamic\.(UseDynamicDistribution|RealPlayerWeight))|SyncLevelWithPlayers)" "$PB" 2>/dev/null | sed 's/^/  /'

echo "=== 4. avisos de claves no declaradas durante la instalacion ==="
grep -a "NO declarada\|NO declaradas" ${INSTALL_LOG_FILE:-$HOME/instalacion.log} 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | sed 's/^/  /' | head -20 || echo "  ninguno"

echo "=== 5. modulos compilados ==="
ls "$AC/modules" 2>/dev/null | tr '\n' ' '; echo
echo "  .conf de modulos: $(ls "$AC/env/dist/etc/modules"/*.conf 2>/dev/null | wc -l)"

echo "=== 6. parches aplicados ==="
# Las fases escriben en ~/azerothcore/logs/install.log (lib/utils.sh); el
# ~/instalacion.log de antes ya no existe y esta sección salía vacía.
grep -a "parche aplicado\|ya estaba aplicado\|no aplica" ${INSTALL_LOG_FILE:-$HOME/azerothcore/logs/install.log} 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -iv "^$" | tail -40 | sed 's/^/  /'

echo "=== 7. servicios ==="
systemctl is-active mysql ac-authserver ac-worldserver 2>/dev/null | tr '\n' ' '; echo
if systemctl list-unit-files ac-worldserver.socket >/dev/null 2>&1 && systemctl is-enabled ac-worldserver.socket >/dev/null 2>&1; then
    echo "  modo en espera: ac-worldserver.socket $(systemctl is-active ac-worldserver.socket 2>/dev/null)" \
         "· worldserver $(systemctl is-active ac-worldserver 2>/dev/null) (inactive = dormido, lo despierta la conexión)"
fi

echo "=== 8. modulos propios ==="
for m in queue-bots world-bots quest-mates party-here home-guild update-notice server-help standby adaptive-ai; do
    CONFF=$AC/env/dist/etc/modules/mod_${m/-/_}.conf
    printf "  %-14s carpeta: %-3s conf: %-3s  ultimo log: %s\n" "mod-$m" \
        "$([ -d "$AC/modules/mod-$m" ] && echo si || echo NO)" \
        "$([ -f "$CONFF" ] && echo si || echo NO)" \
        "$(grep -a "\[$m\]" "$AC/env/dist/bin/Server.log" 2>/dev/null | tail -1 | cut -c1-110)"
done
echo "  (sin jugador conectado no hay lineas de log: los dos trabajan solo cuando hay alguien)"
