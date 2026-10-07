# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Traducciones de los módulos propios (modules/shared/ModLocale.h).

Cada módulo con mensajes al jugador envuelve su texto español en
`ModLocale::L(ctx, "literal")` y lleva su tabla en `src/*_locale.h`. Esta prueba
lee el código (no compila nada) y exige que:
  - cada literal envuelto tenga traducción;
  - no sobre ninguna entrada (las de después de la marca «en tiempo de ejecución»
    son textos que se eligen por código y deben seguir existiendo como literal);
  - los `{}` casen entre español e inglés (el inglés puede usar {0}, {1}...);
  - no haya claves repetidas ni traducciones vacías.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULES = ROOT / "modules"
LIT = r'"(?:[^"\\\n]|\\.)*"'
LITS = rf"{LIT}(?:\s*{LIT})*"
DYNAMIC_MARK = "Textos que se eligen en tiempo de ejecución"


def unescape(body):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}.get(m.group(1), m.group(1)), body)


def join_literals(text):
    return "".join(unescape(part[1:-1]) for part in re.findall(LIT, text))


def strip_comments(source):
    source = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def parse_table(path):
    """[(es, en, dinámica)] de un *_locale.h."""
    raw = path.read_text(encoding="utf-8")
    cut = raw.find(DYNAMIC_MARK)
    entries = []
    for match in re.finditer(rf"\{{\s*({LITS})\s*,\s*({LITS})\s*\}}", strip_comments(raw)):
        entries.append((join_literals(match.group(1)), join_literals(match.group(2)), match.start()))
    # La marca está en un comentario, que se quitó: se localiza por el texto de la
    # primera entrada que aparece después de ella en el fichero original.
    dynamic_from = None
    if cut != -1:
        after = raw[cut:]
        first = re.search(rf"\{{\s*({LITS})\s*,", after)
        dynamic_from = join_literals(first.group(1)) if first else None
    result, dynamic = [], False
    for es, en, _ in entries:
        if es == dynamic_from:
            dynamic = True
        result.append((es, en, dynamic))
    return result


def placeholders(text):
    """(automáticos, índices manuales) de un formato fmt."""
    auto = len(re.findall(r"\{\}", text))
    manual = sorted(set(int(n) for n in re.findall(r"\{(\d+)\}", text)))
    return auto, manual


def modules_with_tables():
    return sorted(path.parent.parent for path in MODULES.glob("*/src/*_locale.h"))


class ModLocaleTables(unittest.TestCase):
    def test_hay_modulos_traducidos(self):
        self.assertTrue(modules_with_tables())

    def test_cada_modulo(self):
        for module in modules_with_tables():
            with self.subTest(module=module.name):
                tables = list((module / "src").glob("*_locale.h"))
                self.assertEqual(len(tables), 1, "una tabla por módulo")
                entries = parse_table(tables[0])
                keys = [es for es, _, _ in entries]
                self.assertEqual(len(keys), len(set(keys)), "claves repetidas")

                code = "\n".join(strip_comments(p.read_text(encoding="utf-8")) for p in sorted((module / "src").glob("*.cpp")))
                # Sin estas tres líneas la tabla compila pero nunca se registra (o ni compila).
                self.assertIn(f'#include "{tables[0].name}"', code, "el .cpp no incluye su tabla")
                self.assertIn('#include "ModLocale.h"', code, "el .cpp no incluye ModLocale.h")
                self.assertIn("ModLocale::Register(", code, "la tabla no se registra en AddSC_*")
                wrapped = {join_literals(m.group(1)) for m in re.finditer(rf"ModLocale::L\(\s*[^,()]+(?:\([^()]*\))?\s*,\s*({LITS})", code)}
                # Los textos dinámicos pueden vivir en una cabecera compartida (p. ej. los motivos de BotPopulationCoordinator.h).
                shared = "\n".join(strip_comments(p.read_text(encoding="utf-8")) for p in sorted((MODULES / "shared").glob("*.h")))
                literals = {join_literals(m.group(0)) for m in re.finditer(LITS, code + "\n" + shared)}

                static = {es for es, _, dyn in entries if not dyn}
                dynamic = {es for es, _, dyn in entries if dyn}
                self.assertEqual(sorted(wrapped - set(keys)), [], "literales envueltos sin traducción")
                self.assertEqual(sorted(static - wrapped), [], "entradas que ningún L(...) usa (muévelas a la parte dinámica o bórralas)")
                self.assertEqual(sorted(dynamic - literals), [], "entradas dinámicas cuyo literal ya no existe en el código")

                for es, en, _ in entries:
                    self.assertTrue(en.strip(), f"traducción vacía: {es!r}")
                    es_auto, es_manual = placeholders(es)
                    en_auto, en_manual = placeholders(en)
                    self.assertFalse(es_manual, f"el español usa {{n}} manual: {es!r}")
                    if en_manual:
                        self.assertEqual(en_manual, list(range(es_auto)), f"índices del inglés distintos de los {{}} del español: {es!r}")
                        self.assertEqual(en_auto, 0, f"el inglés mezcla {{}} y {{n}}: {en!r}")
                    else:
                        self.assertEqual(en_auto, es_auto, f"distinto número de {{}}: {es!r} / {en!r}")


class IdiomaDelCliente(unittest.TestCase):
    def test_nadie_decide_el_idioma_con_el_de_los_dbc_del_servidor(self):
        """GetSessionDbcLocale() es el idioma de los DBC cargados (enUS aquí), no el del cliente.

        Decidir español/inglés con él hace que todos los clientes parezcan enUS; el idioma
        real es GetSessionDbLocaleIndex(). Sólo vale para indexar tablas de los propios DBC
        (area_name[...]), que es lo que lista PERMITIDOS."""
        permitidos = {("mod-world-bots", "mod_world_bots.cpp", "area_name[handler.GetSessionDbcLocale()]")}
        for path in sorted(MODULES.glob("*/src/*.cpp")):
            for number, line in enumerate(strip_comments(path.read_text(encoding="utf-8")).splitlines(), 1):
                if "GetSessionDbcLocale" not in line:
                    continue
                self.assertTrue(any(p[2] in line and p[0] == path.parent.parent.name for p in permitidos),
                                f"{path.relative_to(MODULES)}:{number} decide el idioma con GetSessionDbcLocale(): usa GetSessionDbLocaleIndex()")


if __name__ == "__main__":
    unittest.main()
