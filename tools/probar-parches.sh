#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# =============================================================================
#  tools/probar-parches.sh — banco de pruebas de apply_module_patches (lib/utils.sh)
#
#  Monta un repositorio git temporal con tres parches apilados que tocan las
#  mismas lineas (el caso de mod-congrats-on-level del 05/09/2026) y comprueba
#  seis escenarios: arbol limpio, pila completa, pila parcial con parche nuevo,
#  cambios ajenos, upstream cambiado y limpieza de los worktrees temporales.
#  No toca la VM ni ~/azerothcore: todo ocurre en un directorio temporal.
#
#  Uso (desde Windows con Git Bash, o en la VM):
#      bash tools/probar-parches.sh "<proyecto>"
#      bash tools/probar-parches.sh ~/azerothcore-installer
#  Sale con el numero de comprobaciones fallidas (0 = todo bien).
# =============================================================================
set -u
REPO_ROOT="${1:?ruta del repositorio del instalador}"
T="$(mktemp -d)"
export AC_DIR="$T/ac"
export INSTALLER_DIR="$REPO_ROOT"
export HOME="$T"
mkdir -p "$AC_DIR/modules" "$T/patches/mod-x"
source "$REPO_ROOT/lib/utils.sh"

M="$AC_DIR/modules/mod-x"
git init -q "$M"; git -C "$M" config user.email t@t; git -C "$M" config user.name t
printf 'uno\ndos\ntres\ncuatro\ncinco\nseis\nsiete\n' > "$M/a.txt"
git -C "$M" add -A; git -C "$M" commit -q -m base

# Parche 01: cambia "tres" por "TRES". 02: sobre eso, "cuatro" por "CUATRO" (mismo
# contexto que el 01, asi la inversa del 01 deja de casar). 03: anade un fichero.
mk() { git -C "$M" diff > "$T/patches/mod-x/$1"; git -C "$M" add -A; git -C "$M" commit -q -m "$1"; }
sed -i 's/^tres$/TRES/' "$M/a.txt"; mk 01-tres.patch
sed -i 's/^cuatro$/CUATRO/' "$M/a.txt"; mk 02-cuatro.patch
echo nuevo > "$M/nuevo.txt"; git -C "$M" add -N nuevo.txt; mk 03-nuevo.patch
git -C "$M" reset -q --hard HEAD~3
P1="$T/patches/mod-x/01-tres.patch"; P2="$T/patches/mod-x/02-cuatro.patch"; P3="$T/patches/mod-x/03-nuevo.patch"
FALLOS=0
caso() { echo; echo "=== $1"; }
esperar() { if [ "$1" = "$2" ]; then echo "    OK ($3)"; else echo "    FALLO: esperaba $1 y salio $2 ($3)"; FALLOS=$((FALLOS+1)); fi; }

caso "A. arbol limpio: aplica los tres"
apply_module_patches mod-x "$P1" "$P2" "$P3"; esperar 0 $? "codigo de salida"
esperar "TRES" "$(sed -n 3p "$M/a.txt")" "01 puesto"; esperar "CUATRO" "$(sed -n 4p "$M/a.txt")" "02 puesto"; esperar nuevo "$(cat "$M/nuevo.txt")" "03 puesto"

caso "B. todo aplicado (la inversa del 01 NO casa): ya estaban los tres, sin [ERR]"
OUT=$(apply_module_patches mod-x "$P1" "$P2" "$P3" 2>&1); RC=$?; echo "$OUT" | sed 's/^/    /'
esperar 0 $RC "codigo de salida"; esperar 3 "$(echo "$OUT" | grep -c 'ya estaba aplicado')" "tres 'ya estaba'"
esperar 1 "$(git -C "$M" apply --reverse --check "$P1" >/dev/null 2>&1; echo $?)" "la inversa del 01 falla de verdad (el caso viejo)"

caso "C. pila parcial (01+02) y el 03 es nuevo: aplica solo el 03"
rm "$M/nuevo.txt"
OUT=$(apply_module_patches mod-x "$P1" "$P2" "$P3" 2>&1); RC=$?; echo "$OUT" | sed 's/^/    /'
esperar 0 $RC "codigo de salida"; esperar 2 "$(echo "$OUT" | grep -c 'ya estaba')" "dos 'ya estaba'"; esperar 1 "$(echo "$OUT" | grep -c 'parche aplicado: 03')" "03 aplicado"

caso "D. cambios ajenos en el arbol: avisa y no toca nada"
git -C "$M" checkout -q -- .; rm -f "$M/nuevo.txt"; sed -i 's/^siete$/SIETE/' "$M/a.txt"
OUT=$(apply_module_patches mod-x "$P1" "$P2" "$P3" 2>&1); RC=$?; echo "$OUT" | sed 's/^/    /'
esperar 1 $RC "codigo de salida"; esperar "SIETE" "$(sed -n 7p "$M/a.txt")" "no ha tocado el arbol"; esperar "tres" "$(sed -n 3p "$M/a.txt")" "no ha aplicado el 01"

caso "E. el upstream cambio las lineas: el 01 NO aplica"
git -C "$M" checkout -q -- .; printf 'uno\ndos\nTRESCAMBIADO\ncuatro\ncinco\nseis\nsiete\n' > "$M/a.txt"; git -C "$M" commit -q -am upstream
OUT=$(apply_module_patches mod-x "$P1" "$P2" "$P3" 2>&1); RC=$?; echo "$OUT" | sed 's/^/    /'
esperar 1 $RC "codigo de salida"; esperar 1 "$(echo "$OUT" | grep -c 'NO aplica')" "aviso NO aplica"

caso "F. worktrees temporales limpiados"
esperar 1 "$(git -C "$M" worktree list | wc -l)" "solo el worktree principal"

echo; echo "FALLOS: $FALLOS"; rm -rf "$T"; exit $FALLOS
