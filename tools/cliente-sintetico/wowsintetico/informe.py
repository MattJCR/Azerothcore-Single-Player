# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Resultados de verificación: un informe con un resultado por caso.

Estados de una comprobación: OK, AVISO, FALLO. Estados de un caso: además
OMITIDO (no se ejecutó: `--dry-run` o sin comprobaciones), BLOQUEADO (falta una
precondición: perfil sin permiso, dependencia ausente, entorno inesperado) y
ERROR (infraestructura: no hay conexión, se cayó el servidor). Ni OMITIDO ni
BLOQUEADO ni ERROR cuentan como éxito.

Cada FALLO lleva su origen: `desarrollo` (lo que se prueba no hace lo que debe),
`cliente` (el cliente sintético no supo leer o responder) o `entorno` (red,
servidor, cuenta). El JSON sigue el esquema `cliente-sintetico/2` (README).
"""
import datetime
import json
from pathlib import Path
import time

OK, AVISO, FALLO = "OK", "AVISO", "FALLO"
OMITIDO, BLOQUEADO, ERROR = "OMITIDO", "BLOQUEADO", "ERROR"
ORIGENES = ("desarrollo", "cliente", "entorno")
ESQUEMA = "cliente-sintetico/2"

# Código de salida de verificar.py
SALIDA = {OK: 0, AVISO: 0, FALLO: 1, BLOQUEADO: 2, OMITIDO: 2, ERROR: 3}
USO_INCORRECTO = 4


def _ahora():
    return datetime.datetime.now().isoformat(timespec="seconds")


class ResultadoCaso:
    """Lo que se anota de un caso. Los escenarios reciben esto como `inf`."""

    def __init__(self, caso, parametros: dict, eco=print):
        self.caso = caso
        self.parametros = parametros
        self.comprobaciones = []      # dicts
        self.pasos = []               # (segundos desde el inicio, texto)
        self.eventos = []             # eventos filtrados que el caso quiere conservar
        self.errores = []
        self.artefactos = []          # rutas de ficheros generados
        self.datos = {}               # evidencia en bruto
        self.forzado = None           # (estado, motivo) para BLOQUEADO/OMITIDO/ERROR
        self.inicio = None
        self.t0 = None
        self.duracion = None
        self._eco = eco

    # ── registro ────────────────────────────────────────────────────────────
    def empezar(self):
        self.inicio, self.t0 = _ahora(), time.time()

    def terminar(self):
        if self.t0:
            self.duracion = round(time.time() - self.t0, 1)

    def segundos(self) -> float:
        return round(time.time() - self.t0, 1) if self.t0 else 0.0

    def paso(self, texto: str):
        self.pasos.append((self.segundos(), texto))
        self._eco("  · %s" % texto)

    def anotar(self, seccion, estado, comprobacion, detalle="", esperado=None, observado=None, origen="desarrollo"):
        c = {"seccion": seccion, "estado": estado, "comprobacion": comprobacion, "detalle": detalle}
        if esperado is not None:
            c["esperado"] = esperado
        if observado is not None:
            c["observado"] = observado
        if estado == FALLO:
            c["origen"] = origen if origen in ORIGENES else "desarrollo"
        c["t"] = self.segundos()
        self.comprobaciones.append(c)
        self._eco("  [%-5s] %s — %s%s" % (estado, seccion, comprobacion, (": " + str(detalle)) if detalle else ""))

    def comprobar(self, seccion, condicion, comprobacion, detalle="", estado_si_no=FALLO,
                  esperado=None, observado=None, origen="desarrollo"):
        self.anotar(seccion, OK if condicion else estado_si_no, comprobacion, detalle, esperado, observado, origen)
        return bool(condicion)

    def error(self, texto, origen="entorno"):
        self.errores.append({"t": self.segundos(), "origen": origen, "texto": texto})

    def forzar(self, estado, motivo):
        """BLOQUEADO / OMITIDO / ERROR: el primero que se fija manda."""
        if self.forzado is None:
            self.forzado = (estado, motivo)
            self._eco("  [%s] %s" % (estado, motivo))

    # ── resultado ───────────────────────────────────────────────────────────
    def resultado(self) -> str:
        estados = {c["estado"] for c in self.comprobaciones}
        if self.forzado and self.forzado[0] == ERROR:
            return ERROR
        if FALLO in estados:
            return FALLO
        if self.forzado:
            return self.forzado[0]
        if not self.comprobaciones:
            return OMITIDO
        return AVISO if AVISO in estados else OK

    def como_dict(self) -> dict:
        c = self.caso
        return {
            "id": c.id, "titulo": c.titulo, "etiquetas": sorted(c.etiquetas), "protege": list(c.protege),
            "control": c.control, "acciones": sorted(c.acciones), "parametros": self.parametros,
            "resultado": self.resultado(), "motivo": self.forzado[1] if self.forzado else "",
            "inicio": self.inicio, "duracion_s": self.duracion,
            "pasos": [{"t": t, "texto": x} for t, x in self.pasos],
            "comprobaciones": self.comprobaciones, "eventos": self.eventos,
            "errores": self.errores, "artefactos": self.artefactos, "datos": self.datos,
        }

    def resumen(self) -> str:
        n = {e: sum(1 for c in self.comprobaciones if c["estado"] == e) for e in (OK, AVISO, FALLO)}
        return "%-9s %s — %d OK, %d avisos, %d fallos%s" % (
            self.resultado(), self.caso.id, n[OK], n[AVISO], n[FALLO],
            (" (%s)" % self.forzado[1]) if self.forzado else "")


class Informe:
    def __init__(self, herramienta: dict, entorno: dict):
        self.casos = []
        self.meta = {"esquema": ESQUEMA, "herramienta": herramienta, "entorno": entorno,
                     "inicio": _ahora(), "fin": None, "servidor": {}, "cliente": {}, "argumentos": {}}

    def agregar(self, r: ResultadoCaso):
        self.casos.append(r)

    def resultado(self) -> str:
        orden = [ERROR, FALLO, BLOQUEADO, OMITIDO, AVISO, OK]
        estados = {r.resultado() for r in self.casos}
        return next((e for e in orden if e in estados), OMITIDO)

    def codigo_salida(self, dry_run=False) -> int:
        if dry_run:
            return 0
        return SALIDA[self.resultado()]

    def como_dict(self) -> dict:
        d = dict(self.meta)
        d["fin"] = d["fin"] or _ahora()
        d["resultado"] = self.resultado()
        d["casos"] = [r.como_dict() for r in self.casos]
        d["recuento"] = {e: sum(1 for r in self.casos if r.resultado() == e)
                         for e in (OK, AVISO, FALLO, OMITIDO, BLOQUEADO, ERROR)}
        return d

    def guardar_json(self, ruta):
        Path(ruta).parent.mkdir(parents=True, exist_ok=True)
        with open(ruta, "w", encoding="utf-8") as f:
            json.dump(self.como_dict(), f, ensure_ascii=False, indent=1, default=_serializable)

    def resumen(self) -> str:
        lineas = [r.resumen() for r in self.casos]
        lineas.append("RESULTADO: %s" % self.resultado())
        return "\n".join(lineas)


def _serializable(o):
    if isinstance(o, (set, frozenset)):
        return sorted(o)
    if isinstance(o, bytes):
        return o.hex()
    return str(o)
