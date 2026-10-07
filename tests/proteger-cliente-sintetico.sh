#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Un pull request no puede cambiar lo que ya existe en tools/cliente-sintetico/ (es la vara
# de medir de las pruebas en vivo): sólo se admiten ficheros nuevos y líneas añadidas.
#
#   proteger-cliente-sintetico.sh <base> <cabeza>   compara <base>...<cabeza> (como un PR)
#   proteger-cliente-sintetico.sh --autotest        prueba el propio comprobador en un repo temporal
#
# Es una ayuda para la revisión, no la garantía: un PR también podría tocar este fichero o el
# flujo que lo ejecuta; por eso CODEOWNERS reserva .github/ y tests/ a quien mantiene el proyecto
# y cada PR se revisa a mano. Código de salida: 0 sin cambios prohibidos, 1 con ellos, 2 uso.
set -euo pipefail
CARPETA="tools/cliente-sintetico"

comprobar() {
    local base="$1" cabeza="$2" bad=0 estado ruta borradas
    while IFS=$'\t' read -r estado ruta; do
        [ -n "$estado" ] || continue
        case "$estado" in
            A) ;;
            M)
                # numstat de ese fichero: «añadidas<TAB>borradas<TAB>ruta»; «-» en binarios.
                borradas=$(git diff --numstat "$base...$cabeza" -- "$ruta" | cut -f2)
                if [ "$borradas" != "0" ]; then
                    echo "ERROR: se modifican o borran líneas de $ruta ($borradas)" >&2; bad=1
                fi ;;
            *) echo "ERROR: $ruta: cambio no permitido ($estado: borrado, renombrado o cambio de tipo)" >&2; bad=1 ;;
        esac
    done < <(git diff --no-renames --name-status "$base...$cabeza" -- "$CARPETA")
    # Un cambio de permisos no borra líneas pero tampoco es «sólo añadir».
    if git diff --summary --no-renames "$base...$cabeza" -- "$CARPETA" | grep -q 'mode change'; then
        echo "ERROR: cambio de permisos en $CARPETA" >&2; bad=1
    fi
    return $bad
}

autotest() {
    local d rc fallos=0
    d=$(mktemp -d)
    trap 'rm -rf "$d"' RETURN
    cd "$d"
    git init -q . && git config user.email t@t && git config user.name t && git config core.autocrlf false
    mkdir -p "$CARPETA/c"
    printf 'uno\ndos\n' > "$CARPETA/verificar.py"
    printf 'x\n' > "$CARPETA/c/a.py"
    printf 'fuera\n' > otro.txt
    git add -A && git commit -qm base
    local base; base=$(git rev-parse HEAD)
    caso() {  # caso <nombre> <esperado 0|1> <orden...>
        local nombre="$1" esperado="$2"; shift 2
        git checkout -q "$base" && git checkout -q -b "t-$nombre"
        "$@"
        git add -A && git commit -qm "$nombre" --allow-empty
        set +e; comprobar "$base" HEAD >/dev/null 2>&1; rc=$?; set -e
        if [ "$rc" -ne "$esperado" ]; then echo "FALLA el caso $nombre: rc=$rc, esperado $esperado" >&2; fallos=1; fi
    }
    caso fuera 0 sh -c 'echo mas >> otro.txt'
    caso fichero-nuevo 0 sh -c "echo n > $CARPETA/c/nuevo.py"
    caso lineas-anadidas 0 sh -c "echo tres >> $CARPETA/verificar.py"
    caso linea-editada 1 sh -c "sed -i 's/dos/DOS/' $CARPETA/verificar.py"
    caso linea-borrada 1 sh -c "sed -i '1d' $CARPETA/verificar.py"
    caso fichero-borrado 1 git rm -q "$CARPETA/c/a.py"
    caso renombrado 1 git mv "$CARPETA/c/a.py" "$CARPETA/c/b.py"
    caso permisos 1 sh -c "chmod +x $CARPETA/verificar.py && git update-index --chmod=+x $CARPETA/verificar.py"
    [ "$fallos" -eq 0 ] && echo "proteger-cliente-sintetico: autotest OK" || return 1
}

if [ "${1:-}" = "--autotest" ]; then autotest; exit $?; fi
[ $# -eq 2 ] || { echo "uso: $0 <base> <cabeza> | --autotest" >&2; exit 2; }
if comprobar "$1" "$2"; then echo "Cliente sintético intacto (sólo se añade)"; else exit 1; fi
