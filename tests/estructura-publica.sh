#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Estructura de la edición pública: lo que debe estar, lo que no debe aparecer y
# que los scripts viajen con fin de línea Unix. Se ejecuta sobre la raíz del
# checkout (ZIP o repositorio); no necesita Git ni red.
set -euo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT"
FAILED=0
fail() { echo "ERROR: $*" >&2; FAILED=1; }

# Entradas imprescindibles (una ausente es un paquete incompleto).
for f in LICENSE NOTICE THIRD-PARTY-NOTICES.txt REUSE.toml LICENSES/AGPL-3.0-or-later.txt \
         LICENSES/GPL-3.0-only.txt LICENSES/LicenseRef-Upstream-Undeclared.txt LICENSES/AGPL-3.0-only.txt \
         LICENSES/GPL-2.0-only.txt LICENSES/GPL-2.0-or-later.txt LICENSES/MIT.txt \
         cliente/Interface/AddOns/MultiBot/LICENSE \
         README.md README_ES.md README_EN.md INSTALL_ES.md INSTALL_EN.md CONTRIBUTING.md CONTRIBUTING_ES.md CONTRIBUTING_EN.md REFERENCES.md CHANGELOG.md \
         .github/CODEOWNERS install.sh config.sh \
         versions.lock addons.lock mirrors/MANIFEST.tsv mirrors/versions.lock \
         web-panel/package.json web-panel/package-lock.json \
         web-panel/addons/catalog.json web-panel/addons/fuentes.json web-panel/addons/arbol.tsv \
         web-panel/addons/descriptions-es.json web-panel/addons/patches.json \
         cliente/manifest.tsv cliente/instalar-cliente.ps1 \
         cliente/Interface/AddOns/MultiBot/MultiBot.toc cliente/Interface/AddOns/ServerHelp/ServerHelp.toc \
         tools/construir-parche-cliente-items.py web-panel/tools/build-addons.mjs; do
    [ -f "$f" ] || fail "falta $f"
done
for d in lib scripts/phases modules patches patches-cliente tests tools; do
    [ -d "$d" ] || fail "falta la carpeta $d"
done

# Únicos Markdown permitidos: los documentos comunes (README, INSTALL y CONTRIBUTING en tres, dos y tres ficheros; REFERENCES y CHANGELOG) (los addons y node_modules
# traen los suyos y se ignoran).
while IFS= read -r f; do
    case "$f" in ./README.md|./README_ES.md|./README_EN.md|./INSTALL_ES.md|./INSTALL_EN.md|./CONTRIBUTING.md|./CONTRIBUTING_ES.md|./CONTRIBUTING_EN.md|./REFERENCES.md|./CHANGELOG.md) ;; *) fail "Markdown no permitido: $f" ;; esac
done < <(find . -name '*.md' \
    -not -path './web-panel/node_modules/*' -not -path './cliente/Interface/AddOns/*' \
    -not -path './.instalacion/*' -not -path './.git/*')

# Nada de datos locales, secretos ni binarios del juego en el árbol versionable.
while IFS= read -r f; do fail "fichero que no debe viajar: $f"; done < <(find . \( -name '.env' -o -name '*.pem' -o -name '*.key' -o -name '*.log' -o -name '*.sql.gz' -o -name '*.old' \) \
    -not -path './web-panel/node_modules/*' -not -path './.git/*' -not -path './.instalacion/*' -not -path './cliente/Interface/AddOns/*')
while IFS= read -r f; do fail "recurso del juego en el árbol: $f"; done < <(find ./cliente/Data ./web-panel/public/assets/item-icons ./web-panel/public/assets/maps -type f 2>/dev/null \
    \( -iname '*.mpq' -o -name '*.webp' -o -name '*.jpg' \) | head -5 || true)
while IFS= read -r f; do fail "fichero de más de 100 MiB: $f"; done < <(find . -type f -size +100M \
    -not -path './.git/*' -not -path './.instalacion/*' -not -path './mirrors/*' -not -path './web-panel/node_modules/*' -not -path './cliente/Interface/AddOns/*' | head -5)

# Fin de línea Unix en lo que ejecuta Linux.
while IFS= read -r f; do
    if grep -q $'\r' "$f"; then fail "CRLF en $f"; fi
done < <(find . -type f \( -name '*.sh' -o -name '*.service' -o -name '*.tsv' -o -name '*.lock' -o -name '*.dist' \) \
    -not -path './web-panel/node_modules/*' -not -path './.git/*' -not -path './.instalacion/*' -not -path './mirrors/*' -not -path './cliente/Interface/AddOns/*')

# Cada línea de MANIFEST.tsv tiene sus seis campos y un SHA-256 con forma válida.
bad=$(awk -F'\t' '!/^#/ && NF && (NF < 6 || $5 !~ /^[0-9a-f]{64}$/ || $2 !~ /^[0-9a-f]{40}$/)' mirrors/MANIFEST.tsv | wc -l)
[ "$bad" -eq 0 ] || fail "MANIFEST.tsv tiene $bad línea(s) mal formadas"

# Licencias: los textos coinciden con los de referencia, cada snapshot de terceros
# figura en el aviso y todo el código propio y cada parche llevan su cabecera.
cmp -s LICENSE LICENSES/AGPL-3.0-or-later.txt || fail "LICENSE difiere de LICENSES/AGPL-3.0-or-later.txt"
cmp -s cliente/Interface/AddOns/MultiBot/LICENSE LICENSES/GPL-3.0-only.txt \
    || fail "MultiBot/LICENSE difiere de LICENSES/GPL-3.0-only.txt"
while IFS=$'\t' read -r nombre _; do
    case "$nombre" in ''|\#*) continue ;; esac
    grep -q -F "$nombre" THIRD-PARTY-NOTICES.txt || fail "THIRD-PARTY-NOTICES.txt no menciona $nombre"
done < mirrors/MANIFEST.tsv
while IFS= read -r f; do
    head -n 6 "$f" | grep -q 'SPDX-License''-Identifier:' || fail "sin cabecera SPDX: $f"
done < <(find install.sh lib scripts tools tests modules web-panel/src web-panel/public web-panel/test \
    web-panel/tools web-panel/deploy -type f \
    \( -name '*.sh' -o -name '*.py' -o -name '*.js' -o -name '*.mjs' -o -name '*.cpp' -o -name '*.h' -o -name '*.ps1' \) \
    -not -path '*/node_modules/*' -not -path '*/__pycache__/*' -not -path 'web-panel/public/assets/*' 2>/dev/null)
while IFS= read -r f; do
    head -n 1 "$f" | grep -q '^Licencia:' || fail "parche sin línea de licencia: $f"
done < <(find patches patches-cliente -type f -name '*.patch')

# Autoría y privacidad en los documentos comunes: ni agentes de IA ni sus empresas (la única
# excepción es la nota de INSTALL_ES.md y de su traducción INSTALL_EN.md) ni nombres propios del desarrollador.
for f in README.md README_ES.md README_EN.md CONTRIBUTING.md CONTRIBUTING_ES.md CONTRIBUTING_EN.md REFERENCES.md CHANGELOG.md; do
    if grep -n -i -E 'anthropic|chatgpt|openai|\bclaude\b|codex|co-authored-by|generated with' "$f" >/dev/null; then
        fail "$f menciona a un agente de IA o a su empresa"
    fi
done
for f in README.md README_ES.md README_EN.md INSTALL_ES.md INSTALL_EN.md CONTRIBUTING.md CONTRIBUTING_ES.md CONTRIBUTING_EN.md REFERENCES.md CHANGELOG.md; do
    if grep -n -E '\bMatt\b|\bmateo\b' "$f" >/dev/null; then fail "$f contiene un nombre propio del desarrollador"; fi
done

[ "$FAILED" -eq 0 ] && echo "Estructura pública OK" || exit 1
