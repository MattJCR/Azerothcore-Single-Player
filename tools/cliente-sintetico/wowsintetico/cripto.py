# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""SRP6 del lado del cliente y cifrado de cabeceras del mundo.

Espejo de src/common/Cryptography/Authentication/SRP6.cpp y AuthCrypt.cpp del
core fijado: todos los números grandes viajan en little-endian de 32 bytes.
"""
import hashlib
import hmac
import os

G = 7
K_SRP = 3


def sha1(*partes: bytes) -> bytes:
    h = hashlib.sha1()
    for p in partes:
        h.update(p)
    return h.digest()


def _le(n: int, largo: int = 32) -> bytes:
    return n.to_bytes(largo, "little")


def entrelazar_sha1(S: bytes) -> bytes:
    """SRP6::SHA1Interleave: parte S (LE) en pares/impares, salta los ceros
    iniciales (redondeado a par) y entrelaza los dos SHA1."""
    p = 0
    while p < len(S) and S[p] == 0:
        p += 1
    if p & 1:
        p += 1
    p //= 2
    pares = bytes(S[2 * i] for i in range(len(S) // 2))
    impares = bytes(S[2 * i + 1] for i in range(len(S) // 2))
    h0, h1 = sha1(pares[p:]), sha1(impares[p:])
    return bytes(b for par in zip(h0, h1) for b in par)


class ClienteSRP6:
    """Calcula A, la clave de sesión K y la prueba M1 a partir del reto del servidor."""

    def __init__(self, usuario: str, clave: str, a: int = None):
        self.I = usuario.upper()
        self.P = clave.upper()
        self.a = a if a is not None else int.from_bytes(os.urandom(19), "little")

    def responder(self, B_le: bytes, N_le: bytes, s: bytes):
        N = int.from_bytes(N_le, "little")
        B = int.from_bytes(B_le, "little")
        if B % N == 0:
            raise ValueError("B inválido")
        x = int.from_bytes(sha1(s, sha1((self.I + ":" + self.P).encode())), "little")
        A = pow(G, self.a, N)
        A_le = _le(A)
        u = int.from_bytes(sha1(A_le, B_le), "little")
        S = pow((B - K_SRP * pow(G, x, N)) % N, self.a + u * x, N)
        K = entrelazar_sha1(_le(S))
        hN, hg = sha1(N_le), sha1(bytes([G]))
        ng = bytes(a ^ b for a, b in zip(hN, hg))
        M1 = sha1(ng, sha1(self.I.encode()), s, A_le, B_le, K)
        self.A, self.K, self.M1 = A_le, K, M1
        self.M2_esperado = sha1(A_le, M1, K)
        return A_le, M1, K


class ARC4:
    def __init__(self, clave: bytes):
        S = list(range(256))
        j = 0
        for i in range(256):
            j = (j + S[i] + clave[i % len(clave)]) & 0xFF
            S[i], S[j] = S[j], S[i]
        self.S, self.i, self.j = S, 0, 0

    def aplicar(self, datos: bytes) -> bytes:
        S, i, j = self.S, self.i, self.j
        out = bytearray(len(datos))
        for n, b in enumerate(datos):
            i = (i + 1) & 0xFF
            j = (j + S[i]) & 0xFF
            S[i], S[j] = S[j], S[i]
            out[n] = b ^ S[(S[i] + S[j]) & 0xFF]
        self.i, self.j = i, j
        return bytes(out)


# AuthCrypt.cpp: el servidor cifra con ServerEncryptionKey y descifra con
# ServerDecryptionKey; el cliente, al revés.
_CLAVE_SERVIDOR_CIFRA = bytes([0xCC, 0x98, 0xAE, 0x04, 0xE8, 0x97, 0xEA, 0xCA,
                               0x12, 0xDD, 0xC0, 0x93, 0x42, 0x91, 0x53, 0x57])
_CLAVE_SERVIDOR_DESCIFRA = bytes([0xC2, 0xB3, 0x72, 0x3C, 0xC6, 0xAE, 0xD9, 0xB5,
                                  0x34, 0x3C, 0x53, 0xEE, 0x2F, 0x43, 0x67, 0xCE])


class CifradoCabeceras:
    """ARC4-drop1024 sobre las cabeceras, con las claves HMAC-SHA1 de K."""

    def __init__(self, K: bytes):
        self.enviar = ARC4(hmac.new(_CLAVE_SERVIDOR_DESCIFRA, K, hashlib.sha1).digest())
        self.recibir = ARC4(hmac.new(_CLAVE_SERVIDOR_CIFRA, K, hashlib.sha1).digest())
        self.enviar.aplicar(bytes(1024))
        self.recibir.aplicar(bytes(1024))
