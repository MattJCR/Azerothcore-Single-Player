# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""DBC efectivos de un cliente 3.3.5a real, leídos de sus MPQ (necesita mpyq).

A diferencia de tools/construir-parche-cliente-items.py, que parte del DBC
prístino, aquí se incluye nuestro patch-<idioma>-4.MPQ: interesa lo que ve de
verdad el jugador. El orden de prioridad es el mismo modelo que usa ese
script, con patch-<idioma>-4 por encima de -3. Cada DBC informa de qué MPQ salió.
"""
import os
import struct

try:
    import mpyq
except ImportError:                       # sólo hace falta para las comprobaciones de cliente
    mpyq = None

DBF = "DBFilesClient" + chr(92)
# Índice de idioma en los campos *_Lang de 3.3.5a
IDIOMA_INDICE = {"enUS": 0, "esES": 6, "esMX": 7}
SPELL_NAME_BASE = 136                     # Spell.dbc: Name_Lang[0]
SKILL_CATEGORY_CLASS = 7                  # SkillLine.dbc CategoryID de las líneas de clase


def _prioridad(nombre: str, loc: str) -> int:
    n = nombre.lower()
    base = {"common.mpq": 0, "common-2.mpq": 1, "expansion.mpq": 2, "lichking.mpq": 3,
            "patch.mpq": 4, "patch-2.mpq": 5, "patch-3.mpq": 6}
    if n in base:
        return base[n]
    locales = {"locale-%s.mpq" % loc: 10, "patch-%s.mpq" % loc: 20, "patch-%s-2.mpq" % loc: 21,
               "patch-%s-3.mpq" % loc: 22, "patch-%s-4.mpq" % loc: 23}
    if n in locales:
        return locales[n]
    if n.startswith("patch-") and n.endswith(".mpq"):
        tag = n[6:-4]
        if tag.startswith("zava"):
            return 200
        if len(tag) == 1 and tag.isalnum():
            return 30 + ord(tag)
        return 100
    return 50


class TablaDBC:
    def __init__(self, crudo: bytes, origen: str):
        magic, self.filas, self.campos, self.tam, tam_cad = struct.unpack("<4siiii", crudo[:20])
        if magic != b"WDBC":
            raise ValueError("no es un WDBC (%s)" % origen)
        self.crudo, self.origen = crudo, origen
        self._cadenas = 20 + self.filas * self.tam

    def u32(self, fila: int, campo: int) -> int:
        return struct.unpack_from("<I", self.crudo, 20 + fila * self.tam + campo * 4)[0]

    def f32(self, fila: int, campo: int) -> float:
        return struct.unpack_from("<f", self.crudo, 20 + fila * self.tam + campo * 4)[0]

    def u8(self, fila: int, byte: int) -> int:
        return self.crudo[20 + fila * self.tam + byte]

    def cadena(self, fila: int, campo: int) -> str:
        off = self.u32(fila, campo)
        fin = self.crudo.index(b"\x00", self._cadenas + off)
        return self.crudo[self._cadenas + off:fin].decode("utf-8", "replace")


class ClienteDBC:
    def __init__(self, carpeta_wow: str, idioma: str = "esES"):
        if mpyq is None:
            raise RuntimeError("falta mpyq (pip install mpyq) para leer los DBC del cliente")
        self.idioma, self.loc = idioma, idioma.lower()
        data = os.path.join(carpeta_wow, "Data")
        cand = []
        for d in (data, os.path.join(data, idioma)):
            if os.path.isdir(d):
                cand += [os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith(".mpq")]
        self.mpqs = sorted(cand, key=lambda p: _prioridad(os.path.basename(p), self.loc))
        self._abiertos, self._cache, self.ilegibles = {}, {}, {}

    def tabla(self, nombre: str) -> TablaDBC:
        if nombre not in self._cache:
            ganador = None
            for p in self.mpqs:
                if p not in self._abiertos:
                    try:
                        self._abiertos[p] = mpyq.MPQArchive(p, listfile=False)
                    except Exception as e:     # noqa: BLE001 — MPQ ilegible: se anota y se salta
                        self._abiertos[p] = None
                        self.ilegibles[os.path.basename(p)] = str(e)
                if self._abiertos[p] is None:
                    continue
                try:
                    crudo = self._abiertos[p].read_file(DBF + nombre)
                except Exception:              # noqa: BLE001 — MPQ sin ese fichero
                    crudo = None
                if crudo:
                    ganador = (os.path.basename(p), crudo)
            if not ganador:
                raise FileNotFoundError("%s no está en ningún MPQ del cliente" % nombre)
            self._cache[nombre] = TablaDBC(ganador[1], ganador[0])
        return self._cache[nombre]

    # ── consultas usadas por los escenarios ─────────────────────────────────
    def combinaciones_charbaseinfo(self) -> set:
        t = self.tabla("CharBaseInfo.dbc")
        return {(t.u8(i, 0), t.u8(i, 1)) for i in range(t.filas)}

    def nombres_hechizos(self) -> dict:
        t = self.tabla("Spell.dbc")
        campo = SPELL_NAME_BASE + IDIOMA_INDICE.get(self.idioma, 0)
        return {t.u32(i, 0): t.cadena(i, campo) for i in range(t.filas)}

    def lineas_de_clase(self) -> set:
        t = self.tabla("SkillLine.dbc")
        return {t.u32(i, 0) for i in range(t.filas) if t.u32(i, 1) == SKILL_CATEGORY_CLASS}

    def habilidades_por_hechizo(self) -> dict:
        """spell -> [(skillline, racemask, classmask)] de SkillLineAbility.dbc."""
        t = self.tabla("SkillLineAbility.dbc")
        res = {}
        for i in range(t.filas):
            res.setdefault(t.u32(i, 2), []).append((t.u32(i, 1), t.u32(i, 3), t.u32(i, 4)))
        return res

    def areatriggers(self, mapa: int = None) -> list:
        """AreaTrigger.dbc: id, mapa, x, y, z, radio, largo, ancho, alto, orientación (la tabla
        `areatrigger` del mundo sale de este DBC; Wow.exe detecta la entrada con él)."""
        t = self.tabla("AreaTrigger.dbc")
        res = []
        for i in range(t.filas):
            fila = [t.u32(i, 0), t.u32(i, 1)] + [t.f32(i, c) for c in range(2, 10)]
            if mapa is None or fila[1] == mapa:
                res.append(dict(zip(("id", "mapa", "x", "y", "z", "radio", "largo", "ancho", "alto", "o"), fila)))
        return res

    def mazmorras_lfg(self) -> list:
        """LFGDungeons.dbc (DBCStructure.h LFGDungeonEntry): id, nombre, niveles, mapa, dificultad, tipo."""
        t = self.tabla("LFGDungeons.dbc")
        idx = 1 + IDIOMA_INDICE.get(self.idioma, 0)
        return [{"id": t.u32(i, 0), "nombre": t.cadena(i, idx), "nivel_min": t.u32(i, 18),
                 "nivel_max": t.u32(i, 19), "mapa": t.u32(i, 23), "dificultad": t.u32(i, 24),
                 "tipo": t.u32(i, 26)} for i in range(t.filas)]

    def origenes(self) -> dict:
        return {n: t.origen for n, t in self._cache.items()}
