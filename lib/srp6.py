#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
lib/srp6.py — Calcula el par (salt, verifier) de una cuenta de AzerothCore.

Por qué existe: crear la cuenta de administrador metiendo la contraseña en la
tabla `account` no vale, porque AzerothCore no guarda contraseñas: guarda un
verificador SRP6. La alternativa sería mandar "account create" a la consola del
worldserver, pero eso obliga a que el servidor esté arrancado justo en ese
momento — y la fase 8 se ejecuta antes de arrancarlo de forma permanente.

Algoritmo (src/common/Cryptography/Authentication/SRP6.cpp del core):

    N = 894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7  (hex)
    g = 7
    s = 32 bytes aleatorios
    x = SHA1(s || SHA1(USUARIO:CONTRASEÑA))        <- leído como little-endian
    v = g^x mod N                                  <- escrito como little-endian, 32 bytes

Usuario y contraseña van SIEMPRE en mayúsculas: es lo que hace
AccountMgr::CreateAccount con Utf8ToUpperOnlyLatin antes de calcular nada.

Uso:  python3 srp6.py <usuario> <contraseña>
Sale: "<salt_hex> <verifier_hex>"  (los dos en el orden de bytes que espera la BD)
"""

import hashlib
import os
import sys

N = int("894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7", 16)
G = 7


def make_registration_data(username: str, password: str):
    user = username.upper().encode("utf-8")
    pwd = password.upper().encode("utf-8")

    salt = os.urandom(32)
    inner = hashlib.sha1(user + b":" + pwd).digest()
    x = int.from_bytes(hashlib.sha1(salt + inner).digest(), "little")
    verifier = pow(G, x, N).to_bytes(32, "little")

    return salt, verifier


def main():
    if len(sys.argv) != 3:
        print("uso: srp6.py <usuario> <contraseña>", file=sys.stderr)
        return 2

    salt, verifier = make_registration_data(sys.argv[1], sys.argv[2])
    print(f"{salt.hex()} {verifier.hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
