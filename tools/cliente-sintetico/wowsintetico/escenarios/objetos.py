# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Escenario de objetos propios y botín (M39: ganzúa de recompensa, 600000).

Personaje nuevo sin Herrería. `.additem` le da la ganzúa (600000), un cajón de
pícaro real (Battered Junkbox, 16882) y, como control negativo, la Llave
esqueleto de titanio real (43853, de la que se clonó la ganzúa) — mismo
hechizo (59403), pero con `RequiredSkill=164`.

`BagFamily=256` en la ganzúa y la llave (heredado de la Titanium Skeleton Key):
van al LLAVERO (`PLAYER_FIELD_KEYRING_SLOT_1`, ranuras de inventario 86..117),
no a la mochila — al principio, comparando sólo `objetos_bolsas()` antes/
después, `.additem 600000 1`/`.additem 43853 1` parecían no hacer nada (sin
mensaje de chat, sin cambio en la mochila); en realidad sí funcionaban, sólo
que en el llavero. `objetos_bolsas()` no basta para objetos de esta familia:
hace falta `objetos_llavero()`.

El caso confirma, contra el estado real del servidor (no el texto de un
comando):
  1. el cajón empieza cerrado (CMSG_OPEN_ITEM → EQUIP_ERR_ITEM_LOCKED),
  2. la llave real SÍ exige Herrería (`Player::CanUseItem` la rechaza con
     EQUIP_ERR_NO_REQUIRED_PROFICIENCY antes de leer ningún objetivo) — así el
     caso demuestra que sabe detectar ese rechazo cuando de verdad ocurre,
  3. la ganzúa se usa sobre el cajón SIN que el servidor la rechace (prueba
     en vivo que `RequiredSkill=0` no bloquea el uso), y
  4. el cajón queda abierto de verdad (SMSG_LOOT_RESPONSE, no un segundo
     EQUIP_ERR_ITEM_LOCKED).
"""
from ..catalogo import caso
from . import PersonajeTemporal, asegurar_vivo

RAZA, CLASE = 1, 1                              # humano, guerrero (sin Herrería de partida)
GANZUA, JUNKBOX, LLAVE_HERRERO = 600000, 16882, 43853
HECHIZO_ABRIR = 59403                           # spellid_1 compartido por 600000 y 43853
MOCHILA_BASE = 23                               # PLAYER_FIELD_PACK_SLOT_1 == ranura de inventario 23
LLAVERO_BASE = 86                               # PLAYER_FIELD_KEYRING_SLOT_1 == ranura de inventario 86
EQUIP_ERR_NO_REQUIRED_PROFICIENCY = 8
EQUIP_ERR_ITEM_LOCKED = 36


def _nueva_ranura(antes, ahora):
    for i, (par_antes, par_ahora) in enumerate(zip(antes, ahora)):
        if par_ahora != (0, 0) and par_ahora != par_antes:
            return i, par_ahora[0] | (par_ahora[1] << 32)
    return None, None


def _dar_objeto(m, entry, llavero: bool, cantidad=1, espera=2.5):
    """`.additem <entry> <cantidad>` y localiza dónde cayó comparando el estado real
    (mochila o llavero, según `BagFamily`) antes/después — no el texto del GM, que
    `.additem` no siempre manda (ver docstring del módulo). Devuelve (base + índice, GUID
    completo) listo para pasar a `usar_objeto`/`abrir_objeto`, o (None, None) si no se ve
    el cambio."""
    leer = m.objetos_llavero if llavero else m.objetos_bolsas
    base = LLAVERO_BASE if llavero else MOCHILA_BASE
    antes = leer()
    m.comando("additem %d %d" % (entry, cantidad), espera)
    i, guid = _nueva_ranura(antes, leer())
    return (base + i, guid) if i is not None else (None, None)


@caso(id="objetos-ganzua-recompensa",
      titulo="Ganzúa de recompensa (600000): sin Herrería, y abre un cajón real",
      descripcion="""
Objeto propio 600000 (`patches/custom-items/600000-ganzua-de-recompensa.sql`):
copia de la Titanium Skeleton Key (43853) sin `RequiredSkill`, para que un
personaje sin Herrería pueda abrir los cajones de pícaro que entrega
`congrats_on_level_rewards.sql` en los niveles impares. Las propiedades
estáticas del objeto (`bonding=1`, `RequiredSkill=0`, `spellid_1=59403`,
`BagFamily=256`, `BuyPrice=0`) se comprobaron por SQL contra `item_template`
antes de escribir este caso (no se repite aquí: el cliente sintético habla
protocolo, no SQL).

Lo que sí prueba este caso, en vivo, con un personaje sin Herrería:
- un cajón de pícaro real (16882) EMPIEZA cerrado (EQUIP_ERR_ITEM_LOCKED);
- la Llave esqueleto de titanio REAL (43853, mismo hechizo 59403) SÍ es
  rechazada por `Player::CanUseItem` con EQUIP_ERR_NO_REQUIRED_PROFICIENCY —
  control negativo: si esto no saliera, un "no hay error" de la ganzúa no
  demostraría nada (podría ser el arnés el que no lee bien el paquete);
- la ganzúa (600000) se usa sobre el cajón sin que el servidor la rechace, y
- el cajón queda REALMENTE abierto después (SMSG_LOOT_RESPONSE con
  `loot_type != LOOT_NONE`, no un EQUIP_ERR_ITEM_LOCKED repetido).

`spellcharges_1=-1` ("se gasta en un uso"): tras abrir el cajón se comprueba
que la ganzúa ha desaparecido del llavero.
""",
      etiquetas=("objetos", "loot", "inventario", "gm"),
      acciones=("conectar", "leer", "personaje", "gm"), duracion_max=180,
      protege=("patches/custom-items/600000-ganzua-de-recompensa.sql", "PLAN M39"),
      control="directo",
      observa=("PLAYER_FIELD_KEYRING_SLOT_1..32 (ganzúa/llave) y PLAYER_FIELD_PACK_SLOT_1..16 "
               "(cajón) antes/después de `.additem`",
               "SMSG_INVENTORY_CHANGE_FAILURE (Player::CanUseItem / SendEquipError) al usar cada objeto",
               "SMSG_LOOT_RESPONSE (Player::SendLoot/SendLootError, mismo opcode para éxito y error) "
               "al abrir el cajón antes y después de la ganzúa"),
      no_cubre=("el estado dinámico de `bonding` (que el objeto quede realmente soulbound tras el primer "
                "uso, `Bonding == BIND_WHEN_USE/PICKED_UP` en SpellHandler.cpp): no hay parser de "
                "ITEM_FIELD_FLAGS en el cliente sintético; se confía en `bonding=1` de item_template, "
                "comprobado por SQL antes de escribir este caso, no aquí",
                "contenido exacto del botín del cajón: sólo se confirma que la ventana de botín se abre "
                "(loot_type != LOOT_NONE), no objeto por objeto",
                "icono y texto verde \"Uso: ...\" del cliente real: eso depende del parche de cliente "
                "(patch-<idioma>-4.MPQ), no del protocolo"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "objetos"
    m = ctx.nueva_sesion()
    with PersonajeTemporal(ctx, m, RAZA, CLASE, sec) as pj:
        ctx.anotar_servidor(m)
        asegurar_vivo(m)

        ranura_g, guid_g = _dar_objeto(m, GANZUA, llavero=True)
        ranura_c, guid_c = _dar_objeto(m, JUNKBOX, llavero=False)
        ranura_k, guid_k = _dar_objeto(m, LLAVE_HERRERO, llavero=True)
        if not inf.comprobar(sec, None not in (ranura_g, ranura_c, ranura_k),
                             "`.additem` entrega ganzúa y llave (llavero) y cajón (mochila)",
                             "ranuras %s/%s/%s" % (ranura_g, ranura_c, ranura_k)):
            return

        if not inf.comprobar(sec, asegurar_vivo(m),
                             "el personaje sigue (o vuelve a estar) vivo antes de usar los objetos",
                             origen="entorno"):
            return

        antes = m.abrir_objeto(ranura_c)
        inf.comprobar(sec, antes["error_equipo"] == EQUIP_ERR_ITEM_LOCKED and not antes["abierto"],
                      "el cajón (16882) empieza cerrado", str(antes),
                      esperado=EQUIP_ERR_ITEM_LOCKED, observado=antes["error_equipo"])

        err_k = m.usar_objeto(ranura_k, HECHIZO_ABRIR, guid_k)
        inf.comprobar(sec, err_k == EQUIP_ERR_NO_REQUIRED_PROFICIENCY,
                      "control: la llave real (43853, RequiredSkill=164) SÍ exige Herrería",
                      "SMSG_INVENTORY_CHANGE_FAILURE código %s" % err_k,
                      esperado=EQUIP_ERR_NO_REQUIRED_PROFICIENCY, observado=err_k)

        err_g = m.usar_objeto(ranura_g, HECHIZO_ABRIR, guid_g, guid_objetivo=guid_c)
        if not inf.comprobar(sec, err_g is None,
                             "la ganzúa (600000, RequiredSkill=0) se usa sobre el cajón sin ser rechazada",
                             "SMSG_INVENTORY_CHANGE_FAILURE código %s" % err_g, esperado=None, observado=err_g):
            return

        despues = m.abrir_objeto(ranura_c)
        if inf.comprobar(sec, despues["abierto"],
                         "el cajón queda REALMENTE abierto tras la ganzúa (SMSG_LOOT_RESPONSE)",
                         str(despues), esperado=True, observado=despues["abierto"]):
            m.liberar_loot(despues["guid"])

        m.bombear(1.0)
        libre = m.objetos_llavero()[ranura_g - LLAVERO_BASE] == (0, 0)
        inf.comprobar(sec, libre, "la ganzúa se consume en el único uso (spellcharges_1=-1)",
                      "ranura %d = %s" % (ranura_g, m.objetos_llavero()[ranura_g - LLAVERO_BASE]),
                      estado_si_no="AVISO")
