# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Catálogo de casos: qué hay, qué necesita cada uno y con qué parámetros.

Un escenario se registra con el decorador `@caso(...)` en su módulo de
`escenarios/`. El catálogo es la interfaz estable para una IA:
`verificar.py listar --json` y `verificar.py describir <id> --json` devuelven
exactamente estos campos.
"""
import importlib
import pkgutil


class Parametro:
    def __init__(self, tipo, defecto, ayuda, opciones=None, minimo=None, maximo=None):
        self.tipo, self.defecto, self.ayuda = tipo, defecto, ayuda
        self.opciones, self.minimo, self.maximo = opciones, minimo, maximo

    def convertir(self, texto):
        if self.tipo is bool:
            v = str(texto).lower() in ("1", "si", "sí", "true", "yes", "s")
        else:
            v = self.tipo(texto)
        if self.opciones and v not in self.opciones:
            raise ValueError("valor %r fuera de %s" % (v, self.opciones))
        if self.minimo is not None and v < self.minimo:
            raise ValueError("valor %r menor que %s" % (v, self.minimo))
        if self.maximo is not None and v > self.maximo:
            raise ValueError("valor %r mayor que %s" % (v, self.maximo))
        return v

    def como_dict(self):
        d = {"tipo": self.tipo.__name__, "defecto": self.defecto, "ayuda": self.ayuda}
        for k in ("opciones", "minimo", "maximo"):
            if getattr(self, k) is not None:
                d[k] = getattr(self, k)
        return d


class Caso:
    def __init__(self, id, titulo, funcion, descripcion="", etiquetas=(), acciones=("conectar",),
                 requiere=(), parametros=None, duracion_max=300, protege=(), control="directo",
                 observa=(), no_cubre=(), en_todo=True):
        self.id, self.titulo, self.funcion = id, titulo, funcion
        self.descripcion = descripcion.strip()
        self.etiquetas = set(etiquetas)
        self.acciones = set(acciones)
        self.requiere = set(requiere)          # "dbc": DBC del cliente real (mpyq + carpeta)
        self.parametros = parametros or {}
        self.duracion_max = duracion_max
        self.protege = tuple(protege)          # tareas / módulos / parches que protege
        self.control = control                 # lectura | directo | delegado
        self.observa = tuple(observa)
        self.no_cubre = tuple(no_cubre)
        self.en_todo = en_todo                 # entra en la selección "todo"

    @property
    def escribe(self) -> bool:
        return bool(self.acciones - {"conectar", "leer"})

    def parametros_efectivos(self, dados: dict) -> dict:
        res = {}
        for nombre, p in self.parametros.items():
            res[nombre] = p.convertir(dados[nombre]) if nombre in dados else p.defecto
        sobran = set(dados) - set(self.parametros)
        if sobran:
            raise ValueError("el caso %s no tiene los parámetros %s" % (self.id, ", ".join(sorted(sobran))))
        return res

    def como_dict(self, completo=False) -> dict:
        d = {"id": self.id, "titulo": self.titulo, "etiquetas": sorted(self.etiquetas),
             "escribe": self.escribe, "control": self.control, "duracion_max_s": self.duracion_max,
             "protege": list(self.protege)}
        if completo:
            d.update(descripcion=self.descripcion, acciones=sorted(self.acciones), requiere=sorted(self.requiere),
                     parametros={k: p.como_dict() for k, p in self.parametros.items()},
                     observa=list(self.observa), no_cubre=list(self.no_cubre), en_todo=self.en_todo)
        return d


_CASOS = {}


def caso(**kw):
    def registrar(funcion):
        c = Caso(funcion=funcion, **kw)
        if c.id in _CASOS:
            raise RuntimeError("caso duplicado: %s" % c.id)
        _CASOS[c.id] = c
        return funcion
    return registrar


def cargar() -> dict:
    from . import escenarios
    for mod in pkgutil.iter_modules(escenarios.__path__):
        importlib.import_module("%s.%s" % (escenarios.__name__, mod.name))
    return dict(sorted(_CASOS.items()))


def seleccionar(casos: dict, nombres) -> list:
    """`nombres`: ids, `@etiqueta` o `todo`. Devuelve la lista sin repetir, en orden de catálogo."""
    elegidos = []
    for n in nombres:
        if n == "todo":
            elegidos += [c for c in casos.values() if c.en_todo]
        elif n.startswith("@"):
            et = n[1:]
            encontrados = [c for c in casos.values() if et in c.etiquetas]
            if not encontrados:
                raise KeyError("ninguna prueba con la etiqueta %s" % et)
            elegidos += encontrados
        elif n in casos:
            elegidos.append(casos[n])
        else:
            raise KeyError("no existe el caso %s (verificar.py listar)" % n)
    vistos, res = set(), []
    for c in elegidos:
        if c.id not in vistos:
            vistos.add(c.id)
            res.append(c)
    return res
