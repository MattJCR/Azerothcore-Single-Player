# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Registro local de los personajes temporales que crea el cliente sintético.

Cada creación se apunta aquí antes de entrar con el personaje y se quita al
borrarlo. Si el proceso muere a medias, `verificar.py limpiar` sólo borra lo que
cumpla las tres condiciones: está en la cuenta del perfil, el nombre lleva su
prefijo y el GUID coincide con una entrada de este registro. Lo demás se
informa, no se toca.
"""
import datetime
import json
import os

from .entorno import CARPETA_HERRAMIENTA

CARPETA_ESTADO = os.path.join(CARPETA_HERRAMIENTA, ".estado")
FICHERO = os.path.join(CARPETA_ESTADO, "personajes.json")


def _leer() -> list:
    try:
        with open(FICHERO, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return []


def _escribir(entradas: list):
    os.makedirs(CARPETA_ESTADO, exist_ok=True)
    tmp = FICHERO + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(entradas, f, ensure_ascii=False, indent=1)
    os.replace(tmp, FICHERO)


def apuntar(host, cuenta, nombre, guid, caso):
    entradas = [e for e in _leer() if not (e["host"] == host and e["guid"] == guid)]
    entradas.append({"host": host, "cuenta": cuenta.upper(), "nombre": nombre, "guid": guid, "caso": caso,
                     "creado": datetime.datetime.now().isoformat(timespec="seconds"), "pid": os.getpid()})
    _escribir(entradas)


def quitar(host, guid):
    _escribir([e for e in _leer() if not (e["host"] == host and e["guid"] == guid)])


def pendientes(host, cuenta) -> list:
    return [e for e in _leer() if e["host"] == host and e["cuenta"] == cuenta.upper()]


def clasificar(personajes: list, host, cuenta, prefijo) -> tuple:
    """Parte la lista de la cuenta en (borrables, ajenos_con_prefijo, registrados_ausentes)."""
    registrados = {e["guid"]: e for e in pendientes(host, cuenta)}
    borrables, sospechosos = [], []
    for p in personajes:
        if not p["nombre"].startswith(prefijo):
            continue
        e = registrados.get(p["guid"])
        if e and e["nombre"] == p["nombre"]:
            borrables.append(p)
        else:
            sospechosos.append(p)
    presentes = {p["guid"] for p in personajes}
    ausentes = [e for g, e in registrados.items() if g not in presentes]
    return borrables, sospechosos, ausentes
