# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Reparto de loot en grupo con dos jugadores humanos reales y un bot (M40).

`AiPlayerbot.LootRollAfterRealPlayersPass` (activo en la VM, confirmado por
SSH antes de escribir este caso: `grep LootRollAfterRealPlayersPass
env/dist/etc/modules/playerbots.conf` → `= 1`; `config.sh` lo trae ya a
`true` por defecto) hace que, con reparto «Botín de grupo» o «Necesidad antes
que codicia», un bot no vote una tirada de botín mientras algún jugador
HUMANO participante siga sin votar; y si un humano ya reclamó el objeto
(necesidad/codicia/desencantar), el bot pasa directamente en vez de competir.

Monta un grupo real: `VERIFICADOR` (líder) pide un compañero bot con
`.grupo 1` (mod-party-here; mucho más rápido que `.grupo mazmorra`, que pide
4), invita a `VERIFICADOR2` y fija el reparto a Botín de grupo. Un `.npc add
5807` ("The Rake", nivel 10, con objetos verdes garantizados en su tabla de
botín) y `.die` (GM) generan un cadáver real; `CMSG_LOOT` sobre él ARRANCA de
verdad las tiradas de grupo (la muerte por sí sola no las crea, hace falta
que alguien abra el botín — comprobado en vivo: sin este paso no llega
ningún SMSG_LOOT_START_ROLL).

Con los objetos que salgan (normalmente varios: la tabla de "The Rake" tiene
más de un objeto verde+, no sólo el garantizado al 100%) se cubren, en este
orden y con el mismo grupo:
  1. humano pendiente: `VERIFICADOR` pasa, `VERIFICADOR2` NO vota todavía →
     el bot no debe votar mientras siga pendiente.
  2. se libera: `VERIFICADOR2` también pasa (ya no hay pendientes, ninguno
     reclamó) → el bot cae al comportamiento normal de la IA y termina
     votando algo.
  3. humano vota necesidad (con el otro ya pasado) → el bot pasa.
  4. humano vota codicia (con el otro ya pasado) → el bot pasa.

Fuera de alcance, documentado en `no_cubre` en vez de forzado (desproporcionado
frente a lo que prueba el propio parche):
  - desencantar: exige un objeto desencantable Y un personaje con Encantamiento;
  - grupo sólo de bots: este arnés siempre actúa como un jugador real, no hay
    forma de formar un grupo "sin humanos" sin salir de la prueba;
  - Botín maestro: bajo ese método el core NO genera ningún SMSG_LOOT_START_ROLL
    (`Group::MasterLoot` asigna directo) — no hay nada que observar por
    protocolo; el guard `!= MASTER_LOOT` ya se leyó en el propio parche;
  - salida de un jugador a mitad de una tirada abierta y expiración a los
    60 s: hace falta sincronizar el corte exacto de una ventana de un minuto
    por repetición sin cambiar el resultado observable (el core trata "no
    votó" como pase implícito, no un estado que la IA distinga) — desproporcionado
    para esta tarea, como ya adelantaba el propio encargo.
"""
import time

from .. import actualizaciones as upd
from ..catalogo import caso
from ..ejecutor import Bloqueo
from ..mundo import LOOT_METHOD_GROUP_LOOT, ROLL_GREED, ROLL_NEED, ROLL_PASS
from . import PersonajeTemporal, asegurar_vivo

RAZA, CLASE = 1, 1                              # humano, guerrero
CREATURA_PRUEBA = 5807                          # "The Rake": creature_loot_template con verdes garantizados
CREATURA_PRUEBA_NIVEL = 10                      # nivel de 5807; los jugadores suben a este nivel antes de matarla
UMBRAL_VERDE = 2                                # ITEM_QUALITY_UNCOMMON: el umbral por defecto del cliente


def _formar_con_un_bot(m, inf, sec, espera=90) -> dict:
    """`.grupo 1` (mod-party-here): un solo companero bot, mucho más rápido que
    `.grupo mazmorra` (4). Devuelve guid -> nombre de los companeros (sin contar `m`)."""
    m.comando("grupo 1", 2)
    limite = time.time() + espera
    while time.time() < limite:
        m.bombear(2)
        miembros = [x for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
        if len(miembros) >= 1 and all(x["conectado"] for x in miembros):
            break
    miembros = {x["guid"]: x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid}
    inf.comprobar(sec, len(miembros) == 1, "party-here trae un companero bot (`.grupo 1`)",
                  "%d companero(s): %s" % (len(miembros), ", ".join(miembros.values())),
                  esperado=1, observado=len(miembros))
    return miembros


def _matar_criatura_de_prueba(m, entry=CREATURA_PRUEBA):
    """`.npc add <entry>` junto al grupo y muerte por un golpe cuerpo a cuerpo REAL
    (`.modify hp 1` + `CMSG_ATTACKSWING`), no `.die`/`.damage` de GM. Comprobado en vivo:
    `Unit::Kill` sólo genera botín si `Creature::IsDamageEnoughForLootingAndReward()` es
    cierto, y eso exige daño real de un jugador contado por `Unit::DealDamage`
    (`LowerPlayerDamageReq`) — `.die` lo evita del todo (llama a `Unit::Kill` directamente) y
    `.damage` tampoco lo cuenta bien aquí; sin ese requisito el cadáver sale sin
    `UNIT_DYNFLAG_LOOTABLE` y CMSG_LOOT siempre devuelve LOOT_ERROR_DIDNT_KILL (¡0 tiradas en
    seis intentos seguidos hasta encontrar esto!). Compara antes/después por GUID (no basta
    con buscar `entry`: una copia vieja de una prueba anterior, sin `.npc delete`, dejaría el
    mismo entry ya rondando y esta función cogería la equivocada)."""
    antes = {g for g, o in m.objetos.items() if o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entry}
    m.comando("npc add %d" % entry, 3)
    m.bombear(1)
    ahora = {g for g, o in m.objetos.items() if o.get("valores", {}).get(upd.OBJECT_FIELD_ENTRY) == entry}
    nuevos = ahora - antes
    objetivo = next(iter(nuevos), None)
    if objetivo is None:
        return None
    m.seleccionar(objetivo)
    m.bombear(1)
    m.comando("modify hp 1", 2)
    m.bombear(1)
    m.atacar(objetivo)
    limite = time.time() + 40
    muerto = False
    while time.time() < limite and not muerto:
        m.bombear(1)
        muerto = m.objetos.get(objetivo, {}).get("valores", {}).get(upd.UNIT_FIELD_HEALTH) == 0
    m.dejar_de_atacar()
    m.bombear(1)
    return objetivo if muerto else None


@caso(id="botin-grupo-jugadores-reales", titulo="Reparto de loot en grupo con dos jugadores reales y un bot",
      descripcion=__doc__,
      etiquetas=("loot", "grupo", "mod-playerbots", "dos-cuentas", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"), duracion_max=600,
      protege=("patches/mod-playerbots/04-loot-roll-real-players-priority.patch", "PLAN M40"),
      control="directo",
      observa=("SMSG_LOOT_START_ROLL/SMSG_LOOT_ROLL (voto de cada participante, incluido el bot)",
               "CMSG_LOOT_ROLL propio (`VERIFICADOR`/`VERIFICADOR2`) y del bot (por difusión)",
               "SMSG_GROUP_LIST con el bot y `VERIFICADOR2` en el mismo grupo"),
      no_cubre=("desencantar (objeto desencantable + personaje con Encantamiento)",
                "grupo compuesto sólo por bots (este arnés siempre es un jugador real)",
                "Botín maestro: no genera ningún paquete de tirada que observar por protocolo",
                "salida de un jugador a mitad de una tirada abierta",
                "expiración de los 60 s sin voto (el core la trata como pase implícito, "
                "sin estado intermedio para la IA)"),
      en_todo=False)
def ejecutar(ctx):
    if not ctx.entorno.tiene_cuenta("secundaria"):
        raise Bloqueo("falta cuentas_adicionales.secundaria en el perfil")
    inf, sec = ctx.inf, "botin-grupo"
    a = ctx.nueva_sesion()
    b = ctx.nueva_sesion("secundaria")

    eventos = []
    a.oyentes.append(lambda t, ev, d: eventos.append((t, ev, dict(d))) if ev == "tirada_voto" else None)

    def deshacer(m):
        if (m.grupo or {}).get("miembros"):
            m.comando("grupo fuera", 2)
            m.bombear(1)

    with PersonajeTemporal(ctx, a, RAZA, CLASE, sec, antes_de_salir=lambda: deshacer(a)) as pa:
        with PersonajeTemporal(ctx, b, RAZA, CLASE, sec, antes_de_salir=lambda: deshacer(b)) as pb:
            ctx.anotar_servidor(a)
            asegurar_vivo(a)
            asegurar_vivo(b)

            # Nivel 1 no basta: `Group::GroupLoot()` sólo tira por un objeto si hay al menos un
            # participante que pueda usarlo (`Item::AllowedForPlayer`, RequiredLevel incluido) —
            # comprobado en vivo: con los dos jugadores en nivel 1, seis muertes seguidas de la
            # criatura de prueba no generaron NINGUNA tirada pese a tener un objeto verde
            # garantizado al 100% en su tabla (nivel de la criatura: 10). Subir a su nivel lo evita.
            a.comando("levelup %d" % (CREATURA_PRUEBA_NIVEL - 1), 3)
            b.comando("levelup %d" % (CREATURA_PRUEBA_NIVEL - 1), 3)
            a.bombear(1)
            b.bombear(1)

            companeros = _formar_con_un_bot(a, inf, sec)
            if not companeros:
                return
            guid_bot, nombre_bot = next(iter(companeros.items()))

            a.invitar_grupo(pb.nombre)
            b.bombear(1)
            b.aceptar_grupo()
            a.bombear(2)
            b.bombear(2)
            juntos = (any(x["guid"] == pb.guid for x in (a.grupo or {}).get("miembros", []))
                      and any(x["guid"] == pa.guid for x in (b.grupo or {}).get("miembros", [])))
            if not inf.comprobar(sec, juntos, "los dos jugadores reales y el bot comparten grupo",
                                 "grupo de A: %s" % [x["nombre"] for x in (a.grupo or {}).get("miembros", [])]):
                return

            a.establecer_metodo_loot(LOOT_METHOD_GROUP_LOOT, umbral=UMBRAL_VERDE)
            a.bombear(1)

            # El objeto garantizado (100%) de "The Rake" no siempre alcanza el umbral verde en
            # cada muerte (el resto de su tabla es probabilística); varias muertes seguidas
            # hasta reunir tiradas reales, en vez de forzar un único intento con RNG en contra.
            intentos_matanza = 0
            while len(a.tiradas) < 1 and intentos_matanza < 3:
                intentos_matanza += 1
                objetivo = _matar_criatura_de_prueba(a)
                if objetivo is None:
                    continue
                try:
                    a.abrir_criatura(objetivo, espera=25.0)
                except TimeoutError:
                    pass                                 # servidor cargado (600 bots activos): reintenta
                a.bombear(2)
                b.bombear(1)
                if not a.tiradas:
                    try:
                        a.liberar_loot(objetivo)
                    except Exception:                     # noqa: BLE001 — mejor esfuerzo entre intentos
                        pass
                    a.bombear(1)
            inf.anotar(sec, "OK" if a.tiradas else "AVISO",
                      "se coloca y mata una criatura de prueba (%d) hasta obtener botín verde+" % CREATURA_PRUEBA,
                      "%d intento(s)" % intentos_matanza)
            tiradas = list(a.tiradas.items())
            if not inf.comprobar(sec, len(tiradas) >= 1,
                                 "abrir el cadáver arranca tiradas de botín de grupo reales",
                                 "%d tirada(s) tras %d intento(s): %s"
                                 % (len(tiradas), intentos_matanza, [t["itemid"] for _, t in tiradas])):
                return
            inf.comprobar(sec, len(tiradas) >= 3,
                          "salen al menos 3 tiradas (para cubrir pendiente/necesidad/codicia por separado)",
                          "%d tirada(s): %s" % (len(tiradas), [t["itemid"] for _, t in tiradas]),
                          esperado=">=3", observado=len(tiradas), estado_si_no="AVISO")

            def voto_de(guid_objeto, jugador, espera=15):
                limite = time.time() + espera
                while time.time() < limite:
                    a.bombear(1)
                    v = next((d["tipo"] for _, ev, d in eventos
                             if d["guid"] == guid_objeto and d["jugador"] == jugador), None)
                    if v is not None:
                        return v
                return None

            # ── sub-caso 1: humano pendiente ────────────────────────────────
            guid1, t1 = tiradas[0]
            a.votar_loot(guid1, t1["ranura"], ROLL_PASS)
            a.bombear(1)
            b.bombear(1)
            time.sleep(8)
            a.bombear(2)
            voto_bot_pendiente = next((d["tipo"] for _, ev, d in eventos
                                      if d["guid"] == guid1 and d["jugador"] == guid_bot), None)
            inf.comprobar(sec, voto_bot_pendiente is None,
                          "humano pendiente: el bot NO vota mientras VERIFICADOR2 no ha votado",
                          "voto del bot tras 8 s: %s" % voto_bot_pendiente,
                          esperado=None, observado=voto_bot_pendiente)

            # se libera: el segundo humano también pasa (nadie reclamó) -> comportamiento normal
            b.votar_loot(guid1, t1["ranura"], ROLL_PASS)
            b.bombear(1)
            voto_bot_liberado = voto_de(guid1, guid_bot)
            inf.comprobar(sec, voto_bot_liberado is not None,
                          "sin humanos pendientes y sin reclamar: el bot ya vota (IA normal)",
                          "voto del bot: %s" % voto_bot_liberado)

            # ── sub-caso 2: humano vota necesidad ───────────────────────────
            if len(tiradas) >= 2:
                guid2, t2 = tiradas[1]
                a.votar_loot(guid2, t2["ranura"], ROLL_NEED)
                b.votar_loot(guid2, t2["ranura"], ROLL_PASS)
                a.bombear(1)
                b.bombear(1)
                voto_bot_necesidad = voto_de(guid2, guid_bot)
                inf.comprobar(sec, voto_bot_necesidad == ROLL_PASS,
                              "humano vota necesidad: el bot pasa en vez de competir",
                              "voto del bot: %s" % voto_bot_necesidad,
                              esperado=ROLL_PASS, observado=voto_bot_necesidad)
            else:
                inf.anotar(sec, "AVISO", "humano vota necesidad: sin segunda tirada real que usar",
                          "sólo salió %d tirada" % len(tiradas))

            # ── sub-caso 3: humano vota codicia ─────────────────────────────
            if len(tiradas) >= 3:
                guid3, t3 = tiradas[2]
                a.votar_loot(guid3, t3["ranura"], ROLL_PASS)
                b.votar_loot(guid3, t3["ranura"], ROLL_GREED)
                a.bombear(1)
                b.bombear(1)
                voto_bot_codicia = voto_de(guid3, guid_bot)
                inf.comprobar(sec, voto_bot_codicia == ROLL_PASS,
                              "humano vota codicia: el bot pasa en vez de competir",
                              "voto del bot: %s" % voto_bot_codicia,
                              esperado=ROLL_PASS, observado=voto_bot_codicia)
            else:
                inf.anotar(sec, "AVISO", "humano vota codicia: sin tercera tirada real que usar",
                          "sólo salieron %d tirada(s)" % len(tiradas))

            a.liberar_loot(objetivo)
