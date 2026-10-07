# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Escenario ARAC (PLAN AR01): combinaciones raza/clase de mod-arac contra un control nativo.

Por cada combinación: crear, entrar, hechizos y habilidades iniciales, pestaña
del libro de hechizos según los DBC del cliente, instructor de clase más
cercano (lista, requisitos que el cliente sabe nombrar), subir a nivel 10,
comprar un hechizo y comprobar que se aprende. El personaje se borra al final.
"""
from .. import actualizaciones as upd
from ..catalogo import Parametro, caso
from . import CLASES, RAZAS, PersonajeTemporal

# (raza, clase, es_arac). Los nativos son el control de la misma clase.
COMBINACIONES = [
    (4, 2, True),     # paladín elfo de la noche
    (1, 2, False),    # paladín humano (control)
    (1, 11, True),    # druida humano
    (4, 11, False),   # druida elfo de la noche (control)
]
FORMA_DE_OSO = 5487
NIVEL_PRUEBA = 10
RADIO_INSTRUCTOR = 250.0


def _mascara(valor, bit):
    return valor == 0 or bool(valor & (1 << (bit - 1)))


def _pestanas(m, dbc, raza, clase):
    """Reparte los hechizos conocidos entre pestaña de clase y General (modelo del cliente:
    fila de SkillLineAbility de una línea de clase, con máscaras que incluyan raza y clase,
    y habilidad presente en PLAYER_SKILL_INFO)."""
    lineas_clase = dbc.lineas_de_clase()
    sla = dbc.habilidades_por_hechizo()
    habilidades = m.habilidades()
    en_clase, en_general = [], []
    for sid in sorted(m.hechizos):
        filas = [f for f in sla.get(sid, []) if f[0] in lineas_clase]
        if not filas:
            continue                         # racial, idioma, profesión...: no es de pestaña de clase
        if any(f[0] in habilidades and _mascara(f[1], raza) and _mascara(f[2], clase) for f in filas):
            en_clase.append(sid)
        else:
            en_general.append(sid)
    return en_clase, en_general


def _buscar_instructor(m, inf, sec):
    candidatos = sorted((c for c in m.criaturas() if c["npcflags"] & upd.UNIT_NPC_FLAG_TRAINER_CLASS),
                        key=lambda c: m.distancia(c["pos"]))
    for c in candidatos:
        c["distancia"] = m.distancia(c["pos"])
        if c["distancia"] > RADIO_INSTRUCTOR:
            break
        m.ir_a(c["pos"])
        lista = m.lista_instructor(c["guid"])
        if lista is not None:
            return c, lista
    return None, None


def _comprobar_lista(lista, nombres, inf, sec):
    hechizos = lista["hechizos"]
    estados = {e: sum(1 for h in hechizos if h["estado"] == e) for e in (0, 1, 2)}
    inf.comprobar(sec, len(hechizos) > 0, "lista del instructor no vacía",
                  "%d hechizos (verdes %d, rojos %d, grises %d)" % (len(hechizos), estados[0], estados[1], estados[2]))
    sin_nombre = sorted({r for h in hechizos for r in h["requiere"] if r and not nombres.get(r)})
    inf.comprobar(sec, not sin_nombre, "todo requisito tiene nombre en el Spell.dbc del cliente",
                  "sin nombre: %s (Blizzard_TrainerUI vacía la lista)" % sin_nombre if sin_nombre else "")
    fuera = sorted(h["hechizo"] for h in hechizos if h["hechizo"] not in nombres)
    inf.comprobar(sec, not fuera, "todo hechizo de la lista existe en el cliente",
                  "no existen: %s" % fuera[:20] if fuera else "")


def verificar_combinacion(ctx, m, dbc, raza, clase, es_arac, inf):
    sec = "%s %s%s" % (CLASES[clase], RAZAS[raza], " (ARAC)" if es_arac else " (control)")
    datos = inf.datos.setdefault("arac", {}).setdefault(sec, {})
    with PersonajeTemporal(ctx, m, raza, clase, sec) as pj:
        ctx.anotar_servidor(m)
        inf.comprobar(sec, not m.errores_lectura, "paquetes del mundo leídos sin error",
                      "; ".join(m.errores_lectura[:3]), origen="cliente")
        inf.comprobar(sec, len(m.hechizos) > 0, "hechizos iniciales", "%d" % len(m.hechizos))
        habilidades = m.habilidades()
        inf.comprobar(sec, len(habilidades) > 0, "habilidades iniciales", "%d" % len(habilidades))
        datos.update(nombre=pj.nombre, mapa=m.mapa, hechizos_iniciales=sorted(m.hechizos),
                     habilidades_iniciales=habilidades)

        if dbc:
            inf.comprobar(sec, (raza, clase) in dbc.combinaciones_charbaseinfo(),
                          "combinación en CharBaseInfo.dbc del cliente",
                          dbc.tabla("CharBaseInfo.dbc").origen)
            en_clase, en_general = _pestanas(m, dbc, raza, clase)
            nombres = dbc.nombres_hechizos()
            inf.comprobar(sec, not en_general, "hechizos de clase en su pestaña (no en General)",
                          "%d en pestaña de clase%s" % (len(en_clase), "; en General: " + ", ".join(
                              "%d %s" % (s, nombres.get(s, "?")) for s in en_general) if en_general else ""))
            datos.update(pestana_clase=en_clase, pestana_general=en_general)
        else:
            nombres = {}

        instructor, lista = _buscar_instructor(m, inf, sec)
        if not inf.comprobar(sec, instructor is not None, "instructor de clase cercano que atiende",
                             "entrada %d a %.0f m del inicio" % (instructor["entrada"], instructor["distancia"]) if instructor else
                             "ninguno responde en %.0f m" % RADIO_INSTRUCTOR):
            return
        datos["instructor"] = instructor["entrada"]
        if dbc:
            _comprobar_lista(lista, nombres, inf, sec)

        m.comando("levelup %d" % (NIVEL_PRUEBA - 1))
        m.comando("modify money 1000000")
        inf.comprobar(sec, m.valor_propio(upd.PLAYER_FIELD_COINAGE) >= 1000000, "`.modify money` surte efecto",
                      "%d cobres" % m.valor_propio(upd.PLAYER_FIELD_COINAGE), origen="entorno")
        nivel = m.valor_propio(upd.UNIT_FIELD_LEVEL)
        inf.comprobar(sec, nivel == NIVEL_PRUEBA, "subida a nivel %d" % NIVEL_PRUEBA, "nivel %d" % nivel)
        armas = {s: v for s, v in m.habilidades().items() if v[1] == 5 * NIVEL_PRUEBA}
        inf.comprobar(sec, bool(armas), "habilidades de arma suben con el nivel (máx. %d)" % (5 * NIVEL_PRUEBA),
                      "%d con máximo %d" % (len(armas), 5 * NIVEL_PRUEBA))

        lista = m.lista_instructor(instructor["guid"])
        if not inf.comprobar(sec, lista is not None, "lista del instructor a nivel %d" % NIVEL_PRUEBA):
            return
        if dbc:
            _comprobar_lista(lista, nombres, inf, sec)
        por_id = {h["hechizo"]: h for h in lista["hechizos"]}
        verdes = sorted((h for h in lista["hechizos"] if h["estado"] == 0), key=lambda h: (h["nivel"], h["hechizo"]))
        objetivo = verdes[0]["hechizo"] if verdes else None
        if clase == 11 and FORMA_DE_OSO in por_id:
            # Catálogo completo de druida (TrainerId 33, parche arac/08): Forma de oso a nivel 10
            # y el requisito visible en las filas que dependen de ella (p. ej. 9634).
            oso = por_id[FORMA_DE_OSO]
            inf.comprobar(sec, oso["estado"] == 0, "Forma de oso en venta y disponible", "estado %d" % oso["estado"])
            dependientes = sorted(h["hechizo"] for h in lista["hechizos"] if FORMA_DE_OSO in h["requiere"])
            inf.comprobar(sec, bool(dependientes), "las filas que dependen de Forma de oso la muestran como requisito",
                          "%d filas (%s)" % (len(dependientes), ", ".join(map(str, dependientes[:6]))))
            objetivo = FORMA_DE_OSO
        elif clase == 11:
            # El instructor de la zona de inicio (TrainerId 34) sólo enseña los primeros hechizos:
            # para un ARAC, el genérico que le toca debe tener el catálogo completo.
            if es_arac:
                inf.anotar(sec, "FALLO", "instructor con el catálogo completo de druida (Forma de oso)",
                           "el instructor %d no la vende" % instructor["entrada"])
            else:
                inf.anotar(sec, "OK", "Forma de oso: no aplica (instructor %d de zona de inicio, catálogo corto)"
                           % instructor["entrada"])
        if not inf.comprobar(sec, objetivo is not None, "hay un hechizo que comprar"):
            return
        ok, motivo = m.comprar(instructor["guid"], objetivo)
        inf.comprobar(sec, ok, "compra de %d %s" % (objetivo, nombres.get(objetivo, "")),
                      "" if ok else "fallo, motivo %s" % motivo)
        if ok:
            inf.comprobar(sec, objetivo in m.hechizos or any(objetivo == a for a in m.aprendidos),
                          "el hechizo comprado queda aprendido")
        inf.comprobar(sec, not m.errores_lectura, "sin errores de lectura al terminar",
                      "; ".join(m.errores_lectura[:3]), origen="cliente")


@caso(id="arac", titulo="Combinaciones ARAC frente a su control nativo (AR01)",
      descripcion="""
Paladín elfo de la noche y druida humano (mod-arac) frente a paladín humano y
druida elfo de la noche. Crea, entra, lee hechizos y habilidades iniciales,
comprueba la combinación en CharBaseInfo.dbc del cliente real y la pestaña del
libro (modelada con SkillLineAbility), abre el instructor de clase como
Wow.exe, comprueba que todo requisito tiene nombre en el Spell.dbc del cliente,
sube a nivel 10 y compra un hechizo (Forma de oso en el druida ARAC).
""",
      etiquetas=("arac", "instructores", "dbc", "personaje"),
      acciones=("conectar", "leer", "personaje", "gm", "comprar"), requiere=("dbc",),
      parametros={"combinaciones": Parametro(str, "todas", "todas | arac (sólo las ARAC) | control")},
      duracion_max=600, protege=("AR01", "mod-arac", "patches/arac", "mod-arac-trainer-audit"),
      control="directo",
      observa=("SMSG_INITIAL_SPELLS", "PLAYER_SKILL_INFO", "SMSG_TRAINER_LIST", "SMSG_TRAINER_BUY_*",
               "DBC del cliente (CharBaseInfo, Spell, SkillLine, SkillLineAbility)"),
      no_cubre=("pestañas reales del libro de hechizos y textos del instructor en Wow.exe (AR01 manual)",
                "errores Lua de Blizzard_TrainerUI"))
def ejecutar(ctx):
    """Una sesión por combinación, para que un fallo a medias no arrastre a las siguientes."""
    filtro = ctx.p["combinaciones"]
    lista = [c for c in COMBINACIONES if filtro == "todas" or (filtro == "arac") == c[2]]
    for raza, clase, es_arac in lista:
        m = None
        try:
            m = ctx.nueva_sesion()
            verificar_combinacion(ctx, m, ctx.dbc, raza, clase, es_arac, ctx.inf)
        except Exception as e:                         # noqa: BLE001 — una combinación no para las demás
            ctx.inf.anotar("%s %s" % (CLASES[clase], RAZAS[raza]), "FALLO", "excepción", repr(e),
                           origen="entorno" if isinstance(e, OSError) else "cliente")
        finally:
            if m:
                m.cerrar()


# Una raza no nativa para cada clase que ARAC amplía. El caballero de la Muerte
# ya admite todas las razas en WotLK y necesita un personaje de nivel 55 en la cuenta.
CLASES_ARAC = (
    (10, 1),   # guerrero elfo de sangre
    (4, 2),    # paladín elfo de la noche
    (1, 3),    # cazador humano
    (6, 4),    # pícaro tauren
    (2, 5),    # sacerdote orco
    (1, 7),    # chamán humano
    (6, 8),    # mago tauren
    (6, 9),    # brujo tauren
    (1, 11),   # druida humano
)


@caso(id="arac-clases", titulo="Una combinación ARAC por clase",
      descripcion="""Crea temporalmente una combinación no nativa de cada una de las
nueve clases ampliadas por ARAC. Contrasta creación, entrada, hechizos,
habilidades, pestañas modeladas con los DBC reales e instructor de clase.""",
      etiquetas=("arac", "clases", "dbc", "personaje"),
      acciones=("conectar", "leer", "personaje"), requiere=("dbc",),
      duracion_max=900, protege=("AR01", "mod-arac", "patches/arac"),
      control="directo", en_todo=False,
      observa=("SMSG_INITIAL_SPELLS", "PLAYER_SKILL_INFO", "SMSG_TRAINER_LIST",
               "DBC del cliente (CharBaseInfo, Spell, SkillLine, SkillLineAbility)"),
      no_cubre=("interfaz Lua y presentación visual del libro y del instructor",
                "compra y uso de hechizos por cada clase"))
def ejecutar_clases(ctx):
    for raza, clase in CLASES_ARAC:
        sec = "%s %s%s" % (CLASES[clase], RAZAS[raza], " (control)" if clase == 6 else " (ARAC)")
        m = None
        try:
            m = ctx.nueva_sesion()
            with PersonajeTemporal(ctx, m, raza, clase, sec):
                ctx.anotar_servidor(m)
                ctx.inf.comprobar(sec, not m.errores_lectura, "paquetes del mundo leídos sin error",
                                 "; ".join(m.errores_lectura[:3]), origen="cliente")
                ctx.inf.comprobar(sec, bool(m.hechizos), "hechizos iniciales",
                                 "%d" % len(m.hechizos))
                habilidades = m.habilidades()
                ctx.inf.comprobar(sec, bool(habilidades), "habilidades iniciales",
                                 "%d" % len(habilidades))
                ctx.inf.comprobar(sec, (raza, clase) in ctx.dbc.combinaciones_charbaseinfo(),
                                 "combinación en CharBaseInfo.dbc del cliente",
                                 ctx.dbc.tabla("CharBaseInfo.dbc").origen)
                en_clase, en_general = _pestanas(m, ctx.dbc, raza, clase)
                ctx.inf.comprobar(sec, bool(en_clase), "hechizos en pestaña de clase (modelo DBC)",
                                 "%d" % len(en_clase))
                ctx.inf.comprobar(sec, not en_general, "ningún hechizo de clase en General (modelo DBC)",
                                 ", ".join(map(str, en_general)))
                datos = ctx.inf.datos.setdefault("arac_clases", {})
                datos[sec] = {"hechizos_iniciales": len(m.hechizos),
                              "habilidades_iniciales": len(habilidades),
                              "pestana_clase": en_clase, "pestana_general": en_general}
                instructor, lista = _buscar_instructor(m, ctx.inf, sec)
                if ctx.inf.comprobar(sec, instructor is not None,
                                     "instructor de clase cercano que atiende",
                                     "entrada %d" % instructor["entrada"] if instructor else "ninguno"):
                    datos[sec]["instructor"] = instructor["entrada"]
                    _comprobar_lista(lista, ctx.dbc.nombres_hechizos(), ctx.inf, sec)
                ctx.inf.comprobar(sec, not m.errores_lectura, "sin errores de lectura al terminar",
                                 "; ".join(m.errores_lectura[:3]), origen="cliente")
        except Exception as e:  # noqa: BLE001 — una clase no impide revisar las restantes
            ctx.inf.anotar(sec, "FALLO", "excepción", repr(e),
                           origen="entorno" if isinstance(e, OSError) else "cliente")
        finally:
            if m:
                m.cerrar()
