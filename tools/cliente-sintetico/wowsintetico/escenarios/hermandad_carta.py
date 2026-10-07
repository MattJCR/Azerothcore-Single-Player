# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fundar una hermandad con una carta REAL comprada al Vendedor de Cartas, sin
ningún firmante, para probar `MinPetitionSigns=0` (config.sh, sección
"SERVIDOR — HERMANDADES") por el camino que de verdad usa un jugador.

Los casos `hermandad`/`hermandad-avanzada` fundan con `.guild create`, el
comando de GM que `GuildScript::OnCreate` no distingue del flujo normal para
mod-home-guild — pero ese comando crea la guild directamente y NUNCA pasa por
`HandleTurnInPetitionOpcode`, así que no demuestra nada sobre `MinPetitionSigns`.
Aquí el personaje compra la carta de verdad (`CMSG_PETITION_BUY`, item 5863) a
un Vendedor de Cartas (`UNIT_NPC_FLAG_PETITIONER|TABARDDESIGNER`, entry 4974
"Aldwin Laughlin", el mismo de Stormwind) y la entrega sin firmas
(`CMSG_TURN_IN_PETITION`). El servidor decide solo con `MinPetitionSigns`
(`WorldConfig.cpp`): a 0 debe crear la guild con 0 firmantes.

No se navega hasta Stormwind: se spawnea una copia temporal del NPC junto al
personaje con `.npc add 4974` (mismo patrón que `botin_grupo.py`), se compara
la lista de criaturas antes/después por `entrada` para coger su GUID, y se
borra al terminar.
"""
from .. import actualizaciones as upd
from ..catalogo import caso
from ..ejecutor import Bloqueo
from ..mundo import PETITION_TURN_OK
from . import PersonajeTemporal, nombre_aleatorio, sin_colores

NPC_VENDEDOR_CARTAS = 4974   # "Aldwin Laughlin", Vendedor de Cartas de Stormwind


def _spawnear_vendedor(m, inf, sec):
    """`.npc add <entry>` junto al personaje; diff por `entrada` (no basta con
    buscarla sola: una copia vieja de una prueba anterior sin `.npc delete`
    dejaría el mismo entry ya rondando, mismo motivo que en `botin_grupo.py`)."""
    antes = {g for g, o in m.objetos.items() if o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == NPC_VENDEDOR_CARTAS}
    m.comando("npc add %d" % NPC_VENDEDOR_CARTAS, 3)
    m.bombear(1)
    ahora = {g for g, o in m.objetos.items() if o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == NPC_VENDEDOR_CARTAS}
    nuevos = ahora - antes
    guid = next(iter(nuevos), None)
    flags = m.objetos.get(guid, {}).get("valores", {}).get(upd.UNIT_NPC_FLAGS, 0) if guid else 0
    necesarios = upd.UNIT_NPC_FLAG_PETITIONER | upd.UNIT_NPC_FLAG_TABARDDESIGNER
    inf.comprobar(sec, guid is not None and (flags & necesarios) == necesarios,
                  "`.npc add %d` da un Vendedor de Cartas real (PETITIONER|TABARDDESIGNER)" % NPC_VENDEDOR_CARTAS,
                  "guid=%s flags=0x%X" % (guid, flags), esperado="0x%X" % necesarios, observado="0x%X" % flags)
    return guid


@caso(id="hermandad-carta", titulo="Fundar hermandad con carta real y 0 firmantes (MinPetitionSigns=0)",
      descripcion=__doc__,
      etiquetas=("home-guild", "core", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=120, protege=("MinPetitionSigns", "config.sh HERMANDADES", "GUILD_MIN_PETITION_SIGNS"),
      control="directo",
      observa=("SMSG_TURN_IN_PETITION_RESULTS (PETITION_TURN_OK)", "PLAYER_GUILDID tras la entrega"),
      no_cubre=("CMSG_PETITION_SHOWLIST/CMSG_PETITION_SIGN: no hace falta ningún firmante con "
                "MinPetitionSigns=0, así que el caso no los ejerce",
                "cartas de banda de arena (mismo mecanismo, MinPetitionSigns no las toca: usan `type - 1`)",
                "MinPetitionSigns > 0 (probaría PETITION_TURN_NEED_MORE_SIGNATURES, no esta tarea)"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "home-guild-carta"
    m = ctx.nueva_sesion()
    nombre_h = nombre_aleatorio("Vs")
    estado = {"npc": None, "hermandad": None}

    def deshacer():
        if estado["npc"]:
            m.seleccionar(estado["npc"])
            m.comando("npc delete", 2)
        if estado["hermandad"]:
            m.comando('guild delete "%s"' % estado["hermandad"], 4)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer) as pj:
        ctx.anotar_servidor(m)

        m.comando("modify money 1000000", 2)
        inf.comprobar(sec, m.valor_propio(upd.PLAYER_FIELD_COINAGE) >= 1000000,
                      "`.modify money` da para pagar la carta", "%d cobre" % m.valor_propio(upd.PLAYER_FIELD_COINAGE))

        npc = _spawnear_vendedor(m, inf, sec)
        if npc is None:
            raise Bloqueo("no se pudo spawnear un Vendedor de Cartas válido")
        estado["npc"] = npc

        guid_carta = m.comprar_carta_hermandad(npc, nombre_h)
        inf.datos["carta"] = {"nombre_hermandad": nombre_h, "guid": guid_carta}
        inf.comprobar(sec, guid_carta is not None,
                      "`CMSG_PETITION_BUY` deja la carta (item 5863) en la mochila",
                      "guid=%s" % guid_carta)
        if guid_carta is None:
            raise Bloqueo("la compra de la carta no dejó ningún objeto nuevo en la mochila")

        codigo = m.entregar_carta(guid_carta)
        inf.datos["turn_in_codigo"] = codigo
        inf.comprobar(sec, codigo == PETITION_TURN_OK,
                      "MinPetitionSigns=0: `CMSG_TURN_IN_PETITION` sin firmantes funda la hermandad",
                      "SMSG_TURN_IN_PETITION_RESULTS = %d" % codigo,
                      esperado=PETITION_TURN_OK, observado=codigo)
        if codigo == PETITION_TURN_OK:
            estado["hermandad"] = nombre_h

        m.bombear(3)
        guild_id = m.valor_propio(upd.PLAYER_GUILDID)
        inf.comprobar(sec, guild_id != 0,
                      "el personaje queda como miembro (líder) de la hermandad recién fundada",
                      "PLAYER_GUILDID=%d" % guild_id, esperado="!=0", observado=guild_id)
