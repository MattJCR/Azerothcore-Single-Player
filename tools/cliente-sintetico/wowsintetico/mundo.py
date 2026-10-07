# SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Sesión contra el worldserver.

Formatos contrastados con WorldSocket.cpp, CharacterHandler.cpp, Player.cpp
(BuildEnumData, SendInitialSpells, SendMirrorTimer), Chat.cpp (BuildChatPacket),
MovementHandler.cpp, MovementPacketBuilder.cpp, NPCPackets.cpp, GroupHandler.cpp,
Group.cpp, LFGHandler.cpp, CombatLogPackets.cpp y Opcodes.h del core fijado.

Además de las acciones directas, la sesión cumple lo que un cliente real debe
hacer cuando el servidor mueve al personaje (selfbot): confirmar teletransportes
cercanos y lejanos, mandar CMSG_AREATRIGGER al pisar un trigger (ver
areatriggers.py), responder al buscador y a TIME_SYNC. NO confirma cambios de
velocidad, raíz ni empujones (CMSG_FORCE_*_ACK, CMSG_MOVE_KNOCK_BACK_ACK): esas
respuestas llevan una MovementInfo y el core recolocaría al jugador en la
posición estimada del cliente, pisando la del spline del servidor. Se cuentan
como `forzados_sin_ack` en la telemetría.
"""
import os
import re
import socket
import struct
import time
import zlib

from . import actualizaciones as upd
from . import grupo as grp
from . import movimiento as mov
from .binario import Escritor, Lector
from .cripto import CifradoCabeceras, sha1

# Opcodes.h
CMSG_CHAR_CREATE, CMSG_CHAR_ENUM, CMSG_CHAR_DELETE = 0x036, 0x037, 0x038
SMSG_CHAR_CREATE, SMSG_CHAR_ENUM, SMSG_CHAR_DELETE = 0x03A, 0x03B, 0x03C
CMSG_PLAYER_LOGIN, SMSG_NEW_WORLD, SMSG_TRANSFER_PENDING, SMSG_TRANSFER_ABORTED = 0x03D, 0x03E, 0x03F, 0x040
CMSG_LOGOUT_REQUEST, SMSG_LOGOUT_RESPONSE, SMSG_LOGOUT_COMPLETE = 0x04B, 0x04C, 0x04D
CMSG_GROUP_INVITE, SMSG_GROUP_INVITE, CMSG_GROUP_ACCEPT, CMSG_GROUP_DISBAND = 0x06E, 0x06F, 0x072, 0x07B
SMSG_GROUP_DESTROYED, SMSG_GROUP_LIST, SMSG_PARTY_MEMBER_STATS = 0x07C, 0x07D, 0x07E
SMSG_PARTY_COMMAND_RESULT = 0x07F
CMSG_MESSAGECHAT, SMSG_MESSAGECHAT = 0x095, 0x096
SMSG_UPDATE_OBJECT, SMSG_DESTROY_OBJECT = 0x0A9, 0x0AA
CMSG_AREATRIGGER = 0x0B4
MSG_MOVE_TELEPORT_ACK, MSG_MOVE_WORLDPORT_ACK = 0x0C7, 0x0DC
SMSG_MONSTER_MOVE, SMSG_MONSTER_MOVE_TRANSPORT = 0x0DD, 0x2AE
SMSG_INITIAL_SPELLS, SMSG_LEARNED_SPELL, SMSG_SUPERCEDED_SPELL = 0x12A, 0x12B, 0x12C
SMSG_ATTACKSTART, SMSG_ATTACKSTOP = 0x143, 0x144
SMSG_SPELL_GO = 0x132
SMSG_RESURRECT_REQUEST = 0x15B
CMSG_TRAINER_LIST, SMSG_TRAINER_LIST = 0x1B0, 0x1B1
CMSG_TRAINER_BUY_SPELL, SMSG_TRAINER_BUY_SUCCEEDED, SMSG_TRAINER_BUY_FAILED = 0x1B2, 0x1B3, 0x1B4
CMSG_GOSSIP_HELLO, CMSG_GOSSIP_SELECT_OPTION, SMSG_GOSSIP_MESSAGE = 0x17B, 0x17C, 0x17D
CMSG_PETITION_SHOWLIST, SMSG_PETITION_SHOWLIST = 0x1BB, 0x1BC
CMSG_PETITION_BUY = 0x1BD
CMSG_TURN_IN_PETITION, SMSG_TURN_IN_PETITION_RESULTS = 0x1C4, 0x1C5
SMSG_NOTIFICATION = 0x1CB
SMSG_START_MIRROR_TIMER, SMSG_PAUSE_MIRROR_TIMER, SMSG_STOP_MIRROR_TIMER = 0x1D9, 0x1DA, 0x1DB
CMSG_PING, SMSG_PONG = 0x1DC, 0x1DD
SMSG_AUTH_CHALLENGE, CMSG_AUTH_SESSION, SMSG_AUTH_RESPONSE = 0x1EC, 0x1ED, 0x1EE
SMSG_PARTYKILLLOG = 0x1F5
SMSG_COMPRESSED_UPDATE_OBJECT = 0x1F6
SMSG_ENVIRONMENTAL_DAMAGE_LOG = 0x1FC
SMSG_REMOVED_SPELL = 0x203
SMSG_LOGIN_VERIFY_WORLD = 0x236
SMSG_AREA_TRIGGER_MESSAGE = 0x2B8
SMSG_INSTANCE_SAVE_CREATED = 0x2CB
SMSG_PARTY_MEMBER_STATS_FULL = 0x2F2
SMSG_RAID_INSTANCE_MESSAGE = 0x2FA
CMSG_LFG_JOIN, CMSG_LFG_LEAVE = 0x35C, 0x35D
SMSG_LFG_PROPOSAL_UPDATE, CMSG_LFG_PROPOSAL_RESULT, SMSG_LFG_ROLE_CHECK_UPDATE = 0x361, 0x362, 0x363
SMSG_LFG_JOIN_RESULT, SMSG_LFG_QUEUE_STATUS = 0x364, 0x365
SMSG_LFG_UPDATE_PLAYER, SMSG_LFG_UPDATE_PARTY = 0x367, 0x368
CMSG_LFG_SET_ROLES = 0x36A
CMSG_LFG_TELEPORT = 0x370
CMSG_REQUEST_PARTY_MEMBER_STATS = 0x27F
CMSG_SET_SELECTION = 0x13D
CMSG_QUESTGIVER_ACCEPT_QUEST, CMSG_QUESTLOG_REMOVE_QUEST = 0x189, 0x194
SMSG_TIME_SYNC_REQ, CMSG_TIME_SYNC_RESP = 0x390, 0x391
CMSG_REPOP_REQUEST = 0x15A
CMSG_ATTACKSWING, CMSG_ATTACKSTOP = 0x141, 0x142
SMSG_BATTLEFIELD_STATUS, CMSG_BATTLEFIELD_PORT = 0x2D4, 0x2D5
CMSG_BATTLEMASTER_JOIN_ARENA = 0x358
CMSG_BATTLEMASTER_JOIN = 0x2EE
CMSG_USE_ITEM, CMSG_OPEN_ITEM = 0x0AB, 0x0AC
SMSG_INVENTORY_CHANGE_FAILURE = 0x112
CMSG_LOOT_RELEASE, SMSG_LOOT_RESPONSE = 0x15F, 0x160
CMSG_LOOT, CMSG_LOOT_METHOD = 0x15D, 0x07A
SMSG_LOOT_ROLL_WON, CMSG_LOOT_ROLL, SMSG_LOOT_START_ROLL, SMSG_LOOT_ROLL = 0x29F, 0x2A0, 0x2A1, 0x2A2
# Group::RollVote (Group.h): qué contesta cada jugador a una tirada de botín.
ROLL_PASS, ROLL_NEED, ROLL_GREED, ROLL_DISENCHANT, ROLL_NOT_EMITED_YET = 0, 1, 2, 3, 4
# LootMethod (LootMgr.h): reparto del grupo.
LOOT_METHOD_FREE_FOR_ALL, LOOT_METHOD_MASTER_LOOT, LOOT_METHOD_GROUP_LOOT, LOOT_METHOD_NEED_BEFORE_GREED = 0, 2, 3, 4
# Cambios forzados de movimiento que Wow.exe confirmaría (ver docstring)
_FORZADOS = {0x0E2: "velocidad correr", 0x0E4: "velocidad atrás", 0x0E6: "velocidad nadar",
             0x2DA: "velocidad andar", 0x2DC: "velocidad nadar atrás", 0x2DE: "giro",
             0x381: "velocidad vuelo", 0x383: "velocidad vuelo atrás", 0x45C: "cabeceo",
             0x0E8: "raíz", 0x0EA: "quitar raíz", 0x0EF: "empujón", 0x0DE: "andar sobre el agua",
             0x0DF: "andar en tierra", 0x0F2: "caída lenta", 0x0F3: "caída normal",
             0x0F4: "flotar", 0x0F5: "dejar de flotar", 0x343: "poder volar", 0x344: "no poder volar"}

# SharedDefines.h
AUTH_OK = 0x0C
CHAR_CREATE_SUCCESS, CHAR_DELETE_SUCCESS = 0x2F, 0x47
CHAT_MSG_SYSTEM, CHAT_MSG_SAY, CHAT_MSG_PARTY, CHAT_MSG_WHISPER = 0x00, 0x01, 0x02, 0x07
CHAT_MSG_WHISPER_INFORM, CHAT_MSG_PARTY_LEADER = 0x09, 0x33
LANG_UNIVERSAL, LANG_ORCISH, LANG_COMMON, LANG_ADDON = 0, 1, 7, -1
RAZAS_HORDA = {2, 5, 6, 8, 10}
GOSSIP_ICON_TRAINER = 3          # GossipDef.h

# PetitionMgr.h / WorldSession.h (CharterTypes) / Guild.h (PetitionTurnResult)
GUILD_CHARTER, CHARTER_DISPLAY_ID, GUILD_CHARTER_TYPE = 5863, 16161, 9
PETITION_TURN_OK, PETITION_TURN_ALREADY_IN_GUILD, PETITION_TURN_NEED_MORE_SIGNATURES = 0, 2, 4
TEMPORIZADORES = {0: "fatiga", 1: "respiración", 2: "fuego"}
DANO_AMBIENTAL = {0: "agotamiento", 1: "ahogamiento", 2: "caída", 3: "lava", 4: "limo", 5: "fuego",
                  6: "caída al vacío"}

# Grupos de tipos de chat de ChatHandler::BuildChatPacket (Chat.cpp)
_CHAT_MONSTRUO = {0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x29, 0x2A, 0x2F}   # MONSTER_*, RAID_BOSS_*, BATTLENET
_CHAT_WHISPER_FOREIGN = 0x08
_CHAT_BG_SYSTEM = {0x24, 0x25, 0x26}
_CHAT_LOGRO = {0x30, 0x31}
_CHAT_CANAL = 0x11

_NUM = r"(-?\d+(?:\.\d+)?)"


class ErrorMundo(Exception):
    pass


class PlazoAgotado(Exception):
    """El caso ha superado su tiempo máximo."""


def es_jugador(guid: int) -> bool:
    return (guid >> 48) & 0xFFFF == 0


# ── objetos: construir CMSG_USE_ITEM y leer SMSG_LOOT_RESPONSE (M39) ─────────
# Funciones puras (sin socket) para poder probarlas en tests/test_cliente_sintetico.py.
def _paquete_usar_objeto(ranura: int, hechizo: int, guid_objeto: int, guid_objetivo=None) -> bytes:
    """Formato exacto de WorldSession::HandleUseItemOpcode (SpellHandler.cpp):
    bagIndex(u8)=255 · slot(u8) · castCount(u8) · spellId(u32) · itemGUID(u64, sin empaquetar)
    · glyphIndex(u32) · castFlags(u8) · targetMask(u32) · [si TARGET_FLAG_ITEM=0x10] itemTargetGUID
    empaquetado (SpellCastTargets::Read)."""
    mascara = 0x10 if guid_objetivo is not None else 0             # TARGET_FLAG_ITEM
    e = Escritor().u8(255).u8(ranura).u8(1).u32(hechizo).u64(guid_objeto).u32(0).u8(0).u32(mascara)
    if guid_objetivo is not None:
        e.guid_empaquetado(guid_objetivo)
    return e.valor()


def _leer_respuesta_loot(datos: bytes) -> dict:
    """SMSG_LOOT_RESPONSE: Player::SendLoot manda guid(u64) + loot_type(u8) + LootView cuando
    hay botín; Player::SendLootError manda el MISMO opcode con guid(u64) + u8(LOOT_NONE=0) +
    u8(LootError) cuando no lo hay — hay que leer el byte de tipo para distinguirlos, el
    opcode solo no basta."""
    L = Lector(datos)
    guid, tipo = L.u64(), L.u8()
    if tipo == 0:                                                  # LOOT_NONE: sigue el LootError
        return {"abierto": False, "error_loot": L.u8(), "guid": guid}
    oro, cantidad = L.u32(), L.u8()
    objetos = []
    for _ in range(cantidad):
        # LootMgr.cpp: slot + LootItem(itemid, count, display, suffix,
        # randomProperty) + slotType. Conservar la ranura para CMSG_AUTOSTORE.
        ranura, entrada, cuenta = L.u8(), L.u32(), L.u32()
        L.u32(), L.u32(), L.i32()
        tipo_ranura = L.u8()
        objetos.append({"ranura": ranura, "entrada": entrada,
                        "cantidad": cuenta, "tipo": tipo_ranura})
    return {"abierto": True, "error_loot": None, "guid": guid,
            "oro": oro, "objetos": objetos}


# ── tiradas de botín en grupo (M40) ───────────────────────────────────────────
# Formato exacto de Group::SendLootStartRoll/SendLootRoll/SendLootRollWon (Group.cpp) y
# WorldSession::HandleLootMethodOpcode/HandleLootRoll (GroupHandler.cpp), del core fijado en la VM.
def _paquete_metodo_loot(metodo: int, umbral: int, maestro_guid: int = 0) -> bytes:
    return Escritor().u32(metodo).u64(maestro_guid).u32(umbral).valor()


def _paquete_votar_loot(guid_objeto: int, ranura: int, tipo: int) -> bytes:
    return Escritor().u64(guid_objeto).u32(ranura).u8(tipo).valor()


def _leer_loot_start_roll(datos: bytes) -> dict:
    """SMSG_LOOT_START_ROLL: sólo llega a quien sigue en NOT_EMITED_YET para esa tirada."""
    L = Lector(datos)
    guid, mapa, ranura, itemid = L.u64(), L.u32(), L.u32(), L.u32()
    L.u32(); L.u32()                                                # randomSuffix, randomPropId: sin usar
    cantidad, cuenta_atras, mascara = L.u32(), L.u32(), L.u8()
    return {"guid": guid, "mapa": mapa, "ranura": ranura, "itemid": itemid,
            "cantidad": cantidad, "cuenta_atras": cuenta_atras, "mascara": mascara}


def _leer_loot_roll(datos: bytes) -> dict:
    """SMSG_LOOT_ROLL: el voto YA CONTADO de un participante, difundido a todo el que
    siga con voto válido (NOT_VALID excluido) en esa tirada — incluido el que vota."""
    L = Lector(datos)
    guid, ranura, jugador, itemid = L.u64(), L.u32(), L.u64(), L.u32()
    L.u32(); L.u32()                                                # randomSuffix, randomPropId: sin usar
    numero, tipo, auto_pase = L.u8(), L.u8(), bool(L.u8())
    return {"guid": guid, "ranura": ranura, "jugador": jugador, "itemid": itemid,
            "numero": numero, "tipo": tipo, "auto_pase": auto_pase}


def _leer_loot_roll_won(datos: bytes) -> dict:
    L = Lector(datos)
    guid, ranura, itemid = L.u64(), L.u32(), L.u32()
    L.u32(); L.u32()                                                # randomSuffix, randomPropId: sin usar
    ganador, numero, tipo = L.u64(), L.u8(), L.u8()
    return {"guid": guid, "ranura": ranura, "itemid": itemid, "ganador": ganador,
            "numero": numero, "tipo": tipo}


class Mundo:
    def __init__(self, host: str, puerto: int, usuario: str, K: bytes, reino: int = 1,
                 timeout: float = 20, registro=None, timeout_conexion: float = 60):
        # Con el modo en espera, la primera conexión despierta al worldserver: la
        # respuesta llega cuando termina de arrancar (medido ~14 s en frío).
        self.sock = socket.create_connection((host, puerto), timeout=timeout_conexion)
        self.usuario, self.K, self.reino = usuario.upper(), K, reino
        self.timeout = timeout
        self.cifrado = None
        self.buffer = b""
        self._cabecera = None
        self.registro = registro or (lambda *_: None)
        self.plazo = None                # time.time() límite del caso
        self.oyentes = []                # f(t, tipo, datos)
        self.guid = None
        self.nombre = None
        self.objetos = {}                # guid -> {"typeid", "pos", "valores"}
        self.hechizos = set()
        self.aprendidos = []
        self.mensajes = []               # (tipo, texto) de chat de sistema y notificaciones
        self.chat = []                   # dicts con tipo, idioma, emisor, texto
        self.addon = []                  # (t, emisor, prefijo, campos)
        self.errores_lectura = []        # paquetes que no se pudieron leer: fallo del cliente
        self.pos = None
        self.mapa = None
        self.idioma_chat = LANG_COMMON
        self.trayectorias = {}           # guid -> movimiento.Trayectoria
        self.vigilante = None            # areatriggers.Vigilante
        self.grupo = None                # leer_lista_grupo()
        self.miembros = {}               # guid -> última estadística conocida
        self.tiradas = {}                # guid del objeto -> última SMSG_LOOT_START_ROLL (M40)
        self.temporizadores = {}         # tipo -> dict
        self.forzados_sin_ack = {}       # nombre -> veces
        self.lfg = {"auto_aceptar": False, "roles": 0, "propuestas": [], "estado": None, "union": None}
        self.bg = {"auto_puerto": False, "status": None}     # SMSG_BATTLEFIELD_STATUS (arena/BG)
        self.teletransportes = 0
        self.ultimo_hechizo = (0.0, 0)   # (t, id) del último SMSG_SPELL_GO propio
        self._tp_provocado_hasta = 0.0   # los teletransportes que pide el agente (.go/.tele) no son anomalías
        self._ultimo_ping = time.time()
        self._ping_seq = 0
        self._autenticar()

    # ── eventos ─────────────────────────────────────────────────────────────
    def emitir(self, _evento, **datos):
        """Avisa a los oyentes: f(t, evento, datos). Los datos pueden llevar su propio `tipo`."""
        t = time.time()
        for f in list(self.oyentes):
            f(t, _evento, datos)

    # ── transporte ──────────────────────────────────────────────────────────
    def _leer_exacto(self, n):
        while len(self.buffer) < n:
            trozo = self.sock.recv(65536)
            if not trozo:
                raise ErrorMundo("el worldserver cerró la conexión")
            self.buffer += trozo
        datos, self.buffer = self.buffer[:n], self.buffer[n:]
        return datos

    def _recibir(self):
        # La cabecera descifrada se guarda antes de leer el cuerpo: si el socket
        # vence a mitad de paquete, la siguiente llamada sigue donde iba (descifrar
        # dos veces la misma cabecera desincronizaría el ARC4 para siempre).
        if self._cabecera is None:
            c = self._leer_exacto(4)
            if self.cifrado:
                c = self.cifrado.recibir.aplicar(c)
            self._cabecera = c
        c = self._cabecera
        if self.cifrado and c[0] & 0x80 and len(c) == 4:     # cabecera grande: 3 bytes de tamaño
            c = self._cabecera = c + self.cifrado.recibir.aplicar(self._leer_exacto(1))
        if c[0] & 0x80 and self.cifrado:
            tam = ((c[0] & 0x7F) << 16) | (c[1] << 8) | c[2]
            op = struct.unpack("<H", c[3:5])[0]
        else:
            tam = (c[0] << 8) | c[1]
            op = struct.unpack("<H", c[2:4])[0]
        cuerpo = self._leer_exacto(tam - 2)
        self._cabecera = None
        return op, cuerpo

    def enviar(self, op: int, cuerpo: bytes = b""):
        cab = struct.pack(">H", len(cuerpo) + 4) + struct.pack("<I", op)
        if self.cifrado:
            cab = self.cifrado.enviar.aplicar(cab)
        self.sock.sendall(cab + cuerpo)

    def _autenticar(self):
        op, datos = self._recibir()
        if op != SMSG_AUTH_CHALLENGE:
            raise ErrorMundo("se esperaba SMSG_AUTH_CHALLENGE y llegó 0x%03X" % op)
        self.sock.settimeout(self.timeout)
        semilla_servidor = datos[4:8]
        semilla_cliente = os.urandom(4)
        digest = sha1(self.usuario.encode(), bytes(4), semilla_cliente, semilla_servidor, self.K)
        addons = struct.pack("<II", 0, 0)            # 0 addons, marca de tiempo 0
        e = (Escritor().u32(12340).u32(0).cadena(self.usuario).u32(0).bytes(semilla_cliente)
             .u32(0).u32(0).u32(self.reino).u64(0).bytes(digest)
             .u32(len(addons)).bytes(zlib.compress(addons)))
        self.enviar(CMSG_AUTH_SESSION, e.valor())
        self.cifrado = CifradoCabeceras(self.K)
        op, datos = self.esperar({SMSG_AUTH_RESPONSE}, timeout=60)
        if datos[0] != AUTH_OK:
            raise ErrorMundo("SMSG_AUTH_RESPONSE código 0x%02X" % datos[0])

    def cerrar(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # ── bucle de recepción ──────────────────────────────────────────────────
    def esperar(self, opcodes, timeout: float = 20, condicion=None):
        """Procesa paquetes hasta recibir uno de `opcodes` (y que cumpla
        `condicion(datos)` si se da). Devuelve (opcode, datos)."""
        limite = time.time() + timeout
        while True:
            ahora = time.time()
            if self.plazo and ahora > self.plazo:
                raise PlazoAgotado("el caso superó su tiempo máximo")
            restante = limite - ahora
            if restante <= 0:
                raise TimeoutError("sin respuesta %s en %.0f s" % (
                    ", ".join("0x%03X" % o for o in sorted(opcodes)), timeout))
            self.sock.settimeout(min(restante, 0.25 if self.vigilante else 2))
            try:
                op, datos = self._recibir()
            except socket.timeout:
                self._mantener()
                continue
            self._procesar(op, datos)
            self._mantener()
            if op in opcodes and (condicion is None or condicion(datos)):
                return op, datos

    def bombear(self, segundos: float):
        """Procesa todo lo que llegue durante `segundos`."""
        try:
            self.esperar(set(), timeout=segundos)
        except TimeoutError:
            pass

    def _mantener(self):
        if time.time() - self._ultimo_ping > 25:
            self._ping_seq += 1
            self.enviar(CMSG_PING, struct.pack("<II", self._ping_seq, 50))
            self._ultimo_ping = time.time()
        if self.vigilante and self.guid and self.mapa is not None:
            p = self.posicion_actual()
            if p:
                for t in self.vigilante.revisar(p["x"], p["y"], p["z"]):
                    self.enviar_areatrigger(t["id"], automatico=True)

    def _procesar(self, op, datos):
        try:
            self._procesar_paquete(op, datos)
        except Exception as e:                         # noqa: BLE001 — se registra y se sigue
            self.errores_lectura.append("0x%03X: %s" % (op, e))
            self.emitir("error_lectura", opcode=op, error=str(e))

    def _procesar_paquete(self, op, datos):
        if op in (SMSG_UPDATE_OBJECT, SMSG_COMPRESSED_UPDATE_OBJECT):
            for b in upd.leer_paquete(datos, op == SMSG_COMPRESSED_UPDATE_OBJECT):
                self._aplicar_bloque(b)
        elif op == SMSG_DESTROY_OBJECT:
            g = struct.unpack("<Q", datos[:8])[0]
            self.objetos.pop(g, None)
            self.trayectorias.pop(g, None)
        elif op == SMSG_INITIAL_SPELLS:
            L = Lector(datos)
            L.u8()
            for _ in range(L.u16()):
                self.hechizos.add(L.u32())
                L.u16()
        elif op == SMSG_LEARNED_SPELL:
            sid = struct.unpack("<I", datos[:4])[0]
            self.hechizos.add(sid)
            self.aprendidos.append(sid)
        elif op == SMSG_SUPERCEDED_SPELL:
            viejo, nuevo = struct.unpack("<II", datos[:8])
            self.hechizos.discard(viejo)
            self.hechizos.add(nuevo)
        elif op == SMSG_REMOVED_SPELL:
            self.hechizos.discard(struct.unpack("<I", datos[:4])[0])
        elif op == SMSG_MESSAGECHAT:
            self._leer_chat(datos)
        elif op == SMSG_NOTIFICATION:
            texto = Lector(datos).cadena()
            self.mensajes.append(("aviso", texto))
            self.emitir("aviso", texto=texto)
        elif op == SMSG_TIME_SYNC_REQ:
            self.enviar(CMSG_TIME_SYNC_RESP, struct.pack("<II", struct.unpack("<I", datos[:4])[0],
                                                         int(time.time() * 1000) & 0xFFFFFFFF))
        elif op == MSG_MOVE_TELEPORT_ACK:
            L = Lector(datos)
            guid = L.guid_empaquetado()
            contador = L.u32()
            x, y, z, o, _ = upd.leer_info_movimiento(L)
            antes = self.posicion_actual()
            self.pos = {"x": x, "y": y, "z": z, "o": o}
            self.trayectorias.pop(self.guid, None)
            self.teletransportes += 1
            self.enviar(MSG_MOVE_TELEPORT_ACK, Escritor().guid_empaquetado(guid).u32(contador)
                        .u32(int(time.time() * 1000) & 0xFFFFFFFF).valor())
            t_h, id_h = self.ultimo_hechizo
            self.emitir("teletransporte", mapa=self.mapa, desde=antes, hasta=dict(self.pos), lejano=False,
                        provocado=time.time() < self._tp_provocado_hasta,
                        hechizo=id_h if time.time() - t_h < 1.5 else None)
        elif op == SMSG_TRANSFER_PENDING:
            self.emitir("cambio_mapa_pendiente", mapa=struct.unpack("<I", datos[:4])[0])
        elif op == SMSG_TRANSFER_ABORTED:
            self.emitir("cambio_mapa_abortado", mapa=struct.unpack("<I", datos[:4])[0],
                        motivo=datos[4] if len(datos) > 4 else None)
        elif op == SMSG_NEW_WORLD:
            mapa, x, y, z, o = struct.unpack("<Iffff", datos[:20])
            antes, mapa_antes = self.posicion_actual(), self.mapa
            self.mapa, self.pos = mapa, {"x": x, "y": y, "z": z, "o": o}
            self.objetos = {k: v for k, v in self.objetos.items() if k == self.guid}
            self.trayectorias = {}
            self.teletransportes += 1
            if self.vigilante:
                self.vigilante.cambiar_mapa(mapa)
            self.enviar(MSG_MOVE_WORLDPORT_ACK)
            self.emitir("teletransporte", mapa=mapa, mapa_antes=mapa_antes, desde=antes, hasta=dict(self.pos),
                        lejano=True, provocado=time.time() < self._tp_provocado_hasta)
        elif op == SMSG_LOGIN_VERIFY_WORLD:
            mapa, x, y, z, o = struct.unpack("<Iffff", datos[:20])
            self.mapa, self.pos = mapa, {"x": x, "y": y, "z": z, "o": o}
            if self.vigilante:
                self.vigilante.cambiar_mapa(mapa)
        elif op in (SMSG_MONSTER_MOVE, SMSG_MONSTER_MOVE_TRANSPORT):
            m = mov.leer_monster_move(datos, op == SMSG_MONSTER_MOVE_TRANSPORT)
            if m["tipo"] == mov.MOVE_STOP:
                self.trayectorias.pop(m["guid"], None)
                if m["guid"] == self.guid:
                    x, y, z = m["inicio"]
                    self.pos = {"x": x, "y": y, "z": z, "o": (self.pos or {}).get("o", 0)}
            else:
                self.trayectorias[m["guid"]] = mov.Trayectoria(m)
            if m["guid"] == self.guid:
                self.emitir("movimiento_propio", inicio=m["inicio"], destino=m["puntos"][-1],
                            duracion_ms=m["duracion_ms"], parada=m["tipo"] == mov.MOVE_STOP)
            elif m["guid"] in self.miembros:
                self.emitir("movimiento_miembro", guid=m["guid"], destino=m["puntos"][-1],
                            duracion_ms=m["duracion_ms"])
        elif op in _FORZADOS:
            nombre = _FORZADOS[op]
            self.forzados_sin_ack[nombre] = self.forzados_sin_ack.get(nombre, 0) + 1
        elif op == SMSG_GROUP_LIST:
            antes = {m["guid"] for m in (self.grupo or {}).get("miembros", [])}
            self.grupo = grp.leer_lista_grupo(datos)
            ahora = {m["guid"] for m in self.grupo["miembros"]}
            if antes != ahora:
                self.emitir("grupo", miembros=[(m["nombre"], m["guid"]) for m in self.grupo["miembros"]],
                            lider=self.grupo["lider"], tipo=self.grupo["tipo"])
        elif op == SMSG_GROUP_DESTROYED:
            self.grupo = None
            self.emitir("grupo_disuelto")
        elif op == SMSG_LOOT_START_ROLL:
            r = _leer_loot_start_roll(datos)
            self.tiradas[r["guid"]] = r
            self.emitir("tirada_inicio", **r)
        elif op == SMSG_LOOT_ROLL:
            self.emitir("tirada_voto", **_leer_loot_roll(datos))
        elif op == SMSG_LOOT_ROLL_WON:
            r = _leer_loot_roll_won(datos)
            self.tiradas.pop(r["guid"], None)
            self.emitir("tirada_ganada", **r)
        elif op in (SMSG_PARTY_MEMBER_STATS, SMSG_PARTY_MEMBER_STATS_FULL):
            e = grp.leer_estadisticas(datos, op == SMSG_PARTY_MEMBER_STATS_FULL)
            viejo = self.miembros.setdefault(e["guid"], {})
            estado_antes = viejo.get("estado")
            viejo.update(e)
            viejo["t"] = time.time()
            if "estado" in e and e["estado"] != estado_antes:
                self.emitir("estado_miembro", guid=e["guid"], estado=grp.estado_texto(e["estado"]),
                            antes=grp.estado_texto(estado_antes) if estado_antes is not None else None)
            self.emitir("stats_miembro", **e)
        elif op == SMSG_START_MIRROR_TIMER:
            tipo, valor, maximo, escala, pausa, hechizo = struct.unpack("<IIIiBI", datos[:21])
            self.temporizadores[tipo] = {"valor": valor, "max": maximo, "escala": escala, "t": time.time()}
            self.emitir("temporizador", tipo=TEMPORIZADORES.get(tipo, tipo), valor=valor, max=maximo,
                        escala=escala, pausa=bool(pausa))
        elif op == SMSG_STOP_MIRROR_TIMER:
            tipo = struct.unpack("<I", datos[:4])[0]
            self.temporizadores.pop(tipo, None)
            self.emitir("temporizador_fin", tipo=TEMPORIZADORES.get(tipo, tipo))
        elif op == SMSG_ENVIRONMENTAL_DAMAGE_LOG:
            victima, tipo, cantidad = struct.unpack("<QBI", datos[:13])
            self.emitir("dano_ambiental", guid=victima, tipo=DANO_AMBIENTAL.get(tipo, tipo), cantidad=cantidad)
        elif op == SMSG_PARTYKILLLOG:
            asesino, victima = struct.unpack("<QQ", datos[:16])
            v = self.objetos.get(victima, {}).get("valores", {})
            self.emitir("muerte_unidad", asesino=asesino, victima=victima,
                        entrada=v.get(upd.OBJECT_FIELD_ENTRY, 0))
        elif op == SMSG_SPELL_GO:
            # Spell::SendSpellGo: guid del objeto (empaquetado), lanzador (empaquetado), u8, u32 hechizo
            L = Lector(datos)
            objeto = L.guid_empaquetado()
            lanzador = L.guid_empaquetado()
            L.u8()
            hechizo = L.u32()
            self.emitir("hechizo_mundo", objeto=objeto, lanzador=lanzador, hechizo=hechizo)
            if lanzador == self.guid:
                self.ultimo_hechizo = (time.time(), hechizo)
        elif op == SMSG_ATTACKSTART:
            atacante, victima = struct.unpack("<QQ", datos[:16])
            self.emitir("ataque", atacante=atacante, victima=victima)
        elif op == SMSG_RESURRECT_REQUEST:
            self.emitir("peticion_resurreccion", de=struct.unpack("<Q", datos[:8])[0])
        elif op == SMSG_AREA_TRIGGER_MESSAGE:
            L = Lector(datos)
            L.u32()
            texto = L.cadena()
            self.emitir("mensaje_trigger", texto=texto)
        elif op == SMSG_RAID_INSTANCE_MESSAGE:
            self.emitir("mensaje_instancia", tipo=struct.unpack("<I", datos[:4])[0])
        elif op == SMSG_INSTANCE_SAVE_CREATED:
            self.emitir("instancia_creada")
        elif op == SMSG_LFG_JOIN_RESULT:
            self.lfg["union"] = grp.leer_resultado_union(datos)
            self.emitir("lfg_union", **self.lfg["union"])
        elif op == SMSG_LFG_ROLE_CHECK_UPDATE:
            estado = struct.unpack("<I", datos[:4])[0]
            self.emitir("lfg_roles", estado=estado)
            if self.lfg["auto_aceptar"] and self.lfg["roles"]:
                self.enviar(CMSG_LFG_SET_ROLES, grp.lfg_roles(self.lfg["roles"]))
        elif op == SMSG_LFG_PROPOSAL_UPDATE:
            p = grp.leer_propuesta(datos)
            self.lfg["propuestas"].append(p)
            self.emitir("lfg_propuesta", **p)
            # Como el botón «Aceptar»: una sola vez por propuesta. El core reenvía la
            # actualización a cada respuesta; aceptar en cada una formaba un bucle.
            aceptadas = self.lfg.setdefault("aceptadas", set())
            if self.lfg["auto_aceptar"] and p["estado"] == 0 and p["id"] not in aceptadas:
                aceptadas.add(p["id"])
                self.enviar(CMSG_LFG_PROPOSAL_RESULT, grp.lfg_respuesta_propuesta(p["id"], True))
                self.emitir("lfg_acepto", id=p["id"])
        elif op in (SMSG_LFG_UPDATE_PLAYER, SMSG_LFG_UPDATE_PARTY, SMSG_LFG_QUEUE_STATUS):
            self.lfg["estado"] = datos[0] if op != SMSG_LFG_QUEUE_STATUS and datos else self.lfg["estado"]
            self.emitir("lfg_actualizacion", opcode=op, dato=datos[0] if datos else None)
        elif op == SMSG_BATTLEFIELD_STATUS:
            e = grp.leer_estado_batalla(datos)
            self.bg.update(e)
            self.emitir("bg_estado", **e)
            # Como el botón «Entrar a la batalla»: acepta la invitación en cuanto
            # se le pide (STATUS_WAIT_JOIN), igual que auto_aceptar del buscador.
            if self.bg.get("auto_puerto") and e.get("status") == grp.BG_STATUS_WAIT_JOIN:
                self.enviar(CMSG_BATTLEFIELD_PORT, grp.puerto_campo(e["arenatype"], e["bg_tipo"]))
                self.emitir("bg_acepto", ranura=e["ranura"])

    def _aplicar_bloque(self, b):
        if b["tipo"] in (upd.OUT_OF_RANGE, upd.NEAR):
            if b["tipo"] == upd.OUT_OF_RANGE:
                for g in b["guids"]:
                    self.objetos.pop(g, None)
                    self.trayectorias.pop(g, None)
            return
        obj = self.objetos.setdefault(b["guid"], {"typeid": None, "pos": {}, "valores": {}})
        if "typeid" in b:
            obj["typeid"] = b["typeid"]
        if b.get("pos"):
            obj["pos"] = b["pos"]
            if b["guid"] == self.guid:
                self.pos = b["pos"]
        valores = b.get("valores", {})
        if b["guid"] == self.guid and upd.UNIT_FIELD_HEALTH in valores:
            antes = obj["valores"].get(upd.UNIT_FIELD_HEALTH)
            ahora = valores[upd.UNIT_FIELD_HEALTH]
            if antes is not None and (antes == 0) != (ahora == 0):
                self.emitir("vida_propia", vivo=ahora > 0)
        obj["valores"].update(valores)

    def _leer_chat(self, datos):
        """Espejo de ChatHandler::BuildChatPacket (sólo SMSG_MESSAGECHAT, no el de GM)."""
        L = Lector(datos)
        tipo = L.u8()
        idioma = L.i32()
        emisor = L.u64()
        L.u32()

        def es_mascota(g):
            return (g >> 48) & 0xFFFF == 0xF140

        canal = None
        if tipo in _CHAT_MONSTRUO:
            L.bytes(L.u32())
            receptor = L.u64()
            if receptor and not es_jugador(receptor) and not es_mascota(receptor):
                L.bytes(L.u32())
        elif tipo == _CHAT_WHISPER_FOREIGN:
            L.bytes(L.u32())
            L.u64()
        elif tipo in _CHAT_BG_SYSTEM:
            receptor = L.u64()
            if receptor and not es_jugador(receptor):
                L.bytes(L.u32())
        elif tipo in _CHAT_LOGRO:
            L.u64()
        else:
            if tipo == _CHAT_CANAL:
                canal = L.cadena()
            L.u64()
        texto = L.bytes(L.u32()).rstrip(b"\x00").decode("utf-8", "replace")
        if idioma == LANG_ADDON:
            prefijo, _, resto = texto.partition("\t")
            campos = resto.split("\t") if resto else []
            self.addon.append((time.time(), emisor, prefijo, campos))
            self.emitir("addon", emisor=emisor, prefijo=prefijo, campos=campos, tipo=tipo)
            return
        self.mensajes.append(("sistema" if tipo == CHAT_MSG_SYSTEM else "chat", texto))
        self.chat.append({"t": time.time(), "tipo": tipo, "idioma": idioma, "emisor": emisor,
                          "canal": canal, "texto": texto})
        if tipo != CHAT_MSG_SYSTEM:
            self.emitir("chat", tipo=tipo, emisor=emisor, canal=canal, texto=texto)

    # ── personajes ──────────────────────────────────────────────────────────
    def personajes(self) -> list:
        self.enviar(CMSG_CHAR_ENUM)
        _, datos = self.esperar({SMSG_CHAR_ENUM})
        L = Lector(datos)
        lista = []
        for _ in range(L.u8()):
            guid, nombre = L.u64(), L.cadena()
            raza, clase, genero = L.u8(), L.u8(), L.u8()
            L.bytes(5)
            nivel, zona, mapa = L.u8(), L.u32(), L.u32()
            L.bytes(12 + 4 + 4 + 4 + 1 + 12)
            L.bytes(23 * 9)
            lista.append({"guid": guid, "nombre": nombre, "raza": raza, "clase": clase,
                          "genero": genero, "nivel": nivel, "zona": zona, "mapa": mapa})
        return lista

    def crear_personaje(self, nombre, raza, clase, genero=0) -> int:
        """Devuelve el código de SMSG_CHAR_CREATE (0x2F = éxito)."""
        e = Escritor().cadena(nombre).u8(raza).u8(clase).u8(genero)
        e.u8(0).u8(0).u8(0).u8(0).u8(0).u8(0)       # piel, cara, peinado, color, vello, equipo
        self.enviar(CMSG_CHAR_CREATE, e.valor())
        _, datos = self.esperar({SMSG_CHAR_CREATE})
        return datos[0]

    def borrar_personaje(self, guid) -> int:
        self.enviar(CMSG_CHAR_DELETE, struct.pack("<Q", guid))
        _, datos = self.esperar({SMSG_CHAR_DELETE})
        return datos[0]

    def entrar(self, guid, raza: int = None, espera_mundo: float = 8, nombre: str = None):
        """`raza` fija el idioma de /decir: el servidor rechaza LANG_UNIVERSAL con
        "Unknown language" antes de interpretar los comandos GM."""
        self.guid, self.nombre = guid, nombre
        if raza is not None:
            self.idioma_chat = LANG_ORCISH if raza in RAZAS_HORDA else LANG_COMMON
        self.enviar(CMSG_PLAYER_LOGIN, struct.pack("<Q", guid))
        self.esperar({SMSG_LOGIN_VERIFY_WORLD}, timeout=60)
        self.bombear(espera_mundo)                    # objetos cercanos, hechizos, habilidades

    def salir(self, timeout: float = 30):
        self.enviar(CMSG_LOGOUT_REQUEST)
        op, datos = self.esperar({SMSG_LOGOUT_COMPLETE, SMSG_LOGOUT_RESPONSE}, timeout=timeout)
        if op == SMSG_LOGOUT_RESPONSE:
            resultado = struct.unpack("<I", datos[:4])[0]
            if resultado != 0:
                raise ErrorMundo("el servidor rechazó el logout (código %d)" % resultado)
            self.esperar({SMSG_LOGOUT_COMPLETE}, timeout=timeout)
        self.guid = None
        self.objetos = {}
        self.trayectorias = {}

    # ── estado del propio personaje ─────────────────────────────────────────
    def valor_propio(self, indice, defecto=0):
        return self.objetos.get(self.guid, {}).get("valores", {}).get(indice, defecto)

    def habilidades(self) -> dict:
        """PLAYER_SKILL_INFO: {skill_id: (valor, máximo)} de las 128 ranuras."""
        res = {}
        for i in range(128):
            base = upd.PLAYER_SKILL_INFO_1_1 + 3 * i
            ident = self.valor_propio(base) & 0xFFFF
            if ident:
                v = self.valor_propio(base + 1)
                res[ident] = (v & 0xFFFF, v >> 16)
        return res

    def equipo_visible(self) -> list:
        """Entradas de objeto de las 19 ranuras visibles (0 = vacía)."""
        return [self.valor_propio(upd.PLAYER_VISIBLE_ITEM_1_ENTRYID + 2 * i) for i in range(19)]

    def objetos_bolsas(self) -> list:
        """GUID empaquetado (lo, hi) de las 16 ranuras de la mochila (PLAYER_FIELD_PACK_SLOT_1);
        (0, 0) = vacía. No cubre los contenidos de bolsas EQUIPADAS aparte de la mochila
        (esas son objetos-contenedor propios, con sus propios campos de ranura)."""
        return [(self.valor_propio(upd.PLAYER_FIELD_PACK_SLOT_1 + 2 * i),
                 self.valor_propio(upd.PLAYER_FIELD_PACK_SLOT_1 + 2 * i + 1)) for i in range(16)]

    def objetos_llavero(self) -> list:
        """GUID empaquetado (lo, hi) de las 32 ranuras del llavero (PLAYER_FIELD_KEYRING_SLOT_1,
        ranuras de inventario 86..117): donde caen los objetos con `BagFamily` que incluye el
        bit de llaves (256) — p. ej. la ganzúa de recompensa (600000) y las llaves de
        esqueleto reales — en vez de la mochila; (0, 0) = vacía."""
        return [(self.valor_propio(upd.PLAYER_FIELD_KEYRING_SLOT_1 + 2 * i),
                 self.valor_propio(upd.PLAYER_FIELD_KEYRING_SLOT_1 + 2 * i + 1)) for i in range(32)]

    # ── objetos: usar y abrir (M39) ─────────────────────────────────────────
    def usar_objeto(self, ranura: int, hechizo: int, guid_objeto: int, guid_objetivo=None,
                     espera: float = 5.0):
        """CMSG_USE_ITEM (SpellHandler.cpp HandleUseItemOpcode): clic derecho sobre un objeto
        de la mochila (`ranura` 23..38, siempre en INVENTORY_SLOT_BAG_0=255). `guid_objetivo`
        es opcional: el GUID de OTRO objeto propio en la mochila (p. ej. un cajón cerrado) al
        que apunta el hechizo del objeto usado (TARGET_FLAG_ITEM) — así se usa una ganzúa sobre
        un cajón sin necesitar un gameobject en el mundo. `Player::CanUseItem` (el hueco de
        `RequiredSkill`) se comprueba ANTES de leer el objetivo, así que este método basta para
        confirmar si el objeto exige una habilidad, aunque no se dé `guid_objetivo`.
        Devuelve `None` si no llega SMSG_INVENTORY_CHANGE_FAILURE en `espera` segundos (uso
        aceptado; el hechizo puede seguir fallando después por rango u objetivo — eso no lo
        cubre este método), o el código `InventoryResult` (p. ej. 8 =
        EQUIP_ERR_NO_REQUIRED_PROFICIENCY, 36 = EQUIP_ERR_ITEM_LOCKED) si el servidor lo rechaza."""
        self.enviar(CMSG_USE_ITEM, _paquete_usar_objeto(ranura, hechizo, guid_objeto, guid_objetivo))
        try:
            _, datos = self.esperar({SMSG_INVENTORY_CHANGE_FAILURE}, timeout=espera)
        except TimeoutError:
            return None
        return Lector(datos).u8()

    def abrir_objeto(self, ranura: int, espera: float = 5.0) -> dict:
        """CMSG_OPEN_ITEM (SpellHandler.cpp HandleOpenItemOpcode): clic derecho para abrir
        (recoger botín de) un objeto-contenedor de la mochila, p. ej. un cajón ya desbloqueado.
        `Player::SendLoot`/`SendLootError` mandan el resultado, bueno o malo, con el MISMO
        opcode SMSG_LOOT_RESPONSE — ver `_leer_respuesta_loot`. Devuelve {'abierto',
        'error_equipo', 'error_loot', 'guid'}; `error_equipo` es un InventoryResult
        (36 = EQUIP_ERR_ITEM_LOCKED si sigue cerrado)."""
        self.enviar(CMSG_OPEN_ITEM, Escritor().u8(255).u8(ranura).valor())
        op, datos = self.esperar({SMSG_INVENTORY_CHANGE_FAILURE, SMSG_LOOT_RESPONSE}, timeout=espera)
        if op == SMSG_INVENTORY_CHANGE_FAILURE:
            return {"abierto": False, "error_equipo": Lector(datos).u8(), "error_loot": None, "guid": None}
        r = _leer_respuesta_loot(datos)
        r["error_equipo"] = None
        return r

    def liberar_loot(self, guid: int):
        """CMSG_LOOT_RELEASE: cierra la ventana de botín abierta por `abrir_objeto`."""
        self.enviar(CMSG_LOOT_RELEASE, struct.pack("<Q", guid))

    def abrir_criatura(self, guid: int, espera: float = 5.0) -> dict:
        """CMSG_LOOT (LootHandler.cpp HandleLootOpcode): clic derecho para abrir el cadáver
        de una criatura o vehículo (`guid.IsCreatureOrVehicle()`; el core lo ignora en
        silencio para cualquier otro tipo de GUID). Es lo que arranca de verdad las
        tiradas de grupo (`Group::GroupLoot`/`NeedBeforeGreed`) para un cadáver: la muerte
        por sí sola NO las crea, hace falta que alguien abra el botín. Mismo formato de
        respuesta que `abrir_objeto` (ver `_leer_respuesta_loot`)."""
        self.enviar(CMSG_LOOT, struct.pack("<Q", guid))
        op, datos = self.esperar({SMSG_INVENTORY_CHANGE_FAILURE, SMSG_LOOT_RESPONSE}, timeout=espera)
        if op == SMSG_INVENTORY_CHANGE_FAILURE:
            return {"abierto": False, "error_equipo": Lector(datos).u8(), "error_loot": None, "guid": None}
        r = _leer_respuesta_loot(datos)
        r["error_equipo"] = None
        return r

    # ── tiradas de botín en grupo (M40) ─────────────────────────────────────
    def establecer_metodo_loot(self, metodo: int, umbral: int = 2, maestro_guid: int = 0):
        """CMSG_LOOT_METHOD (GroupHandler.cpp HandleLootMethodOpcode): sólo lo aplica el
        LÍDER de un grupo que NO sea de LFG (`group->isLFGGroup(true)` lo descarta en
        silencio, sin ningún paquete de vuelta) — un grupo de `.grupo <n>` (mod-party-here)
        no es de LFG, así que sirve. `umbral` es una ItemQualities entre Uncommon (2, verde,
        el valor por defecto del cliente) y Artifact (6)."""
        self.enviar(CMSG_LOOT_METHOD, _paquete_metodo_loot(metodo, umbral, maestro_guid))

    def votar_loot(self, guid_objeto: int, ranura: int, tipo: int):
        """CMSG_LOOT_ROLL (GroupHandler.cpp HandleLootRoll): tipo ROLL_PASS=0, ROLL_NEED=1,
        ROLL_GREED=2, ROLL_DISENCHANT=3 (el core lo cuenta igual que False si el objeto o el
        grupo no lo permiten). `ranura` es `itemSlot` de la propia SMSG_LOOT_START_ROLL, no
        una ranura de inventario."""
        self.enviar(CMSG_LOOT_ROLL, _paquete_votar_loot(guid_objeto, ranura, tipo))

    def posicion_actual(self):
        """Posición propia: la del spline del servidor si hay uno en curso."""
        t = self.trayectorias.get(self.guid)
        if t is not None:
            x, y, z = t.posicion()
            if t.terminada():
                self.trayectorias.pop(self.guid, None)
                self.pos = {"x": x, "y": y, "z": z, "o": (self.pos or {}).get("o", 0)}
            return {"x": x, "y": y, "z": z}
        return self.pos

    def posicion_de(self, guid):
        t = self.trayectorias.get(guid)
        if t is not None:
            x, y, z = t.posicion()
            return {"x": x, "y": y, "z": z}
        return self.objetos.get(guid, {}).get("pos") or None

    # ── chat y comandos GM ──────────────────────────────────────────────────
    def decir(self, texto: str, tipo: int = CHAT_MSG_SAY, destino: str = None, idioma: int = None):
        e = Escritor().u32(tipo).u32((self.idioma_chat if idioma is None else idioma) & 0xFFFFFFFF)
        if tipo == CHAT_MSG_WHISPER:
            e.cadena(destino)
        self.enviar(CMSG_MESSAGECHAT, e.cadena(texto).valor())

    def comando(self, texto: str, espera: float = 2.0) -> list:
        """Envía `.texto` por /decir y devuelve los mensajes de sistema que lleguen."""
        if texto.lstrip(".").split(" ")[0] in ("go", "tele", "summon", "appear", "groupsummon", "recall"):
            self._tp_provocado_hasta = time.time() + max(espera, 3) + 5
        antes = len(self.mensajes)
        self.decir("." + texto.lstrip("."))
        self.bombear(espera)
        return [t for _, t in self.mensajes[antes:]]

    def comando_hasta(self, texto: str, patron: str, timeout: float = 8.0) -> list:
        """Como `comando`, pero espera a que llegue un mensaje que case con `patron`."""
        antes = len(self.mensajes)
        self.decir("." + texto.lstrip("."))
        limite = time.time() + timeout
        rx = re.compile(patron)
        while time.time() < limite:
            self.bombear(0.3)
            if any(rx.search(t) for _, t in self.mensajes[antes:]):
                break
        return [t for _, t in self.mensajes[antes:]]

    def susurrar(self, nombre: str, texto: str):
        self.decir(texto, CHAT_MSG_WHISPER, destino=nombre)

    def gps(self, espera: float = 3.0):
        """`.gps` del propio personaje (cs_misc.cpp HandleGPSCommand, acore_string 101 y 175).
        Devuelve mapa, x/y/z, instancia, suelo (GroundZ), piso (FloorZ) y líquido, o None.
        No sirve para otros: el core usa la unidad seleccionada antes que el nombre
        dado, y sin selección es el propio jugador; `.gps <nombre>` devuelve lo nuestro."""
        lineas = self.comando_hasta("gps", r"(?:GroundZ|Suelo Z)", espera)
        return leer_gps(" ".join(lineas))

    def seleccionar(self, guid: int):
        """CMSG_SET_SELECTION: lo que hace el clic sobre un retrato. Muchos comandos GM
        (`.aura`, `.gps`, `.revive`) actúan sobre la selección. No usarlo mientras la IA
        de playerbots lleva al personaje: su selección es la del combate."""
        self.enviar(CMSG_SET_SELECTION, struct.pack("<Q", guid))

    # ── combate real (M29/M31/M32) ──────────────────────────────────────────
    def atacar(self, guid: int):
        """CMSG_ATTACKSWING: como el botón de ataque; pone Unit::IsInCombat() a
        true tanto en el propio personaje como en `guid` (Unit::Attack)."""
        self.seleccionar(guid)
        self.enviar(CMSG_ATTACKSWING, struct.pack("<Q", guid))

    def dejar_de_atacar(self):
        """CMSG_ATTACKSTOP: sin cuerpo."""
        self.enviar(CMSG_ATTACKSTOP, b"")

    def liberar_espiritu(self):
        """CMSG_REPOP_REQUEST (HandleRepopRequestOpcode: recv_data.read_skip<uint8>()):
        sólo tiene efecto si el personaje está muerto de verdad (no vivo, no ya
        fantasma). Equivale al botón «Liberar espíritu»: BuildPlayerRepop +
        RepopAtGraveyard, así que puede teletransportar al cementerio más cercano."""
        self.enviar(CMSG_REPOP_REQUEST, b"\x00")

    # ── campo de batalla / arena (M33) ──────────────────────────────────────
    def arena_unirse(self, guid_maestro_batalla: int, ranura: int, en_grupo: bool = False,
                     clasificada: bool = False, auto_aceptar: bool = True):
        """CMSG_BATTLEMASTER_JOIN_ARENA (HandleBattlemasterJoinArena): `ranura` 0=2c2,
        1=3c3, 2=5c5. `guid_maestro_batalla` debe ser una criatura con
        UNIT_NPC_FLAG_BATTLEMASTER en el mapa actual (Creature::IsBattleMaster())."""
        self.bg.update(auto_puerto=auto_aceptar, status=None)
        self.enviar(CMSG_BATTLEMASTER_JOIN_ARENA,
                    struct.pack("<QBBB", guid_maestro_batalla, ranura, 1 if en_grupo else 0,
                                1 if clasificada else 0))

    def campo_unirse(self, guid_maestro_batalla: int, bg_tipo: int, en_grupo: bool = False,
                     auto_aceptar: bool = True):
        """CMSG_BATTLEMASTER_JOIN (HandleBattlemasterJoinOpcode): guid, BattlemasterList.dbc id
        (1 AV, 2 WSG, 3 AB...), instancia (0 = la cola normal) y «unirse como grupo»."""
        self.bg.update(auto_puerto=auto_aceptar, status=None)
        self.enviar(CMSG_BATTLEMASTER_JOIN,
                    struct.pack("<QIIB", guid_maestro_batalla, bg_tipo, 0, 1 if en_grupo else 0))

    # ── misiones ────────────────────────────────────────────────────────────
    def misiones(self) -> list:
        """IDs del diario: PLAYER_QUEST_LOG_1_1 + ranura * 5 (Player::GetQuestSlotQuestId), 25 ranuras."""
        return [self.valor_propio(upd.PLAYER_QUEST_LOG_1_1 + 5 * i) for i in range(25)]

    def aceptar_mision(self, guid_npc: int, mision: int):
        """CMSG_QUESTGIVER_ACCEPT_QUEST (QuestHandler.cpp): guid, id y un u32 que el
        cliente manda a 0. El core exige que el NPC la dé y esté a distancia de interacción."""
        self.enviar(CMSG_QUESTGIVER_ACCEPT_QUEST, struct.pack("<QII", guid_npc, mision, 0))

    def abandonar_mision(self, mision: int) -> bool:
        """CMSG_QUESTLOG_REMOVE_QUEST con la ranura del diario, como «Abandonar»: dispara
        OnPlayerQuestAbandon (un `.quest remove` de GM no lo hace)."""
        diario = self.misiones()
        if mision not in diario:
            return False
        self.enviar(CMSG_QUESTLOG_REMOVE_QUEST, struct.pack("<B", diario.index(mision)))
        return True

    def pedir_estadisticas(self, guid: int):
        """CMSG_REQUEST_PARTY_MEMBER_STATS, como el marco de grupo de Wow.exe: el core
        responde SMSG_PARTY_MEMBER_STATS_FULL (vida, zona, x/y) aunque el miembro esté
        a la vista (sin pedirlo, sólo manda estadísticas de los que están fuera de rango)."""
        self.enviar(CMSG_REQUEST_PARTY_MEMBER_STATS, struct.pack("<Q", guid))

    # ── area triggers ───────────────────────────────────────────────────────
    def armar_areatriggers(self, vigilante):
        self.vigilante = vigilante
        vigilante.cambiar_mapa(self.mapa)

    def enviar_areatrigger(self, ident: int, automatico: bool = False):
        self.enviar(CMSG_AREATRIGGER, struct.pack("<I", ident))
        self.emitir("areatrigger", id=ident, mapa=self.mapa, automatico=automatico)

    # ── buscador ────────────────────────────────────────────────────────────
    def lfg_unirse(self, roles: int, mazmorras, auto_aceptar: bool = True):
        self.lfg.update(auto_aceptar=auto_aceptar, roles=roles, propuestas=[], union=None, aceptadas=set())
        self.enviar(CMSG_LFG_JOIN, grp.lfg_unirse(roles, mazmorras))

    def dejar_grupo(self):
        """CMSG_GROUP_DISBAND: el cliente lo usa para «Abandonar grupo»."""
        self.enviar(CMSG_GROUP_DISBAND, b"")

    def invitar_grupo(self, nombre: str):
        """CMSG_GROUP_INVITE: nombre terminado en NUL y campo uint32 legado."""
        self.enviar(CMSG_GROUP_INVITE, nombre.encode("utf-8") + b"\0" + struct.pack("<I", 0))

    def aceptar_grupo(self):
        """CMSG_GROUP_ACCEPT: acepta la invitación de grupo pendiente."""
        self.enviar(CMSG_GROUP_ACCEPT, struct.pack("<I", 0))

    def lfg_salir(self):
        self.lfg["auto_aceptar"] = False
        self.enviar(CMSG_LFG_LEAVE, b"")

    # ── criaturas cercanas e instructores ───────────────────────────────────
    def criaturas(self) -> list:
        res = []
        for g, o in self.objetos.items():
            if o["typeid"] == upd.TYPEID_UNIT:
                v = o["valores"]
                res.append({"guid": g, "entrada": v.get(upd.OBJECT_FIELD_ENTRY, 0),
                            "npcflags": v.get(upd.UNIT_NPC_FLAGS, 0), "pos": o["pos"]})
        return res

    def distancia(self, pos) -> float:
        propia = self.posicion_actual()
        if not propia or not pos:
            return float("inf")
        return sum((propia[k] - pos[k]) ** 2 for k in ("x", "y", "z")) ** 0.5

    def ir_a(self, pos, espera: float = 3, mapa: int = None):
        self.comando("go xyz %.3f %.3f %.3f %d" % (pos["x"], pos["y"], pos["z"],
                                                    self.mapa if mapa is None else mapa), espera)

    def lista_instructor(self, guid, timeout: float = 5):
        """Abre el instructor como Wow.exe: CMSG_GOSSIP_HELLO y, si llega un menú,
        la opción con icono de instructor (los de clase sólo llevan
        UNIT_NPC_FLAG_TRAINER_CLASS, y CMSG_TRAINER_LIST exige UNIT_NPC_FLAG_TRAINER).
        None si el NPC no enseña a esta clase."""
        self.enviar(CMSG_GOSSIP_HELLO, struct.pack("<Q", guid))
        try:
            op, datos = self.esperar({SMSG_TRAINER_LIST, SMSG_GOSSIP_MESSAGE}, timeout=timeout)
            if op == SMSG_GOSSIP_MESSAGE:
                menu, opciones = self._leer_menu(datos)
                elegida = next((o for o in opciones if o["icono"] == GOSSIP_ICON_TRAINER), None)
                if elegida is None:
                    return None
                self.enviar(CMSG_GOSSIP_SELECT_OPTION, struct.pack("<QII", guid, menu, elegida["id"]))
                op, datos = self.esperar({SMSG_TRAINER_LIST}, timeout=timeout)
        except TimeoutError:
            return None
        return self._leer_lista(datos)

    def conversar(self, guid, timeout: float = 5):
        """CMSG_GOSSIP_HELLO. Devuelve (menu_id, opciones) o None si no hay menú."""
        self.enviar(CMSG_GOSSIP_HELLO, struct.pack("<Q", guid))
        try:
            _, datos = self.esperar({SMSG_GOSSIP_MESSAGE}, timeout=timeout)
        except TimeoutError:
            return None
        return self._leer_menu(datos)

    def elegir(self, guid, menu, opcion, timeout: float = 4):
        """CMSG_GOSSIP_SELECT_OPTION. Devuelve el siguiente menú o None si se cierra."""
        self.enviar(CMSG_GOSSIP_SELECT_OPTION, struct.pack("<QII", guid, menu, opcion))
        try:
            _, datos = self.esperar({SMSG_GOSSIP_MESSAGE}, timeout=timeout)
        except TimeoutError:
            return None
        return self._leer_menu(datos)

    @staticmethod
    def _leer_menu(datos):
        L = Lector(datos)
        L.u64()
        menu = L.u32()
        L.u32()
        opciones = []
        for _ in range(L.u32()):
            o = {"id": L.u32(), "icono": L.u8(), "codificada": L.u8()}
            o["dinero"] = L.u32()           # coste del cuadro de confirmación
            o["texto"] = L.cadena()
            o["confirmacion"] = L.cadena()  # texto del cuadro ("" si no lo hay)
            opciones.append(o)
        return menu, opciones

    @staticmethod
    def _leer_lista(datos):
        L = Lector(datos)
        L.u64()
        tipo = L.i32()
        hechizos = []
        for _ in range(L.i32()):
            h = {"hechizo": L.i32(), "estado": L.u8(), "coste": L.i32()}
            L.i32(); L.i32()
            h["nivel"] = L.u8()
            h["habilidad"], h["rango"] = L.i32(), L.i32()
            h["requiere"] = [L.i32(), L.i32(), L.i32()]
            hechizos.append(h)
        return {"tipo": tipo, "hechizos": hechizos, "saludo": L.cadena()}

    def comprar(self, guid, hechizo, timeout: float = 5):
        """CMSG_TRAINER_BUY_SPELL. Devuelve (True, None) o (False, motivo)."""
        self.enviar(CMSG_TRAINER_BUY_SPELL, struct.pack("<QI", guid, hechizo))
        op, datos = self.esperar({SMSG_TRAINER_BUY_SUCCEEDED, SMSG_TRAINER_BUY_FAILED}, timeout=timeout)
        if op == SMSG_TRAINER_BUY_SUCCEEDED:
            self.bombear(1)
            return True, None
        return False, struct.unpack("<i", datos[12:16])[0]

    # ── carta de hermandad (M-min-petition-signs) ───────────────────────────
    def comprar_carta_hermandad(self, guid_npc, nombre_hermandad, espera: float = 3.0):
        """CMSG_PETITION_BUY (PetitionsHandler.cpp HandlePetitionBuyOpcode) contra un NPC con
        UNIT_NPC_FLAG_PETITIONER|TABARDDESIGNER (`IsTabardDesigner`). El formato es el mismo
        paquete "feo" que la carta de banda de arena: guid del NPC, un hueco de 12 bytes, el
        nombre, otra cadena vacía, un bloque de campos a 0 (colores/emblema que el cliente
        real manda vacíos hasta que se diseña el tabardo) y, al final, el índice de cliente
        (1 = hermandad; 2/3 son las bandas de arena 2c2/3c3) y un último hueco de 4 bytes.
        No hay ack directo: se confirma comparando la mochila antes/después (mismo patrón que
        el diff de GUID por `entrada` que ya usan otros casos) y se devuelve el GUID completo
        de la carta, o None si no llegó ningún objeto nuevo (oro insuficiente, ya tienes
        hermandad, nombre inválido/repetido...)."""
        antes = set(self.objetos_bolsas())
        e = (Escritor().u64(guid_npc).u32(0).u64(0).cadena(nombre_hermandad).cadena("")
             .u32(0).u32(0).u32(0).u32(0).u32(0).u32(0).u32(0)
             .u16(0).u32(0).u32(0).u32(0))
        for _ in range(10):
            e.cadena("")
        e.u32(1).u32(0)                                # clientIndex=1 (hermandad), hueco final
        self.enviar(CMSG_PETITION_BUY, e.valor())
        self.bombear(espera)
        nuevos = [(lo, hi) for lo, hi in set(self.objetos_bolsas()) - antes if (lo, hi) != (0, 0)]
        if not nuevos:
            return None
        lo, hi = nuevos[0]
        return lo | (hi << 32)

    def entregar_carta(self, guid_carta, timeout: float = 5.0) -> int:
        """CMSG_TURN_IN_PETITION: entrega la carta ya comprada, con las firmas que tenga (aquí
        siempre 0). El servidor decide solo con `MinPetitionSigns`; devuelve el código de
        SMSG_TURN_IN_PETITION_RESULTS (PETITION_TURN_OK=0, _NEED_MORE_SIGNATURES=4...)."""
        self.enviar(CMSG_TURN_IN_PETITION, Escritor().u64(guid_carta).valor())
        _, datos = self.esperar({SMSG_TURN_IN_PETITION_RESULTS}, timeout=timeout)
        return struct.unpack("<I", datos[:4])[0]


def leer_gps(texto: str):
    """Interpreta la salida de `.gps` en inglés o en español; None si no está."""
    t = re.sub(r"\|c[0-9a-fA-F]{8}|\|r", "", texto)
    m = re.search(r"(?:Map|Mapa): (\d+) .*?X: %s Y: %s Z: %s" % (_NUM, _NUM, _NUM), t, re.S)
    if not m:
        return None
    res = {"mapa": int(m.group(1)), "x": float(m.group(2)), "y": float(m.group(3)), "z": float(m.group(4))}
    inst = re.search(r"InstanceID: (\d+)", t)
    res["instancia"] = int(inst.group(1)) if inst else None
    suelos = re.search(r"(?:GroundZ|Suelo Z): %s (?:FloorZ|Suelo Z): %s" % (_NUM, _NUM), t)
    if suelos:
        res["suelo"], res["piso"] = float(suelos.group(1)), float(suelos.group(2))
    liq = re.search(r"(?:Liquid level|Nivel de líquido): %s, (?:ground|suelo): %s, .*?(?:status|estado): (\d+)"
                    % (_NUM, _NUM), t)
    if liq:
        res["liquido"] = {"nivel": float(liq.group(1)), "fondo": float(liq.group(2)), "estado": int(liq.group(3))}
    res["interior"] = "indoors" in t or "interior" in t
    return res
