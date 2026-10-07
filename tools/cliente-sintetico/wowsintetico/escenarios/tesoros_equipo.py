# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Comprueba las muestras de equipo de mod-treasure con botín determinista."""
import struct

from .. import actualizaciones as upd
from ..catalogo import caso
from ..mundo import SMSG_LOOT_RESPONSE, _leer_respuesta_loot
from . import PersonajeTemporal, sin_colores
from .profesiones import _paquete_hechizo


@caso(id="tesoros-equipo", titulo="SP03: cuatro cofres Vanilla de equipo",
      descripcion="Crea cuatro cofres temporales de equipo y abre el épico para verificar su botín.",
      etiquetas=("tesoros", "sp03", "equipo"),
      acciones=("conectar", "leer", "personaje", "gm"),
      duracion_max=150, protege=("SP03", "mod-treasure"), control="directo",
      en_todo=False,
      observa=("cuatro cofres de los dos tramos visibles", "botín épico garantizado", "retirada limpia"),
      no_cubre=("aspecto visual en Wow.exe",))
def ejecutar(ctx):
    inf, sec = ctx.inf, "tesoros-equipo"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, 1, 1, sec) as personaje:
        ctx.anotar_servidor(m)
        m.comando("gm on", 1)
        m.comando("go xyz -9751.66 184.723 55.732 0", 3)
        gps = m.gps()
        if not inf.comprobar(sec, gps is not None and gps.get("mapa") == 0,
                             "personaje en Elwynn", str(gps), origen="entorno"):
            return
        creados = 0
        try:
            for tier, variant, item in ((1, 1, 1718), (1, 2, 4091),
                                        (2, 1, 19099), (2, 2, 18805)):
                entry = 799000 + (tier - 1) * 2 + variant
                result = " ".join(map(sin_colores, m.comando_hasta(
                    "tesoro prueba equipo %s %d %d" % (personaje.nombre, tier, variant),
                    r"Cofre de equipo|No hay suelo|personaje humano", timeout=10)))
                if not inf.comprobar(sec, "Cofre de equipo" in result,
                                     "cofre %d creado" % entry, result):
                    return
                creados += 1
                m.bombear(1)
                visible = [guid for guid, obj in m.objetos.items()
                           if obj.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entry]
                if not inf.comprobar(sec, bool(visible), "cofre %d visible" % entry,
                                     "GUIDs %s" % visible):
                    return
                if item == 18805:
                    m.enviar(0x12E, _paquete_hechizo(3365, visible[0]))
                    opcode, data = m.esperar({SMSG_LOOT_RESPONSE, 0x130, 0x133}, timeout=8)
                    loot = _leer_respuesta_loot(data) if opcode == SMSG_LOOT_RESPONSE else {}
                    actual = struct.unpack_from("<I", data, 15)[0] if loot.get("abierto") and len(data) >= 19 else None
                    inf.comprobar(sec, actual == item, "el cofre épico entrega equipo épico",
                                  "objeto esperado %d; recibido %s" % (item, actual))
                    if loot.get("abierto"):
                        m.liberar_loot(visible[0])
        finally:
            result = " ".join(map(sin_colores, m.comando_hasta(
                "tesoro prueba retirar %s" % personaje.nombre, r"Retirados", timeout=8)))
            inf.comprobar(sec, "Retirados %d" % creados in result,
                          "cofres creados retirados", result)
