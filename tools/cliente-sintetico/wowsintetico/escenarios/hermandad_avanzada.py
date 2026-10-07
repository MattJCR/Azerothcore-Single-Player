# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""mod-home-guild: renivelado automático, `.hermandad fijar`/`excluir` (PLAN M36)
y reclamación de la hermandad tras `HomeGuild.InactiveOwnerReclaimDays` días de
inactividad del líder.

El caso `hermandad` (M11/M21) ya cubre adopción, reclutamiento y preferencia de
`party-here`; éste cubre justo lo que allí quedaba declarado como hueco.

Dos fases porque la reclamación exige que el DUEÑO esté DESCONECTADO cuando
`ReclaimInactiveGuilds()` corre (`ObjectAccessor::FindPlayer` lo descarta si
sigue dentro), y esa función sólo se llama al arrancar y cada 6 horas -o en el
flanco apagado→encendido de `HomeGuild.Enable` en un `.reload config`, el
mismo truco que ya usó M22 con `IndividualProgression.Enable`-, algo que hay
que hacer desde fuera del cliente sintético (SSH + `.reload config`), no desde
un caso Python de punta a punta:

- `fase=crear`: funda la hermandad, espera la adopción, prueba renivelado
  (`.levelup <negativo> <nombre>` deja a un compañero por debajo de
  `HomeGuild.ReLevelBehind` niveles y se comprueba que sube solo en las
  siguientes pasadas de `Care`), `.hermandad fijar`/`excluir` (efecto visible
  en `.hermandad estado`: «[fijado]» y desaparición del excluido), y termina
  con el personaje DESCONECTADO (sin borrarlo) para la fase de reclamación.
  Devuelve nombre/guid/raza/clase en `datos` para la fase siguiente.
- (fuera del caso, por SSH) `UPDATE account SET last_login = ... WHERE
  username=...` y el flanco `HomeGuild.Enable` 0→1 con `.reload config`.
- `fase=verificar`: reconecta el MISMO personaje (nombre/guid/raza de la fase
  anterior) y confirma que `.hermandad estado` ya no la reconoce como
  hermandad de casa (reclamada). Borra el personaje al final.
"""
import re
import time

from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from ..ejecutor import Bloqueo
from . import nombre_aleatorio, sin_colores
from .. import registro
from .hermandad import _estado

MIEMBRO_FIJADO = re.compile(r"^\s+(\S+) \(nivel (\d+), [^)]*\)(\s*\[fijado\])?")


def _miembros_detalle(m):
    """`.hermandad estado` -> {nombre: {"nivel": N, "fijado": bool}} de los conectados."""
    lineas = [sin_colores(t) for t in m.comando("hermandad estado", 3)]
    out = {}
    for bloque in lineas:
        for linea in bloque.split("\n"):
            x = MIEMBRO_FIJADO.match(linea)
            if x:
                out[x.group(1)] = {"nivel": int(x.group(2)), "fijado": bool(x.group(3))}
    return out


def _fase_crear(ctx, inf, sec):
    m = ctx.nueva_sesion()
    nivel = ctx.p["nivel"]
    nombre_h = nombre_aleatorio("Vs")

    nombre = nombre_aleatorio(ctx.entorno.prefijo)
    r = m.crear_personaje(nombre, 1, 8)   # humano/mago: mismo combo que `hermandad`
    from ..mundo import CHAR_CREATE_SUCCESS
    if not inf.comprobar(sec, r == CHAR_CREATE_SUCCESS, "creación del personaje",
                          "%s (código 0x%02X)" % (nombre, r), origen="entorno"):
        raise Bloqueo("no se pudo crear el personaje de prueba")
    guid = next(p["guid"] for p in m.personajes() if p["nombre"] == nombre)
    registro.apuntar(ctx.entorno.host, m.usuario, nombre, guid, ctx.caso.id)
    inf.datos["personaje"] = {"nombre": nombre, "guid": guid, "raza": 1, "cuenta": m.usuario}

    try:
        m.entrar(guid, 1, nombre=nombre)
        ctx.anotar_servidor(m)
        m.comando("levelup %d" % (nivel - 1), 3)
        inf.comprobar(sec, m.valor_propio(upd.UNIT_FIELD_LEVEL) == nivel, "nivel de prueba",
                      "nivel %d" % m.valor_propio(upd.UNIT_FIELD_LEVEL), origen="entorno")

        r = sin_colores(" ".join(m.comando('guild create %s "%s"' % (nombre, nombre_h), 4)))
        inf.datos["guild_create"] = r[:200]

        limite = time.time() + ctx.p["espera"]
        texto, total, conectados, miembros = "", None, None, {}
        while time.time() < limite:
            texto, total, conectados, miembros = _estado(m)
            if len(miembros) >= 3:
                break
            m.bombear(10)
        # El titular (total/conectados de la cabecera) puede fluctuar un poco
        # entre pasadas de KeepOnline/recruit; lo que de verdad hace falta para
        # el resto de la prueba es el roster de conectados (miembros), no ese
        # número exacto en el instante final del sondeo.
        inf.comprobar(sec, len(miembros) >= 3,
                      "la hermandad recluta y conecta al menos 3 companeros",
                      "%s companeros (cabecera), %s conectados (cabecera), %d en el roster" % (
                          total, conectados, len(miembros)),
                      esperado=">=3 en el roster", observado=len(miembros), estado_si_no="AVISO")
        if not miembros:
            raise Bloqueo("sin companeros conectados: no se puede probar renivelado ni fijar/excluir")

        nombres = sorted(miembros)
        objetivo_relevel = nombres[0]
        nivel_antes = miembros[objetivo_relevel]

        # ── M36a: renivelado automático ──────────────────────────────────
        # Lo deja bastante por debajo de HomeGuild.ReLevelBehind (4 por
        # defecto) niveles del dueño: ReLevelOne sólo actúa sobre quien esté
        # rezagado. `.levelup` es un delta RELATIVO (int16, admite negativos).
        nivel_objetivo = max(1, nivel - 10)
        bajada = nivel_antes - nivel_objetivo
        resp_levelup = ""
        if bajada > 0:
            # '.levelup [Personaje] <delta>': el nombre va PRIMERO (Optional<PlayerIdentifier>
            # antes que el int16 en HandleLevelUpCommand); con el orden cambiado el propio
            # core intenta interpretar el delta como nombre de personaje y falla en silencio.
            resp_levelup = sin_colores(" ".join(m.comando("levelup %s -%d" % (objetivo_relevel, bajada), 3)))
        inf.datos["levelup_respuesta"] = resp_levelup[:200]
        m.bombear(3)
        detalle = _miembros_detalle(m)
        nivel_forzado = detalle.get(objetivo_relevel, {}).get("nivel")
        inf.comprobar(sec, nivel_forzado is not None and nivel_forzado <= nivel - 4,
                      "se deja a un companero rezagado por debajo de ReLevelBehind (4 por defecto)",
                      "%s: nivel %s -> %s (dueño nivel %s)" % (objetivo_relevel, nivel_antes, nivel_forzado, nivel),
                      esperado="<=%s" % (nivel - 4), observado=nivel_forzado, origen="entorno")

        limite = time.time() + ctx.p["espera_relevel"]
        nivel_final = nivel_forzado
        historia = [nivel_final]
        while time.time() < limite and (nivel_final is None or nivel_final < nivel - 4):
            m.bombear(15)   # >= HomeGuild.CareIntervalSeconds (30s por defecto: dos pasadas de margen)
            detalle = _miembros_detalle(m)
            nivel_final = detalle.get(objetivo_relevel, {}).get("nivel")
            historia.append(nivel_final)
        inf.datos["relevel"] = {"companero": objetivo_relevel, "antes": nivel_antes,
                                "forzado_a": nivel_forzado, "evolucion": historia}
        inf.comprobar(sec, nivel_final is not None and nivel_final > nivel_forzado,
                      "M36: el companero rezagado sube de nivel solo, en pasadas de Care",
                      "nivel %s -> %s tras forzarlo a %s (evolución: %s)" % (
                          nivel_antes, nivel_final, nivel_forzado, historia),
                      esperado=">%s" % nivel_forzado, observado=nivel_final)

        # ── M36b: `.hermandad fijar` / `excluir` ──────────────────────────
        detalle = _miembros_detalle(m)
        candidatos = [n for n in detalle if n != objetivo_relevel]
        if len(candidatos) >= 2:
            a_fijar, a_excluir = candidatos[0], candidatos[1]
            r = sin_colores(" ".join(m.comando("hermandad fijar %s" % a_fijar, 3)))
            inf.datos["fijar_respuesta"] = r[:150]
            m.bombear(2)
            detalle = _miembros_detalle(m)
            inf.comprobar(sec, detalle.get(a_fijar, {}).get("fijado", False),
                          "M36: `.hermandad fijar <nombre>` marca «[fijado]» en `.hermandad estado`",
                          "%s: %s" % (a_fijar, detalle.get(a_fijar)),
                          esperado="fijado=True", observado=detalle.get(a_fijar))

            r = sin_colores(" ".join(m.comando("hermandad excluir %s" % a_excluir, 3)))
            inf.datos["excluir_respuesta"] = r[:150]
            limite = time.time() + 60
            sigue = a_excluir in _miembros_detalle(m)
            while time.time() < limite and sigue:
                m.bombear(10)
                sigue = a_excluir in _miembros_detalle(m)
            inf.comprobar(sec, not sigue,
                          "M36: `.hermandad excluir <nombre>` lo saca de la hermandad (Care, nextCareMs=0)",
                          "%s sigue en `.hermandad estado`" % a_excluir if sigue else "%s ya no aparece" % a_excluir,
                          estado_si_no="AVISO")
            # No se vuelve a reclutar aunque queden huecos (fila excluded=1 en
            # mod_home_guild_member): unas pasadas más de margen y se comprueba
            # que sigue sin aparecer.
            m.bombear(35)
            reaparecio = a_excluir in _miembros_detalle(m)
            inf.comprobar(sec, not reaparecio,
                          "M36: un companero excluido no se vuelve a reclutar aunque haya hueco",
                          "reaparecio" if reaparecio else "sigue fuera", estado_si_no="AVISO")
        else:
            inf.anotar(sec, "AVISO", "M36: fijar/excluir",
                       "sólo %d companero(s) distintos del renivelado: no hay bastantes para probar los dos" %
                       len(candidatos))

        inf.datos["guild_name"] = nombre_h
    finally:
        # Desconectado, SIN BORRAR: la reclamación por inactividad (fuera de
        # este caso: SQL sobre account.last_login + reload) sólo actúa sobre
        # un dueño offline. `fase=verificar` reconecta el mismo personaje y
        # se ocupa de borrarlo.
        if m.guid:
            m.salir()


def _fase_verificar(ctx, inf, sec):
    nombre, guid, raza = ctx.p["personaje"], ctx.p["guid"], ctx.p["raza"]
    if not nombre or not guid:
        raise Bloqueo("faltan -p personaje=<nombre> -p guid=<guid> (los que dejó `fase=crear` en sus `datos`)")
    m = ctx.nueva_sesion()
    m.personajes()   # CMSG_CHAR_ENUM: el servidor exige un char-select reciente antes de aceptar CMSG_PLAYER_LOGIN
    m.entrar(guid, raza, nombre=nombre)
    ctx.anotar_servidor(m)
    r = sin_colores(" ".join(m.comando("hermandad estado", 3)))
    inf.datos["estado_tras_reclamo"] = r[:250]
    reclamada = "no es una hermandad de casa" in r
    inf.comprobar(sec, reclamada,
                  "M36: tras el plazo de inactividad del dueño, la hermandad deja de ser una hermandad de casa",
                  r[:200], estado_si_no="AVISO")

    # La reclamación sólo deja de GESTIONAR la hermandad (mod_home_guild);
    # la guild de verdad sigue existiendo con el personaje como líder, y
    # CHAR_DELETE_FAILED_GUILD_LEADER rechaza borrar a un líder de gremio.
    if ctx.p["guild_name"]:
        rg = sin_colores(" ".join(m.comando('guild delete "%s"' % ctx.p["guild_name"], 4)))
        inf.datos["guild_delete"] = rg[:200]
        m.bombear(2)

    m.salir()   # CHAR_DELETE exige estar en la pantalla de selección, no dentro del mundo
    rr = m.borrar_personaje(guid)
    from ..mundo import CHAR_DELETE_SUCCESS
    ok = rr == CHAR_DELETE_SUCCESS
    if ok:
        registro.quitar(ctx.entorno.host, guid)
    inf.comprobar(sec, ok, "borrado del personaje temporal (fase=verificar)", "código 0x%02X" % rr,
                  estado_si_no="AVISO")


@caso(id="hermandad-avanzada", titulo="mod-home-guild: renivelado, fijar/excluir y reclamación por inactividad",
      descripcion=__doc__,
      etiquetas=("home-guild", "grupo", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "grupo"),
      parametros={"fase": Parametro(str, "crear", "qué hacer", opciones=("crear", "verificar")),
                  "nivel": Parametro(int, 20, "nivel del jugador (fase=crear)", minimo=10, maximo=80),
                  "espera": Parametro(int, 240, "segundos esperando adopción (fase=crear)", minimo=60, maximo=900),
                  "espera_relevel": Parametro(int, 150, "segundos esperando el renivelado automático "
                                              "(fase=crear)", minimo=30, maximo=600),
                  "personaje": Parametro(str, "", "nombre del personaje a reconectar (fase=verificar)"),
                  "guid": Parametro(int, 0, "GUID del personaje a reconectar (fase=verificar)", minimo=0),
                  "raza": Parametro(int, 1, "raza del personaje a reconectar (fase=verificar)", minimo=1),
                  "guild_name": Parametro(str, "", "nombre de la hermandad a disolver antes de borrar "
                                          "(fase=verificar; el `guild_name` que dejó `fase=crear`)")},
      duracion_max=900, protege=("mod-home-guild", "PLAN M36"), control="directo",
      observa=("`.hermandad estado` (nivel, «[fijado]», presencia/ausencia de un companero)",
               "`.hermandad estado` tras reconectar: mensaje de «no es una hermandad de casa»"),
      no_cubre=("el plazo real de 30 días (se fuerza con SQL sobre account.last_login, fuera del caso)",
                "el flanco de HomeGuild.Enable que dispara ReclaimInactiveGuilds (se hace por SSH + "
                "`.reload config`, fuera del caso, igual que M22 con IndividualProgression.Enable)"))
def ejecutar(ctx):
    inf, sec = ctx.inf, "home-guild-avanzado"
    if ctx.p["fase"] == "crear":
        _fase_crear(ctx, inf, sec)
    else:
        _fase_verificar(ctx, inf, sec)
