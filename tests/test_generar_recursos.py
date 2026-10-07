# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Generadores de recursos con insumos sueltos (los que recibe el panel del navegador)."""
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
ICONS = os.path.join(ROOT, "web-panel", "tools", "extract-item-icons.py")
PATCH = os.path.join(ROOT, "tools", "construir-parche-cliente-items.py")


def dbc(fields, rows, strings=b"\0"):
    out = bytearray(struct.pack("<4siiii", b"WDBC", len(rows), fields, fields * 4, len(strings)))
    for row in rows:
        values = list(row) + [0] * (fields - len(row))
        out += struct.pack("<%di" % fields, *values)
    return bytes(out) + strings


def blp(color):
    image = Image.new("P", (16, 16))
    image.putpalette([c for i in range(256) for c in (color, i, 0)])
    buffer = io.BytesIO()
    image.save(buffer, "BLP")
    return buffer.getvalue()


def run(script, *args):
    return subprocess.run([sys.executable, script, *args], capture_output=True, text=True, cwd=ROOT)


@unittest.skipIf(Image is None, "falta Pillow")
class IconosDesdeInsumos(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.insumos = os.path.join(self.tmp, "insumos")
        os.makedirs(os.path.join(self.insumos, "dbc"))
        os.makedirs(os.path.join(self.insumos, "iconos"))
        names = b"\0INV_Sword_04\0INV_Misc_QuestionMark\0INV_Sin_Arte\0"
        offsets = {n: names.index(n.encode()) for n in ("INV_Sword_04", "INV_Misc_QuestionMark", "INV_Sin_Arte")}
        rows = [
            (0, 0, 0, 0, 0, offsets["INV_Misc_QuestionMark"], 0),
            (1, 0, 0, 0, 0, offsets["INV_Sword_04"], 0),
            (2, 0, 0, 0, 0, offsets["INV_Sin_Arte"], 0),
        ]
        with open(os.path.join(self.insumos, "dbc", "ItemDisplayInfo.dbc"), "wb") as f:
            f.write(dbc(25, rows, names))
        with open(os.path.join(self.insumos, "iconos", "1.blp"), "wb") as f:
            f.write(blp(10))
        with open(os.path.join(self.insumos, "iconos", "2.blp"), "wb") as f:
            f.write(blp(200))
        with open(os.path.join(self.insumos, "iconos", "indice.json"), "w") as f:
            json.dump({"inv_misc_questionmark": "1", "inv_sword_04": "2", "inv_peligro": "../../x"}, f)

    def test_genera_webp_y_mapa_con_fallback_para_lo_que_no_tiene_arte(self):
        salida = os.path.join(self.tmp, "salida")
        result = run(ICONS, "--insumos", self.insumos, "--salida", salida)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.load(open(os.path.join(salida, "map.json")))
        self.assertEqual(manifest["version"], 1)
        fallback = manifest["fallback"]
        self.assertTrue(fallback.startswith("inv_misc_questionmark-"))
        self.assertEqual(manifest["icons"][0], fallback)
        self.assertTrue(manifest["icons"][1].startswith("inv_sword_04-"))
        self.assertEqual(manifest["icons"][2], fallback, "sin arte -> icono genérico")
        for name in set(manifest["icons"]):
            self.assertTrue(os.path.isfile(os.path.join(salida, name + ".webp")), name)
        source = open(os.path.join(salida, "SOURCE.md"), encoding="utf-8").read()
        self.assertIn("Referencias sin arte: INV_Sin_Arte", source)

    def test_el_mismo_insumo_da_los_mismos_nombres(self):
        a, b = os.path.join(self.tmp, "a"), os.path.join(self.tmp, "b")
        self.assertEqual(run(ICONS, "--insumos", self.insumos, "--salida", a).returncode, 0)
        self.assertEqual(run(ICONS, "--insumos", self.insumos, "--salida", b).returncode, 0)
        self.assertEqual(sorted(os.listdir(a)), sorted(os.listdir(b)))
        self.assertEqual(open(os.path.join(a, "map.json")).read(), open(os.path.join(b, "map.json")).read())

    def test_sin_dbc_falla_con_mensaje(self):
        os.remove(os.path.join(self.insumos, "dbc", "ItemDisplayInfo.dbc"))
        result = run(ICONS, "--insumos", self.insumos, "--salida", os.path.join(self.tmp, "s"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ItemDisplayInfo.dbc", result.stderr)

    def test_un_dbc_con_otro_formato_se_rechaza(self):
        with open(os.path.join(self.insumos, "dbc", "ItemDisplayInfo.dbc"), "wb") as f:
            f.write(dbc(8, [(1, 2)]))
        result = run(ICONS, "--insumos", self.insumos, "--salida", os.path.join(self.tmp, "s"))
        self.assertNotEqual(result.returncode, 0)


class ParcheDesdeInsumos(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.insumos = os.path.join(self.tmp, "insumos")
        carpeta = os.path.join(self.insumos, "dbc", "esES")
        os.makedirs(carpeta)
        with open(os.path.join(carpeta, "Item.dbc"), "wb") as f:
            f.write(dbc(8, [(25, 0, 0, -1, 0, 100, 0, 0), (600005, 1, 1, -1, 1, 5, 0, 0)], b"\0\0"))
        spells = [(8690,), (59403,)]
        with open(os.path.join(carpeta, "Spell.dbc"), "wb") as f:
            f.write(dbc(234, spells, b"\0"))

    def test_construye_el_mpq_del_idioma_con_los_objetos_propios(self):
        salida = os.path.join(self.tmp, "salida")
        result = run(PATCH, "--insumos", self.insumos, "--idioma", "esES", "--sin-arac", "--salida", salida)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        mpq = os.path.join(salida, "esES", "patch-esES-4.MPQ")
        with open(mpq, "rb") as f:
            data = f.read()
        self.assertEqual(data[:4], b"MPQ\x1a")
        try:
            import mpyq
        except ImportError:
            return
        archive = mpyq.MPQArchive(mpq, listfile=False)
        item = archive.read_file("DBFilesClient\\Item.dbc")
        rows = struct.unpack_from("<i", item, 4)[0]
        self.assertEqual(rows, 4, "dos filas de entrada y los dos objetos propios (600000 y 600001)")
        ids = {struct.unpack_from("<i", item, 20 + i * 32)[0] for i in range(rows)}
        self.assertTrue({600000, 600001} <= ids)
        spell = archive.read_file("DBFilesClient\\Spell.dbc")
        self.assertEqual(struct.unpack_from("<4s", spell)[0], b"WDBC")

    def test_con_arac_toma_los_dbc_de_la_carpeta_indicada(self):
        arac = os.path.join(self.tmp, "arac")
        os.makedirs(arac)
        for nombre in ("CharBaseInfo.dbc", "CharStartOutfit.dbc", "SkillRaceClassInfo.dbc"):
            with open(os.path.join(arac, nombre), "wb") as f:
                f.write(dbc(4, [(1, 2, 3, 4)]))
        salida = os.path.join(self.tmp, "salida")
        result = run(PATCH, "--insumos", self.insumos, "--idioma", "esES", "--arac", arac, "--salida", salida)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("+ARAC", result.stdout)

    def test_sin_insumos_de_ese_idioma_falla(self):
        result = run(PATCH, "--insumos", self.insumos, "--idioma", "enUS", "--sin-arac", "--salida", os.path.join(self.tmp, "s"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Faltan los insumos", result.stdout + result.stderr)

    def test_insumos_e_instalar_no_se_mezclan(self):
        result = run(PATCH, "--insumos", self.insumos, "--instalar", "--sin-arac")
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
