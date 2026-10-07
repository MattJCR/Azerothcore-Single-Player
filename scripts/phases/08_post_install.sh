#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  08_post_install.sh — Pasos post-instalación: realmlist, cuenta GM
#                       Ejecutar DESPUÉS del primer arranque del worldserver
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"

header "FASE 8 — Post-instalación"

# =============================================================================
# Verificar que la BD ya fue creada (worldserver ya arrancó al menos una vez)
# =============================================================================
if ! db_exists acore_auth; then
    error "La base de datos 'acore_auth' no existe todavía."
    error "Primero debes arrancar el worldserver al menos una vez para que cree las BBDs:"
    error ""
    error "  cd $AC_DIR/env/dist/bin"
    error "  ./worldserver"
    error ""
    error "Responde 'yes' a cada pregunta de creación de BD, espera a ver 'World initialized',"
    error "y luego vuelve a ejecutar: ./install.sh --only 8"
    exit 1
fi

# =============================================================================
# Actualizar realmlist
# =============================================================================
header "Actualizando realmlist"

CURRENT_IP=$(mysql -u "$AC_DB_USER" -p"$AC_DB_PASS" acore_auth \
    -se "SELECT address FROM realmlist WHERE id=1;" 2>/dev/null || echo "desconocida")

info "IP actual en realmlist: $CURRENT_IP"
info "IP configurada en config.sh: $REALM_IP"

if mysql -u "$AC_DB_USER" -p"$AC_DB_PASS" acore_auth >> "$INSTALL_LOG" 2>&1 << SQL
UPDATE realmlist SET
    address  = '${REALM_IP}',
    name     = '${REALM_NAME}',
    gamebuild = 12340
WHERE id = 1;
SQL
then
    log "Realmlist actualizado: IP=${REALM_IP} | Nombre='${REALM_NAME}'"
else
    error "Error actualizando el realmlist. Revisa: $INSTALL_LOG"
fi

# =============================================================================
# Cuenta de administrador
#
# Ya no se imprimen instrucciones para que las escribas a mano: la cuenta se
# crea aquí, calculando el verificador SRP6 igual que hace el core (ver
# lib/srp6.py). No hace falta que el worldserver esté arrancado.
# =============================================================================
header "Cuenta de administrador"

if [ "${CREATE_ADMIN_ACCOUNT:-false}" = true ]; then
    if create_admin_account; then
        echo ""
        echo -e "  Usuario:    ${CYAN}${ADMIN_ACCOUNT_NAME}${NC}"
        echo -e "  Contraseña: ${CYAN}${ADMIN_ACCOUNT_PASS}${NC}"
        echo -e "  Nivel GM:   ${CYAN}${ADMIN_ACCOUNT_GMLEVEL}${NC} (en todos los reinos)"
        echo -e "  Personajes: ${CYAN}ninguno${NC} — créalos tú desde el cliente"
        echo ""
    fi
else
    info "CREATE_ADMIN_ACCOUNT=false: no se crea ninguna cuenta."
    echo -e "  Para crearla a mano, en la consola del worldserver:"
    echo -e "    ${CYAN}account create <USUARIO> <CONTRASEÑA>${NC}"
    echo -e "    ${CYAN}account set gmlevel <USUARIO> 3 -1${NC}"
    echo ""
fi

# =============================================================================
# Resumen del estado actual
# =============================================================================
header "Estado del servidor"

echo -e "${BOLD}Bases de datos:${NC}"
mysql -u "$AC_DB_USER" -p"$AC_DB_PASS" \
    -e "SHOW DATABASES LIKE 'acore_%';" 2>/dev/null | grep acore | while read -r DB; do
    echo -e "  ${GREEN}OK${NC} $DB"
done || true

echo ""
echo -e "${BOLD}Módulos configurados en etc/modules/:${NC}"
ls "$AC_DIR/env/dist/etc/modules/"*.conf 2>/dev/null | while read -r CONF; do
    echo -e "  ${GREEN}OK${NC} $(basename "$CONF")"
done || true

echo ""
echo -e "${BOLD}Realmlist:${NC}"
mysql -u "$AC_DB_USER" -p"$AC_DB_PASS" acore_auth \
    -e "SELECT id, name, address, port FROM realmlist;" 2>/dev/null

echo ""

# =============================================================================
# Reintentos: todo lo que la fase 5 no pudo hacer porque las BBDD aún no
# existían. Es idempotente, así que ejecutarlo otra vez no rompe nada.
# =============================================================================
header "Contenido pendiente de la fase 5"

if [ "${INSTALL_MOD_GUILDHOUSE:-false}" = true ]; then
    if [ -f "$AC_DIR/env/dist/bin/dbc/Spell.dbc" ]; then
        python3 "$INSTALLER_DIR/tools/piedra_sede_dbc.py" \
            "$AC_DIR/env/dist/bin/dbc/Spell.dbc" "$AC_DIR/env/dist/bin/dbc/Spell.dbc"
    else
        error "[mod-guildhouse] falta Spell.dbc: no se puede instalar la piedra de la sede."
        exit 1
    fi
fi

if [ "$INSTALL_MOD_INDIVIDUAL_PROGRESSION" = true ]; then
    apply_ip_optional_sql || true
fi

# SQL de los módulos de contenido añadidos. El core también los aplica solo al
# arrancar, así que esto es sólo una red de seguridad (y es idempotente).
POST_MODULES=(
    "mod-challenge-modes:$INSTALL_MOD_CHALLENGE_MODES"
    "mod-instanced-worldbosses:$INSTALL_MOD_INSTANCED_WORLDBOSSES"
    "mod-war-effort:$INSTALL_MOD_WAR_EFFORT"
    "mod-racial-trait-swap:$INSTALL_MOD_RACIAL_TRAIT_SWAP"
    "mod-reagent-bank:$INSTALL_MOD_REAGENT_BANK"
    "mod-aoe-loot:$INSTALL_MOD_AOE_LOOT"
    "mod-quest-loot-party:${INSTALL_MOD_QUEST_LOOT_PARTY:-false}"
    "mod-instance-reset:$INSTALL_MOD_INSTANCE_RESET"
    "mod-1v1-arena:$INSTALL_MOD_1V1_ARENA"
    "mod-guildhouse:${INSTALL_MOD_GUILDHOUSE:-false}"
)
for ENTRY in "${POST_MODULES[@]}"; do
    MOD_NAME="${ENTRY%%:*}"
    [ "${ENTRY##*:}" = true ] || continue
    [ -d "$AC_DIR/modules/$MOD_NAME/data/sql/db-world" ] || continue
    apply_sql_dir "$MOD_NAME" acore_world "$AC_DIR/modules/$MOD_NAME/data/sql/db-world"
    if [ -d "$AC_DIR/modules/$MOD_NAME/data/sql/db-characters" ] && db_exists acore_characters; then
        apply_sql_dir "$MOD_NAME" acore_characters "$AC_DIR/modules/$MOD_NAME/data/sql/db-characters"
    fi
done

# mod-treasure (SP03): catálogo y plantillas idempotentes.
if [ "${INSTALL_MOD_TREASURE:-false}" = true ] \
   && [ -d "$AC_DIR/modules/mod-treasure/data/sql/db-world" ]; then
    apply_sql_dir "mod-treasure (SP03)" acore_world \
        "$AC_DIR/modules/mod-treasure/data/sql/db-world"
fi

# mod-progression-skip (módulo propio): el NPC "Cronista de las Eras". Necesita
# mod-individual-progression. Red de seguridad idempotente, igual que arriba.
if [ "${INSTALL_MOD_PROGRESSION_SKIP:-false}" = true ] && [ "${INSTALL_MOD_INDIVIDUAL_PROGRESSION:-false}" = true ] \
   && [ -d "$AC_DIR/modules/mod-progression-skip/data/sql/db-world" ]; then
    apply_sql_dir "mod-progression-skip (NPC Cronista)" acore_world \
        "$AC_DIR/modules/mod-progression-skip/data/sql/db-world"
fi

# Objetos propios (item_template, entry >= 600000): todos los .sql de
# patches/custom-items/. ANTES de la tabla de recompensas, que referencia alguno.
if [ -d "$PATCHES_DIR/custom-items" ]; then
    apply_sql_dir "objetos propios" acore_world "$PATCHES_DIR/custom-items"
fi

# Tabla de recompensas por nivel (se aplica DESPUÉS del SQL del módulo, que
# inserta sus filas de ejemplo).
if [ "$INSTALL_MOD_CONGRATS_ON_LEVEL" = true ]; then
    if [ -f "$INSTALLER_DIR/congrats_on_level_rewards.sql" ]; then
        if mysql -u "$AC_DB_USER" -p"$AC_DB_PASS" acore_world \
             < "$INSTALLER_DIR/congrats_on_level_rewards.sql" >> "$INSTALL_LOG" 2>&1; then
            log "[recompensas] Tabla de recompensas por nivel aplicada."
        else
            warn "[recompensas] Falló la aplicación — revisa $INSTALL_LOG"
        fi
    fi
fi

if [ "${LOCALE_ES:-false}" = true ]; then
    apply_sql_dir "traducciones es" acore_world "$PATCHES_DIR/locales-es"

    # Lo que dicen los bots por el chat: la fase 5 lo difiere aquí si
    # acore_playerbots todavía no tenía sus tablas (instalación desde cero).
    # A estas alturas el primer arranque ya las creó.
    if [ "$INSTALL_MOD_PLAYERBOTS" = true ]; then
        apply_sql_dir "bots en español" acore_playerbots "$PATCHES_DIR/locales-es-playerbots"
    fi
fi

# Los NPC de servicio van AL FINAL: necesitan que las plantillas de los módulos
# (creature_template) ya estén en la base de datos, y eso lo hace el core al
# arrancar por primera vez.
if [ "${SPAWN_SERVICE_NPCS:-false}" = true ]; then
    spawn_service_npcs || true
fi

# =============================================================================
# Pasos manuales restantes
# =============================================================================
header "Pasos manuales pendientes"

echo -e "  ${YELLOW}1.${NC} ${GREEN}Cuenta de administrador creada automáticamente${NC} (ver arriba)"
echo ""

if [ "$INSTALL_MOD_TRANSMOG" = true ]; then
    echo -e "  ${YELLOW}2.${NC} ${BOLD}mod-transmog${NC} — ${GREEN}NPC colocado automáticamente${NC}"
    echo -e "     Está en Ventormenta y en Orgrimmar, junto al posadero."
    echo -e "     Si prefieres ponerlo tú donde quieras: ${CYAN}SPAWN_SERVICE_NPCS=false${NC}"
    echo -e "     en config.sh y colócalo con ${CYAN}.npc add 190010${NC}"
    echo ""
fi

if [ "$INSTALL_MOD_AH_BOT_PLUS" = true ]; then
    # Ya no hay pasos manuales: creamos (o reutilizamos) la cuenta de servicio y
    # los personajes vendedores y escribimos sus GUID en el conf. Esto es el
    # reintento para el caso de que en la fase 5 las BBDD aún no existieran.
    AHBOT_CONF="$AC_DIR/env/dist/etc/modules/mod_ahbot.conf"
    if [ -f "$AHBOT_CONF" ]; then
        if setup_ah_bot_characters; then
            set_conf_value "$AHBOT_CONF" "AuctionHouseBot.GUIDs"        "$AH_BOT_GUIDS"
            set_conf_value "$AHBOT_CONF" "AuctionHouseBot.EnableSeller" "true"
            flush_conf_files   # set_conf_value() sólo encola
            log "[mod-ah-bot-plus] Bot de subastas configurado (GUIDs = $AH_BOT_GUIDS)."
            echo -e "  ${YELLOW}3.${NC} ${BOLD}mod-ah-bot-plus${NC} — ${GREEN}configurado automáticamente${NC}:"
            echo -e "     Vendedores: ${CYAN}${AH_BOT_CHAR_ALLIANCE}${NC} (Alianza) y ${CYAN}${AH_BOT_CHAR_HORDE}${NC} (Horda)"
            echo -e "     La AH se llena sola a razón de ${CYAN}${AH_ITEMS_PER_CYCLE}${NC} items por ciclo (tarda unas horas)."
            echo -e "     Para llenarla ya, con tu GM dentro del juego: ${CYAN}.ahbot update${NC} (repite 5-10 veces)"
            echo ""
        else
            warn "[mod-ah-bot-plus] No se pudieron crear los vendedores — revisa el log."
        fi
    else
        warn "[mod-ah-bot-plus] No existe $AHBOT_CONF — ¿se completó la fase 5?"
    fi
fi

if [ "$INSTALL_MOD_PLAYERBOTS" = true ]; then
    echo -e "  ${YELLOW}4.${NC} ${BOLD}mod-playerbots${NC} — Addon de control (opcional pero recomendado):"
    echo -e "     En la carpeta ${CYAN}cliente/Interface/AddOns/MultiBot${NC} del instalador (copiar a WoW/Interface/AddOns/)."
    echo -e "     Alternativa: ${CYAN}https://github.com/liyunfan1223/unbot-addon${NC}"
    echo ""
fi

# DBC del servidor de IP y ARAC: red de seguridad idempotente (la copia normal
# va al final de la fase 5). Si cambia algo con el worldserver ya en marcha,
# hay que reiniciarlo: los DBC sólo se leen al arrancar.
SERVER_DBC_OK=true
if apply_server_dbc_overrides; then
    if [ "$SERVER_DBC_CHANGED" -gt 0 ] && systemctl is-active --quiet ac-worldserver 2>/dev/null; then
        warn "Se actualizaron $SERVER_DBC_CHANGED DBC del servidor con el worldserver en marcha: reinícialo"
        warn "  (sudo systemctl restart ac-worldserver) para que los cargue."
    fi
else
    SERVER_DBC_OK=false
fi

if [ "$INSTALL_MOD_ARAC" = true ]; then
    # Reintento idempotente: si en la fase 5 acore_world no existía todavía,
    # el SQL del módulo y nuestros parches de razas se aplican ahora.
    if db_exists acore_world; then
        apply_sql_dir "mod-arac" acore_world "$AC_DIR/extras/mod-arac/data/sql/db-world"
        apply_sql_dir "mod-arac (parches de razas)" acore_world "$PATCHES_DIR/arac"
    fi

    echo -e "  ${YELLOW}5.${NC} ${BOLD}mod-arac${NC} — All Races All Classes:"
    if [ "$SERVER_DBC_OK" = true ]; then
        echo -e "     El SQL y los DBC del servidor están aplicados (comprobados byte a byte)."
    else
        echo -e "     ${RED}Los DBC del servidor NO están aplicados${NC} (ver avisos de arriba): sin ellos"
        echo -e "     no hay razas y clases cruzadas. Corrige la causa y ejecuta ${CYAN}./install.sh --only 5${NC}."
    fi
    echo -e "     Paso de CLIENTE (vive en cada PC jugador):"
    echo -e "        ${CYAN}cliente/Data/<idioma>/patch-<idioma>-4.MPQ${NC} (del instalador)  →  ${CYAN}WoW/Data/<idioma>/${NC}"
    echo -e "        (lleva fundidos los mismos DBC que extras/mod-arac/patch-contents/DBFilesContent/;"
    echo -e "        en Windows: cliente/instalar-cliente.ps1)"
    echo ""
fi

if [ "$INSTALL_MOD_CONGRATS_ON_LEVEL" = true ]; then
    # Si en la fase 5 acore_world todavía no existía, el SQL no se pudo aplicar.
    # Lo reintentamos aquí ahora que la BD ya está creada (idempotente).
    COL_SQL_DIR="$AC_DIR/modules/mod-congrats-on-level/data/sql/db-world"
    apply_sql_dir "mod-congrats-on-level" acore_world "$COL_SQL_DIR"
fi

if [ "$INSTALL_MOD_DUNGEON_MASTER" = true ]; then
    if [ ! -d "$AC_DIR/modules/mod-dungeon-master/.git" ]; then
        warn "[mod-dungeon-master] El módulo no está clonado en $AC_DIR/modules/mod-dungeon-master."
        warn "  Esto pasa si instalaste antes de que 03_clone_core.sh incluyera este módulo."
        warn "  Solución: ejecuta './install.sh --only 3' para clonarlo, luego './install.sh --from 4'"
        warn "  para recompilar y reconfigurar (fases 4-7), y finalmente './install.sh --post'."
    else
    # Mismo reintento que el resto: si las BBDD no existían en fase 5, aplicamos
    # ahora los dos SQL (world + characters) que necesita este módulo.
    DM_DIR="$AC_DIR/modules/mod-dungeon-master/data/sql"
    apply_sql_dir "mod-dungeon-master" acore_world      "$DM_DIR/db-world"
    apply_sql_dir "mod-dungeon-master" acore_characters "$DM_DIR/db-characters"

    echo -e "  ${YELLOW}6.${NC} ${BOLD}mod-dungeon-master${NC} — ${YELLOW}⚠ módulo experimental${NC}:"
    echo -e "     El NPC 'Dungeon Master' ya aparece en las capitales (entry 500000)."
    echo -e "     La dificultad/recompensas ya se aplicaron desde config.sh (DM_*)."
    echo -e "     Si quieres ajustarlas, edita config.sh y reaplica con './install.sh --only 5',"
    echo -e "     o edita el conf directamente para pruebas puntuales:"
    echo -e "        ${CYAN}nano $AC_DIR/env/dist/etc/modules/mod_dungeon_master.conf${NC}"
    echo -e "     Si ves comportamiento extraño en instancias, desactívalo con"
    echo -e "     ${CYAN}INSTALL_MOD_DUNGEON_MASTER=false${NC} en config.sh y recompila."
    echo ""
    fi
fi


if [ "$INSTALL_MOD_CHALLENGE_MODES" = true ] || [ "$INSTALL_MOD_WAR_EFFORT" = true ]; then
    echo -e "  ${YELLOW}7.${NC} ${BOLD}Contenido nuevo${NC} — nada que hacer, sólo saber que está:"
    if [ "$INSTALL_MOD_CHALLENGE_MODES" = true ]; then
        echo -e "     ${CYAN}Santuario del Desafío${NC}: junto al cementerio de cada zona inicial."
        echo -e "       Se activa a nivel 1 (55 en Caballero de la Muerte) y es por personaje."
    fi
    if [ "$INSTALL_MOD_INSTANCED_WORLDBOSSES" = true ]; then
        echo -e "     ${CYAN}Jefes de mundo${NC}: instanciados por grupo, ya se pueden hacer jugando solo."
    fi
    if [ "$INSTALL_MOD_WAR_EFFORT" = true ]; then
        echo -e "     ${CYAN}Esfuerzo de Guerra de AQ${NC}: objetivos x${WAR_EFFORT_GOAL_SCALE}, alcanzables por un jugador."
    fi
    echo ""
fi

echo -e "  ${YELLOW}8.${NC} ${BOLD}Arrancar los servicios de forma permanente:${NC}"
echo -e "     ${CYAN}sudo systemctl start ac-authserver ac-worldserver${NC}"
if [ "${WORLDSERVER_STANDBY:-false}" = true ]; then
    echo -e "     ${YELLOW}(modo en espera: el worldserver se apaga solo sin jugadores y lo${NC}"
    echo -e "     ${YELLOW} despierta la primera conexión; \`.standby\` para verlo)${NC}"
fi
echo ""
echo -e "  ${YELLOW}9.${NC} ${BOLD}Cliente WoW${NC} — Edita ${CYAN}Data/realmlist.wtf${NC}:"
echo -e "     ${CYAN}set realmlist $REALM_IP${NC}"
echo ""

log "Fase 8 completada."
