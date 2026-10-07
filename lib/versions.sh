#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  lib/versions.sh — Fijado de versiones (pinning) y aviso de actualizaciones
#
#  El problema que resuelve: el core y los módulos son repositorios de terceros
#  que cambian a diario. Una actualización automática puede romper el servidor
#  un domingo a las 3 de la mañana sin que nadie se entere hasta el lunes.
#
#  La solución: versions.lock guarda el commit EXACTO de cada repositorio con
#  el que sabemos que el servidor funciona. La instalación se clava a esos
#  commits, y las actualizaciones dejan de ser automáticas: pasan a ser una
#  decisión tuya, avisada por correo dentro del juego a las cuentas GM.
#
#  Formato de versions.lock (TSV, una línea por repositorio):
#      nombre <TAB> rama <TAB> commit <TAB> fecha <TAB> url
# =============================================================================

VERSIONS_LOCK="${INSTALLER_DIR}/versions.lock"

# Emite "nombre<TAB>ruta" de cada repositorio gestionado: el core, cada módulo
# (activo o apartado) y mod-arac, que vive en extras/.
_managed_repos() {
    printf 'core\t%s\n' "$AC_DIR"

    local D
    for D in "$AC_DIR/modules"/*/ "$AC_DIR/modules-disabled"/*/; do
        [ -d "$D/.git" ] || continue
        printf '%s\t%s\n' "$(basename "$D")" "${D%/}"
    done

    [ -d "$AC_DIR/extras/mod-arac/.git" ] && \
        printf 'mod-arac\t%s\n' "$AC_DIR/extras/mod-arac"
    return 0
}

# =============================================================================
# freeze_versions — congela el estado actual en versions.lock
#
# Se ejecuta cuando el servidor está funcionando bien: "esta combinación de
# versiones es buena, guárdala". A partir de ahí es el punto al que se puede
# volver si una actualización rompe algo.
# =============================================================================
freeze_versions() {
    local NAME DIR BRANCH COMMIT DATE URL COUNT=0
    local TMP PANEL_LOCK_COPY PANEL_LOCK_DIR PANEL_TMP
    TMP="$(mktemp)"

    {
        echo "# versions.lock — versiones conocidas como funcionales"
        echo "# Generado por ./install.sh --freeze el $(date '+%Y-%m-%d %H:%M:%S')"
        echo "#"
        echo "# Para volver a una de estas versiones:  ./install.sh --only 3 && ./install.sh --from 4"
        echo "# Para actualizar el lock a lo instalado: ./install.sh --freeze"
        echo "#"
        printf '# %-28s\t%-12s\t%-40s\t%-10s\t%s\n' nombre rama commit fecha url
    } > "$TMP"

    while IFS=$'\t' read -r NAME DIR; do
        [ -n "$NAME" ] || continue
        BRANCH=$(git -C "$DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
        # Si estamos en HEAD separado (porque ya se aplicó un pin), recuperamos
        # la rama que quedó anotada en el lock anterior.
        if [ "$BRANCH" = "HEAD" ] && [ -f "$VERSIONS_LOCK" ]; then
            BRANCH=$(awk -F'\t' -v n="$NAME" '$1==n {print $2}' "$VERSIONS_LOCK" | head -1)
            [ -n "$BRANCH" ] || BRANCH="master"
        fi
        COMMIT=$(git -C "$DIR" rev-parse HEAD 2>/dev/null || echo "?")
        DATE=$(git -C "$DIR" log -1 --format=%cd --date=short 2>/dev/null || echo "?")
        URL=$(git -C "$DIR" config --get remote.origin.url 2>/dev/null || echo "?")
        printf '%s\t%s\t%s\t%s\t%s\n' "$NAME" "$BRANCH" "$COMMIT" "$DATE" "$URL" >> "$TMP"
        COUNT=$((COUNT + 1))
    done < <(_managed_repos)

    mv "$TMP" "$VERSIONS_LOCK"

    # Si está instalado el panel web, actualiza también su copia aislada. El
    # directorio pertenece al usuario de AzerothCore, mientras que el servicio
    # DynamicUser sólo puede leerlo por ProtectSystem=strict.
    PANEL_LOCK_COPY="${PANEL_VERSIONS_LOCK_COPY:-/opt/azerothcore-panel/update-locks/versions.lock}"
    PANEL_LOCK_DIR="$(dirname "$PANEL_LOCK_COPY")"
    if [ -d "$PANEL_LOCK_DIR" ] && [ -w "$PANEL_LOCK_DIR" ]; then
        if PANEL_TMP="$(mktemp "$PANEL_LOCK_DIR/.versions.lock.XXXXXX")"; then
            if ! cp "$VERSIONS_LOCK" "$PANEL_TMP" || \
               ! chmod 0644 "$PANEL_TMP" || \
               ! mv "$PANEL_TMP" "$PANEL_LOCK_COPY"; then
                rm -f "$PANEL_TMP"
                warn "No se pudo refrescar la copia de versions.lock del panel web."
            fi
        else
            warn "No se pudo preparar la copia de versions.lock del panel web."
        fi
    fi

    log "versions.lock actualizado con $COUNT repositorios."
    info "  $VERSIONS_LOCK"

    # Refresca también el informe que lee mod-update-notice (P8): si no, un
    # --freeze puede dejar "al día" el panel web (compara en vivo contra
    # versions.lock) mientras el aviso dentro del juego sigue enseñando la
    # comprobación anterior al --freeze como si siguiera pendiente. Sin red
    # (o sin node para los addons) no rompe el --freeze: solo se queda sin
    # refrescar, como advierte el aviso de "informe desfasado" del módulo.
    if ! declare -F check_addon_updates >/dev/null && [ -f "${INSTALLER_DIR:-}/lib/addon-versions.sh" ]; then
        source "$INSTALLER_DIR/lib/addon-versions.sh"
    fi
    check_upstream_updates || true
    if declare -F check_addon_updates >/dev/null; then
        check_addon_updates || true
        UPDATE_COUNT=$(( ${UPDATE_COUNT:-0} + ${ADDON_UPDATE_COUNT:-0} ))
        UPDATE_DETAILS+=("${ADDON_UPDATE_DETAILS[@]}")
    fi
    write_update_report
    info "  Informe de mod-update-notice actualizado: $UPDATE_REPORT_FILE"
    return 0
}

# =============================================================================
# apply_pinned_versions — deja cada repositorio en el commit del lock
#
# Idempotente: si ya está en el commit correcto, no toca nada. Si el commit no
# existe en local, lo busca en origin antes de rendirse.
# =============================================================================
apply_pinned_versions() {
    if [ "${PIN_VERSIONS:-true}" != true ]; then
        info "Fijado de versiones desactivado (PIN_VERSIONS=false): se usa lo último de cada rama."
        return 0
    fi
    if [ ! -f "$VERSIONS_LOCK" ]; then
        warn "No existe $VERSIONS_LOCK — no hay versiones fijadas todavía."
        warn "  Cuando el servidor funcione bien, congélalas con: ./install.sh --freeze"
        return 0
    fi

    local NAME BRANCH COMMIT DATE URL DIR CURRENT FAILED=0
    while IFS=$'\t' read -r NAME BRANCH COMMIT DATE URL; do
        case "$NAME" in ''|\#*) continue ;; esac

        if [ "$NAME" = "core" ]; then
            DIR="$AC_DIR"
        elif [ -d "$AC_DIR/modules/$NAME/.git" ]; then
            DIR="$AC_DIR/modules/$NAME"
        elif [ -d "$AC_DIR/modules-disabled/$NAME/.git" ]; then
            DIR="$AC_DIR/modules-disabled/$NAME"
        elif [ -d "$AC_DIR/extras/$NAME/.git" ]; then
            DIR="$AC_DIR/extras/$NAME"
        else
            continue   # módulo no instalado: no es un error
        fi

        CURRENT=$(git -C "$DIR" rev-parse HEAD 2>/dev/null || echo "")
        [ "$CURRENT" = "$COMMIT" ] && continue

        # Descarta parches locales antes de moverse; se reaplican después.
        git -C "$DIR" checkout -- . >/dev/null 2>&1 || true

        if ! git -C "$DIR" cat-file -e "${COMMIT}^{commit}" 2>/dev/null; then
            git -C "$DIR" fetch --quiet origin "$BRANCH" >/dev/null 2>&1 || true
        fi

        if git -C "$DIR" checkout --quiet --detach "$COMMIT" >/dev/null 2>&1; then
            log "[$NAME] fijado en ${COMMIT:0:12} ($DATE)"
        else
            error "[$NAME] no se pudo fijar en $COMMIT — ¿el upstream reescribió el historial?"
            FAILED=1
        fi
    done < "$VERSIONS_LOCK"

    return $FAILED
}

# =============================================================================
# check_upstream_updates — ¿hay versiones nuevas río arriba?
#
# NO actualiza nada: solo mira y describe. Deja el informe en la variable
# global UPDATE_REPORT y devuelve 0 si hay novedades, 1 si no hay ninguna.
# =============================================================================
check_upstream_updates() {
    UPDATE_REPORT=""
    UPDATE_COUNT=0
    UPDATE_CHECKED=0
    UPDATE_UNREACHABLE=""
    # Una línea por repositorio, también los que están al día, para la tabla
    # de tools/revisar-actualizaciones.sh:
    #   nombre <TAB> N <TAB> fecha_fijada <TAB> fecha_upstream <TAB> commit_upstream
    UPDATE_ROWS=()
    # Sólo los que tienen novedades, en texto llano, para el fichero que lee
    # mod-update-notice y para el correo.
    UPDATE_DETAILS=()

    [ -f "$VERSIONS_LOCK" ] || return 1

    local NAME BRANCH COMMIT DATE URL DIR N UP_COMMIT UP_DATE
    while IFS=$'\t' read -r NAME BRANCH COMMIT DATE URL; do
        case "$NAME" in ''|\#*) continue ;; esac

        if [ "$NAME" = "core" ]; then
            DIR="$AC_DIR"
        elif [ -d "$AC_DIR/modules/$NAME/.git" ]; then
            DIR="$AC_DIR/modules/$NAME"
        elif [ -d "$AC_DIR/extras/$NAME/.git" ]; then
            DIR="$AC_DIR/extras/$NAME"
        else
            continue   # solo avisamos de lo que está en uso
        fi

        if ! git -C "$DIR" fetch --quiet origin "$BRANCH" >/dev/null 2>&1; then
            UPDATE_UNREACHABLE="${UPDATE_UNREACHABLE}${UPDATE_UNREACHABLE:+ }${NAME}"
            continue
        fi
        UPDATE_CHECKED=$((UPDATE_CHECKED + 1))

        N=$(git -C "$DIR" rev-list --count "${COMMIT}..origin/${BRANCH}" 2>/dev/null || echo 0)
        UP_COMMIT=$(git -C "$DIR" rev-parse "origin/${BRANCH}" 2>/dev/null || echo "?")
        UP_DATE=$(git -C "$DIR" log -1 --format=%cd --date=short "origin/${BRANCH}" 2>/dev/null || echo "?")
        UPDATE_ROWS+=("${NAME}	${N:-0}	${DATE}	${UP_DATE}	${UP_COMMIT}")

        if [ "${N:-0}" -gt 0 ]; then
            UPDATE_REPORT="${UPDATE_REPORT}${NAME}: ${N} commits nuevos; "
            # Severidad para mod-update-notice (CS-4.5): el core y los repos con
            # parche propio (patches/<nombre>/) pueden traer incompatibilidades;
            # el resto son cambios funcionales.
            local SEV="funcional"
            if [ "$NAME" = "core" ] || [ -d "$INSTALLER_DIR/patches/$NAME" ] || [ -d "$INSTALLER_DIR/patches-cliente/$NAME" ]; then
                SEV="incompatibilidad"
            fi
            UPDATE_DETAILS+=("[${SEV}] ${NAME}: ${N} commits nuevos (fijado ${DATE}, upstream ${UP_DATE})")
            UPDATE_COUNT=$((UPDATE_COUNT + 1))
        fi
    done < "$VERSIONS_LOCK"

    [ "$UPDATE_COUNT" -gt 0 ]
}

# =============================================================================
# write_update_report — el fichero que lee mod-update-notice
#
# Se escribe SIEMPRE tras una comprobación: con novedades, una línea por
# repositorio; sin novedades, vacío salvo un comentario con la fecha (el
# módulo ignora las líneas que empiezan por #). Así "no hay fichero" y "hay
# fichero vacío" significan lo mismo: nada que avisar.
#
# Vive junto al worldserver porque el módulo lo lee con una ruta relativa al
# directorio desde el que arranca (env/dist/bin). mod-update-notice compara
# la fecha de ESTE fichero con la del versions.lock REAL (UpdateNotice.
# VersionsLockFile, ruta absoluta desde 05_configure_server.sh) para avisar
# si el informe quedó anterior a un --freeze o a una edición a mano (P8). NO
# copiar versions.lock aquí: una copia que este fichero reescribiera en cada
# comprobación tendría siempre su misma fecha y el aviso de "informe
# desfasado" no podría dispararse nunca (panel-versions-lock-copia-estatica).
# =============================================================================
UPDATE_REPORT_FILE="${AC_DIR}/env/dist/bin/updates-pending.txt"

write_update_report() {
    local FILE="${1:-$UPDATE_REPORT_FILE}"
    local STAMP
    STAMP="$(date '+%Y-%m-%d %H:%M')"

    mkdir -p "$(dirname "$FILE")" 2>/dev/null || true

    {
        echo "# Informe de lib/check-updates.sh (módulos del servidor + addons de cliente), ${STAMP}."
        echo "# Lo lee mod-update-notice al conectarse una cuenta GM. Se regenera en cada comprobación."
        if [ "${UPDATE_COUNT:-0}" -gt 0 ]; then
            echo "Comprobado el ${STAMP}: ${UPDATE_COUNT} repositorio(s) con version nueva rio arriba."
            local LINE
            for LINE in "${UPDATE_DETAILS[@]}"; do
                echo "- ${LINE}"
            done
        fi
        if [ -n "${UPDATE_UNREACHABLE:-}" ]; then
            echo "Sin acceso a: ${UPDATE_UNREACHABLE} (sin red, o repositorio movido)."
        fi
    } > "$FILE"
}
