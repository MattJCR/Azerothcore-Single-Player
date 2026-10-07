# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Traspaso de reserva de mod-quest-mates entre dos jugadores humanos reales
(PLAN M43).

`BotClaims` sólo sabe que un bot es "de quest-mates", no de qué jugador: la
identidad del vínculo vive en `g_bonds` (bot -> humano, generación) dentro del
propio módulo (`modules/mod-quest-mates/src/mod_quest_mates.cpp`). El
traspaso que hay que confirmar en vivo:

1. A acepta una misión en el Ayudante Willem (823); quest-mates reserva
   bots libres de su zona/nivel (`g_bonds[bot] = {A, gen}`).
2. A abandona la misión (`HandleAbandon`): sólo se libera el vínculo si el
   `Mate` de A sigue siendo su dueño vigente (`HoldsBond`); el bot queda
   libre para cualquiera.
3. B acepta la MISMA misión: `IsFreeBot` deja coger un bot que ya no está
   vinculado a nadie.
4. A, todavía conectado, reintenta la misma misión mientras B la sostiene:
   `IsFreeBot(bot, A)` debe rechazar cualquier bot cuyo vínculo vigente sea
   de B (`BondedTo(A, bot)` es falso), así que A no debe recuperar ninguno
   de los que aparecen en el aviso de B.
5. A cierra sesión de verdad (logout real, `REQ_LOGOUT` -> `ReleaseMates`):
   sólo toca el `Roster` de A, nunca el vínculo vigente de B.

M01 (24/09/2026) cerró esta hipótesis con dos binarios C++ aislados (fuentes
reales de `mod_quest_mates.cpp` y `BotClaims.h`, sustitutos del core) porque
no había un segundo jugador real; este caso es la comprobación en vivo con
`VERIFICADOR`/`VERIFICADOR2` que quedó pendiente.

Requiere `QuestMates.Announce=1` puesto A MANO (SSH + `.reload config`) antes
de ejecutar: con el valor por defecto de la VM (0) no hay ninguna señal en el
cliente de qué bots coge cada jugador (ver `no_cubre` de `misiones`); hay que
devolverlo a 0 al terminar, igual que M35/M36 con otros flancos.
"""
import re
import time

from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from . import PersonajeTemporal, sin_colores
from .misiones import AMENAZA_INTERIOR, AYUDANTE_WILLEM


def _aviso_companeros(m, desde_idx):
    """Busca «X, Y también van a por "..."» entre los mensajes recibidos desde `desde_idx`."""
    texto = next((t for _, t in m.mensajes[desde_idx:] if "también van a por" in t), None)
    if not texto:
        return None, set()
    limpio = sin_colores(texto)
    cabeza = limpio.split(" también van a por")[0].strip()
    nombres = {n.strip() for n in cabeza.split(",") if n.strip()}
    return limpio, nombres


def _aceptar_en_willem(ctx, m, sec, pre_wait=3):
    if pre_wait:
        m.bombear(pre_wait)
    willem = next((c for c in m.criaturas() if c["entrada"] == AYUDANTE_WILLEM), None)
    if not ctx.inf.comprobar(sec, willem is not None, "el Ayudante Willem (823) está a la vista",
                             "mapa %s" % m.mapa, origen="entorno"):
        return None, set()
    m.ir_a(willem["pos"], 2)
    antes = len(m.mensajes)
    m.aceptar_mision(willem["guid"], AMENAZA_INTERIOR)
    m.bombear(3)
    limite = time.time() + 45
    aviso, nombres = None, set()
    while time.time() < limite and aviso is None:
        m.bombear(2)
        aviso, nombres = _aviso_companeros(m, antes)
    return aviso, nombres


@caso(id="traspaso-reserva", titulo="Traspaso de reserva de quest-mates entre dos jugadores (M43)",
      descripcion=__doc__,
      etiquetas=("misiones", "quest-mates", "dos-cuentas", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm"),
      parametros={
          "espera": Parametro(int, 120, "segundos dentro antes de que A acepte (entrada de bots)",
                               minimo=0, maximo=900),
          "espera_barrido": Parametro(int, 15, "segundos de margen tras soltar/reclutar para que "
                                       "corra CleanupMates antes del siguiente paso", minimo=3, maximo=120),
      },
      duracion_max=900, protege=("mod-quest-mates", "PLAN M43", "PLAN M01"), control="directo",
      observa=("aviso «X también van a por…» de A y de B", "solape de nombres entre ambos avisos",
               "A no recupera un bot que sigue en el aviso de B tras reintentar"),
      no_cubre=("con QuestMates.Announce=0 (valor por defecto de la VM) no hay señal en el cliente: "
                "hay que poner QuestMates.Announce=1 a mano (SSH + `.reload config`) antes de "
                "ejecutar este caso y devolverlo a 0 al acabar",
                "la vía «entrega» (turn-in real) no se ejercita: exige completar objetivos con "
                "combate real; sólo se prueban abandono y logout de A",
                "que la reserva de B sobreviva más allá del propio caso: no hay una tercera ronda "
                "tras el logout de A para reconfirmarlo"),
      en_todo=False)
def ejecutar(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    inf, sec = ctx.inf, "traspaso-reserva"
    a = ctx.nueva_sesion()

    def deshacer_a():
        if AMENAZA_INTERIOR in a.misiones():
            a.abandonar_mision(AMENAZA_INTERIOR)

    with PersonajeTemporal(ctx, a, 1, 1, sec, antes_de_salir=deshacer_a) as pa:
        ctx.anotar_servidor(a)
        inf.paso("esperando %d s a que los bots entren (DisabledWithoutRealPlayer)" % ctx.p["espera"])
        a.bombear(ctx.p["espera"])

        aviso_a, mates_a = _aceptar_en_willem(ctx, a, sec, pre_wait=0)
        if not inf.comprobar(sec, AMENAZA_INTERIOR in a.misiones(), "A acepta «A Threat Within» en Willem"):
            return
        inf.datos["aviso_a"] = aviso_a
        if not inf.comprobar(sec, bool(mates_a), "quest-mates reserva compañero(s) para A", aviso_a or
                             "sin aviso en 45 s: repetir con QuestMates.Announce=1 y `.reload config` en la VM",
                             estado_si_no="AVISO" if aviso_a is None else "FALLO"):
            return
        inf.paso("A reserva a %s" % ", ".join(sorted(mates_a)))

        inf.comprobar(sec, a.abandonar_mision(AMENAZA_INTERIOR),
                      "A abandona la misión: HandleAbandon suelta el vínculo (M01)")
        a.bombear(ctx.p["espera_barrido"])  # barridos de CleanupMates con A conectado pero sin misión

        b = ctx.nueva_sesion("secundaria")

        def deshacer_b():
            if AMENAZA_INTERIOR in b.misiones():
                b.abandonar_mision(AMENAZA_INTERIOR)

        with PersonajeTemporal(ctx, b, 1, 4, sec, antes_de_salir=deshacer_b) as pb:
            ctx.anotar_servidor(b)
            aviso_b, mates_b = _aceptar_en_willem(ctx, b, sec, pre_wait=5)
            if not inf.comprobar(sec, AMENAZA_INTERIOR in b.misiones(), "B acepta la misma misión tras A"):
                return
            inf.datos["aviso_b"] = aviso_b
            if not inf.comprobar(sec, bool(mates_b), "quest-mates reserva compañero(s) para B", aviso_b or
                                 "sin aviso en 45 s", estado_si_no="AVISO" if aviso_b is None else "FALLO"):
                return
            inf.paso("B reserva a %s" % ", ".join(sorted(mates_b)))

            solape = mates_a & mates_b
            inf.datos.update(mates_a=sorted(mates_a), mates_b=sorted(mates_b), solape=sorted(solape))
            inf.comprobar(sec, True, "B recluta tras soltarlos A (bots libres reutilizados o nuevos)",
                          "A: %s · B: %s · comunes: %s" % (", ".join(sorted(mates_a)), ", ".join(sorted(mates_b)),
                                                            ", ".join(sorted(solape)) or "ninguno"))

            # A sigue conectado: reintenta la misma misión mientras B la sostiene. No debe
            # recuperar ningún bot que siga vinculado a B (IsFreeBot(bot, A) lo rechaza).
            a.bombear(ctx.p["espera_barrido"])
            aviso_a2, mates_a2 = _aceptar_en_willem(ctx, a, sec, pre_wait=0)
            inf.datos["aviso_a2"], inf.datos["mates_a2"] = aviso_a2, sorted(mates_a2)
            choque = mates_a2 & mates_b
            inf.comprobar(sec, not choque,
                          "A no recupera ningún compañero que sigue reservado por B",
                          "A(2): %s · B: %s · choque: %s" % (", ".join(sorted(mates_a2)), ", ".join(sorted(mates_b)),
                                                              ", ".join(sorted(choque)) or "ninguno"),
                          esperado="sin choque", observado=", ".join(sorted(choque)) or "ninguno")

            inf.comprobar(sec, a.abandonar_mision(AMENAZA_INTERIOR), "A vuelve a soltar la misión (limpieza)")
            inf.comprobar(sec, not a.errores_lectura and not b.errores_lectura,
                          "paquetes de ambas sesiones leídos sin error",
                          "; ".join((a.errores_lectura + b.errores_lectura)[:3]), origen="cliente")
