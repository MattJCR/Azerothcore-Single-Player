#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Pila del core: aplicar, reaplicar sin leer extras operativos y rechazar cambios ajenos.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_PARENT=$(cd "${TMPDIR:-/tmp}" && pwd -P)
TEST_DIR=$(mktemp -d "$TEST_PARENT/azeroth-core-patches.XXXXXX")
cleanup() {
    case "$TEST_DIR" in "$TEST_PARENT"/azeroth-core-patches.*) rm -rf -- "$TEST_DIR" ;; esac
}
trap cleanup EXIT
export AC_DIR="$TEST_DIR/core"
mkdir -p "$AC_DIR"
source "$ROOT/lib/utils.sh"
git -C "$AC_DIR" init -q
printf 'original\n' > "$AC_DIR/source.cpp"
git -C "$AC_DIR" add source.cpp
git -C "$AC_DIR" -c user.name=Test -c user.email=test@localhost commit -qm base
printf 'patched\n' > "$AC_DIR/source.cpp"
git -C "$AC_DIR" diff > "$TEST_DIR/fix.patch"
git -C "$AC_DIR" restore source.cpp
mkdir "$AC_DIR/extras"
printf 'dato operativo\n' > "$AC_DIR/extras/no-tocar"
apply_module_patches core "$TEST_DIR/fix.patch"
apply_module_patches core "$TEST_DIR/fix.patch"
test "$(cat "$AC_DIR/source.cpp")" = patched
test "$(cat "$AC_DIR/extras/no-tocar")" = 'dato operativo'
printf 'ajeno\n' > "$AC_DIR/source.cpp"
if apply_module_patches core "$TEST_DIR/fix.patch"; then
    echo 'ERROR: aceptó cambios ajenos' >&2
    exit 1
fi
test "$(cat "$AC_DIR/source.cpp")" = ajeno
echo 'Parches del core: aplicación, idempotencia, extras y rechazo de cambios ajenos OK'
