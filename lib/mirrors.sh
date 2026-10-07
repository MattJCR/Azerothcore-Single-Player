#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  lib/mirrors.sh — Copia offline de los repositorios de terceros
#
#  Por qué existe: versions.lock fija el commit exacto de cada repositorio,
#  pero eso solo sirve mientras ese repositorio siga existiendo. Varios de los
#  módulos son proyectos de una sola persona; si alguien archiva, renombra o
#  borra su repo, la instalación deja de poder reproducirse.
#
#  mirrors/ guarda un snapshot .tar.gz del árbol de cada repositorio en el
#  commit fijado. No lleva historia de git — es una foto — pero es suficiente
#  para reinstalar exactamente lo que hoy funciona aunque el original
#  desaparezca de internet.
#
#  El core NO se incluye por defecto: son 211 MB y GitHub rechaza ficheros de
#  más de 100 MB. Ver MIRROR_INCLUDE_CORE en config.sh y la nota del final.
# =============================================================================

MIRRORS_DIR="${INSTALLER_DIR}/mirrors"
MIRRORS_MANIFEST="${MIRRORS_DIR}/MANIFEST.tsv"

# =============================================================================
# mirror_all_repos — genera los snapshots desde lo que hay instalado
# =============================================================================
mirror_all_repos() {
    mkdir -p "$MIRRORS_DIR"

    local NAME DIR COMMIT SHORT URL OUT SIZE SUM COUNT=0
    local TMP_MANIFEST
    TMP_MANIFEST="$(mktemp)"

    {
        echo "# MANIFEST.tsv — copias offline de los repositorios de terceros"
        echo "# Generado por ./install.sh --mirror el $(date '+%Y-%m-%d %H:%M:%S')"
        echo "#"
        printf '# %-28s\t%-40s\t%-12s\t%s\n' nombre commit tamano fichero
    } > "$TMP_MANIFEST"

    while IFS=$'\t' read -r NAME DIR; do
        [ -n "$NAME" ] || continue

        if [ "$NAME" = "core" ] && [ "${MIRROR_INCLUDE_CORE:-false}" != true ]; then
            info "[espejo] core omitido (211 MB; MIRROR_INCLUDE_CORE=false)"
            continue
        fi

        COMMIT=$(git -C "$DIR" rev-parse HEAD 2>/dev/null) || continue
        SHORT="${COMMIT:0:12}"
        URL=$(git -C "$DIR" config --get remote.origin.url 2>/dev/null || echo "?")
        OUT="$MIRRORS_DIR/${NAME}@${SHORT}.tar.gz"

        if [ -f "$OUT" ]; then
            info "[espejo] $NAME ya tiene copia de ${SHORT}"
        else
            # git archive saca el árbol EXACTO del commit, sin ficheros sueltos
            # ni los parches locales que tengamos aplicados encima.
            if ! git -C "$DIR" archive --format=tar "$COMMIT" | gzip -9 > "$OUT"; then
                warn "[espejo] no se pudo generar la copia de $NAME"
                rm -f "$OUT"
                continue
            fi
            log "[espejo] $NAME@${SHORT} -> $(basename "$OUT")"
        fi

        SIZE=$(stat -c%s "$OUT")
        SUM=$(sha256sum "$OUT" | cut -d' ' -f1)
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$NAME" "$COMMIT" "$SIZE" "$(basename "$OUT")" "$SUM" "$URL" >> "$TMP_MANIFEST"
        COUNT=$((COUNT + 1))

        # Borra copias antiguas del mismo repo: la buena es la del lock.
        local OLD
        for OLD in "$MIRRORS_DIR/${NAME}@"*.tar.gz; do
            [ -f "$OLD" ] || continue
            [ "$OLD" = "$OUT" ] && continue
            rm -f "$OLD"
            info "[espejo] copia antigua eliminada: $(basename "$OLD")"
        done
    done < <(_managed_repos)

    mv "$TMP_MANIFEST" "$MIRRORS_MANIFEST"

    # El versions.lock viaja CON las copias. Sin él, mirrors/ es un montón de
    # tarballs sin saber qué combinación de commits era la buena: el manifiesto
    # dice qué hay, pero el lock dice qué rama y qué fecha, y es el fichero que
    # lee la fase 3. Si algún día se pierde el repositorio de este instalador,
    # o se restaura mirrors/ desde una copia de seguridad suelta, esto es lo que
    # hace que el respaldo sea autosuficiente.
    if [ -f "${INSTALLER_DIR}/versions.lock" ]; then
        cp "${INSTALLER_DIR}/versions.lock" "$MIRRORS_DIR/versions.lock"
        log "[espejo] versions.lock copiado junto a las copias offline."
    else
        warn "[espejo] no hay versions.lock que copiar: el respaldo queda incompleto."
    fi

    log "$COUNT copias offline en $MIRRORS_DIR ($(du -sh "$MIRRORS_DIR" | cut -f1) en total)."
    return 0
}

# =============================================================================
# hydrate_snapshot — deja en mirrors/ el snapshot verificado de un repositorio
#
# La edición pública no lleva los .tar.gz (pesan ~270 MiB y GitHub rechaza los
# ficheros de más de 100 MiB): lleva MANIFEST.tsv, que dice qué snapshot
# corresponde a cada commit fijado y su SHA-256. Esta función lo reconstruye por
# la primera vía que dé un fichero con ese hash exacto:
#
#   1. el que ya esté en mirrors/ (si no cuadra con el manifiesto se retira);
#   2. MIRROR_ASSET_BASE_URL/<fichero> (asset de versión, opcional);
#   3. el upstream, pidiendo SÓLO el commit fijado (git fetch --depth 1 <sha>) y
#      repitiendo el `git archive | gzip -9` de `--mirror`. Es reproducible: da
#      los mismos bytes que el snapshot original.
#
# Una caché o un asset no sustituyen la comprobación: un fichero que no cuadra
# con el SHA-256 del manifiesto no se instala, venga de donde venga. Los siete
# módulos sin licencia explícita no se redistribuyen como asset: sólo la vía 3.
# =============================================================================
_mirror_sha_ok() {
    # $1 fichero, $2 sha esperado
    [ -f "$1" ] && [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$2" ]
}

hydrate_snapshot() {
    local NAME="$1" COMMIT="$2"
    local FILE SIZE SHA URL OUT TMP
    local ROW
    ROW=$(awk -F'\t' -v n="$NAME" -v c="$COMMIT" '$1==n && $2==c {print $3 "\t" $4 "\t" $5 "\t" $6; exit}' "$MIRRORS_MANIFEST" 2>/dev/null)
    if [ -z "$ROW" ]; then
        error "[hidratar] '$NAME': MANIFEST.tsv no tiene entrada para el commit fijado (${COMMIT:0:12})."
        return 1
    fi
    IFS=$'\t' read -r SIZE FILE SHA URL <<< "$ROW"
    case "$FILE" in */*|*..*|'') error "[hidratar] '$NAME': nombre de fichero no válido en MANIFEST.tsv."; return 1 ;; esac
    [[ "$SHA" =~ ^[0-9a-f]{64}$ ]] || { error "[hidratar] '$NAME': SHA-256 no válido en MANIFEST.tsv."; return 1; }
    OUT="$MIRRORS_DIR/$FILE"

    if [ -f "$OUT" ]; then
        if _mirror_sha_ok "$OUT" "$SHA"; then
            info "[hidratar] $NAME@${COMMIT:0:12} ya está y su SHA-256 cuadra."
            return 0
        fi
        warn "[hidratar] $FILE no cuadra con el manifiesto: se descarta y se reconstruye."
        rm -f "$OUT"
    fi

    mkdir -p "$MIRRORS_DIR"
    TMP=$(mktemp -d "$MIRRORS_DIR/.hidratar.XXXXXX") || { error "[hidratar] no se pudo crear un temporal en $MIRRORS_DIR."; return 1; }

    if [ -n "${MIRROR_ASSET_BASE_URL:-}" ]; then
        case "$MIRROR_ASSET_BASE_URL" in
            https://*)
                if curl -fsSL --proto '=https' --retry 3 --connect-timeout 20 \
                        -o "$TMP/$FILE" "${MIRROR_ASSET_BASE_URL%/}/$FILE" 2>/dev/null; then
                    if _mirror_sha_ok "$TMP/$FILE" "$SHA"; then
                        mv "$TMP/$FILE" "$OUT"; rm -rf "$TMP"
                        log "[hidratar] $NAME@${COMMIT:0:12} descargado del asset y verificado."
                        return 0
                    fi
                    warn "[hidratar] el asset de $FILE no cuadra con el SHA-256 del manifiesto: se ignora."
                    rm -f "$TMP/$FILE"
                else
                    info "[hidratar] sin asset para $FILE en $MIRROR_ASSET_BASE_URL; se reconstruye desde el upstream."
                fi ;;
            *) warn "[hidratar] MIRROR_ASSET_BASE_URL debe ser https://; se ignora." ;;
        esac
    fi

    if [ -z "$URL" ] || [ "$URL" = "?" ]; then
        URL=$(awk -F'\t' -v n="$NAME" '$1==n {print $5; exit}' "${VERSIONS_LOCK:-${INSTALLER_DIR}/versions.lock}" 2>/dev/null)
    fi
    # AC_TEST_ALLOW_LOCAL_URL=1 sólo lo usa tests/hidratar-mirrors.sh (upstream local).
    case "$URL" in https://*) ;; *)
        if [ "${AC_TEST_ALLOW_LOCAL_URL:-}" != 1 ]; then
            error "[hidratar] '$NAME': sin URL https de upstream para reconstruir el snapshot."
            rm -rf "$TMP"; return 1
        fi ;;
    esac

    info "[hidratar] $NAME@${COMMIT:0:12}: reconstruyendo desde $URL (sólo ese commit)..."
    # core.autocrlf/eol fijados: git archive aplica las conversiones de fin de
    # línea de la configuración local; con las del sistema los bytes cambiarían.
    local G=(git -c core.autocrlf=false -c core.eol=lf -C "$TMP/r")
    if git init -q "$TMP/r" \
        && "${G[@]}" fetch -q --depth 1 --no-tags "$URL" "$COMMIT" 2>>"${INSTALL_LOG:-/dev/null}" \
        && [ "$("${G[@]}" rev-parse 'FETCH_HEAD^{commit}')" = "$COMMIT" ] \
        && "${G[@]}" archive --format=tar "$COMMIT" | gzip -9 > "$TMP/$FILE"; then
        :
    else
        error "[hidratar] '$NAME': no se pudo obtener el commit ${COMMIT:0:12} de $URL (repositorio movido, commit retirado o sin red)."
        rm -rf "$TMP"; return 1
    fi
    if ! _mirror_sha_ok "$TMP/$FILE" "$SHA"; then
        error "[hidratar] '$NAME': el snapshot reconstruido no cuadra con el SHA-256 del manifiesto (la herramienta de empaquetado difiere o el upstream cambió el commit)."
        rm -rf "$TMP"; return 1
    fi
    mv "$TMP/$FILE" "$OUT"; rm -rf "$TMP"
    log "[hidratar] $NAME@${COMMIT:0:12} reconstruido desde el upstream y verificado."
    return 0
}

# hydrate_mirrors [nombre...] — hidrata los snapshots de versions.lock (o sólo
# los nombrados). Con MIRROR_INCLUDE_CORE distinto de true el core se omite.
# Termina con 0 sólo si TODOS los pedidos quedaron verificados.
hydrate_mirrors() {
    local -a WANT=("$@")
    local NAME BRANCH COMMIT DATE URL FAILED=0 DONE=0 W SEL
    [ -f "$MIRRORS_MANIFEST" ] || { error "[hidratar] no existe $MIRRORS_MANIFEST."; return 1; }
    [ -f "${VERSIONS_LOCK:-}" ] || { error "[hidratar] no existe versions.lock."; return 1; }
    while IFS=$'\t' read -r NAME BRANCH COMMIT DATE URL; do
        case "$NAME" in ''|\#*) continue ;; esac
        if [ "${#WANT[@]}" -gt 0 ]; then
            SEL=false
            for W in "${WANT[@]}"; do [ "$W" = "$NAME" ] && SEL=true; done
            [ "$SEL" = true ] || continue
        elif [ "$NAME" = "core" ] && [ "${MIRROR_INCLUDE_CORE:-false}" != true ]; then
            continue
        fi
        if hydrate_snapshot "$NAME" "$COMMIT"; then DONE=$((DONE + 1)); else FAILED=1; fi
    done < "$VERSIONS_LOCK"
    if [ "$FAILED" -eq 0 ]; then
        log "[hidratar] $DONE snapshot(s) verificados en $MIRRORS_DIR."
    else
        error "[hidratar] hay snapshots sin verificar (ver arriba); no se da por hidratado."
    fi
    return $FAILED
}

# =============================================================================
# extract_arac_inputs — los tres DBC de mod-arac verificados, para el panel
#
# El panel genera los patch-<idioma>-4.MPQ con estos ficheros y no puede leer el
# directorio personal ni mirrors/. Se sacan del snapshot verificado por SHA-256
# (hidratándolo si falta). Sin mod-arac fijado en versions.lock no hay nada que
# hacer y no es un error.
# =============================================================================
extract_arac_inputs() {
    local DEST="$1" COMMIT FILE TMP NAME
    COMMIT=$(awk -F'\t' '$1=="mod-arac" {print $3; exit}' "${VERSIONS_LOCK:-${INSTALLER_DIR}/versions.lock}" 2>/dev/null)
    if [ -z "$COMMIT" ]; then
        info "[arac] mod-arac no está fijado en versions.lock: sin entradas de ARAC para el panel."
        return 0
    fi
    hydrate_snapshot mod-arac "$COMMIT" || return 1
    FILE=$(awk -F'\t' -v c="$COMMIT" '$1=="mod-arac" && $2==c {print $4; exit}' "$MIRRORS_MANIFEST")
    TMP="${DEST}.nuevo"
    rm -rf "$TMP"; mkdir -p "$TMP"
    for NAME in CharBaseInfo.dbc CharStartOutfit.dbc SkillRaceClassInfo.dbc; do
        if ! tar --force-local -xzOf "$MIRRORS_DIR/$FILE" "patch-contents/DBFilesContent/$NAME" > "$TMP/$NAME" 2>/dev/null || [ ! -s "$TMP/$NAME" ]; then
            error "[arac] no se pudo extraer $NAME de $FILE."
            rm -rf "$TMP"; return 1
        fi
    done
    rm -rf "$DEST"
    mv "$TMP" "$DEST"
    log "[arac] DBC de mod-arac verificados en $DEST"
}

# =============================================================================
# restore_from_mirror — recupera un repositorio desde su copia offline
#
# La usa clone_module cuando el clonado desde internet falla: repo borrado,
# renombrado, puesto en privado, o simplemente sin conexión.
# =============================================================================
restore_from_mirror() {
    local NAME="$1"
    local DEST="$2"
    local FILE

    # Se prefiere SIEMPRE la copia del commit que fija versions.lock. Con un
    # 'head -1' a secas, una copia vieja que se hubiera quedado por ahí se
    # restauraría en silencio y acabarías con código distinto del que dice el
    # lock — justo lo contrario de lo que este mecanismo promete.
    local LOCK WANT_COMMIT=""
    LOCK="${VERSIONS_LOCK:-${INSTALLER_DIR}/versions.lock}"
    if [ -f "$LOCK" ]; then
        WANT_COMMIT=$(awk -F'\t' -v n="$NAME" '$1==n {print $3; exit}' "$LOCK")
    fi

    # La edición pública no trae los snapshots: se piden (asset verificado o
    # reconstrucción desde el commit exacto) antes de buscar el fichero.
    if [ -n "$WANT_COMMIT" ] && [ -f "$MIRRORS_MANIFEST" ]; then
        hydrate_snapshot "$NAME" "$WANT_COMMIT" >/dev/null 2>&1 || true
    fi

    FILE=""
    if [ -n "$WANT_COMMIT" ]; then
        local CANDIDATO
        for CANDIDATO in "$MIRRORS_DIR/${NAME}@"*.tar.gz; do
            [ -f "$CANDIDATO" ] || continue
            case "$(basename "$CANDIDATO")" in
                "${NAME}@${WANT_COMMIT:0:12}".tar.gz|"${NAME}@${WANT_COMMIT}".tar.gz)
                    FILE="$CANDIDATO"; break ;;
            esac
        done
    fi

    if [ -z "$FILE" ]; then
        FILE=$(ls -1 "$MIRRORS_DIR/${NAME}@"*.tar.gz 2>/dev/null | head -1)
        if [ -n "$FILE" ] && [ -n "$WANT_COMMIT" ]; then
            warn "[espejo] '$NAME': NO hay copia del commit fijado (${WANT_COMMIT:0:12})."
            warn "  Se usará $(basename "$FILE"), que es OTRA versión. Regenera las"
            warn "  copias con './install.sh --mirror' en cuanto el repo vuelva."
        fi
    fi

    if [ -z "$FILE" ] || [ ! -f "$FILE" ]; then
        return 1
    fi

    # Verifica la integridad antes de usarla: una copia corrupta que se
    # descomprime a medias es peor que no tener copia. Sin manifiesto, o sin
    # una entrada de hash para ESTE fichero exacto, no hay nada que comprobar
    # el hash contra — y usar el tarball sin verificar sería precisamente el
    # fallo silencioso que este mecanismo existe para evitar.
    if [ ! -f "$MIRRORS_MANIFEST" ]; then
        error "[espejo] no existe $MIRRORS_MANIFEST: no se puede verificar $(basename "$FILE")."
        error "  No se usa un tarball sin verificar. Regenera el manifiesto con './install.sh --mirror'."
        return 1
    fi

    local WANT GOT
    WANT=$(awk -F'\t' -v f="$(basename "$FILE")" '$4==f {print $5}' "$MIRRORS_MANIFEST" | head -1)
    if [ -z "$WANT" ]; then
        error "[espejo] $(basename "$FILE") no tiene SHA-256 registrado en MANIFEST.tsv."
        error "  No se usa sin verificar. Regenera el manifiesto con './install.sh --mirror' o corrígelo a mano."
        return 1
    fi

    GOT=$(sha256sum "$FILE" | cut -d' ' -f1)
    if [ "$WANT" != "$GOT" ]; then
        error "[espejo] $(basename "$FILE") no cuadra con su sha256 del manifiesto."
        return 1
    fi

    mkdir -p "$DEST"
    if tar xzf "$FILE" -C "$DEST"; then
        warn "[espejo] '$NAME' restaurado desde la copia offline $(basename "$FILE")."
        warn "  OJO: es una foto del árbol, sin historia de git. No se podrá"
        warn "  actualizar con git hasta que el repositorio original vuelva."
        return 0
    fi

    error "[espejo] falló la extracción de $(basename "$FILE")"
    return 1
}

# =============================================================================
# verify_mirrors_consistency — versions.lock, mirrors/versions.lock,
# MANIFEST.tsv y los tarballs deben describir exactamente lo mismo
#
# Sin esto, es fácil que una actualización parcial (p.ej. sustituir un tarball
# a mano sin regenerar el manifiesto) deje mirrors/ con un fichero que ya no
# tiene entrada de hash, o con un hash que apunta a un fichero que ya no
# existe: el respaldo offline deja de ser fiable sin que nada avise. No
# modifica nada — solo informa. La llaman la fase 3 (aviso, no aborta: los
# espejos son la vía de emergencia, no el camino normal) y el comando
# `doctor`, que sí trata el fallo como error.
# =============================================================================
verify_mirrors_consistency() {
    if [ ! -f "$VERSIONS_LOCK" ]; then
        info "[mirrors] no existe $VERSIONS_LOCK todavía: nada que comprobar."
        return 0
    fi
    if [ ! -f "$MIRRORS_MANIFEST" ]; then
        info "[mirrors] no existe $MIRRORS_MANIFEST todavía: sin copias offline generadas."
        return 0
    fi

    local NAME BRANCH COMMIT DATE URL FAILED=0 NOT_HYDRATED=0
    local MIRROR_COMMIT MANIFEST_FILE MANIFEST_SHA MANIFEST_SIZE TARBALL ACTUAL_SIZE ACTUAL_SHA

    while IFS=$'\t' read -r NAME BRANCH COMMIT DATE URL; do
        case "$NAME" in ''|\#*) continue ;; esac
        if [ "$NAME" = "core" ] && [ "${MIRROR_INCLUDE_CORE:-false}" != true ]; then
            continue   # el core no tiene copia offline a propósito (211 MB)
        fi

        MIRROR_COMMIT=$(awk -F'\t' -v n="$NAME" '$1==n {print $3; exit}' "$MIRRORS_DIR/versions.lock" 2>/dev/null)
        if [ -z "$MIRROR_COMMIT" ]; then
            error "[mirrors] '$NAME' no aparece en mirrors/versions.lock (falta desde el último --mirror)."
            FAILED=1
        elif [ "$MIRROR_COMMIT" != "$COMMIT" ]; then
            error "[mirrors] '$NAME': versions.lock fija ${COMMIT:0:12} pero mirrors/versions.lock tiene ${MIRROR_COMMIT:0:12}."
            FAILED=1
        fi

        MANIFEST_FILE=$(awk -F'\t' -v n="$NAME" -v c="$COMMIT" '$1==n && $2==c {print $4; exit}' "$MIRRORS_MANIFEST")
        if [ -z "$MANIFEST_FILE" ]; then
            error "[mirrors] '$NAME': MANIFEST.tsv no tiene entrada para el commit fijado (${COMMIT:0:12})."
            FAILED=1
            continue
        fi

        TARBALL="$MIRRORS_DIR/$MANIFEST_FILE"
        if [ ! -f "$TARBALL" ]; then
            # La edición pública no lleva los snapshots: se hidratan con
            # './install.sh --hidratar'. Su ausencia sólo es un fallo si se
            # pide comprobar con los ficheros presentes (MIRRORS_REQUIRE_FILES=true).
            if [ "${MIRRORS_REQUIRE_FILES:-false}" = true ]; then
                error "[mirrors] '$NAME': MANIFEST.tsv referencia $MANIFEST_FILE, que no existe en mirrors/ (./install.sh --hidratar)."
                FAILED=1
            else
                NOT_HYDRATED=$((NOT_HYDRATED + 1))
            fi
            continue
        fi

        MANIFEST_SIZE=$(awk -F'\t' -v n="$NAME" -v c="$COMMIT" '$1==n && $2==c {print $3; exit}' "$MIRRORS_MANIFEST")
        ACTUAL_SIZE=$(stat -c%s "$TARBALL" 2>/dev/null || echo "")
        if [ -n "$MANIFEST_SIZE" ] && [ -n "$ACTUAL_SIZE" ] && [ "$ACTUAL_SIZE" != "$MANIFEST_SIZE" ]; then
            error "[mirrors] '$NAME': $MANIFEST_FILE mide ${ACTUAL_SIZE} bytes, MANIFEST.tsv dice ${MANIFEST_SIZE}."
            FAILED=1
        fi

        MANIFEST_SHA=$(awk -F'\t' -v n="$NAME" -v c="$COMMIT" '$1==n && $2==c {print $5; exit}' "$MIRRORS_MANIFEST")
        if [ -z "$MANIFEST_SHA" ]; then
            error "[mirrors] '$NAME': $MANIFEST_FILE no tiene SHA-256 registrado en MANIFEST.tsv."
            FAILED=1
            continue
        fi
        ACTUAL_SHA=$(sha256sum "$TARBALL" | cut -d' ' -f1)
        if [ "$ACTUAL_SHA" != "$MANIFEST_SHA" ]; then
            error "[mirrors] '$NAME': $MANIFEST_FILE no cuadra con su SHA-256 del manifiesto."
            FAILED=1
        fi
    done < "$VERSIONS_LOCK"

    if [ "$FAILED" -eq 0 ]; then
        if [ "$NOT_HYDRATED" -gt 0 ]; then
            log "[mirrors] versions.lock, mirrors/versions.lock y MANIFEST.tsv coinciden; $NOT_HYDRATED snapshot(s) sin hidratar (./install.sh --hidratar)."
        else
            log "[mirrors] versions.lock, mirrors/versions.lock, MANIFEST.tsv y los tarballs coinciden."
        fi
    fi
    return $FAILED
}
