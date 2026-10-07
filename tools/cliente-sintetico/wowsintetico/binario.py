# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Lectura y escritura little-endian con el mismo formato que ByteBuffer del core."""
import struct


class FaltanDatos(Exception):
    pass


class Lector:
    def __init__(self, datos: bytes):
        self.datos = datos
        self.pos = 0

    def resto(self) -> int:
        return len(self.datos) - self.pos

    def bytes(self, n: int) -> bytes:
        if self.pos + n > len(self.datos):
            raise FaltanDatos("se piden %d bytes y quedan %d" % (n, self.resto()))
        b = self.datos[self.pos:self.pos + n]
        self.pos += n
        return b

    def _st(self, fmt):
        return struct.unpack("<" + fmt, self.bytes(struct.calcsize(fmt)))[0]

    def u8(self):  return self._st("B")
    def u16(self): return self._st("H")
    def u32(self): return self._st("I")
    def i32(self): return self._st("i")
    def u64(self): return self._st("Q")
    def f32(self): return self._st("f")

    def cadena(self) -> str:
        fin = self.datos.index(b"\x00", self.pos)
        s = self.datos[self.pos:fin].decode("utf-8", "replace")
        self.pos = fin + 1
        return s

    def guid_empaquetado(self) -> int:
        """ObjectGuid::ReadAsPacked: máscara de bytes no nulos y luego esos bytes."""
        mascara = self.u8()
        guid = 0
        for i in range(8):
            if mascara & (1 << i):
                guid |= self.u8() << (8 * i)
        return guid


class Escritor:
    def __init__(self):
        self.partes = []

    def _st(self, fmt, v):
        self.partes.append(struct.pack("<" + fmt, v))
        return self

    def u8(self, v):  return self._st("B", v)
    def u16(self, v): return self._st("H", v)
    def u32(self, v): return self._st("I", v)
    def i32(self, v): return self._st("i", v)
    def u64(self, v): return self._st("Q", v)
    def f32(self, v): return self._st("f", v)

    def bytes(self, b: bytes):
        self.partes.append(bytes(b))
        return self

    def cadena(self, s: str):
        self.partes.append(s.encode("utf-8") + b"\x00")
        return self

    def guid_empaquetado(self, guid: int):
        mascara, cuerpo = 0, bytearray()
        for i in range(8):
            b = (guid >> (8 * i)) & 0xFF
            if b:
                mascara |= 1 << i
                cuerpo.append(b)
        self.partes.append(bytes([mascara]) + bytes(cuerpo))
        return self

    def valor(self) -> bytes:
        return b"".join(self.partes)
