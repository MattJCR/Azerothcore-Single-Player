# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Login contra el authserver (src/server/apps/authserver/Server/AuthSession.cpp)."""
import socket
import struct

from .binario import Lector
from .cripto import ClienteSRP6, sha1

BUILD = 12340
# build_info.macHash de 12340: con OS "OSX" el authserver pide SHA1(A, macHash)
# como prueba de versión (VerifyVersion). Se usa "OSX" porque así el mundo no
# arranca Warden (WorldSession::InitWarden sólo lo crea para "Win").
MAC_HASH_12340 = bytes.fromhex("B706D13FF2F4018839729461E3F8A0E2B5FDC034")


class ErrorAuth(Exception):
    pass


def _recibir(sock, n):
    datos = b""
    while len(datos) < n:
        trozo = sock.recv(n - len(datos))
        if not trozo:
            raise ErrorAuth("el authserver cerró la conexión")
        datos += trozo
    return datos


def iniciar_sesion(host: str, usuario: str, clave: str, puerto: int = 3724, timeout: float = 15):
    """Devuelve (K, reinos): la clave de sesión de 40 bytes y la lista de reinos."""
    sock = socket.create_connection((host, puerto), timeout=timeout)
    try:
        I = usuario.upper().encode()
        cuerpo = (b"WoW\x00" + bytes([3, 3, 5]) + struct.pack("<H", BUILD)
                  + b"68x\x00" + b"XSO\x00" + b"SEse"
                  + struct.pack("<I", 0) + bytes([127, 0, 0, 1])
                  + bytes([len(I)]) + I)
        sock.sendall(bytes([0x00, 0x08]) + struct.pack("<H", len(cuerpo)) + cuerpo)

        cab = _recibir(sock, 3)
        if cab[0] != 0x00:
            raise ErrorAuth("respuesta inesperada al reto: %r" % cab)
        if cab[2] != 0:
            raise ErrorAuth("reto rechazado, código %d (4=cuenta desconocida, 5=suspendida, 9=versión)" % cab[2])
        B = _recibir(sock, 32)
        g_len = _recibir(sock, 1)[0]
        _recibir(sock, g_len)
        n_len = _recibir(sock, 1)[0]
        N = _recibir(sock, n_len)
        s = _recibir(sock, 32)
        _recibir(sock, 16)                       # VersionChallenge
        flags = _recibir(sock, 1)[0]
        if flags:
            raise ErrorAuth("la cuenta pide PIN/matriz/token (securityFlags=%d): no soportado" % flags)

        srp = ClienteSRP6(usuario, clave)
        A, M1, K = srp.responder(B, N, s)
        prueba_version = sha1(A, MAC_HASH_12340)
        sock.sendall(bytes([0x01]) + A + M1 + prueba_version + bytes([0, 0]))

        r = _recibir(sock, 2)
        if r[0] != 0x01 or r[1] != 0:
            raise ErrorAuth("prueba rechazada (código %d): contraseña incorrecta o versión no aceptada" % r[1])
        M2 = _recibir(sock, 20)
        _recibir(sock, 4 + 4 + 2)
        if M2 != srp.M2_esperado:
            raise ErrorAuth("el M2 del servidor no cuadra: la sesión no es de fiar")

        sock.sendall(bytes([0x10]) + struct.pack("<I", 0))
        cab = _recibir(sock, 3)
        tam = struct.unpack("<H", cab[1:3])[0]
        L = Lector(_recibir(sock, tam))
        L.u32()
        reinos = []
        for _ in range(L.u16()):
            tipo, bloqueado, flags_r = L.u8(), L.u8(), L.u8()
            nombre, direccion = L.cadena(), L.cadena()
            L.f32(); L.u8(); L.u8()
            rid = L.u8()
            if flags_r & 0x04:
                L.bytes(5)
            reinos.append({"id": rid, "nombre": nombre, "direccion": direccion,
                           "tipo": tipo, "bloqueado": bloqueado})
        return K, reinos
    finally:
        sock.close()
