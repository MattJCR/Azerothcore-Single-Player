# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Pruebas sin conexión del cliente sintético (tools/cliente-sintetico).

Lo que se prueba contra el servidor real está en verificar.py; aquí van las
piezas que pueden fallar en silencio: la criptografía (contra una réplica del
lado servidor de SRP6.cpp), el empaquetado de GUID y el lector de
SMSG_UPDATE_OBJECT con un paquete construido a mano.
"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "cliente-sintetico"))

from wowsintetico import actualizaciones as upd                       # noqa: E402
from wowsintetico.binario import Escritor, Lector                    # noqa: E402
from wowsintetico.cripto import ARC4, ClienteSRP6, entrelazar_sha1, sha1  # noqa: E402
from wowsintetico.escenarios import sin_colores                       # noqa: E402

N_HEX = "894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7"


def servidor_srp(usuario, clave, s, b, A_le):
    """Réplica de SRP6::CalculateVerifier / _B / VerifyChallengeResponse del core."""
    N = int(N_HEX, 16)
    N_le = N.to_bytes(32, "little")
    x = int.from_bytes(sha1(s, sha1((usuario.upper() + ":" + clave.upper()).encode())), "little")
    v = pow(7, x, N)
    B = (pow(7, b, N) + 3 * v) % N
    B_le = B.to_bytes(32, "little")
    A = int.from_bytes(A_le, "little")
    u = int.from_bytes(sha1(A_le, B_le), "little")
    S = pow(A * pow(v, u, N), b, N).to_bytes(32, "little")
    K = entrelazar_sha1(S)
    ng = bytes(a ^ c for a, c in zip(sha1(N_le), sha1(bytes([7]))))
    return N_le, B_le, K, ng


class PruebasCripto(unittest.TestCase):
    def test_srp6_cliente_y_servidor_coinciden(self):
        s = bytes(range(32))
        cliente = ClienteSRP6("Verificador", "verificador", a=0x1234567890ABCDEF)
        # A depende de a; el servidor necesita B antes: se calcula en dos pasos.
        N_le = int(N_HEX, 16).to_bytes(32, "little")
        b = 0xCAFEBABE12345
        N = int(N_HEX, 16)
        x = int.from_bytes(sha1(s, sha1(b"VERIFICADOR:VERIFICADOR")), "little")
        B_le = ((pow(7, b, N) + 3 * pow(7, x, N)) % N).to_bytes(32, "little")
        A_le, M1, K = cliente.responder(B_le, N_le, s)
        _, B2, K_srv, ng = servidor_srp("Verificador", "verificador", s, b, A_le)
        self.assertEqual(B2, B_le)
        self.assertEqual(K, K_srv)
        self.assertEqual(M1, sha1(ng, sha1(b"VERIFICADOR"), s, A_le, B_le, K_srv))
        self.assertEqual(cliente.M2_esperado, sha1(A_le, M1, K_srv))

    def test_entrelazado_salta_ceros_iniciales_en_pares(self):
        S = bytes([0, 0, 0]) + bytes(range(1, 30))
        # p = 3 ceros -> 4 -> offset 2 en cada mitad
        pares = bytes(S[2 * i] for i in range(16))[2:]
        impares = bytes(S[2 * i + 1] for i in range(16))[2:]
        esperado = bytes(b for par in zip(sha1(pares), sha1(impares)) for b in par)
        self.assertEqual(entrelazar_sha1(S), esperado)

    def test_arc4_vector_conocido(self):
        self.assertEqual(ARC4(b"Key").aplicar(b"Plaintext").hex(), "bbf316e8d940af0ad3")


class PruebasBinario(unittest.TestCase):
    def test_guid_empaquetado_ida_y_vuelta(self):
        for guid in (0, 1, 0xF130000000001234, 0x0000000000895AE7, 0xFFFFFFFFFFFFFFFF):
            datos = Escritor().guid_empaquetado(guid).valor()
            self.assertEqual(Lector(datos).guid_empaquetado(), guid)
        self.assertEqual(Escritor().guid_empaquetado(0x0102).valor(), bytes([0x03, 0x02, 0x01]))


def _bloque_criatura(guid, entrada, npcflags):
    """CREATE_OBJECT de una criatura con LIVING + trayectoria activa (Final_Target)
    y HAS_TARGET, más un VALUES posterior: ejercita los saltos del lector."""
    e = Escritor().u8(upd.CREATE).guid_empaquetado(guid).u8(upd.TYPEID_UNIT)
    e.u16(0x20 | 0x04)                                            # LIVING | HAS_TARGET
    e.u32(0x08000000 | 0x00001000).u16(0).u32(123)               # SPLINE_ENABLED | FALLING
    e.f32(1.0).f32(2.0).f32(3.0).f32(0.5)
    e.u32(0)                                                     # fallTime
    e.bytes(bytes(16))                                           # salto
    e.bytes(bytes(9 * 4))                                        # velocidades
    e.u32(0x10000).u64(0xABCDEF)                                  # Final_Target
    e.u32(1).u32(2).u32(3).f32(1).f32(1).f32(0).u32(0)
    e.u32(2).bytes(bytes(24)).u8(0).bytes(bytes(12))              # 2 nodos, modo, destino
    e.guid_empaquetado(0x55)                                      # objetivo
    campos = {upd.OBJECT_FIELD_ENTRY: entrada, upd.UNIT_NPC_FLAGS: npcflags}
    bloques = max(campos) // 32 + 1
    mascaras = [0] * bloques
    for i in campos:
        mascaras[i // 32] |= 1 << (i % 32)
    e.u8(bloques)
    for m in mascaras:
        e.u32(m)
    for i in sorted(campos):
        e.u32(campos[i])
    return e.valor()


class PruebasActualizaciones(unittest.TestCase):
    def test_criatura_con_trayectoria_y_valores(self):
        datos = struct.pack("<I", 2) + _bloque_criatura(0xF1300065BC000042, 26324, 0x20)
        datos += Escritor().u8(upd.VALUES).guid_empaquetado(0xF1300065BC000042).u8(1).u32(1 << 3).u32(99).valor()
        bloques = upd.leer_paquete(datos)
        self.assertEqual(len(bloques), 2)
        self.assertEqual(bloques[0]["typeid"], upd.TYPEID_UNIT)
        self.assertEqual(bloques[0]["pos"], {"x": 1.0, "y": 2.0, "z": 3.0, "o": 0.5})
        self.assertEqual(bloques[0]["valores"][upd.OBJECT_FIELD_ENTRY], 26324)
        self.assertEqual(bloques[0]["valores"][upd.UNIT_NPC_FLAGS], 0x20)
        self.assertEqual(bloques[1]["valores"], {3: 99})

    def test_fuera_de_rango(self):
        e = Escritor().u32(1).u8(upd.OUT_OF_RANGE).u32(2).guid_empaquetado(7).guid_empaquetado(9)
        self.assertEqual(upd.leer_paquete(e.valor()), [{"tipo": upd.OUT_OF_RANGE, "guids": [7, 9]}])


def mundo_sin_red():
    """Un Mundo con el estado inicial pero sin socket ni autenticación."""
    from wowsintetico.mundo import Mundo
    m = Mundo.__new__(Mundo)
    m.__dict__.update(mensajes=[], chat=[], addon=[], oyentes=[], errores_lectura=[], objetos={},
                      trayectorias={}, miembros={}, grupo=None, tiradas={}, temporizadores={}, forzados_sin_ack={},
                      guid=None, pos=None, mapa=None, vigilante=None, teletransportes=0, hechizos=set(),
                      aprendidos=[], lfg={"auto_aceptar": False, "roles": 0, "propuestas": [], "estado": None,
                                          "union": None}, bg={"auto_puerto": False, "status": None},
                      _cabecera=None, buffer=b"", cifrado=None,
                      ultimo_hechizo=(0.0, 0), _tp_provocado_hasta=0.0)
    m.enviados = []
    m.enviar = lambda op, cuerpo=b"": m.enviados.append((op, cuerpo))
    return m


class PruebasChat(unittest.TestCase):
    def _mundo_sin_red(self):
        return mundo_sin_red()

    def test_mensaje_de_canal_lleva_el_nombre_del_canal(self):
        # CHAT_MSG_CHANNEL: los bots hablan por General/BuscarGrupo; sin leer el canal
        # el tamaño del texto salía de bytes equivocados (fallo visto el 24/09/2026).
        datos = (Escritor().u8(0x11).i32(0).u64(0x42).u32(0).cadena("General - Villanorte")
                 .u64(0).u32(5).bytes(b"hola\x00").u8(0).valor())
        m = self._mundo_sin_red()
        m._leer_chat(datos)
        self.assertEqual(m.mensajes, [("chat", "hola")])

    def test_mensaje_de_sistema(self):
        datos = Escritor().u8(0x00).i32(0).u64(0).u32(0).u64(0).u32(3).bytes(b"ok\x00").u8(0).valor()
        m = self._mundo_sin_red()
        m._leer_chat(datos)
        self.assertEqual(m.mensajes, [("sistema", "ok")])


class PruebasEscenarios(unittest.TestCase):
    def test_sin_colores_no_deja_cifras_hexadecimales(self):
        # El caso que engañó al primer regex: el 00 de cff00ffff se leía como la etapa.
        self.assertEqual(sin_colores("Progression Level for |cff00ffffVsx|r = |cff00ffff8|r"),
                         "Progression Level for Vsx = 8")


class PruebasMovimiento(unittest.TestCase):
    def _monster_move(self, guid, inicio, puntos, duracion, flags=0, tipo=0):
        from wowsintetico.movimiento import empaquetar_xyz
        e = Escritor().guid_empaquetado(guid).u8(0).f32(inicio[0]).f32(inicio[1]).f32(inicio[2]).u32(7).u8(tipo)
        if tipo == 1:
            return e.valor()
        e.u32(flags).u32(duracion)
        if flags & 0x40000:
            e.u32(len(puntos))
            for p in puntos:
                e.f32(p[0]).f32(p[1]).f32(p[2])
        else:
            destino = puntos[-1]
            e.u32(len(puntos)).f32(destino[0]).f32(destino[1]).f32(destino[2])
            medio = [(a + b) / 2 for a, b in zip(inicio, destino)]
            for p in puntos[:-1]:
                e.u32(empaquetar_xyz(*(m - q for m, q in zip(medio, p))))
        return e.valor()

    def test_trayectoria_lineal_con_intermedios_empaquetados(self):
        from wowsintetico.movimiento import Trayectoria, leer_monster_move
        datos = self._monster_move(0x55, (0, 0, 0), [(10, 0, 0), (10, 10, 0)], 2000)
        m = leer_monster_move(datos)
        self.assertEqual(m["guid"], 0x55)
        self.assertEqual(len(m["puntos"]), 3)
        self.assertAlmostEqual(m["puntos"][1][0], 10.0, places=1)
        self.assertAlmostEqual(m["puntos"][2][1], 10.0, places=3)
        t = Trayectoria(m, t0=100.0)
        self.assertEqual(t.posicion(100.0), (0.0, 0.0, 0.0))
        x, y, _ = t.posicion(101.0)                 # mitad del camino: esquina (10, 0)
        self.assertAlmostEqual(x, 10.0, places=1)
        self.assertAlmostEqual(y, 0.0, places=1)
        self.assertTrue(t.terminada(102.5))
        self.assertAlmostEqual(t.posicion(105.0)[1], 10.0, places=3)

    def test_parada_y_catmullrom(self):
        from wowsintetico.movimiento import MOVE_STOP, leer_monster_move
        parada = leer_monster_move(self._monster_move(0x55, (1, 2, 3), [], 0, tipo=1))
        self.assertEqual(parada["tipo"], MOVE_STOP)
        self.assertEqual(parada["puntos"], [(1.0, 2.0, 3.0)])
        cr = leer_monster_move(self._monster_move(0x55, (0, 0, 0), [(1, 1, 1), (2, 2, 2)], 500, flags=0x40000))
        self.assertEqual(len(cr["puntos"]), 3)

    def test_monster_move_truncado_no_rompe_la_sesion(self):
        m = mundo_sin_red()
        datos = self._monster_move(0x55, (0, 0, 0), [(10, 0, 0)], 1000)[:-6]
        m._procesar(0x0DD, datos)
        self.assertEqual(len(m.errores_lectura), 1)
        self.assertIn("0x0DD", m.errores_lectura[0])

    def test_propio_movido_por_el_servidor(self):
        m = mundo_sin_red()
        m.guid = 0x55
        eventos = []
        m.oyentes.append(lambda t, e, d: eventos.append(e))
        m._procesar(0x0DD, self._monster_move(0x55, (0, 0, 0), [(30, 0, 0)], 60000))
        self.assertIn("movimiento_propio", eventos)
        self.assertLess(m.posicion_actual()["x"], 30)


class PruebasTeletransporte(unittest.TestCase):
    def _ack(self, guid, x):
        return (Escritor().guid_empaquetado(guid).u32(1).u32(0).u16(0).u32(0)
                .f32(x).f32(0).f32(0).f32(0).u32(0).valor())

    def test_traslacion_no_es_un_rescate(self):
        from wowsintetico.telemetria import Telemetria
        m = mundo_sin_red()
        m.guid, m.pos = 0x55, {"x": 0, "y": 0, "z": 0, "o": 0}
        t = Telemetria(m, None, eco=lambda *_: None)
        go = Escritor().guid_empaquetado(0x55).guid_empaquetado(0x55).u8(0).u32(1953).u32(0).u32(0).valor()
        m._procesar(0x132, go)
        m._procesar(0x0C7, self._ack(0x55, 20))
        self.assertEqual(t.anomalias, [])
        self.assertEqual(t.teletransportes[-1]["hechizo"], 1953)
        m.ultimo_hechizo = (0.0, 0)
        m._procesar(0x0C7, self._ack(0x55, 60))
        self.assertEqual([a["tipo"] for a in t.anomalias], ["teletransporte"])
        self.assertEqual(m.enviados[-1][0], 0x0C7)            # el cliente confirma cada uno


class PruebasGrupo(unittest.TestCase):
    def test_lista_de_grupo(self):
        from wowsintetico.grupo import leer_lista_grupo
        e = Escritor().u8(0).u8(0).u8(0).u8(8).u64(0x1F4000000000001).u32(3).u32(2)
        e.cadena("Tanque").u64(0x10).u8(1).u8(0).u8(0).u8(2)
        e.cadena("Sanador").u64(0x11).u8(1).u8(0).u8(0).u8(4)
        e.u64(0x99).u8(3).u64(0).u8(2).u8(0).u8(0).u8(0)
        g = leer_lista_grupo(e.valor())
        self.assertEqual([x["nombre"] for x in g["miembros"]], ["Tanque", "Sanador"])
        self.assertEqual(g["lider"], 0x99)

    def test_lista_de_grupo_del_buscador_lleva_la_mazmorra(self):
        from wowsintetico.grupo import GROUP_TYPE_LFG, leer_lista_grupo
        e = Escritor().u8(GROUP_TYPE_LFG).u8(0).u8(0).u8(8).u8(0).u32(10).u64(1).u32(0).u32(0).u64(0x99)
        self.assertEqual(leer_lista_grupo(e.valor())["tipo"], GROUP_TYPE_LFG)

    def test_estadisticas_con_posicion_negativa_y_auras(self):
        from wowsintetico.grupo import F_AURAS, F_CUR_HP, F_POSITION, F_STATUS, F_ZONE, leer_estadisticas
        mascara = F_STATUS | F_CUR_HP | F_ZONE | F_POSITION | F_AURAS
        e = Escritor().guid_empaquetado(0x10).u32(mascara).u16(0x05).u32(1234).u16(719)
        e.u16((-151) & 0xFFFF).u16(106).u64(0b101).u32(111).u8(1).u32(333).u8(1)
        s = leer_estadisticas(e.valor())
        self.assertEqual((s["x"], s["y"], s["zona"], s["vida"]), (-151, 106, 719, 1234))
        self.assertEqual(s["auras"], [111, 333])
        from wowsintetico.grupo import estado_texto
        self.assertEqual(estado_texto(s["estado"]), "muerto")

    def test_lfg_unirse_tiene_el_formato_de_lfgjoin_read(self):
        from wowsintetico.grupo import lfg_unirse
        L = Lector(lfg_unirse(8, [10], "x"))
        self.assertEqual((L.u32(), L.u8(), L.u8(), L.u8(), L.u32(), L.u8()), (8, 0, 0, 1, 10, 3))
        L.bytes(3)
        self.assertEqual(L.cadena(), "x")


class PruebasBuscador(unittest.TestCase):
    def test_acepta_cada_propuesta_una_sola_vez(self):
        m = mundo_sin_red()
        m.lfg.update(auto_aceptar=True, roles=8, aceptadas=set())
        propuesta = Escritor().u32(4).u8(0).u32(77).u32(0).u8(0).u8(0).valor()
        for _ in range(5):
            m._procesar(0x361, propuesta)
        self.assertEqual([op for op, _ in m.enviados], [0x362])
        self.assertEqual(len(m.lfg["propuestas"]), 5)


class PruebasCombateReal(unittest.TestCase):
    """M29/M31/M32: ataque, liberar espíritu y campo de batalla/arena (PLAN M33)."""

    def test_atacar_selecciona_y_manda_attackswing(self):
        m = mundo_sin_red()
        m.atacar(0x77)
        self.assertEqual(m.enviados, [(0x13D, struct.pack("<Q", 0x77)), (0x141, struct.pack("<Q", 0x77))])

    def test_dejar_de_atacar_sin_cuerpo(self):
        m = mundo_sin_red()
        m.dejar_de_atacar()
        self.assertEqual(m.enviados, [(0x142, b"")])

    def test_liberar_espiritu_manda_repop_request_con_el_byte_que_lee_read_skip(self):
        m = mundo_sin_red()
        m.liberar_espiritu()
        self.assertEqual(m.enviados, [(0x15A, b"\x00")])

    def test_arena_unirse_tiene_el_formato_de_handlebattlemasterjoinarena(self):
        m = mundo_sin_red()
        m.arena_unirse(0xF130001234560001, 0, en_grupo=False, clasificada=False)
        self.assertEqual(len(m.enviados), 1)
        op, cuerpo = m.enviados[0]
        self.assertEqual(op, 0x358)
        L = Lector(cuerpo)
        self.assertEqual((L.u64(), L.u8(), L.u8(), L.u8()), (0xF130001234560001, 0, 0, 0))
        self.assertTrue(m.bg["auto_puerto"])

    def test_leer_estado_batalla_sin_status_solo_trae_la_ranura(self):
        from wowsintetico.grupo import BG_STATUS_NONE, leer_estado_batalla
        # BuildBattlegroundStatusPacket manda u32(slot) + u64(0) legado cuando
        # StatusID == STATUS_NONE: los 8 bytes de más no deben confundirse con
        # el principio de un paquete real (bug visto en vivo 25/09/2026: leía
        # esos ceros como arenatype/es_arena/... y reventaba sin datos).
        for cuerpo in (Escritor().u32(2).valor(), Escritor().u32(2).u64(0).valor()):
            estado = leer_estado_batalla(cuerpo)
            self.assertEqual(estado, {"ranura": 2, "status": BG_STATUS_NONE})

    def test_leer_estado_batalla_wait_join_arena(self):
        from wowsintetico.grupo import BG_STATUS_WAIT_JOIN, leer_estado_batalla
        e = Escritor().u32(1).u8(1).u8(0xE).u32(559).u16(0x1F90).u8(0).u8(80).u32(42)
        e.u8(0).u32(BG_STATUS_WAIT_JOIN).u32(4406).u64(0).u32(80000)
        estado = leer_estado_batalla(e.valor())
        self.assertEqual(estado["status"], BG_STATUS_WAIT_JOIN)
        self.assertEqual((estado["arenatype"], estado["bg_tipo"], estado["mapa"], estado["quitar_cola_ms"]),
                          (1, 559, 4406, 80000))
        self.assertFalse(estado["clasificada"])

    def test_puerto_campo_tiene_el_formato_de_handlebattlefieldportopcode(self):
        from wowsintetico.grupo import puerto_campo
        L = Lector(puerto_campo(1, 559))
        self.assertEqual((L.u8(), L.u8(), L.u32(), L.u16(), L.u8()), (1, 0, 559, 0x1F90, 1))

    def test_auto_puerto_acepta_al_llegar_wait_join(self):
        from wowsintetico.grupo import BG_STATUS_WAIT_JOIN
        m = mundo_sin_red()
        m.bg["auto_puerto"] = True
        e = Escritor().u32(0).u8(1).u8(0xE).u32(559).u16(0x1F90).u8(0).u8(80).u32(42)
        e.u8(0).u32(BG_STATUS_WAIT_JOIN).u32(4406).u64(0).u32(80000)
        m._procesar(0x2D4, e.valor())
        self.assertEqual(m.enviados[-1][0], 0x2D5)


class PruebasObjetos(unittest.TestCase):
    """M39: ganzúa de recompensa (600000) — construir CMSG_USE_ITEM y leer SMSG_LOOT_RESPONSE."""

    def test_paquete_usar_objeto_sin_objetivo(self):
        from wowsintetico.mundo import _paquete_usar_objeto
        L = Lector(_paquete_usar_objeto(23, 59403, 0xF140000000112233))
        self.assertEqual((L.u8(), L.u8(), L.u8(), L.u32(), L.u64(), L.u32(), L.u8(), L.u32()),
                          (255, 23, 1, 59403, 0xF140000000112233, 0, 0, 0))
        self.assertEqual(L.resto(), 0)

    def test_paquete_usar_objeto_con_objetivo_lleva_target_flag_item_empaquetado(self):
        from wowsintetico.mundo import _paquete_usar_objeto
        datos = _paquete_usar_objeto(24, 59403, 0xF140000000000001, guid_objetivo=0xF140000000000099)
        L = Lector(datos)
        L.u8(); L.u8(); L.u8(); L.u32(); L.u64(); L.u32(); L.u8()   # cabecera común, ya probada arriba
        self.assertEqual(L.u32(), 0x10)                              # TARGET_FLAG_ITEM
        self.assertEqual(L.guid_empaquetado(), 0xF140000000000099)
        self.assertEqual(L.resto(), 0)

    def test_leer_respuesta_loot_exito(self):
        from wowsintetico.mundo import _leer_respuesta_loot
        # Player::SendLoot + LootView: oro, número de filas y una fila con slot
        # de misión. El cliente debe enviar exactamente ese slot al recogerla.
        datos = (Escritor().u64(0xF140000000000042).u8(1).u32(0).u8(1)
                 .u8(3).u32(750).u32(1).u32(1234).u32(0).i32(0).u8(0).valor())
        r = _leer_respuesta_loot(datos)
        self.assertEqual(r, {"abierto": True, "error_loot": None,
                             "guid": 0xF140000000000042, "oro": 0,
                             "objetos": [{"ranura": 3, "entrada": 750,
                                           "cantidad": 1, "tipo": 0}]})

    def test_leer_respuesta_loot_error_mismo_opcode_que_el_exito(self):
        from wowsintetico.mundo import _leer_respuesta_loot
        # Player::SendLootError: mismo SMSG_LOOT_RESPONSE, pero loot_type=LOOT_NONE=0 + LootError.
        datos = Escritor().u64(0xF140000000000042).u8(0).u8(36).valor()   # 36 = ITEM_LOCKED
        r = _leer_respuesta_loot(datos)
        self.assertEqual(r, {"abierto": False, "error_loot": 36, "guid": 0xF140000000000042})

    def test_usar_objeto_manda_el_paquete_y_sin_respuesta_es_aceptado(self):
        m = mundo_sin_red()
        m.esperar = lambda *a, **k: (_ for _ in ()).throw(TimeoutError())
        self.assertIsNone(m.usar_objeto(23, 59403, 0xF140000000000001))
        self.assertEqual(m.enviados[0][0], 0x0AB)

    def test_usar_objeto_devuelve_el_inventoryresult_si_lo_rechaza(self):
        m = mundo_sin_red()
        m.esperar = lambda *a, **k: (0x112, bytes([8]))              # NO_REQUIRED_PROFICIENCY
        self.assertEqual(m.usar_objeto(23, 59403, 0xF140000000000001), 8)

    def test_abrir_objeto_bloqueado_lee_el_equip_error(self):
        m = mundo_sin_red()
        m.esperar = lambda *a, **k: (0x112, bytes([36]))             # ITEM_LOCKED
        r = m.abrir_objeto(24)
        self.assertEqual(r, {"abierto": False, "error_equipo": 36, "error_loot": None, "guid": None})
        self.assertEqual(m.enviados[0], (0x0AC, bytes([255, 24])))

    def test_abrir_objeto_con_exito_trae_el_guid(self):
        m = mundo_sin_red()
        m.esperar = lambda *a, **k: (0x160, Escritor().u64(0x77).u8(1).u32(0).u8(0).valor())
        r = m.abrir_objeto(24)
        self.assertEqual(r, {"abierto": True, "error_equipo": None, "error_loot": None,
                             "guid": 0x77, "oro": 0, "objetos": []})

    def test_liberar_loot_manda_el_guid_sin_empaquetar(self):
        m = mundo_sin_red()
        m.liberar_loot(0xF140000000000042)
        self.assertEqual(m.enviados, [(0x15F, struct.pack("<Q", 0xF140000000000042))])

    def test_objetos_bolsas_lee_player_field_pack_slot(self):
        m = mundo_sin_red()
        m.guid = 0x55
        base = upd.PLAYER_FIELD_PACK_SLOT_1
        m.objetos[0x55] = {"typeid": 4, "pos": {}, "valores": {base: 0x112233, base + 1: 0xF140}}
        bolsas = m.objetos_bolsas()
        self.assertEqual(len(bolsas), 16)
        self.assertEqual(bolsas[0], (0x112233, 0xF140))
        self.assertEqual(bolsas[1], (0, 0))


class PruebasTiradasLoot(unittest.TestCase):
    """M40: reparto de loot con jugadores reales (AiPlayerbot.LootRollAfterRealPlayersPass)."""

    def test_paquete_metodo_loot(self):
        from wowsintetico.mundo import _paquete_metodo_loot
        L = Lector(_paquete_metodo_loot(3, 2, 0xF130000000000001))    # GROUP_LOOT, verde
        self.assertEqual((L.u32(), L.u64(), L.u32()), (3, 0xF130000000000001, 2))
        self.assertEqual(L.resto(), 0)

    def test_paquete_votar_loot(self):
        from wowsintetico.mundo import _paquete_votar_loot
        L = Lector(_paquete_votar_loot(0xF140000000000042, 3, 1))     # ROLL_NEED
        self.assertEqual((L.u64(), L.u32(), L.u8()), (0xF140000000000042, 3, 1))
        self.assertEqual(L.resto(), 0)

    def test_leer_loot_start_roll(self):
        from wowsintetico.mundo import _leer_loot_start_roll
        e = Escritor().u64(0xF140000000000042).u32(559).u32(3).u32(17922)
        e.u32(0).u32(0).u32(1).u32(60000).u8(0x07)             # rollVoteMask: pase+necesidad+codicia
        r = _leer_loot_start_roll(e.valor())
        self.assertEqual(r, {"guid": 0xF140000000000042, "mapa": 559, "ranura": 3, "itemid": 17922,
                             "cantidad": 1, "cuenta_atras": 60000, "mascara": 0x07})

    def test_leer_loot_roll_autopase(self):
        from wowsintetico.mundo import _leer_loot_roll
        e = Escritor().u64(0xF140000000000042).u32(3).u64(0xF130000000000001).u32(17922)
        e.u32(0).u32(0).u8(128).u8(0).u8(1)                    # rollNumber=128 (pase), autoPass=1
        r = _leer_loot_roll(e.valor())
        self.assertEqual(r, {"guid": 0xF140000000000042, "ranura": 3, "jugador": 0xF130000000000001,
                             "itemid": 17922, "numero": 128, "tipo": 0, "auto_pase": True})

    def test_leer_loot_roll_won(self):
        from wowsintetico.mundo import _leer_loot_roll_won
        e = Escritor().u64(0xF140000000000042).u32(3).u32(17922).u32(0).u32(0)
        e.u64(0xF130000000000001).u8(97).u8(1)                 # gana con NEED
        r = _leer_loot_roll_won(e.valor())
        self.assertEqual(r, {"guid": 0xF140000000000042, "ranura": 3, "itemid": 17922,
                             "ganador": 0xF130000000000001, "numero": 97, "tipo": 1})

    def test_establecer_metodo_loot_manda_cmsg_loot_method(self):
        m = mundo_sin_red()
        m.establecer_metodo_loot(3, umbral=2)
        self.assertEqual(m.enviados, [(0x07A, struct.pack("<IQI", 3, 0, 2))])

    def test_abrir_criatura_manda_cmsg_loot_y_lee_la_respuesta(self):
        m = mundo_sin_red()
        m.esperar = lambda *a, **k: (0x160, Escritor().u64(0x99).u8(1).u32(0).u8(0).valor())
        r = m.abrir_criatura(0x99)
        self.assertEqual(r, {"abierto": True, "error_equipo": None, "error_loot": None,
                             "guid": 0x99, "oro": 0, "objetos": []})
        self.assertEqual(m.enviados, [(0x15D, struct.pack("<Q", 0x99))])

    def test_votar_loot_manda_cmsg_loot_roll(self):
        m = mundo_sin_red()
        m.votar_loot(0xF140000000000042, 3, 0)
        self.assertEqual(m.enviados, [(0x2A0, struct.pack("<QIB", 0xF140000000000042, 3, 0))])

    def test_dispatch_tirada_inicio_guarda_y_emite(self):
        m = mundo_sin_red()
        vistos = []
        m.oyentes.append(lambda t, ev, d: vistos.append((ev, d)) if ev == "tirada_inicio" else None)
        e = Escritor().u64(0x42).u32(559).u32(3).u32(17922).u32(0).u32(0).u32(1).u32(60000).u8(7)
        m._procesar(0x2A1, e.valor())
        self.assertEqual(m.tiradas[0x42]["itemid"], 17922)
        self.assertEqual(vistos[0][0], "tirada_inicio")

    def test_dispatch_tirada_ganada_borra_la_tirada_pendiente(self):
        m = mundo_sin_red()
        m.tiradas[0x42] = {"itemid": 17922}
        vistos = []
        m.oyentes.append(lambda t, ev, d: vistos.append((ev, d)) if ev == "tirada_ganada" else None)
        e = Escritor().u64(0x42).u32(3).u32(17922).u32(0).u32(0).u64(0x77).u8(50).u8(1)
        m._procesar(0x29F, e.valor())
        self.assertNotIn(0x42, m.tiradas)
        self.assertEqual(vistos[0][1]["ganador"], 0x77)


class PruebasMisiones(unittest.TestCase):
    def test_abandonar_usa_la_ranura_del_diario(self):
        m = mundo_sin_red()
        m.guid = 0x55
        m.objetos[0x55] = {"typeid": 4, "pos": {}, "valores": {upd.PLAYER_QUEST_LOG_1_1 + 5 * 2: 783}}
        self.assertEqual(m.misiones()[2], 783)
        self.assertTrue(m.abandonar_mision(783))
        self.assertEqual(m.enviados, [(0x194, bytes([2]))])
        self.assertFalse(m.abandonar_mision(155))
        m.aceptar_mision(0xF130000337000001, 783)
        self.assertEqual(m.enviados[-1], (0x189, struct.pack("<QII", 0xF130000337000001, 783, 0)))


class PruebasAreaTriggers(unittest.TestCase):
    def test_esfera_y_caja_girada(self):
        from wowsintetico.areatriggers import Vigilante, dentro
        esfera = {"id": 1, "mapa": 0, "x": 0, "y": 0, "z": 0, "radio": 5, "largo": 0, "ancho": 0, "alto": 0, "o": 0}
        self.assertTrue(dentro(esfera, 3, 3, 0))
        self.assertFalse(dentro(esfera, 4, 4, 0))
        import math
        caja = {"id": 2, "mapa": 0, "x": 0, "y": 0, "z": 0, "radio": 0, "largo": 20, "ancho": 2, "alto": 10,
                "o": math.pi / 2}
        self.assertTrue(dentro(caja, 0, 9, 0))           # el lado largo va por Y al girar 90°
        self.assertFalse(dentro(caja, 9, 0, 0))
        v = Vigilante([esfera, caja])
        v.cambiar_mapa(0)
        self.assertEqual([t["id"] for t in v.revisar(0, 0, 0)], [1, 2])
        self.assertEqual(v.revisar(0, 0, 0), [])         # sólo el flanco de entrada
        v.revisar(100, 100, 0)
        self.assertEqual([t["id"] for t in v.revisar(0.5, 1, 0)], [1, 2])

    def test_el_vigilante_manda_el_trigger_al_pisarlo(self):
        from wowsintetico.areatriggers import Vigilante
        m = mundo_sin_red()
        m.guid, m.mapa, m.pos = 0x55, 48, {"x": 100, "y": 100, "z": 0}
        m._ultimo_ping = 1e18
        m.armar_areatriggers(Vigilante([{"id": 259, "mapa": 48, "x": 0, "y": 0, "z": 0, "radio": 6, "largo": 0,
                                         "ancho": 0, "alto": 0, "o": 0}]))
        m._mantener()
        self.assertEqual(m.enviados, [])
        m.pos = {"x": 1, "y": 1, "z": 0}
        m._mantener()
        self.assertEqual(m.enviados, [(0x0B4, struct.pack("<I", 259))])


class PruebasGps(unittest.TestCase):
    def test_salida_en_espanol_con_liquido(self):
        from wowsintetico.mundo import leer_gps
        texto = ("You are indoors Mapa: 48 (Cavernas de Brazanegra) Zona: 719 (x) Área: 719 (x) Fase: 1\n"
                 "X: -151.89 Y: 106.96 Z: -39.87 Orientación: 4.53\nCuadrícula[31,32] Celda[1,2] InstanceID: 7\n"
                 " Zona X: 0.5 Zona Y: 0.5\nSuelo Z: -45.5 Suelo Z: -41.2 Tiene datos de altura (Mapa: 1 VMap: 1 MMap: 1)"
                 " Nivel de líquido: -30.0, suelo: -41.2, tipo: 1, flags 1, estado: 8.")
        g = leer_gps(texto)
        self.assertEqual((g["mapa"], g["instancia"]), (48, 7))
        self.assertEqual((g["suelo"], g["piso"]), (-45.5, -41.2))
        self.assertEqual(g["liquido"]["estado"], 8)
        self.assertTrue(g["interior"])

    def test_salida_en_ingles_y_basura(self):
        from wowsintetico.mundo import leer_gps
        g = leer_gps("Map: 0 (Eastern Kingdoms) Zone: 12 (Elwynn) Area: 9 (Northshire) Phase: 1\n"
                     "X: -8913.2 Y: -136.6 Z: 80.5 Orientation: 5.1\ngrid[1,2]cell[3,4] InstanceID: 0\n"
                     " ZoneX: 1 ZoneY: 2\nGroundZ: 80.4 FloorZ: 80.4 Have height data (Map: 1 VMap: 1 MMap: 1)")
        self.assertEqual(g["piso"], 80.4)
        self.assertIsNone(leer_gps("Unknown command"))


class PruebasRecepcion(unittest.TestCase):
    def test_timeout_a_mitad_de_paquete_no_pierde_la_cabecera(self):
        import socket as sk
        from wowsintetico.cripto import CifradoCabeceras
        from wowsintetico.mundo import Mundo
        K = bytes(range(40))
        servidor = CifradoCabeceras(K)
        # El servidor cifra con la clave "recibir" del cliente: se simula con otra instancia igual.
        cab = servidor.recibir.aplicar(struct.pack(">H", 2 + 3) + struct.pack("<H", 0x1CB))
        trozos = [cab, sk.timeout(), b"ok\x00"]

        class SocketFalso:
            def recv(self, n):
                t = trozos.pop(0)
                if isinstance(t, Exception):
                    raise t
                return t

        m = mundo_sin_red()
        m.sock, m.cifrado = SocketFalso(), CifradoCabeceras(K)
        with self.assertRaises(sk.timeout):
            Mundo._recibir(m)
        self.assertEqual(Mundo._recibir(m), (0x1CB, b"ok\x00"))


class PruebasInforme(unittest.TestCase):
    def _caso(self, **kw):
        from wowsintetico.catalogo import Caso
        return Caso(id=kw.pop("id", "x"), titulo="t", funcion=lambda ctx: None, **kw)

    def test_guardar_fallo_en_carpeta_nueva(self):
        import json
        import tempfile
        from pathlib import Path
        from wowsintetico.informe import Informe, ResultadoCaso

        informe = Informe({}, {})
        caso = ResultadoCaso(self._caso(), {}, eco=lambda *_: None)
        caso.comprobar("recorrido", False, "todos los jefes muertos")
        informe.agregar(caso)
        with tempfile.TemporaryDirectory() as carpeta:
            ruta = Path(carpeta) / "ejecuciones" / "mazmorra" / "informe.json"
            informe.guardar_json(ruta)
            datos = json.loads(ruta.read_text(encoding="utf-8"))
            self.assertEqual(datos["resultado"], "FALLO")
            self.assertEqual(datos["casos"][0]["comprobaciones"][0]["origen"], "desarrollo")
            self.assertEqual(informe.codigo_salida(), 1)
            informe.guardar_json(str(ruta))
            self.assertEqual(json.loads(ruta.read_text(encoding="utf-8"))["casos"], datos["casos"])

    def test_estados_y_codigos_de_salida(self):
        from wowsintetico.informe import BLOQUEADO, ERROR, Informe, ResultadoCaso
        silencio = lambda *_: None                       # noqa: E731
        inf = Informe({}, {})
        ok = ResultadoCaso(self._caso(), {}, eco=silencio)
        ok.comprobar("s", True, "a")
        inf.agregar(ok)
        self.assertEqual(inf.codigo_salida(), 0)
        vacio = ResultadoCaso(self._caso(id="y"), {}, eco=silencio)
        inf.agregar(vacio)
        self.assertEqual(vacio.resultado(), "OMITIDO")
        self.assertEqual(inf.codigo_salida(), 2)          # un caso sin comprobaciones no es un éxito
        bloq = ResultadoCaso(self._caso(id="z"), {}, eco=silencio)
        bloq.comprobar("s", True, "a")
        bloq.forzar(BLOQUEADO, "sin permiso")
        self.assertEqual(bloq.resultado(), BLOQUEADO)
        f = ResultadoCaso(self._caso(id="w"), {}, eco=silencio)
        f.comprobar("s", False, "b", origen="cliente")
        inf.agregar(f)
        self.assertEqual(inf.codigo_salida(), 1)
        self.assertEqual(f.como_dict()["comprobaciones"][0]["origen"], "cliente")
        e = ResultadoCaso(self._caso(id="v"), {}, eco=silencio)
        e.forzar(ERROR, "sin red")
        inf.agregar(e)
        self.assertEqual(inf.codigo_salida(), 3)
        self.assertEqual(inf.como_dict()["esquema"], "cliente-sintetico/2")

    def test_catalogo_parametros_y_seleccion(self):
        from wowsintetico import catalogo
        casos = catalogo.cargar()
        self.assertIn("mazmorra", casos)
        self.assertNotIn("mazmorra", [c.id for c in catalogo.seleccionar(casos, ["todo"])])
        self.assertTrue(all("rapido" in c.etiquetas for c in catalogo.seleccionar(casos, ["@rapido"])))
        p = casos["mazmorra"].parametros_efectivos({"duracion": "600", "intervenir": "no"})
        self.assertEqual((p["duracion"], p["intervenir"], p["mazmorra"]), (600, False, "bfd"))
        with self.assertRaises(ValueError):
            casos["mazmorra"].parametros_efectivos({"mazmorra": "karazhan"})
        with self.assertRaises(ValueError):
            casos["mazmorra"].parametros_efectivos({"inexistente": "1"})
        with self.assertRaises(KeyError):
            catalogo.seleccionar(casos, ["@no-existe"])

    def test_precondiciones_bloquean_escrituras_sin_permiso(self):
        from wowsintetico import catalogo
        from wowsintetico.ejecutor import precondiciones
        from wowsintetico.entorno import Entorno, ErrorEntorno
        casos = catalogo.cargar()
        lectura = Entorno({"host": "h", "cuenta": "c", "clave": "k"}, "x")
        self.assertIn("no permite escrituras", precondiciones(casos["mazmorra"], lectura, dbc=object()))
        self.assertEqual(precondiciones(casos["diagnostico"], lectura, dbc=None), "")
        escritura = Entorno({"host": "h", "cuenta": "c", "clave": "k", "permitir_escrituras": True}, "x")
        self.assertIn("DBC", precondiciones(casos["mazmorra"], escritura, dbc=None))
        self.assertNotIn("k", str(escritura.publico().values()).replace("cuenta", ""))
        with self.assertRaises(ErrorEntorno):
            Entorno({"host": "h", "cuenta": "c"}, "x")


class PruebasRegistro(unittest.TestCase):
    def test_solo_se_borra_lo_registrado_con_prefijo_y_guid(self):
        import tempfile
        from wowsintetico import registro
        viejo = registro.FICHERO
        with tempfile.TemporaryDirectory() as d:
            registro.FICHERO = os.path.join(d, "p.json")
            registro.CARPETA_ESTADO = d
            try:
                registro.apuntar("h", "cta", "Vsabc", 10, "arac")
                registro.apuntar("h", "cta", "Vsfalta", 12, "arac")
                lista = [{"nombre": "Vsabc", "guid": 10}, {"nombre": "Vsotro", "guid": 11},
                         {"nombre": "Lightcore", "guid": 1}, {"nombre": "Vsabc2", "guid": 10 + 100}]
                borrables, sospechosos, ausentes = registro.clasificar(lista, "h", "CTA", "Vs")
                self.assertEqual([p["nombre"] for p in borrables], ["Vsabc"])
                self.assertEqual({p["nombre"] for p in sospechosos}, {"Vsotro", "Vsabc2"})
                self.assertEqual([e["nombre"] for e in ausentes], ["Vsfalta"])
                registro.quitar("h", 10)
                self.assertEqual([e["guid"] for e in registro.pendientes("h", "cta")], [12])
            finally:
                registro.FICHERO = viejo


class PruebasTelemetria(unittest.TestCase):
    def _tele(self):
        from wowsintetico.telemetria import Telemetria
        m = mundo_sin_red()
        m.guid = 0x1
        t = Telemetria(m, None, mapa_objetivo=48, eco=lambda *_: None)
        t.fijar_grupo({0x10: "Tanque", 0x11: "Sanador"})
        return m, t

    def test_estado_de_dungeon_clear_jefes_y_atascos(self):
        m, t = self._tele()
        m.emitir("addon", emisor=0x10, prefijo="DC",
                 campos=["STATUS", "1", "4887", "Ghamoo-ra", "", "0", "moving", "En route", "2", "0"])
        self.assertEqual(t.tanque, 0x10)
        self.assertTrue(t.dc["visto_activo"])
        m.emitir("addon", emisor=0x10, prefijo="DC", campos=["BOSS", "4887", "1", "Ghamoo-ra", "alive", "1", "2", "3"])
        m.emitir("addon", emisor=0x10, prefijo="DC", campos=["BOSS", "4887", "1", "Ghamoo-ra", "dead", "1", "2", "3"])
        self.assertIsNotNone(t.jefes[4887]["t_muerte"])
        m.emitir("addon", emisor=0x10, prefijo="DC",
                 campos=["STATUS", "1", "4831", "Lady Sarevess", "no path to boss", "0", "stalled", "", "2", "0"])
        m.emitir("addon", emisor=0x10, prefijo="DC", campos=["BOSS", "4832", "4", "Aku'mai", "skipped", "0", "0", "0"])
        tipos = {a["tipo"] for a in t.anomalias}
        self.assertTrue({"dc_atasco", "jefe_saltado"} <= tipos)

    def test_muerte_wipe_ahogo_y_bajo_el_suelo(self):
        m, t = self._tele()
        m.objetos[0x1] = {"typeid": 4, "pos": {}, "valores": {0x18: 0}}
        m.emitir("estado_miembro", guid=0x10, estado="muerto", antes="vivo")
        m.emitir("estado_miembro", guid=0x11, estado="muerto", antes="vivo")
        m.emitir("temporizador", tipo="respiración", valor=40000, max=60000, escala=-1, pausa=False)
        m.emitir("dano_ambiental", guid=0x11, tipo="ahogamiento", cantidad=50)
        t.muestra_gps(0x10, {"mapa": 48, "x": 1, "y": 1, "z": -60, "suelo": -40, "piso": -40})
        t.muestra_gps(0x11, {"mapa": 36, "x": 1, "y": 1, "z": 0, "suelo": -100000.0, "piso": -100000.0})
        tipos = {a["tipo"] for a in t.anomalias}
        self.assertTrue({"muerte", "wipe", "respiracion", "dano_ahogamiento", "bajo_el_suelo", "fuera_del_mapa",
                         "sin_suelo"} <= tipos, tipos)

    def test_sumergido_no_es_bajo_el_suelo(self):
        m, t = self._tele()
        t.muestra_gps(0x10, {"mapa": 48, "x": 1, "y": 1, "z": -60, "suelo": -40, "piso": -40,
                             "liquido": {"nivel": -30, "fondo": -70, "estado": 8}})
        self.assertEqual({a["tipo"] for a in t.anomalias}, {"bajo_el_agua"})

    def test_sin_progreso_y_separacion(self):
        import time as tm
        m, t = self._tele()
        m.emitir("addon", emisor=0x10, prefijo="DC",
                 campos=["STATUS", "1", "4887", "Ghamoo-ra", "", "0", "idle", "Holding", "2", "0"])
        t.miembros[0x10]["pos"] = {"x": 0, "y": 0}
        t.miembros[0x11].update(pos={"x": 200, "y": 0}, lejos_desde=tm.time() - 100)
        t.revisar()
        t.ultimo_progreso -= 1000
        t.revisar()
        tipos = {a["tipo"] for a in t.anomalias}
        self.assertTrue({"sin_progreso", "separado"} <= tipos, tipos)


class PruebasMenuGossip(unittest.TestCase):
    """SMSG_GOSSIP_MESSAGE: id de opción, icono, codificada, dinero del cuadro, texto y confirmación."""

    @staticmethod
    def _paquete(opciones):
        w = Escritor().u64(0xF130000001).u32(7).u32(4259).u32(len(opciones))
        for i, (icono, dinero, texto, caja) in enumerate(opciones):
            w.u32(i).u8(icono).u8(1 if caja else 0).u32(dinero).cadena(texto).cadena(caja)
        return w.u32(0).valor()

    def test_lee_dinero_y_confirmacion(self):
        from wowsintetico.mundo import Mundo
        menu, opciones = Mundo._leer_menu(self._paquete([
            (6, 1000000, "Invocar banquero de materiales", "¿Invocar banquero de materiales?"),
            (0, 0, "Volver...", "")]))
        self.assertEqual(menu, 7)
        self.assertEqual([o["id"] for o in opciones], [0, 1])
        self.assertEqual(opciones[0]["dinero"], 1000000)
        self.assertEqual(opciones[0]["confirmacion"], "¿Invocar banquero de materiales?")
        self.assertEqual(opciones[1]["dinero"], 0)
        self.assertEqual(opciones[1]["confirmacion"], "")

    def test_menu_truncado_falla_en_vez_de_inventar(self):
        from wowsintetico.mundo import Mundo
        datos = self._paquete([(6, 5, "Texto", "Caja")])
        with self.assertRaises(Exception):
            Mundo._leer_menu(datos[:-6])


if __name__ == "__main__":
    unittest.main()
