#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Cliente sintético de WoW 3.3.5a: verifica el servidor sin jugar. Ver README.md y GUIA-IA.md.
    python tools/cliente-sintetico/verificar.py listar [--json]
    python tools/cliente-sintetico/verificar.py describir mazmorra [--json]
    python tools/cliente-sintetico/verificar.py ejecutar diagnostico
    python tools/cliente-sintetico/verificar.py ejecutar @rapido --json informe.json
    python tools/cliente-sintetico/verificar.py ejecutar mazmorra -p mazmorra=bfd -p duracion=5400
    python tools/cliente-sintetico/verificar.py ejecutar todo --dry-run
    python tools/cliente-sintetico/verificar.py limpiar
Atajo compatible: `verificar.py arac|progresion|bots|todo` = `ejecutar ...`.
Códigos de salida: 0 todo OK/AVISO · 1 algún FALLO · 2 BLOQUEADO u OMITIDO ·
3 ERROR de infraestructura · 4 uso incorrecto.
"""
import argparse
import datetime
import json
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wowsintetico import catalogo, entorno as ent             # noqa: E402
from wowsintetico.ejecutor import ejecutar_caso, limpiar_huerfanos  # noqa: E402
from wowsintetico.informe import USO_INCORRECTO, Informe      # noqa: E402
VERSION = "2.0"
CARPETA = os.path.dirname(os.path.abspath(__file__))
def _versiones_fijadas() -> dict:
    """Commits fijados de mirrors/MANIFEST.tsv (core y terceros)."""
    ruta = os.path.join(CARPETA, "..", "..", "mirrors", "MANIFEST.tsv")
    res = {}
    try:
        with open(ruta, encoding="utf-8") as f:
            for linea in f:
                if linea.startswith("#") or not linea.strip():
                    continue
                campos = [c.strip() for c in linea.split("\t")]
                if len(campos) >= 2:
                    res[campos[0]] = campos[1][:12]
    except OSError:
        pass
    return res


def _parametros(lista) -> dict:
    res = {}
    for p in lista or []:
        if "=" not in p:
            raise ValueError("parámetro sin '=': %s" % p)
        k, v = p.split("=", 1)
        res[k.strip()] = v.strip()
    return res


def cmd_listar(casos, args):
    if args.json:
        print(json.dumps([c.como_dict() for c in casos.values()], ensure_ascii=False, indent=1))
        return 0
    for c in casos.values():
        print("%-12s %-9s %-8s %5ds  %s  [%s]" % (c.id, c.control, "escribe" if c.escribe else "lectura",
                                                  c.duracion_max, c.titulo, ", ".join(sorted(c.etiquetas))))
    return 0


def cmd_describir(casos, args):
    if args.caso not in casos:
        print("no existe el caso %s" % args.caso, file=sys.stderr)
        return USO_INCORRECTO
    d = casos[args.caso].como_dict(completo=True)
    if args.json:
        print(json.dumps(d, ensure_ascii=False, indent=1))
        return 0
    print("%s — %s\n\n%s\n" % (d["id"], d["titulo"], d["descripcion"]))
    for k in ("etiquetas", "acciones", "requiere", "control", "duracion_max_s", "protege", "observa", "no_cubre",
              "en_todo"):
        print("%-15s %s" % (k, d[k]))
    print("parámetros:")
    for k, p in d["parametros"].items():
        print("  %-14s %-5s defecto=%r  %s%s" % (k, p["tipo"], p["defecto"], p["ayuda"],
                                                 (" opciones=%s" % (p["opciones"],)) if "opciones" in p else ""))
    return 0


def cmd_ejecutar(casos, args):
    try:
        seleccion = catalogo.seleccionar(casos, args.casos)
        dados = _parametros(args.param)
    except (KeyError, ValueError) as e:
        print(str(e), file=sys.stderr)
        return USO_INCORRECTO
    try:
        perfil = ent.cargar(args.entorno)
    except ent.ErrorEntorno as e:
        print("perfil: %s" % e, file=sys.stderr)
        return USO_INCORRECTO
    informe = Informe({"nombre": "cliente-sintetico", "version": VERSION, "fijado": _versiones_fijadas()},
                      perfil.publico())
    informe.meta["argumentos"] = {"casos": args.casos, "parametros": dados, "dry_run": args.dry_run}
    dbc = None
    cliente = args.cliente if args.cliente is not None else perfil.cliente_wow
    if cliente and any("dbc" in c.requiere for c in seleccion):
        try:
            from wowsintetico.dbc import ClienteDBC
            dbc = ClienteDBC(cliente, perfil.idioma)
            dbc.tabla("Spell.dbc")
        except Exception as e:                              # noqa: BLE001 — el caso queda BLOQUEADO
            print("DBC del cliente no disponibles: %s" % e, file=sys.stderr)
            dbc = None
    marca = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    artefactos = args.artefactos or os.path.join(CARPETA, ".estado", "ejecuciones", marca)
    for c in seleccion:
        try:
            parametros = c.parametros_efectivos({k: v for k, v in dados.items() if k in c.parametros}
                                                if len(seleccion) > 1 else dados)
        except ValueError as e:
            print(str(e), file=sys.stderr)
            return USO_INCORRECTO
        ejecutar_caso(c, perfil, parametros, dbc, artefactos, informe, dry_run=args.dry_run)
    if dbc:
        informe.meta["cliente"] = {"carpeta": cliente, "idioma": perfil.idioma, "dbc_origen": dbc.origenes()}
    if args.json:
        informe.guardar_json(args.json)
    elif not args.dry_run:
        os.makedirs(artefactos, exist_ok=True)
        informe.guardar_json(os.path.join(artefactos, "informe.json"))
    print("\n" + informe.resumen())
    return informe.codigo_salida(args.dry_run)


def cmd_limpiar(args):
    try:
        perfil = ent.cargar(args.entorno)
    except ent.ErrorEntorno as e:
        print("perfil: %s" % e, file=sys.stderr)
        return USO_INCORRECTO
    if "personaje" in perfil.admite({"personaje"}):
        print("el perfil %s no permite borrar personajes" % perfil.nombre, file=sys.stderr)
        return 2
    res = limpiar_huerfanos(perfil, forzar_nombres=tuple(args.forzar or ()), cuenta=args.cuenta)
    print(json.dumps(res, ensure_ascii=False, indent=1))
    return 0


def main(argv=None):
    for flujo in (sys.stdout, sys.stderr):
        try:
            flujo.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in ("arac", "progresion", "bots", "todo"):
        argv = ["ejecutar"] + argv
        if "--espera" in argv:                               # compatibilidad con la versión 1
            i = argv.index("--espera")
            argv[i:i + 2] = ["-p", "espera=" + argv[i + 1]]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--entorno", help="perfil JSON (por defecto entorno.local.json o VERIFICADOR_ENTORNO)")
    sub = ap.add_subparsers(dest="orden", required=True)
    s = sub.add_parser("listar", help="catálogo de casos")
    s.add_argument("--json", action="store_true")
    s = sub.add_parser("describir", help="un caso con sus parámetros")
    s.add_argument("caso")
    s.add_argument("--json", action="store_true")
    s = sub.add_parser("ejecutar", help="ejecuta casos: ids, @etiqueta o todo")
    s.add_argument("casos", nargs="+")
    s.add_argument("-p", "--param", action="append", help="parámetro nombre=valor (repetible)")
    s.add_argument("--dry-run", action="store_true", help="comprueba precondiciones y parámetros sin conectar")
    s.add_argument("--json", help="guardar el informe aquí (si no, en .estado/ejecuciones/<fecha>/)")
    s.add_argument("--artefactos", help="carpeta de artefactos (líneas de tiempo)")
    s.add_argument("--cliente", help="carpeta de WoW cuyos DBC se leen ('' para ninguno)")
    s = sub.add_parser("limpiar", help="borra personajes temporales huérfanos propios")
    s.add_argument("--forzar", action="append", help="nombre con el prefijo pero sin registro que también se borra")
    s.add_argument("--cuenta", default="principal", help="alias de la cuenta (principal por defecto)")
    args = ap.parse_args(argv)
    if args.orden == "limpiar":
        return cmd_limpiar(args)
    casos = catalogo.cargar()
    if args.orden == "listar":
        return cmd_listar(casos, args)
    if args.orden == "describir":
        return cmd_describir(casos, args)
    return cmd_ejecutar(casos, args)


if __name__ == "__main__":
    sys.exit(main())
# linea nueva
