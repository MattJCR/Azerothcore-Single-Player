# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Misiones: mod-quest-mates y las misiones de grupo automáticas de mod-party-here.

Dos personajes temporales, uno por módulo, para que los compañeros de uno no
enturbien al otro:

1. **quest-mates.** Guerrero humano de nivel 1 en Villanorte. Tras `espera`
   segundos (los bots sólo entran con un jugador dentro), acepta «A Threat
   Within» (783) al Ayudante Willem (823) como Wow.exe
   (CMSG_QUESTGIVER_ACCEPT_QUEST). quest-mates debe elegir bots de la zona y
   avisar «X también van a por "…"». Luego la abandona como «Abandonar»
   (CMSG_QUESTLOG_REMOVE_QUEST, que dispara OnPlayerQuestAbandon).
2. **party-here automático.** Mago humano de nivel 18 en Colina del Centinela.
   «The Defias Brotherhood» (155, dos jugadores sugeridos) entra con `.quest
   add` (atajo GM: el core pasa por el mismo AddQuestAndCheckCompletion →
   OnPlayerQuestAccept que la aceptación en el NPC). party-here debe traer
   hasta dos compañeros «por mision de grupo» y, al abandonarla, despedirlos
   pasados ~120 s. Con `parte=party-here-conserva` (M19), en vez de dejarlos
   ir se pide `.grupo conserva` antes de abandonar: deben pasar a manuales
   («por mision de grupo» desaparece de `.grupo estado`) y quedarse pese a
   abandonar la misión y esperar de sobra.
3. **party-here: sincronizar misión al grupo (SP05).** Mago humano de nivel
   10. Con `.grupo 1` (grupo de dos: sin reparto forzado de rol) se trae un
   compañero manual; se prueba en los dos órdenes que importan:
   formar el grupo y ACEPTAR DESPUÉS «A Threat Within» (783, `.quest add`), y
   aceptarla ANTES de formar el grupo. En ambos, party-here debe avisar «X
   también lleva tu misión "…"» (la única señal observable desde una cuenta
   humana de que el compañero recibió su propia copia). Se comprueba también
   que abandonar la misión NO despide al compañero manual (sólo se le retira
   su copia; a diferencia de un compañero automático, éste vino por
   ".grupo", no por la misión) y que ".grupo fuera" lo despide al terminar.
"""
import re
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores

AYUDANTE_WILLEM, AMENAZA_INTERIOR = 823, 783
HERMANDAD_DEFIAS = 155


def _companeros(m) -> dict:
    return {x["guid"]: x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid}


def quest_mates(ctx, espera):
    inf, sec = ctx.inf, "quest-mates"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec) as pj:
        ctx.anotar_servidor(m)
        inf.paso("esperando %d s a que los bots entren (DisabledWithoutRealPlayer)" % espera)
        m.bombear(espera)
        willem = next((c for c in m.criaturas() if c["entrada"] == AYUDANTE_WILLEM), None)
        if not inf.comprobar(sec, willem is not None, "el Ayudante Willem (823) está a la vista al empezar",
                             "mapa %s" % m.mapa, origen="entorno"):
            return
        m.ir_a(willem["pos"], 2)
        antes = len(m.mensajes)
        m.aceptar_mision(willem["guid"], AMENAZA_INTERIOR)
        m.bombear(3)
        inf.comprobar(sec, AMENAZA_INTERIOR in m.misiones(), "se acepta «A Threat Within» en el NPC",
                      "diario: %s" % [q for q in m.misiones() if q])
        limite = time.time() + 45
        aviso = None
        while time.time() < limite and aviso is None:
            m.bombear(2)
            aviso = next((t for _, t in m.mensajes[antes:] if "también van a por" in t), None)
        inf.datos["quest_mates"] = {"personaje": pj.nombre, "aviso": aviso,
                                    "bots": sin_colores(" ".join(m.comando("bots estado", 2)))[:400]}
        # El aviso sólo existe con QuestMates.Announce=1; con 0 (valor de la VM) el cliente no tiene
        # señal propia: el diario de otro jugador sólo viaja a los de su grupo. Se deja en AVISO.
        inf.comprobar(sec, aviso is not None, "quest-mates da la misión a bots de la zona y lo avisa",
                      aviso or "sin aviso en 45 s: con QuestMates.Announce=0 no se avisa al jugador; "
                                "confirmar con grep 'quest-mates' Server.log («… cogen '…' con …»)",
                      estado_si_no="AVISO")
        inf.comprobar(sec, m.abandonar_mision(AMENAZA_INTERIOR), "se abandona como «Abandonar» (ranura del diario)")
        m.bombear(3)
        inf.comprobar(sec, AMENAZA_INTERIOR not in m.misiones(), "la misión sale del diario")
        inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")


def party_here_auto(ctx, espera_despedida, conservar=False):
    inf, sec = ctx.inf, "party-here (misión de grupo)" if not conservar else "party-here (M19: .grupo conserva)"
    m = ctx.nueva_sesion()
    estado = {"grupo": False}

    def deshacer():
        if estado["grupo"]:
            m.comando("grupo fuera", 3)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer) as pj:
        m.comando("levelup 17", 3)
        nivel = m.valor_propio(upd.UNIT_FIELD_LEVEL)
        if not inf.comprobar(sec, nivel == 18, "nivel de prueba", "nivel %d" % nivel, origen="entorno"):
            return
        texto = sin_colores(" ".join(m.comando("lookup tele sentinel", 2)))
        nombre = next(iter(re.findall(r"\[([^\]]+)\]", texto)), None)
        if nombre:
            m.comando("tele %s" % nombre, 5)
        inf.paso("en %s (mapa %s)" % (nombre or "el sitio de inicio", m.mapa))
        m.bombear(20)
        antes = len(m.mensajes)
        m.comando("quest add %d" % HERMANDAD_DEFIAS, 3)
        inf.comprobar(sec, HERMANDAD_DEFIAS in m.misiones(), "«The Defias Brotherhood» (2 sugeridos) en el diario",
                      origen="entorno")
        limite = time.time() + 90
        while time.time() < limite and not _companeros(m):
            m.bombear(2)
        m.bombear(8)
        comp = _companeros(m)
        estado["grupo"] = bool(comp)
        texto = sin_colores(" ".join(m.comando("grupo estado", 2)))
        inf.datos["party_here_auto"] = {"personaje": pj.nombre, "companeros": list(comp.values()),
                                        "grupo_estado": texto[:600],
                                        "mensajes": [t for _, t in m.mensajes[antes:]][:20]}
        inf.comprobar(sec, 1 <= len(comp) <= 2, "party-here invita solo al aceptar una misión de grupo (tope 2)",
                      ", ".join(comp.values()) or "nadie en 90 s", esperado="1-2", observado=len(comp))
        if not comp:
            return
        inf.comprobar(sec, "por mision de grupo" in texto, "`.grupo estado` los marca «por mision de grupo»",
                      texto[:200])

        if not conservar:
            inf.comprobar(sec, m.abandonar_mision(HERMANDAD_DEFIAS),
                          "se abandona la misión (CMSG_QUESTLOG_REMOVE_QUEST)")
            t0 = time.time()
            limite = t0 + espera_despedida
            while time.time() < limite and _companeros(m):
                m.bombear(5)
            restantes = _companeros(m)
            estado["grupo"] = bool(restantes)
            inf.comprobar(sec, not restantes, "al abandonarla, los compañeros se van solos",
                          "%s a los %.0f s" % ("se han ido" if not restantes
                                               else "siguen: " + ", ".join(restantes.values()), time.time() - t0),
                          esperado="grupo vacío en ≤ %d s" % espera_despedida)
            inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                          origen="cliente")
            return

        # M19: '.grupo conserva' los pasa a manuales antes de abandonar; deben
        # quedarse pese a abandonar la misión y esperar de sobra (más que el
        # margen de despedida normal, que ya no debe aplicarles).
        m.comando("grupo conserva", 3)
        m.bombear(3)
        texto_tras_conservar = sin_colores(" ".join(m.comando("grupo estado", 2)))
        inf.datos["party_here_conserva"] = {"personaje": pj.nombre, "companeros": list(comp.values()),
                                            "grupo_estado_tras_conservar": texto_tras_conservar[:600]}
        inf.comprobar(sec, "por mision de grupo" not in texto_tras_conservar,
                      "M19: '.grupo conserva' los convierte en manuales (ya no «por mision de grupo»)",
                      texto_tras_conservar[:200])
        inf.comprobar(sec, m.abandonar_mision(HERMANDAD_DEFIAS),
                      "se abandona la misión tras conservarlos (CMSG_QUESTLOG_REMOVE_QUEST)")
        t0 = time.time()
        limite = t0 + espera_despedida
        while time.time() < limite:
            m.bombear(5)
        restantes = _companeros(m)
        estado["grupo"] = bool(restantes)
        inf.comprobar(sec, len(restantes) == len(comp),
                      "M19: conservados, no se van al abandonar la misión ni pasado el margen de despedida",
                      "%d/%d siguen: %s" % (len(restantes), len(comp), ", ".join(restantes.values())),
                      esperado=len(comp), observado=len(restantes))
        inf.comprobar(sec, not m.errores_lectura, "paquetes leídos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")


def _uno_sync(ctx, espera_ciclo, orden):
    sec = "party-here (SP05: sincronizar mision, %s)" % orden
    inf = ctx.inf
    m = ctx.nueva_sesion()
    estado = {"grupo": False}

    def deshacer():
        if estado["grupo"]:
            m.comando("grupo fuera", 3)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer) as pj:
        m.comando("levelup 9", 3)   # el personaje temporal nace en nivel 1: 1+9 = 10
        nivel = m.valor_propio(upd.UNIT_FIELD_LEVEL)
        if not inf.comprobar(sec, nivel == 10, "nivel de prueba", "nivel %d" % nivel, origen="entorno"):
            return

        def esperar_companero(etiqueta):
            limite = time.time() + 90
            while time.time() < limite and not _companeros(m):
                m.bombear(2)
            comp = _companeros(m)
            estado["grupo"] = bool(comp)
            inf.comprobar(sec, bool(comp), "%s: '.grupo 1' trae un companero manual" % etiqueta,
                          ", ".join(comp.values()) or "nadie en 90 s", origen="entorno")
            return comp

        antes = len(m.mensajes)
        if orden == "grupo_primero":
            m.comando("grupo 1", 3)
            comp = esperar_companero("grupo antes de aceptar")
            if not comp:
                return
            m.comando("quest add %d" % AMENAZA_INTERIOR, 3)
            inf.comprobar(sec, AMENAZA_INTERIOR in m.misiones(),
                          "se acepta la mision CON el companero ya en el grupo", origen="entorno")
        else:
            m.comando("quest add %d" % AMENAZA_INTERIOR, 3)
            inf.comprobar(sec, AMENAZA_INTERIOR in m.misiones(),
                          "se acepta la mision ANTES de formar el grupo", origen="entorno")
            m.comando("grupo 1", 3)
            comp = esperar_companero("mision aceptada, luego '.grupo 1'")
            if not comp:
                return

        limite = time.time() + espera_ciclo
        aviso = None
        while time.time() < limite and aviso is None:
            m.bombear(3)
            aviso = next((t for _, t in m.mensajes[antes:] if "tambien lleva tu mision" in t), None)
        inf.datos.setdefault("party_here_sync", []).append(
            {"personaje": pj.nombre, "orden": orden, "companeros": list(comp.values()), "aviso": aviso})
        # Igual que quest-mates con QuestMates.Announce=0: sin el aviso de chat
        # (PartyHere.Announce=1 en la VM) no hay otra señal en el cliente de
        # que el companero recibio su propia copia de la mision.
        inf.comprobar(sec, aviso is not None,
                      "party-here sincroniza la mision al companero y avisa por chat",
                      aviso or ("sin aviso en %d s: confirmar con grep 'party-here' Server.log "
                                "(«... tambien lleva '...' de ...»)" % espera_ciclo),
                      estado_si_no="AVISO")

        inf.comprobar(sec, m.abandonar_mision(AMENAZA_INTERIOR),
                      "se abandona la mision (CMSG_QUESTLOG_REMOVE_QUEST)")
        m.bombear(5)
        restantes = _companeros(m)
        estado["grupo"] = bool(restantes)
        inf.comprobar(sec, bool(restantes),
                      "abandonar la mision NO despide al companero manual (solo se le retira su copia; "
                      "vino por '.grupo', no por la mision)",
                      ", ".join(restantes.values()) or "se ha ido (no esperado)")
        inf.comprobar(sec, not m.errores_lectura, "paquetes leidos sin error", "; ".join(m.errores_lectura[:3]),
                      origen="cliente")

        m.comando("grupo fuera", 3)
        m.bombear(3)
        quedan = _companeros(m)
        estado["grupo"] = bool(quedan)
        inf.comprobar(sec, not quedan, "'.grupo fuera' despide al companero (limpieza)",
                      ", ".join(quedan.values()) or "grupo vacio")


def party_here_quest_sync(ctx, espera_ciclo):
    for orden in ("grupo_primero", "mision_primero"):
        _uno_sync(ctx, espera_ciclo, orden)


@caso(id="misiones", titulo="quest-mates y misiones de grupo de party-here",
      descripcion=__doc__,
      etiquetas=("misiones", "quest-mates", "party-here", "sp05", "bots", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={
          "espera": Parametro(int, 120, "segundos dentro antes de aceptar (entrada de bots)", minimo=0, maximo=900),
          "espera_despedida": Parametro(int, 200, "segundos para que se vayan tras abandonar / se sincronice", minimo=130, maximo=900),
          "parte": Parametro(str, "ambas", "ambas | quest-mates | party-here | party-here-conserva | party-here-sinc",
                              opciones=["ambas", "quest-mates", "party-here", "party-here-conserva", "party-here-sinc"]),
      },
      duracion_max=1500, protege=("mod-quest-mates", "mod-party-here (misiones de grupo)", "PLAN M19", "SP05"),
      control="directo",
      observa=("aviso de sistema de quest-mates", "aviso de party-here («tambien lleva tu mision»)",
               "PLAYER_QUEST_LOG", "SMSG_GROUP_LIST", "`.grupo estado`"),
      no_cubre=("con QuestMates.Announce=0 no hay señal en el cliente de que los bots cojan la misión "
                "(Server.log: «… cogen '…' con …»)",
                "que los bots de quest-mates completen y entreguen la misión (sólo Server.log)",
                "liberación de la reserva de quest-mates al abandonar (sólo Server.log)",
                "'party-here-conserva' y 'party-here-sinc' no se ejecutan con 'ambas': pedirlos aparte",
                "SP05 con PartyHere.Announce=0: sin aviso en el cliente (Server.log: «... tambien lleva ...»)",
                "recogida real de la copia de botín de SP04 en la bolsa del compañero (sin acceso a su "
                "ventana de saqueo desde una cuenta humana) y un compañero concreto no elegible por "
                "raza/clase para una misión concreta (no determinista desde el cliente)"))
def ejecutar(ctx):
    if ctx.p["parte"] in ("ambas", "quest-mates"):
        quest_mates(ctx, ctx.p["espera"])
    if ctx.p["parte"] in ("ambas", "party-here"):
        party_here_auto(ctx, ctx.p["espera_despedida"])
    if ctx.p["parte"] == "party-here-conserva":
        party_here_auto(ctx, ctx.p["espera_despedida"], conservar=True)
    if ctx.p["parte"] == "party-here-sinc":
        party_here_quest_sync(ctx, ctx.p["espera_despedida"])
