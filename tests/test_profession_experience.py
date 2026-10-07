# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Formato de lanzamiento usado por SP01, cotejado con SpellHandler.cpp."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/cliente-sintetico'))
from wowsintetico.escenarios.profesiones import _paquete_hechizo
from wowsintetico.binario import Lector


class PaquetesProfesiones(unittest.TestCase):
    def test_sin_objetivo(self):
        self.assertEqual(_paquete_hechizo(7620), bytes.fromhex('01c41d00000000000000'))

    def test_objeto_empaquetado(self):
        p = _paquete_hechizo(2366, 0xF110000651000001)
        l = Lector(p)
        self.assertEqual((l.u8(), l.u32(), l.u8(), l.u32()), (1, 2366, 0, 0x800))
        self.assertEqual(l.guid_empaquetado(), 0xF110000651000001)
        self.assertEqual(l.pos, len(p))


if __name__ == '__main__':
    unittest.main()
