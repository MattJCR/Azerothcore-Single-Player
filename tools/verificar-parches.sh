#!/bin/bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  tools/verificar-parches.sh — ¿aplican los parches propios sobre el commit
#  fijado, SIN clonar nada del repositorio original?
#
#  EL PROBLEMA
#  apply_all_source_patches (lib/utils.sh) sólo se puede comprobar de verdad
#  instalando el módulo entero (clonar, fijar versión, aplicar). Eso hace
#  falta en la VM real, pero para una comprobación rápida — a mano o en CI, en
#  cualquier máquina, sin red hacia GitHub — sobra: el commit exacto que
#  versions.lock fija ya está en mirrors/ como snapshot (git archive). Solo
#  hace falta reconstruirlo en un directorio de usar y tirar, con un commit
#  local para que `git apply` tenga un árbol de trabajo donde operar (no le
#  hace falta el historial real).
#
#  LA SOLUCIÓN
#  Por cada patches/mod-*/ con parches: busca el commit en versions.lock,
#  localiza y verifica el tarball de mirrors/ (mismo criterio de SHA-256 que
#  restore_from_mirror), lo extrae, y aplica los parches EN ORDEN, cada uno de
#  verdad antes de comprobar el siguiente — igual que apply_module_patches,
#  para detectar tanto un parche que ya no aplica solo como uno que sólo deja
#  de aplicar tras los anteriores.
#
#  Solo lee el repositorio (no toca nada de fuera de un directorio temporal).
#  Salida distinta de cero si algún módulo no tiene commit/tarball/hash
#  verificable, o si algún parche no aplica.
# =============================================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$SCRIPT_DIR/config.sh" >/dev/null 2>&1
source "$SCRIPT_DIR/lib/utils.sh"
source "$SCRIPT_DIR/lib/versions.sh"
source "$SCRIPT_DIR/lib/mirrors.sh"

FAILED=0
CHECKED=0

for MODULE_PATCH_DIR in "$SCRIPT_DIR"/patches/*/; do
    [ -d "$MODULE_PATCH_DIR" ] || continue
    MODULE="$(basename "$MODULE_PATCH_DIR")"

    # Mismo filtro que apply_all_source_patches: sólo carpetas mod-* llegan a
    # ser un módulo de compilación real con copia en mirrors/. arac,
    # custom-items y locales-es* se aplican por otras vías (ver patches/README.md).
    case "$MODULE" in mod-*|core) ;; *) continue ;; esac

    PATCHES=()
    while IFS= read -r PATCH; do
        [ -n "$PATCH" ] && PATCHES+=("$PATCH")
    done < <(find "$MODULE_PATCH_DIR" -maxdepth 1 -type f -name '*.patch' | sort)
    [ "${#PATCHES[@]}" -gt 0 ] || continue

    CHECKED=$((CHECKED + 1))
    COMMIT=$(awk -F'\t' -v m="$MODULE" '$1==m {print $3; exit}' "$VERSIONS_LOCK" 2>/dev/null)
    if [ -z "$COMMIT" ]; then
        error "[verificar-parches] '$MODULE': no está en versions.lock, no se puede comprobar."
        FAILED=1
        continue
    fi

    # La edición pública no trae los snapshots: se hidratan (asset o upstream,
    # siempre verificados por SHA-256) antes de buscar el tarball. Sin red ni
    # asset esto falla; no se omite el módulo.
    hydrate_snapshot "$MODULE" "$COMMIT" >/dev/null 2>&1 || true

    TARBALL=""
    for CANDIDATE in "$MIRRORS_DIR/${MODULE}@"*.tar.gz; do
        [ -f "$CANDIDATE" ] || continue
        case "$(basename "$CANDIDATE")" in
            "${MODULE}@${COMMIT:0:12}.tar.gz"|"${MODULE}@${COMMIT}.tar.gz")
                TARBALL="$CANDIDATE"; break ;;
        esac
    done
    if [ -z "$TARBALL" ]; then
        error "[verificar-parches] '$MODULE': sin copia en mirrors/ del commit fijado (${COMMIT:0:12})."
        FAILED=1
        continue
    fi

    WANT_SHA=$(awk -F'\t' -v f="$(basename "$TARBALL")" '$4==f {print $5}' "$MIRRORS_MANIFEST" 2>/dev/null | head -1)
    if [ -z "$WANT_SHA" ] || [ "$WANT_SHA" != "$(sha256sum "$TARBALL" | cut -d' ' -f1)" ]; then
        error "[verificar-parches] '$MODULE': $(basename "$TARBALL") sin SHA-256 verificable en MANIFEST.tsv."
        FAILED=1
        continue
    fi

    WORKDIR="$(mktemp -d)"
    tar xzf "$TARBALL" -C "$WORKDIR"
    # autocrlf=false: es un árbol de usar y tirar que nadie vuelve a
    # comprobar (aquí sí importan los bytes exactos, no la comodidad de
    # editarlo); sin esto, un core.autocrlf=true global (habitual en Windows)
    # mete avisos de conversión de línea en cada `add`.
    git -C "$WORKDIR" -c core.autocrlf=false init -q
    git -C "$WORKDIR" -c core.autocrlf=false -c user.email=doctor@localhost -c user.name=doctor add -A
    git -C "$WORKDIR" -c user.email=doctor@localhost -c user.name=doctor commit -q -m "mirror ${MODULE}@${COMMIT:0:12}"

    MODULE_FAILED=0
    for PATCH in "${PATCHES[@]}"; do
        # Se lee el blob de HEAD, no el fichero del árbol de trabajo: un
        # checkout local con core.autocrlf=true puede dejar CRLF en el árbol
        # de trabajo aunque el commit (y .gitattributes: "*.patch text
        # eol=lf") sean LF puro. Comprobar el árbol de trabajo directamente
        # dio un falso positivo real la primera vez que se escribió esto.
        CANON="$WORKDIR/.canon.patch"
        # Sin repositorio (ZIP o checkout exportado) se lee el fichero tal cual:
        # el exportador y los .gitattributes ya lo dejan con sus bytes exactos.
        if git -C "$SCRIPT_DIR" rev-parse --verify -q HEAD >/dev/null 2>&1; then
            if ! git -C "$SCRIPT_DIR" show "HEAD:${PATCH#"$SCRIPT_DIR"/}" > "$CANON" 2>/dev/null; then
                error "[verificar-parches] '$MODULE': no se pudo leer $(basename "$PATCH") desde HEAD (¿sin comitear?)."
                FAILED=1
                MODULE_FAILED=1
                break
            fi
        else
            cp "$PATCH" "$CANON"
        fi

        # core.autocrlf=false también al aplicar: con autocrlf=true (Git para
        # Windows) los parches con líneas CRLF no aplican aunque en la VM sí.
        if git -C "$WORKDIR" -c core.autocrlf=false apply --unidiff-zero --check "$CANON" 2>/dev/null; then
            git -C "$WORKDIR" -c core.autocrlf=false apply --unidiff-zero "$CANON"
        else
            error "[verificar-parches] '$MODULE': $(basename "$PATCH") NO aplica sobre ${COMMIT:0:12}."
            FAILED=1
            MODULE_FAILED=1
            break
        fi
    done
    rm -rf "$WORKDIR"

    [ "$MODULE_FAILED" -eq 1 ] || log "[verificar-parches] '$MODULE': ${#PATCHES[@]} parche(s) aplican limpios sobre ${COMMIT:0:12}."
done

if [ "$CHECKED" -eq 0 ]; then
    info "[verificar-parches] Ningún módulo con parches propios que comprobar."
elif [ "$FAILED" -eq 0 ]; then
    log "[verificar-parches] ${CHECKED} módulo(s) con parches, todos aplican limpios sobre el commit fijado."
fi

exit $FAILED
