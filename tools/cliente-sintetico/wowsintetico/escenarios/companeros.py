# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-party-here: `.grupo mazmorra`, composición, `.grupo quieto`/`.grupo sigue` (M20)
y `.grupo fuera`, fuera de instancias."""
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import PersonajeTemporal, sin_colores
from .mazmorra import formar_grupo, roles, roles_del_grupo


def _lejos(m, propia, miembros, umbral=60):
    """Nombres de `miembros` (guid -> nombre) a más de `umbral` m de `propia`, o sin posición conocida."""
    # El servidor sólo envía las estadísticas espontáneamente si el miembro
    # está fuera del rango de visión. Al llegar por summon conserva en el
    # cliente la última posición lejana hasta que se pide el paquete FULL.
    for g in miembros:
        m.pedir_estadisticas(g)
    m.bombear(1)
    lejos = []
    for g, n in miembros.items():
        p = m.miembros.get(g, {})
        if propia and "x" in p:
            d = ((p["x"] - propia["x"]) ** 2 + (p["y"] - propia["y"]) ** 2) ** 0.5
            if d > umbral:
                lejos.append("%s a %.0f m" % (n, d))
        else:
            lejos.append("%s sin posicion" % n)
    return lejos


@caso(id="companeros", titulo="party-here reúne un grupo de cinco y lo despide",
      descripcion="""
Un mago de nivel `nivel` pide `.grupo mazmorra`: party-here debe traer cuatro
bots de su facción y tramo (ninguno por encima), conectados y junto al
jugador; `.grupo quieto` (antes del primer summon) debe impedir que lleguen y
`.grupo sigue` (M20) reanudar el viaje; `.grupo estado` debe explicarlos, con
tanque y sanador o, si en el tramo no hay de esos roles, con el aviso
explícito al jugador (M26); y `.grupo fuera` debe sacarlos y disolver el
grupo.
""",
      etiquetas=("party-here", "grupo", "bots", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"nivel": Parametro(int, 25, "nivel del jugador", minimo=10, maximo=80)},
      duracion_max=480, protege=("mod-party-here", "BotGear.h", "PLAN M20", "PLAN M26"), control="directo",
      observa=("SMSG_GROUP_LIST", "SMSG_PARTY_MEMBER_STATS (nivel, zona, posición)", "`.grupo estado`"),
      no_cubre=("reparto por misiones de grupo automáticas", "recomposición tras relogin",
                "`.grupo quieto`/`.grupo sigue` durante combate (sin combate real que provocar aquí)"))
def ejecutar(ctx):
    inf, sec, nivel = ctx.inf, "party-here", ctx.p["nivel"]
    m = ctx.nueva_sesion()
    estado = {"grupo": False}

    def deshacer():
        if estado["grupo"]:
            m.comando("grupo fuera", 3)

    with PersonajeTemporal(ctx, m, 1, 8, sec, antes_de_salir=deshacer):
        ctx.anotar_servidor(m)
        m.comando("levelup %d" % (nivel - 1), 3)
        inf.comprobar(sec, m.valor_propio(upd.UNIT_FIELD_LEVEL) == nivel, "nivel de prueba",
                      "nivel %d" % m.valor_propio(upd.UNIT_FIELD_LEVEL), origen="entorno")
        miembros = formar_grupo(m, inf, sec)
        estado["grupo"] = bool(miembros)

        # M20: ".grupo quieto" (sin nombre = todos), pedido ya con el grupo
        # formado pero antes de que venza el primer summon (SummonDelay 3 s +
        # hasta 2 s de margen, y el propio Tick sólo pasa cada ScanIntervalMs
        # 3 s): debe impedir que lleguen aunque se espere de sobra.
        m.comando("grupo quieto", 2)
        lejos_al_quieto = _lejos(m, m.posicion_actual(), miembros)
        # SMSG_PARTY_MEMBER_STATS de un compañero muy lejano (otro continente,
        # retenido antes de que llegue a estar cerca) puede no completarse
        # dentro del caso: se juzga sólo el nivel que SÍ se conoce, no se
        # cuenta como fuera de tramo un simple dato aún no recibido.
        limite = time.time() + 10
        niveles = {n: m.miembros.get(g, {}).get("nivel") for g, n in miembros.items()}
        while time.time() < limite and any(v is None for v in niveles.values()):
            m.bombear(2)
            niveles = {n: m.miembros.get(g, {}).get("nivel") for g, n in miembros.items()}
        fuera_tramo = [n for n, v in niveles.items() if v is not None and not (nivel - 3 <= v <= nivel)]
        inf.comprobar(sec, not fuera_tramo,
                      "los compañeros son de tu tramo (hasta 3 por debajo, ninguno por encima)",
                      ", ".join("%s %s" % (n, v if v is not None else "?") for n, v in niveles.items()))
        propia = m.posicion_actual()
        lejos_quieto = _lejos(m, propia, miembros)
        nombres_antes = {x.split(" ", 1)[0] for x in lejos_al_quieto}
        nombres_despues = {x.split(" ", 1)[0] for x in lejos_quieto}
        inf.comprobar(sec, bool(nombres_antes) and nombres_antes <= nombres_despues,
                      "M20: '.grupo quieto' mantiene lejos a quienes estaban lejos",
                      "%d distantes al ordenar, %d siguen lejos; %d ya estaban cerca (%s)" % (
                          len(nombres_antes), len(nombres_antes & nombres_despues),
                          len(miembros) - len(nombres_antes), ", ".join(lejos_quieto)),
                      esperado=sorted(nombres_antes), observado=sorted(nombres_antes & nombres_despues),
                      estado_si_no="AVISO" if not nombres_antes else "FALLO")

        # ".grupo quieto <nombre>" / ".grupo sigue <nombre>" (M20): un
        # compañero concreto, comprobado por el mensaje de confirmación
        # (n companero(s) ...) ya que todos siguen quietos por la orden general.
        un_nombre = next(iter(miembros.values()))
        antes_msj = len(m.mensajes)
        m.comando("grupo quieto %s" % un_nombre, 2)
        msj_quieto = sin_colores(" ".join(t for _, t in m.mensajes[antes_msj:]))
        inf.comprobar(sec, "1 companero(s) se quedan quietos." in msj_quieto,
                      "M20: '.grupo quieto <nombre>' retiene solo a ese companero",
                      msj_quieto[:160], estado_si_no="AVISO")
        antes_msj = len(m.mensajes)
        m.comando("grupo sigue %s" % un_nombre, 2)
        msj_sigue = sin_colores(" ".join(t for _, t in m.mensajes[antes_msj:]))
        inf.comprobar(sec, "1 companero(s) vuelven a seguirte." in msj_sigue,
                      "M20: '.grupo sigue <nombre>' libera solo a ese companero",
                      msj_sigue[:160], estado_si_no="AVISO")

        # ".grupo sigue" (todos): M20 recalcula destino y vigencia al reanudar,
        # así que deben llegar aunque summonMs/giveUpMs hubieran vencido
        # mientras estaban quietos.
        # BotMinWorldSeconds (60 s por defecto, M08) puede seguir bloqueando el
        # summon si el bot se despertó hace poco para este grupo: se espera lo
        # suficiente para cruzar ese margen, no sólo el retry de summon.
        m.comando("grupo sigue", 2)
        limite = time.time() + 75
        lejos = _lejos(m, m.posicion_actual(), miembros)
        while time.time() < limite and lejos:
            m.bombear(3)
            lejos = _lejos(m, m.posicion_actual(), miembros)
        inf.comprobar(sec, not lejos, "M20: '.grupo sigue' reanuda el viaje y llegan junto al jugador",
                      ", ".join(lejos), estado_si_no="AVISO")
        texto = roles_del_grupo(m)
        inf.datos["grupo_estado"] = texto[:800]
        inf.comprobar(sec, all(n in texto for n in miembros.values()), "`.grupo estado` explica a cada compañero",
                      texto[:200], estado_si_no="AVISO")
        # M26: tanque y sanador, o un aviso explícito de que faltan (nunca en silencio).
        r = roles(texto)
        aviso = next((sin_colores(t) for _, t in m.mensajes
                      if any("tu grupo %s sin" % verbo in sin_colores(t)
                             for verbo in ("se completa", "se queda"))), "")
        cambio = [n for n, rol in r.items() if rol in ("tanque", "sanador")]
        inf.datos["roles"] = r
        inf.datos["aviso_rol"] = aviso
        con_roles = "tanque" in r.values() and "sanador" in r.values()
        inf.comprobar(sec, con_roles or bool(aviso),
                      "el grupo lleva tanque y sanador, o se avisa de que falta (M26)",
                      "roles: %s%s" % (", ".join("%s %s" % x for x in r.items()) or "?",
                                       ("; aviso: " + aviso[:120]) if aviso else ""),
                      esperado="tanque+sanador o aviso", observado="tanque+sanador" if con_roles else
                      ("aviso" if aviso else "ni roles ni aviso"))
        inf.comprobar(sec, con_roles, "con tanque y sanador de verdad", ", ".join(cambio) or "ninguno",
                      estado_si_no="AVISO")
        m.comando("grupo fuera", 3)
        m.bombear(5)
        restantes = [x["nombre"] for x in (m.grupo or {}).get("miembros", []) if x["guid"] != m.guid]
        estado["grupo"] = bool(restantes)
        inf.comprobar(sec, not restantes, "`.grupo fuera` despide a todos", ", ".join(restantes))
