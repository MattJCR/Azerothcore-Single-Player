#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Hidratación de snapshots: reconstrucción desde un upstream local, descarte de
# un fichero alterado, rechazo de un SHA-256 que no cuadra y detección de
# entradas ausentes o alteradas en verify_mirrors_consistency.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_PARENT=$(cd "${TMPDIR:-/tmp}" && pwd -P)
T=$(mktemp -d "$TEST_PARENT/azeroth-hidratar.XXXXXX")
trap 'case "$T" in "$TEST_PARENT"/azeroth-hidratar.*) rm -rf -- "$T" ;; esac' EXIT

export AC_DIR="$T/ac" INSTALLER_DIR="$T/inst" AC_TEST_ALLOW_LOCAL_URL=1
mkdir -p "$INSTALLER_DIR/mirrors"
source "$ROOT/lib/utils.sh"
INSTALLER_DIR="$T/inst"   # utils.sh lo fija al repositorio: no tocar mirrors/ reales
source "$ROOT/lib/versions.sh"
source "$ROOT/lib/mirrors.sh"
case "$MIRRORS_DIR" in "$T"/*) ;; *) echo "ERROR: MIRRORS_DIR fuera del temporal" >&2; exit 1 ;; esac
INSTALL_LOG="$T/install.log"
git() { command git -c core.autocrlf=false "$@"; }

# Upstream local con un commit.
UP="$T/up"; command git init -q "$UP"
printf 'uno\n' > "$UP/a.txt"; printf 'dos\r\n' > "$UP/b.txt"
command git -C "$UP" add -A
command git -C "$UP" -c user.name=T -c user.email=t@localhost commit -qm base
COMMIT=$(command git -C "$UP" rev-parse HEAD)
FILE="mod-prueba@${COMMIT:0:12}.tar.gz"
git -C "$UP" archive --format=tar "$COMMIT" | gzip -9 > "$T/ref.tgz"
SHA=$(sha256sum "$T/ref.tgz" | cut -d' ' -f1); SIZE=$(stat -c%s "$T/ref.tgz")
printf 'mod-prueba\tmaster\t%s\t2026-01-01\t%s\n' "$COMMIT" "$UP" > "$INSTALLER_DIR/versions.lock"
cp "$INSTALLER_DIR/versions.lock" "$MIRRORS_DIR/versions.lock"
printf 'mod-prueba\t%s\t%s\t%s\t%s\t%s\n' "$COMMIT" "$SIZE" "$FILE" "$SHA" "$UP" > "$MIRRORS_MANIFEST"
VERSIONS_LOCK="$INSTALLER_DIR/versions.lock"

fail() { echo "ERROR: $*" >&2; exit 1; }

# Sin snapshot: la verificación sólo avisa, y con --hidratados falla.
verify_mirrors_consistency >/dev/null || fail "ausente debería ser aviso"
if MIRRORS_REQUIRE_FILES=true verify_mirrors_consistency >/dev/null 2>&1; then fail "ausente con REQUIRE debería fallar"; fi

hydrate_mirrors >/dev/null || fail "no hidrató desde el upstream"
[ "$(sha256sum "$MIRRORS_DIR/$FILE" | cut -d' ' -f1)" = "$SHA" ] || fail "el snapshot no coincide con el SHA-256"
MIRRORS_REQUIRE_FILES=true verify_mirrors_consistency >/dev/null || fail "hidratado debería verificar"

# Alterado: la verificación falla y la hidratación lo reconstruye.
printf 'x' >> "$MIRRORS_DIR/$FILE"
if verify_mirrors_consistency >/dev/null 2>&1; then fail "un snapshot alterado debería fallar"; fi
hydrate_mirrors >/dev/null || fail "no reconstruyó el snapshot alterado"
verify_mirrors_consistency >/dev/null || fail "tras reconstruir debería verificar"

# SHA-256 del manifiesto que no corresponde: no se instala nada.
rm -f "$MIRRORS_DIR/$FILE"
sed -i "s/$SHA/$(printf '0%.0s' $(seq 64))/" "$MIRRORS_MANIFEST"
if hydrate_mirrors >/dev/null 2>&1; then fail "aceptó un SHA-256 incorrecto"; fi
[ ! -e "$MIRRORS_DIR/$FILE" ] || fail "dejó un snapshot sin verificar"
ls "$MIRRORS_DIR"/.hidratar.* >/dev/null 2>&1 && fail "dejó temporales"

# Commit que el manifiesto no declara.
: > "$MIRRORS_MANIFEST"
if hydrate_snapshot mod-prueba "$COMMIT" >/dev/null 2>&1; then fail "aceptó un commit sin entrada"; fi
echo 'Hidratación de snapshots: reconstrucción, alteración, SHA-256 incorrecto y entradas ausentes OK'
