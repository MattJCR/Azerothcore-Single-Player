#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  03_clone_core.sh — Clona el core de AzerothCore y todos los módulos
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$SCRIPT_DIR/config.sh"
source "$SCRIPT_DIR/lib/utils.sh"
source "$SCRIPT_DIR/lib/versions.sh"
source "$SCRIPT_DIR/lib/mirrors.sh"

header "FASE 3 — Clonar código fuente"

# --- Core de AzerothCore ---
# Casos posibles del directorio $AC_DIR:
#   A) No existe                    → clonar normalmente
#   B) Existe con .git              → ya clonado, saltar
#   C) Existe sin .git pero vacío   → clonar normalmente (git acepta destino vacío)
#   D) Existe sin .git con archivos → solo puede ser logs/ creado por utils.sh
#      antes de que existiera el repo; mover logs a /tmp, borrar el dir y clonar

if [ -d "$AC_DIR/.git" ]; then
    # Caso B
    warn "El directorio $AC_DIR ya es un repositorio git. Saltando clonado del core."
    info "Si quieres reclonarlo desde cero borra el directorio: rm -rf $AC_DIR"
else
    if [ -d "$AC_DIR" ]; then
        # Caso C o D — directorio existe pero no es un repo git
        CONTENTS=$(ls -A "$AC_DIR" 2>/dev/null)
        if [ -z "$CONTENTS" ]; then
            # Caso C — directorio vacío, git clone funciona igual
            info "Directorio $AC_DIR vacío encontrado. Clonando dentro..."
        else
            # Caso D — tiene contenido (casi seguro son los logs de utils.sh)
            warn "El directorio $AC_DIR existe y contiene archivos pero no es un repo git."
            info "Contenido encontrado: $CONTENTS"
            info "Moviendo contenido existente a /tmp/ac_dir_backup_$(date +%s) y continuando..."
            BACKUP="/tmp/ac_dir_backup_$(date +%s)"
            mv "$AC_DIR" "$BACKUP"
            # El log vivia en $AC_DIR/logs y se acaba de ir con el backup:
            # sin esto, el siguiente `run` no puede abrir su redirect, devuelve
            # 1, y la instalacion aborta con un "Clonar AzerothCore - FALLO"
            # que no tiene nada que ver con git.
            INSTALL_LOG="/tmp/azerothcore-install.log"
            info "Backup guardado en: $BACKUP"
        fi
    fi

    # Clonar el core (directorio no existe, vacío, o ya fue movido)
    info "Repositorio: $AC_CORE_REPO (rama: $AC_CORE_BRANCH)"
    run "Clonar AzerothCore" git clone \
        "$AC_CORE_REPO" \
        --branch "$AC_CORE_BRANCH" \
        --single-branch \
        "$AC_DIR"

    # Redirigir el log de /tmp al destino definitivo ahora que $AC_DIR existe
    _redirect_log_if_needed
fi

# --- Directorio de módulos ---
mkdir -p "$AC_DIR/modules"

# =============================================================================
# Modulos desactivados en config.sh
#
# CMake compila TODO lo que encuentre en modules/ (-DMODULES=static), asi que
# poner INSTALL_MOD_X=false despues de una instalacion no bastaba: el modulo
# seguia ahi y se seguia compilando y cargando. Los apartamos a
# modules-disabled/ (reversible: si vuelves a ponerlo en true, se recupera).
#
# Este bucle va ANTES de clonar a propósito: si un módulo se reactiva, se
# recupera la copia apartada en vez de volver a clonarla de cero. Así se
# respeta la versión fijada en versions.lock y no queda una copia huérfana
# en modules-disabled/.
# =============================================================================
MANAGED_MODULES=(
    "mod-individual-progression:$INSTALL_MOD_INDIVIDUAL_PROGRESSION"
    "mod-playerbots:$INSTALL_MOD_PLAYERBOTS"
    "mod-autobalance:$INSTALL_MOD_AUTOBALANCE"
    "mod-transmog:$INSTALL_MOD_TRANSMOG"
    "mod-ah-bot-plus:$INSTALL_MOD_AH_BOT_PLUS"
    "mod-random-enchants:$INSTALL_MOD_RANDOM_ENCHANTS"
    "mod-pvp-titles:$INSTALL_MOD_PVP_TITLES"
    "mod-congrats-on-level:$INSTALL_MOD_CONGRATS_ON_LEVEL"
    "mod-dungeon-master:$INSTALL_MOD_DUNGEON_MASTER"
    "mod-challenge-modes:$INSTALL_MOD_CHALLENGE_MODES"
    "mod-instanced-worldbosses:$INSTALL_MOD_INSTANCED_WORLDBOSSES"
    "mod-war-effort:$INSTALL_MOD_WAR_EFFORT"
    "mod-racial-trait-swap:$INSTALL_MOD_RACIAL_TRAIT_SWAP"
    "mod-reagent-bank:$INSTALL_MOD_REAGENT_BANK"
    "mod-aoe-loot:$INSTALL_MOD_AOE_LOOT"
    "mod-quest-loot-party:${INSTALL_MOD_QUEST_LOOT_PARTY:-false}"
    "mod-instance-reset:$INSTALL_MOD_INSTANCE_RESET"
    "mod-1v1-arena:$INSTALL_MOD_1V1_ARENA"
    "mod-adaptive-ai:${INSTALL_MOD_ADAPTIVE_AI:-false}"
    "mod-dungeon-clear:${INSTALL_MOD_DUNGEON_CLEAR:-false}"
    "mod-world-buff-bots:${INSTALL_MOD_WORLD_BUFF_BOTS:-false}"
    "mod-token-turnin:${INSTALL_MOD_TOKEN_TURNIN:-false}"
    "mod-playerbots-wintergrasp:${INSTALL_MOD_PLAYERBOTS_WINTERGRASP:-false}"
    "mod-guild-levels:${INSTALL_MOD_GUILD_LEVELS:-false}"
    "mod-guildhouse:${INSTALL_MOD_GUILDHOUSE:-false}"
    "mod-profession-experience:${INSTALL_MOD_PROFESSION_EXPERIENCE:-false}"
)

DISABLED_DIR="$AC_DIR/modules-disabled"
NEEDS_REBUILD=false

for ENTRY in "${MANAGED_MODULES[@]}"; do
    MOD_NAME="${ENTRY%%:*}"
    MOD_ON="${ENTRY##*:}"
    if [ "$MOD_ON" != true ]; then
        if [ -d "$AC_DIR/modules/$MOD_NAME" ]; then
            mkdir -p "$DISABLED_DIR"
            rm -rf "${DISABLED_DIR:?}/$MOD_NAME"
            mv "$AC_DIR/modules/$MOD_NAME" "$DISABLED_DIR/$MOD_NAME"
            warn "[$MOD_NAME] desactivado en config.sh -> movido a modules-disabled/"
            NEEDS_REBUILD=true
        fi
        # La limpieza de la BD va FUERA del "si estaba en modules/": si el
        # módulo ya se apartó en una pasada anterior pero sus filas siguen en
        # acore_world, el core seguiría quejándose en cada arranque.
        cleanup_disabled_module_db "$MOD_NAME"
    elif [ -d "$DISABLED_DIR/$MOD_NAME" ]; then
        if [ -d "$AC_DIR/modules/$MOD_NAME" ]; then
            # Está activo y además quedó una copia apartada de una pasada
            # anterior: sobra, ocupa sitio y confunde.
            rm -rf "${DISABLED_DIR:?}/$MOD_NAME"
            info "[$MOD_NAME] copia sobrante de modules-disabled/ eliminada."
        else
            mv "$DISABLED_DIR/$MOD_NAME" "$AC_DIR/modules/$MOD_NAME"
            log "[$MOD_NAME] reactivado -> devuelto a modules/"
            NEEDS_REBUILD=true
        fi
    fi
done

# =============================================================================
# Modulos propios
#
# Los de arriba se clonan de GitHub; los nuestros viven en modules/ dentro de
# este repositorio y se copian a ~/azerothcore/modules. Va aqui, junto al resto
# de la gestion de modulos, para que una sola pasada de la fase 3 deje el
# directorio de compilacion completo.
# =============================================================================
if install_own_modules; then
    NEEDS_REBUILD=true
fi

if [ "$NEEDS_REBUILD" = true ]; then
    warn "La lista de modulos compilados ha cambiado: hay que recompilar"
    warn "  (./install.sh --from 4) para que el cambio surta efecto."
fi

# --- Función auxiliar para clonar módulos ---
clone_module() {
    local NAME="$1"
    local REPO="$2"
    local BRANCH="${3:-master}"
    local DEST="$AC_DIR/modules/$NAME"

    if [ -d "$DEST/.git" ]; then
        warn "Módulo '$NAME' ya existe. Saltando."
        return 0
    fi

    # Si existe sin .git (descarga parcial), borrarlo antes
    if [ -d "$DEST" ]; then
        warn "Directorio $DEST existe sin .git (descarga incompleta). Limpiando..."
        rm -rf "$DEST"
    fi

    # No se usa run() aquí a propósito: si el clonado falla queremos intentar la
    # copia offline de mirrors/ en vez de abortar la instalación entera. Ese es
    # el caso para el que existen las copias: repositorio borrado, renombrado,
    # puesto en privado, o VM sin conexión.
    info "Clonar módulo $NAME..."
    if git clone "$REPO" --branch "$BRANCH" --single-branch "$DEST" >> "$INSTALL_LOG" 2>&1; then
        log "Clonar módulo $NAME — hecho."
        return 0
    fi

    warn "No se pudo clonar '$NAME' desde $REPO"
    warn "  (repositorio borrado, renombrado, privado, o sin conexión)"
    rm -rf "$DEST"

    if restore_from_mirror "$NAME" "$DEST"; then
        log "[$NAME] recuperado desde la copia offline del proyecto."
        return 0
    fi

    error "[$NAME] no se pudo clonar y no hay copia en mirrors/."
    error "  Genera las copias con './install.sh --mirror' mientras el repo siga disponible."
    exit 1
}

# --- Clonar módulos seleccionados ---
header "Clonando módulos"

if [ "$INSTALL_MOD_INDIVIDUAL_PROGRESSION" = true ]; then
    clone_module "mod-individual-progression" \
        "https://github.com/ZhengPeiRu21/mod-individual-progression.git"
fi

if [ "$INSTALL_MOD_PLAYERBOTS" = true ]; then
    clone_module "mod-playerbots" \
        "https://github.com/mod-playerbots/mod-playerbots.git"
fi

if [ "$INSTALL_MOD_AUTOBALANCE" = true ]; then
    clone_module "mod-autobalance" \
        "https://github.com/azerothcore/mod-autobalance.git"
fi

if [ "$INSTALL_MOD_TRANSMOG" = true ]; then
    clone_module "mod-transmog" \
        "https://github.com/azerothcore/mod-transmog.git"
fi

if [ "$INSTALL_MOD_AH_BOT_PLUS" = true ]; then
    clone_module "mod-ah-bot-plus" \
        "https://github.com/NathanHandley/mod-ah-bot-plus.git"
fi

if [ "$INSTALL_MOD_RANDOM_ENCHANTS" = true ]; then
    clone_module "mod-random-enchants" \
        "https://github.com/azerothcore/mod-random-enchants.git"
fi

if [ "$INSTALL_MOD_PVP_TITLES" = true ]; then
    clone_module "mod-pvp-titles" \
        "https://github.com/azerothcore/mod-pvp-titles.git"
fi

if [ "$INSTALL_MOD_CONGRATS_ON_LEVEL" = true ]; then
    clone_module "mod-congrats-on-level" \
        "https://github.com/azerothcore/mod-congrats-on-level.git"
fi

if [ "$INSTALL_MOD_DUNGEON_MASTER" = true ]; then
    clone_module "mod-dungeon-master" \
        "https://github.com/InstanceForge/mod-dungeon-master.git" \
        "main"
fi

# ── Módulos de contenido añadidos (ver README.md) ──────────────────────

if [ "$INSTALL_MOD_CHALLENGE_MODES" = true ]; then
    clone_module "mod-challenge-modes" \
        "https://github.com/ZhengPeiRu21/mod-challenge-modes.git"
fi

if [ "$INSTALL_MOD_INSTANCED_WORLDBOSSES" = true ]; then
    clone_module "mod-instanced-worldbosses" \
        "https://github.com/azerothcore/mod-instanced-worldbosses.git"
fi

if [ "$INSTALL_MOD_WAR_EFFORT" = true ]; then
    clone_module "mod-war-effort" \
        "https://github.com/azerothcore/mod-war-effort.git"
fi

if [ "$INSTALL_MOD_RACIAL_TRAIT_SWAP" = true ]; then
    # Este repo usa "main" como rama por defecto, no "master" como el resto.
    clone_module "mod-racial-trait-swap" \
        "https://github.com/azerothcore/mod-racial-trait-swap.git" \
        "main"
fi

if [ "$INSTALL_MOD_REAGENT_BANK" = true ]; then
    clone_module "mod-reagent-bank" \
        "https://github.com/ZhengPeiRu21/mod-reagent-bank.git"
fi

if [ "$INSTALL_MOD_AOE_LOOT" = true ]; then
    clone_module "mod-aoe-loot" \
        "https://github.com/azerothcore/mod-aoe-loot.git"
fi

if [ "${INSTALL_MOD_QUEST_LOOT_PARTY:-false}" = true ]; then
    clone_module "mod-quest-loot-party" \
        "https://github.com/pangolp/mod-quest-loot-party.git"
fi

if [ "$INSTALL_MOD_INSTANCE_RESET" = true ]; then
    clone_module "mod-instance-reset" \
        "https://github.com/azerothcore/mod-instance-reset.git"
fi

if [ "$INSTALL_MOD_1V1_ARENA" = true ]; then
    clone_module "mod-1v1-arena" \
        "https://github.com/azerothcore/mod-1v1-arena.git"
fi

# El tanque bot lleva la mazmorra. Necesita mod-playerbots compilado en la
# misma biblioteca de módulos (subclasea sus estrategias y acciones).
if [ "${INSTALL_MOD_DUNGEON_CLEAR:-false}" = true ]; then
    if [ "$INSTALL_MOD_PLAYERBOTS" != true ]; then
        warn "[mod-dungeon-clear] necesita mod-playerbots: no se clona."
    else
        clone_module "mod-dungeon-clear" \
            "https://github.com/jrad7/mod-dungeon-clear.git"
    fi
fi

# Buffs de mundo simulados. Autónomo: no necesita playerbots. Rama "main".
if [ "${INSTALL_MOD_WORLD_BUFF_BOTS:-false}" = true ]; then
    clone_module "mod-world-buff-bots" \
        "https://github.com/Rockhopper1776/mod-world-buff-bots.git" "main"
fi

# Canje de tokens de tier para los bots del grupo. Compila sin playerbots,
# pero sin él no hay bots a los que canjearles nada.
if [ "${INSTALL_MOD_TOKEN_TURNIN:-false}" = true ]; then
    if [ "$INSTALL_MOD_PLAYERBOTS" != true ]; then
        warn "[mod-token-turnin] sin mod-playerbots no tiene a quien canjear tokens: no se clona."
    else
        clone_module "mod-token-turnin" \
            "https://github.com/Zerathane/mod-token-turnin.git"
    fi
fi

# Los playerbots aceptan solos cola/entrada a Wintergrasp. Usa el hook
# PlayerbotScript, así que necesita mod-playerbots compilado en la misma
# biblioteca de módulos. Rama "main".
if [ "${INSTALL_MOD_PLAYERBOTS_WINTERGRASP:-false}" = true ]; then
    if [ "$INSTALL_MOD_PLAYERBOTS" != true ]; then
        warn "[mod-playerbots-wintergrasp] necesita mod-playerbots: no se clona."
    else
        clone_module "mod-playerbots-wintergrasp" \
            "https://github.com/vrzgames/mod-playerbots-wintergrasp.git" "main"
    fi
fi

# Experiencia por profesiones, fijada y parcheada igual que los demás módulos.
if [ "${INSTALL_MOD_PROFESSION_EXPERIENCE:-false}" = true ]; then
    clone_module "mod-profession-experience" \
        "https://github.com/Tereneckla/mod-profession-experience.git" "main"
fi

# Experiencia y niveles de hermandad. Solo usa PlayerScript/GuildScript
# genéricos (ningún hook de playerbots): no depende de ningún otro módulo.
# Rama "main".
if [ "${INSTALL_MOD_GUILD_LEVELS:-false}" = true ]; then
    clone_module "mod-guild-levels" \
        "https://github.com/Old-Man-Warcraft/mod-guild-levels.git" "main"
fi

# Sede de hermandad (SP02), fijada en versions.lock.
if [ "${INSTALL_MOD_GUILDHOUSE:-false}" = true ]; then
    clone_module "mod-guildhouse" \
        "https://github.com/azerothcore/mod-guildhouse.git" "master"
fi

# =============================================================================
# Parches locales sobre módulos de terceros
#
# Corrigen bugs que el upstream aún no ha arreglado (ver patches/ y las notas
# de cada función en lib/utils.sh). Se aplican aquí, tras clonar, y se vuelven
# a aplicar en cada actualización semanal desde lib/reapply-patches.sh.
# =============================================================================
header "Versiones fijadas"
if ! apply_pinned_versions; then
    if [ "${ALLOW_PIN_OR_PATCH_FAILURE:-false}" = true ]; then
        warn "Alguna versión fijada no se pudo aplicar (ver arriba). Continuando: ALLOW_PIN_OR_PATCH_FAILURE=true."
    else
        error "Alguna versión fijada no se pudo aplicar (ver arriba)."
        error "  Instalación detenida: arrancar con versiones no fijadas es justo lo que versions.lock evita."
        error "  Para continuar de todos modos (solo desarrollo): ALLOW_PIN_OR_PATCH_FAILURE=true en config.sh."
        exit 1
    fi
fi

# Los parches van DESPUÉS del pin: fijar una versión hace checkout y se los
# llevaría por delante.
header "Parches locales sobre los módulos"
if ! apply_all_source_patches; then
    if [ "${ALLOW_PIN_OR_PATCH_FAILURE:-false}" = true ]; then
        warn "Algún parche no se pudo aplicar (ver arriba). Continuando: ALLOW_PIN_OR_PATCH_FAILURE=true."
    else
        error "Algún parche no se pudo aplicar (ver arriba)."
        error "  Instalación detenida: un parche que no aplica deja código sin el fix que se supone que trae."
        error "  Para continuar de todos modos (solo desarrollo): ALLOW_PIN_OR_PATCH_FAILURE=true en config.sh."
        exit 1
    fi
fi

log "Core y módulos clonados en $AC_DIR"

# Chequeo cruzado de versions.lock / mirrors/versions.lock / MANIFEST.tsv /
# tarballs: solo avisa (los espejos son la vía de emergencia si el clonado
# por red falla, no el camino normal), pero deja constancia clara en el log
# si un './install.sh --mirror' quedó a medias.
verify_mirrors_consistency || warn "mirrors/ tiene inconsistencias (ver arriba). Corrígelas con './install.sh --mirror'."

# =============================================================================
# mod-arac (ARAC — All Races All Classes)
# ⚠️  NO es un módulo de compilación C++ (no tiene CMakeLists.txt), así que
#     NO se clona en modules/ — si lo hiciéramos, CMake fallaría al buscar
#     su build system. Solo necesitamos su SQL y los archivos de parche/DBC,
#     así que lo clonamos en un directorio separado "extras/".
# =============================================================================
if [ "$INSTALL_MOD_ARAC" = true ]; then
    header "Clonando mod-arac (extras, no es módulo de compilación)"
    ARAC_DEST="$AC_DIR/extras/mod-arac"
    mkdir -p "$AC_DIR/extras"

    if [ -d "$ARAC_DEST/.git" ]; then
        warn "mod-arac ya está clonado en extras/. Saltando."
    else
        if [ -d "$ARAC_DEST" ]; then
            warn "Directorio $ARAC_DEST existe sin .git. Limpiando..."
            rm -rf "$ARAC_DEST"
        fi
        run "Clonar mod-arac" git clone \
            "https://github.com/heyitsbench/mod-arac.git" \
            --single-branch \
            "$ARAC_DEST"
    fi
    info "mod-arac requiere pasos manuales adicionales (parche de cliente + DBC)."
    info "Instrucciones detalladas al final de la instalación (fase 8)."
fi
