// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-queue-bots — cuando te pones en una cola, mete en ella los bots que faltan.
 *
 * EL PROBLEMA
 * En un servidor de una sola persona ninguna cola llega a saltar. mod-playerbots
 * trae su propio auto-apuntado, pero va a ciegas: rellena una batalla por cada
 * tramo de nivel de cada campo de batalla —veinticuatro a la vez— repartiendo
 * entre todas ellas los bots conectados, así que no llena ninguna. Y de la cola
 * en la que estás TÚ no sabe nada. Con las mazmorras pasa lo mismo, y la cola
 * 1c1 de mod-1v1-arena es una cola inventada que playerbots ni conoce.
 *
 * LA SOLUCIÓN
 * Mirar en qué cola estás y meter en ESA los bots que le faltan, del tramo de
 * nivel que corresponda:
 *
 *   Refriega 1c1     un bot en la cola de mod-1v1-arena
 *   Campo de batalla los dos bandos hasta el mínimo de la plantilla
 *   Arena            tantos bots como pida el tipo de arena
 *   Mazmorra (5)     los roles que falten: tanque, sanador, daño
 *   Banda            te forma el grupo metiendo bots hasta el tamaño del raid
 *
 * Así 150 bots dan para cualquier cola a cualquier nivel, en vez de no dar para
 * ninguna.
 *
 * POR QUÉ ES UN MÓDULO APARTE Y NO UN PARCHE
 * No toca ni una línea de mod-1v1-arena ni de mod-playerbots. Del primero copia
 * dos constantes (las que registra en el núcleo). Del segundo usa tres llamadas,
 * y entre guardas de compilación: encender un bot dormido, mandarlo a dormir y
 * reconstruirle las estrategias al entrar a una partida. Todo lo demás es API
 * del núcleo. Si playerbots no está, esas tres no se compilan y el resto sigue
 * funcionando; si cambian, falla la compilación con un error claro.
 */

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "BotClaims.h"
#include "BotEligibility.h"
#include "BotGear.h"
#include "BotOperations.h"
#include "BotPopulationCoordinator.h"
#include "BotWake.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Containers.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DBCStructure.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFG.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "QueueBotsPolicy.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SlowTick.h"
#include "SpellDefines.h"
#include "TimeMs.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// La ÚNICA dependencia de código con mod-playerbots, y va entre guardas: si el
// módulo no está, esto no se compila y todo lo demás sigue funcionando.
//
// Hace falta porque un bot que entra a la partida por nuestra puerta de atrás no
// recibe el comportamiento de combate. playerbots se lo da al aceptar él la
// invitación (BattleGroundJoinAction.cpp), y quien lo arreglaría más tarde
// —BGStrategyCheckAction— vive DENTRO de la estrategia de campo de batalla que
// ese bot no tiene: pescadilla que se muerde la cola. ResetStrategies() estando
// ya dentro de la arena es exactamente lo que hace su propio código, y
// AiFactory.cpp:481 añade ahí la estrategia "arena".
#if defined(__has_include)
#  if __has_include("Playerbots.h") && __has_include("PlayerbotAI.h")
#    include "PlayerbotAI.h"
#    include "Playerbots.h"
#    include "RandomPlayerbotMgr.h"
#    define QUEUE_BOTS_WITH_PLAYERBOTS 1
#  endif
#endif

using namespace lfg;

namespace
{
    // Las dos constantes que mod-1v1-arena registra en el núcleo al arrancar.
    // Si algún día las cambia, hay que cambiarlas aquí: es lo único que este
    // módulo comparte con él.
    constexpr uint8 ARENA_TYPE_1V1 = 1;
    constexpr BattlegroundQueueTypeId BG_QUEUE_1V1 =
        static_cast<BattlegroundQueueTypeId>(static_cast<int>(BATTLEGROUND_QUEUE_5v5) + 1);

    struct Config
    {
        bool   enabled        = true;
        bool   arena1v1       = true;
        bool   battlegrounds  = true;
        bool   arenas         = true;
        bool   dungeons       = true;
        bool   raidBrowser    = true;
        bool   announce       = true;
        bool   forceAccept    = true;
        uint32 delaySeconds   = 0;      // margen por si aparece un rival humano
        uint32 forceAfterSecs = 5;
        uint32 scanIntervalMs = 2000;
        uint32 wakeCooldownSecs = 15;   // margen entre tandas de WakeBots
        uint32 maxRaidBots    = 39;
        uint32 raidSize       = 0;      // 0 = deducirlo de la dificultad de banda
        uint32 raidTanks      = 0;      // 0 = según el tamaño de la banda
        uint32 raidHealers    = 0;      // 0 = según el tamaño de la banda
        uint32 raidLevelBelow = 5;      // margen de nivel admitido en una banda (M48)
        bool   wakeBots       = true;   // despertar bots de la reserva si faltan
        uint32 wakeMax        = 40;     // tope de bots a despertar por cola
        uint32 arena1v1Level  = 80;
        uint32 raidSummonSecs = 8;      // segundos hasta traer al bot a tu lado (0 = nunca)
        uint32 raidSummonGiveUpSecs = 60; // se deja de intentar traer al bot tras esto
        bool   dcAuto         = true;   // activar mod-dungeon-clear solo al entrar en la mazmorra
        uint32 dcDelaySecs    = 10;
        uint32 dcTries        = 3;      // intentos de ".dc on"
        uint32 dcReviveDelaySecs = 8;   // margen tras reponerse de una muerte antes de reintentar ".dc on"
        uint32 dungeonTanks   = 1;      // composición del grupo de mazmorra del buscador
        uint32 dungeonHealers = 1;
        uint32 dungeonDamage  = 3;
        bool   classVariety   = true;   // evitar repetir clase mientras haya alternativas
        bool   bgFillToFull   = false;  // llenar el BG a plantilla completa, no al mínimo
        bool   backfill       = true;   // reponer bots que se caen de un BG/arena a mitad
        uint32 backfillIntervalSecs = 20;
        uint32 backfillGiveUpSecs   = 45; // un repuesto que no entra en este tiempo se descarta
        BotGear::Settings gear;         // topar el equipo de los bots que entran en tu grupo
        bool   tokenTurnIn    = true;   // ".tokenturnin redeem" en tu nombre tras cada jefe
        uint32 tokenDelaySecs = 90;     // margen para que terminen las tiradas de botín
    };

    // Un jefe muerto por el jugador: tras el margen se canjean los tokens de
    // los bots del grupo (mod-token-turnin). Apuntado desde el hilo del mapa.
    struct BossKill
    {
        ObjectGuid human;
        uint64     dueMs = 0;
    };

    char const* const OWNER = "queue-bots";      // etiqueta en el registro de reservas

    // Un jugador dentro de una mazmorra al que hay que activarle mod-dungeon-clear.
    struct DcState
    {
        uint32 instanceId  = 0;
        uint64 enteredMs   = 0;
        uint64 nextTryMs   = 0;
        uint8  tries       = 0;
        bool   done        = false;
        bool   gearDone    = false;      // el equipo de los bots del grupo ya está topado
        // Reactivación tras una muerte (ver AutoDungeonClear): true una vez
        // confirmado que hay tanque bot y el jugador no es el tanque, o sea que
        // el auto ".dc on" aplica a este grupo. Sin esto la vigilancia de
        // "¿alguien muerto?" correría también para grupos donde nunca se
        // activó (p. ej. el jugador es el tanque), reintentando un ".dc on"
        // que mod-dungeon-clear rechazaría igualmente.
        bool   dcApplicable = false;
        // true mientras algún miembro del grupo presente en el mapa está
        // muerto; el flanco muerto->vivo es lo que rearma una tanda nueva de
        // intentos (ver el comentario en AutoDungeonClear sobre por qué no
        // hay forma de distinguir esto de un ".dc off" manual).
        bool   anyDead      = false;
        uint64 deadSinceMs  = 0;   // instante en que anyDead pasó a true
    };

    // Un bot recién metido en la banda, con el jugador al que tiene que llegar y
    // el momento en que se le trae. Se atiende desde OnUpdate del mundo.
    struct Summon
    {
        ObjectGuid human;
        uint64     dueMs    = 0;
        uint64     giveUpMs = 0;
    };

    Config cfg;

    // Copia atómica de los dos campos que lee un hook de HILO DE MAPA
    // (OnPlayerCreatureKill), T7. ".reload config" corre en el hilo del mundo
    // con los mapas parados, así que tampoco compite con las lecturas directas
    // de `cfg` en ese hook (modules/README.md, regla 5; M05, 24/09/2026).
    std::atomic<bool> g_enabledHot{true};
    std::atomic<bool> g_tokenTurnInHot{true};

    // Latch de T8: la rama de ProcessRequests con !enabled hacía
    // BotClaims::ReleaseAll(OWNER) en cada tick. Se hace una sola vez por
    // apagado; se rearma en OnAfterConfigLoad.
    std::atomic<bool> g_cleanedWhileOff{false};

    // con el módulo apagado, DoUpdate seguía con AutoDungeonClear,
    // TokenTurnIn y las convocatorias, y AutoDungeonClear volvía a crear
    // estado, convocar y encolar equipo justo después de la limpieza. Latch
    // propio de esa segunda limpieza (canjes y equipo propio pendientes), se
    // rearma igual que el de T8.
    bool g_auxCleanedWhileOff = false;

    enum FillKind : uint8
    {
        FILL_NONE = 0,
        FILL_ARENA_1V1,
        FILL_BATTLEGROUND,
        FILL_ARENA,
        FILL_DUNGEON,
        FILL_RAID
    };

    struct Fill
    {
        struct Bot
        {
            ObjectGuid guid;
            TeamId intendedTeam = TEAM_NEUTRAL;
            bool   entered  = false;   // ya está dentro del BG/arena (backfill lo cuenta)
            uint64 queuedMs = 0;       // != 0: es un repuesto encolado; para el give-up
            uint8  role     = 0;       // mazmorra: rol con el que entró al buscador (M15)
        };

        uint8                    kind = FILL_NONE;
        BattlegroundQueueTypeId  queueType = BATTLEGROUND_QUEUE_NONE;
        uint32                   needed = 0;      // cuántos hacen falta en total
        std::vector<Bot>         bots;            // GUID y bando que debe completar
        bool                     rolesWarned = false;   // mazmorra: ya se avisó de un reparto imposible
    };

    enum PlayerRequestKind : uint8
    {
        REQ_HUMAN_UPDATE,
        REQ_BOT_UPDATE,
        REQ_LOGOUT
    };

    // Productores: PlayerScript (hilos de mapa). Consumidor: exclusivamente
    // mod_queue_bots_world::OnUpdate. No se conserva ningun Player*.
    std::mutex g_playerRequestsLock;
    std::unordered_map<ObjectGuid, uint8> g_playerRequests;

    void PushPlayerRequest(ObjectGuid guid, uint8 kind)
    {
        std::lock_guard<std::mutex> lock(g_playerRequestsLock);
        // Logout domina cualquier update que siga pendiente para ese GUID.
        auto [it, inserted] = g_playerRequests.try_emplace(guid, kind);
        if (!inserted && (kind == REQ_LOGOUT || it->second != REQ_LOGOUT))
            it->second = kind;
    }

    // ─── Comandos .queuebots (hilo de despacho -> mod_queue_bots_world::OnUpdate) ─
    // El handler sólo encola: g_fills y compañía son propiedad exclusiva del
    // hilo del mundo (regla 5 del README, igual que .grupo y .hermandad).
    enum QbCmdKind : uint8 { QB_CMD_STATUS = 0, QB_CMD_TOPUP, QB_CMD_LEAVE };
    struct QbCmd { ObjectGuid human; uint8 kind; };
    std::mutex          g_qbCmdLock;
    std::vector<QbCmd>  g_qbCmd;

    void PushQbCmd(ObjectGuid guid, uint8 kind)
    {
        std::lock_guard<std::mutex> lock(g_qbCmdLock);
        if (g_qbCmd.size() < 128)               // un jugador tecleando no puede desbordarlo
            g_qbCmd.push_back({ guid, kind });
    }

    // Propietario exclusivo de todo este bloque: mod_queue_bots_world::OnUpdate
    // (hilo del mundo). Ningun hook de jugador/mapa puede leerlo ni escribirlo.
    std::unordered_map<ObjectGuid, uint64>     g_waitingSince;   // humano -> ms en que se le vio en cola
    std::unordered_map<ObjectGuid, Fill>       g_fills;          // humano -> lo que le hemos metido
    std::unordered_map<ObjectGuid, ObjectGuid> g_humanOf;        // bot    -> humano
    std::unordered_map<ObjectGuid, uint64>     g_forceAt;        // bot    -> ms para entrarle a la fuerza
    std::unordered_map<ObjectGuid, uint64>     g_raidNextAdd;    // humano -> ms para meter al siguiente en la banda
    std::unordered_map<ObjectGuid, Summon>     g_raidSummons;    // bot    -> a quién y cuándo se le trae
    std::unordered_map<ObjectGuid, uint64>     g_backfillAt;     // humano -> ms de la próxima pasada de backfill del BG
    std::unordered_map<ObjectGuid, DcState>    g_dcState;        // humano -> mazmorra en la que está
    std::mutex                                 g_bossLock;       // los jefes se apuntan desde el hilo del mapa
    std::vector<BossKill>                      g_bossKills;
    uint64 g_nextDcScan = 0;
    // Sellos por jugador: con dos humanos encolando a la vez (grupo de
    // mod-party-here, o GM + jugador) un throttle global hacía que el rellenado
    // de uno retrasase el del otro (B6).
    std::unordered_map<ObjectGuid, uint64>     g_nextScanOf;     // humano -> ms de la próxima pasada de rellenado
    // Encender bots cuesta un login: no en cada pasada. Y el freno es por
    // (tramo, facción): en un campo de batalla se despiertan aliados y rivales
    // en la misma pasada y un cooldown global dejaba a la segunda facción sin
    // bots 15 s (B7). Clave: (nivelMínimo << 8) | facción.
    std::unordered_map<uint64, uint64>         g_nextWakeOf;

    // ArenaNeeds/MissingArenaPlayers y RaidBudget/ComputeRaidBudget viven en
    // QueueBotsPolicy.h (16/09/2026): son políticas puras
    // de selección/presupuesto, probadas con static_assert sin compilar el
    // worldserver.
    using QueueBotsPolicy::ArenaNeeds;
    using QueueBotsPolicy::MissingArenaPlayers;

    // ¿Es un bot aleatorio de verdad, y no el alt/selfbot de un jugador?
    // Este módulo reparte bots entre colas ajenas: meter en la cola de un
    // jugador el bot que otro jugador controla como su propio personaje no
    // es "rellenar", es tocar lo que no es de nadie más que repartir.
    bool IsRandomBot(Player* bot)
    {
#ifdef QUEUE_BOTS_WITH_PLAYERBOTS
        return sRandomPlayerbotMgr.IsRandomBot(bot);
#else
        (void)bot;
        return false;
#endif
    }

    // ─── Bots libres ────────────────────────────────────────────────────────
    // Un bot vale si no está haciendo nada de lo que sacarlo sería una grosería:
    // ni en grupo (es el bot de alguien), ni en mazmorra, ni en combate, ni
    // volando, ni muerto, ni ya metido en otra cola, ni a medio cargar o a
    // medio teletransportar, ni un alt/selfbot de un jugador.
    bool IsFreeBot(Player* bot)
    {
        if (!BotEligibility::IsAvailable(bot))
            return false;

        // Reservado por otro módulo (lo está trayendo, o lo tiene apartado).
        if (BotClaims::IsClaimedByOther(bot->GetGUID().GetCounter(), OWNER))
            return false;

        return IsRandomBot(bot);
    }

    // Índice de la rama de talentos con más puntos (0, 1 o 2), que es lo que
    // distingue a un guerrero de protección de uno de armas. El núcleo devuelve
    // el id de TalentTab.dbc; aquí se traduce a la posición dentro de la clase.
    uint8 SpecTab(Player* player)
    {
        uint32 const tabId = player->GetSpec();
        uint32 const* tabs = GetTalentTabPages(player->getClass());
        if (!tabs)
            return 0;

        for (uint8 i = 0; i < MAX_TALENT_TABS; ++i)
            if (tabs[i] == tabId)
                return i;

        return 0;
    }

    // Rol que le corresponde a un bot. Con playerbots delante se usa SU criterio
    // (IsTank/IsHeal por especialización), que contempla cosas que la simple
    // "rama con más puntos" no ve: la presencia del caballero de la muerte, la
    // forma de oso del druida... Con un bot al que le falten talentos por poner,
    // mirar sólo la rama lo clasifica mal, y así es como una banda acababa sin
    // un solo tanque.
    uint8 BotRole(Player* bot)
    {
#ifdef QUEUE_BOTS_WITH_PLAYERBOTS
        if (PlayerbotAI::IsTank(bot, true))
            return PLAYER_ROLE_TANK;

        if (PlayerbotAI::IsHeal(bot, true))
            return PLAYER_ROLE_HEALER;

        return PLAYER_ROLE_DAMAGE;
#else
        uint8 const spec = SpecTab(bot);

        switch (bot->getClass())
        {
            case CLASS_DRUID:
                // Sin playerbots no se distingue el feral tanque (oso) del feral
                // de daño (gato): comparten rama de talentos, y un oso saldría
                // clasificado como daño. Con playerbots delante —el caso de este
                // servidor— la rama de arriba lo resuelve con PlayerbotAI::IsTank
                // (B9); esta rama solo se compila sin mod-playerbots.
                return spec == 2 ? PLAYER_ROLE_HEALER : PLAYER_ROLE_DAMAGE;
            case CLASS_PALADIN:
                if (spec == 1) return PLAYER_ROLE_TANK;
                if (spec == 0) return PLAYER_ROLE_HEALER;
                return PLAYER_ROLE_DAMAGE;
            case CLASS_PRIEST:
                return spec != 2 ? PLAYER_ROLE_HEALER : PLAYER_ROLE_DAMAGE;
            case CLASS_SHAMAN:
                return spec == 2 ? PLAYER_ROLE_HEALER : PLAYER_ROLE_DAMAGE;
            case CLASS_WARRIOR:
                return spec == 2 ? PLAYER_ROLE_TANK : PLAYER_ROLE_DAMAGE;
            case CLASS_DEATH_KNIGHT:
                return spec == 0 ? PLAYER_ROLE_TANK : PLAYER_ROLE_DAMAGE;
            default:
                return PLAYER_ROLE_DAMAGE;
        }
#endif
    }

    // Recoge hasta 'wanted' bots libres del tramo de nivel pedido. Con
    // team = TEAM_NEUTRAL vale cualquier facción; con role = 0, cualquier rol.
    // Se barajan los candidatos para que no salgan siempre los mismos.
    std::vector<Player*> CollectBots(uint32 wanted, uint32 minLevel, uint32 maxLevel,
                                     TeamId team = TEAM_NEUTRAL, uint8 role = 0,
                                     std::set<uint8> const* avoidClasses = nullptr,
                                     std::set<ObjectGuid> const* avoidBots = nullptr)
    {
        std::vector<Player*> candidates;

        if (!wanted)
            return candidates;

        // El tope "suficientes para barajar" cuenta sólo a los de fuera de tu
        // hermandad y ya no corta el recorrido (M21): antes el corte llegaba
        // antes de preferir a la hermandad y un compañero conocido que venía
        // detrás en ObjectAccessor no se llegaba a ver.
        uint32 const nonHomeCap = wanted * 4 + 8;
        uint32 nonHome = 0;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* bot = pair.second;
            if (!bot)
                continue;
            bool const home = BotClaims::IsHomeGuildBot(bot->GetGUID().GetCounter());
            if (!home && nonHome >= nonHomeCap)
                continue;
            if (!IsFreeBot(bot))
                continue;

            if (bot->GetLevel() < minLevel || bot->GetLevel() > maxLevel)
                continue;

            if (team != TEAM_NEUTRAL && bot->GetTeamId() != team)
                continue;

            if (role && !(BotRole(bot) & role))
                continue;

            // Variedad: un grupo con tres cazadores es peor grupo. No es una
            // prohibición — quien llama vuelve a pedir sin esto si no llega.
            if (avoidClasses && avoidClasses->count(bot->getClass()))
                continue;

            if (avoidBots && avoidBots->count(bot->GetGUID()))
                continue;

            candidates.push_back(bot);
            if (!home)
                ++nonHome;
        }

        // Los de tu hermandad (mod-home-guild) van primero: el mismo tanque cada
        // semana. Se barajan los dos montones por separado y se pega uno tras
        // otro; los de fuera sólo entran si los tuyos no llegan.
        if (candidates.size() > wanted)
        {
            Acore::Containers::RandomShuffle(candidates);
            std::stable_partition(candidates.begin(), candidates.end(), [](Player* bot)
            {
                return BotClaims::IsHomeGuildBot(bot->GetGUID().GetCounter());
            });
            candidates.resize(wanted);
        }

        return candidates;
    }

    // Bots libres del tramo por rol, para decidir qué rol hace un jugador que
    // ha marcado varios (M15): mismo filtro que CollectBots, sin tope.
    struct RoleCounts
    {
        uint32 tanks = 0;
        uint32 healers = 0;
        uint32 damage = 0;
    };

    RoleCounts CountFreeByRole(uint32 minLevel, uint32 maxLevel)
    {
        RoleCounts counts;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* bot = pair.second;
            if (!IsFreeBot(bot) || bot->GetLevel() < minLevel || bot->GetLevel() > maxLevel)
                continue;

            uint8 const role = BotRole(bot);
            if (role == PLAYER_ROLE_TANK)        ++counts.tanks;
            else if (role == PLAYER_ROLE_HEALER) ++counts.healers;
            else                                 ++counts.damage;
        }
        return counts;
    }

    // ─── Despertar bots de la reserva ───────────────────────────────────────
    // El servidor tiene ~1500 personajes de bot pero sólo mantiene despiertos
    // unos cuantos, y el número sube MUY despacio (el módulo cambia su objetivo
    // cada 30-120 minutos). Resultado: te encolas y no hay bastantes bots de tu
    // nivel aunque existan de sobra, dormidos.
    //
    // Aquí se encienden los que hacen falta, elegidos por nivel y facción. Si el
    // mundo ya está en su tope de bots, primero se manda a dormir a otros tantos
    // que estén ociosos y fuera del tramo que nos interesa: nunca a uno que esté
    // en el grupo de alguien, en una partida o en una mazmorra.
    uint32 MakeRoom(uint32 wanted, uint32 keepMin, uint32 keepMax)
    {
        uint32 freed = 0;

#ifdef QUEUE_BOTS_WITH_PLAYERBOTS
        // LogoutPlayerBot es SINCRONO (LogoutPlayer -> RemoveFromWorld ->
        // ObjectAccessor::RemoveObject): llamarlo dentro del range-for sobre
        // GetPlayers() muta el contenedor que se esta iterando -> UB en ++it.
        // Se acumulan los GUID y se desconectan despues de cerrar el bucle.
        std::vector<ObjectGuid> victims;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            if (victims.size() >= wanted)
                break;

            Player* bot = pair.second;
            if (!IsFreeBot(bot))
                continue;

            if (bot->GetLevel() >= keepMin && bot->GetLevel() <= keepMax)
                continue;               // este nos sirve, no lo tocamos

            if (BotClaims::IsHomeGuildBot(bot->GetGUID().GetCounter()))
                continue;               // a los de tu hermandad no se les manda a dormir

            victims.push_back(bot->GetGUID());
        }

        for (ObjectGuid const& guid : victims)
        {
            sRandomPlayerbotMgr.LogoutPlayerBot(guid);
            ++freed;
        }
#endif

        return freed;
    }

    uint32 WakeBots(uint32 wanted, uint32 minLevel, uint32 maxLevel, TeamId team)
    {
        if (!wanted || !cfg.wakeBots)
            return 0;

#ifdef QUEUE_BOTS_WITH_PLAYERBOTS
        // La etapa de progresión activa (mod-world-bots) manda sobre cualquier
        // rango pedido aquí: si no cabe nada por debajo del tope, no se despierta.
        maxLevel = std::min(maxLevel, BotPopulationCoordinator::StageCap());
        if (minLevel > maxLevel)
            return 0;

        // Tardan unos segundos en entrar al mundo: sin este freno se pedirían
        // otros tantos en la pasada siguiente, y otros tantos en la de después.
        // El freno es por (tramo, facción) para no bloquear a la segunda facción
        // de un mismo campo de batalla durante 15 s (B7).
        uint64_t const now = TimeMs::NowMs();
        uint64 const wakeKey = (static_cast<uint64>(minLevel) << 8) | static_cast<uint64>(team);
        uint64& nextWake = g_nextWakeOf[wakeKey];
        if (now < nextWake)
            return 0;
        nextWake = now + TimeMs::SecsToMs(cfg.wakeCooldownSecs);

        wanted = std::min(wanted, cfg.wakeMax);

        // Si el mundo está lleno de bots, se hace sitio antes. El recuento sale
        // de la foto cacheada del coordinador, la misma que usarán los Reserve
        // de abajo: un solo escaneo de ObjectAccessor por pasada.
        uint32 const cap = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxRandomBots", 200);
        uint32 const online = BotPopulationCoordinator::OnlineCount(now);
        if (online + wanted > cap)
            MakeRoom(online + wanted - cap, minLevel, maxLevel);

        // medir el coste real de BotWake (hasta dos consultas
        // síncronas por selección) antes de decidir si hace falta precargar
        // candidatos de forma asíncrona.
        uint32 const tWake0 = getMSTime();
        auto const wakeCandidates = BotWake::SelectOfflineCandidates(
            wanted, minLevel, maxLevel, team == TEAM_ALLIANCE);
        SlowTick::WarnIfSlow("queue-bots", "BotWake::SelectOfflineCandidates", tWake0);

        uint32 woken = 0;
        for (uint32 const lowGuid : wakeCandidates)
        {
            if (!sRandomPlayerbotMgr.IsRandomBot(lowGuid))
                continue;               // no es un bot: ni tocarlo

            BotPopulationCoordinator::Reservation reservation = BotPopulationCoordinator::Reserve(
                lowGuid, OWNER,
                team == TEAM_ALLIANCE ? BotPopulationCoordinator::Faction::Alliance
                                      : BotPopulationCoordinator::Faction::Horde,
                minLevel, maxLevel);
            if (!reservation)
                continue;

            sRandomPlayerbotMgr.AddPlayerBot(ObjectGuid::Create<HighGuid::Player>(lowGuid), 0);
            if (++woken >= wanted)
                break;
        }

        if (woken)
            LOG_INFO("module", "[queue-bots] Despertando {} bots dormidos de nivel {}-{} ({}).",
                     woken, minLevel, maxLevel, team == TEAM_ALLIANCE ? "Alianza" : "Horda");

        return woken;
#else
        return 0;
#endif
    }

    // ─── Colas de batalla (campos de batalla y arenas) ──────────────────────
    bool QueueBotForBg(Player* bot, Battleground* bgTemplate, BattlegroundTypeId bgTypeId,
                       BattlegroundQueueTypeId queueType, uint8 arenaType)
    {
        PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), bot->GetLevel());
        if (!bracket)
            return false;

        if (!bot->HasFreeBattlegroundQueueId())
            return false;

        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueType);
        GroupQueueInfo* ginfo = queue.AddGroup(bot, nullptr, bgTypeId, bracket, arenaType,
                                               false /*isRated*/, false /*isPremade*/, 0, 0, 0, 0);
        if (!ginfo)
            return false;

        uint32 const slot = bot->AddBattlegroundQueueId(queueType);

        // El paquete de estado es lo que le dice a la IA del bot que está en
        // cola; sin él se quedaría apuntado sin enterarse.
        WorldPacket data;
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgTemplate, slot, STATUS_WAIT_QUEUE,
                                                        queue.GetAverageQueueWaitTime(ginfo), 0,
                                                        arenaType, TEAM_NEUTRAL, false);
        bot->GetSession()->SendPacket(&data);

        sBattlegroundMgr->ScheduleQueueUpdate(0, arenaType, queueType, bgTypeId, bracket->GetBracketId());
        return true;
    }

    void DequeueBotFromBg(Player* bot, BattlegroundQueueTypeId queueType)
    {
        if (!bot->InBattlegroundQueueForBattlegroundQueueType(queueType))
            return;

        sBattlegroundMgr->GetBattlegroundQueue(queueType).RemovePlayer(bot->GetGUID(), true);
        bot->RemoveBattlegroundQueueId(queueType);
    }

    // Al bot que acaba de entrar a la partida se le reconstruyen las estrategias:
    // es entonces cuando AiFactory ve que está dentro y le añade las de arena o
    // campo de batalla. Sin esto se queda plantado sin atacar a nadie.
    void GiveBattleBehaviour(Player* bot)
    {
#ifdef QUEUE_BOTS_WITH_PLAYERBOTS
        if (!bot->GetBattleground())
            return;

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        {
            botAI->ResetStrategies();
            LOG_INFO("module", "[queue-bots] {} recibe su comportamiento de combate al entrar.", bot->GetName());
        }
#endif
    }

    // Un bot metido en tu banda por el buscador tiene GetGroup() != nullptr pero
    // su IA no sabe que tiene amo: sigue a lo suyo. Es la misma transicion que
    // hace AcceptInvitationAction de playerbots al aceptar una invitacion (la
    // que ya replica mod-party-here::AdoptMaster).
    void AdoptRaidMaster(Player* bot, Player* human)
    {
#ifdef QUEUE_BOTS_WITH_PLAYERBOTS
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        {
            botAI->SetMaster(human);
            botAI->ResetStrategies();
            botAI->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
            botAI->Reset();
        }
#else
        (void)bot; (void)human;
#endif
    }

    // Simetrico de AdoptRaidMaster: al salir el bot de la banda (lo echaste, se
    // disolvio, logout) se le quita el amo y se le resetea la IA para que vuelva
    // a su actividad, antes de soltar la reserva.
    void ReleaseRaidMaster(ObjectGuid botGuid)
    {
#ifdef QUEUE_BOTS_WITH_PLAYERBOTS
        if (Player* bot = ObjectAccessor::FindPlayer(botGuid))
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                botAI->SetMaster(nullptr);
                if (!bot->InBattleground())
                    botAI->ResetStrategies();
            }
#else
        (void)botGuid;
#endif
    }

    void ForgetBot(ObjectGuid botGuid)
    {
        BotClaims::Release(botGuid.GetCounter(), OWNER);
        g_humanOf.erase(botGuid);
        g_forceAt.erase(botGuid);
    }

    void ForgetFill(ObjectGuid humanGuid)
    {
        auto fill = g_fills.find(humanGuid);
        if (fill != g_fills.end())
        {
            bool const raid = fill->second.kind == FILL_RAID;
            for (Fill::Bot const& bot : fill->second.bots)
            {
                if (raid)
                    ReleaseRaidMaster(bot.guid);
                ForgetBot(bot.guid);
            }
            g_fills.erase(fill);
        }
        g_waitingSince.erase(humanGuid);
    }

    void RemoveBotFromFill(ObjectGuid humanGuid, ObjectGuid botGuid)
    {
        auto fill = g_fills.find(humanGuid);
        if (fill != g_fills.end())
        {
            if (fill->second.kind == FILL_RAID)
                ReleaseRaidMaster(botGuid);
            fill->second.bots.erase(std::remove_if(fill->second.bots.begin(), fill->second.bots.end(),
                [botGuid](Fill::Bot const& bot) { return bot.guid == botGuid; }), fill->second.bots.end());
        }
        ForgetBot(botGuid);
    }

    // ─── Backfill de campo de batalla / arena ───────────────────────────────
    // Cuando un bot se cae de la partida a mitad (muere y sale, abandona, se
    // desconecta), se vuelve a encolar un repuesto del tramo y bando que falte.
    // El núcleo lo invita a la instancia en curso si tiene hueco
    // (GetFreeSlotsForTeam funciona en STATUS_IN_PROGRESS) y HandleBattleBot lo
    // mete con el mismo force-accept que a los originales. Un repuesto que no
    // entra en cfg.backfillGiveUpSecs se descarta para no dejarlo en cola.
    void BackfillBattle(Player* human)
    {
        auto it = g_fills.find(human->GetGUID());
        if (it == g_fills.end())
            return;
        Fill& fill = it->second;
        if (fill.kind != FILL_BATTLEGROUND && fill.kind != FILL_ARENA)
            return;

        Battleground* bg = human->GetBattleground();
        uint64_t const now = TimeMs::NowMs();

        // Soltar los bots que ya no están en ESTA partida ni entrando a ella.
        for (auto b = fill.bots.begin(); b != fill.bots.end();)
        {
            Player* bot = ObjectAccessor::FindPlayer(b->guid);
            bool const inThisBg = bot && bot->IsInWorld() && bg && bot->GetBattleground() == bg;
            bool const pending  = bot && !b->entered
                                  && bot->InBattlegroundQueueForBattlegroundQueueType(fill.queueType);
            bool const staleReplacement = b->queuedMs && !b->entered
                && now - b->queuedMs > TimeMs::SecsToMs(cfg.backfillGiveUpSecs);

            if ((inThisBg || pending) && !staleReplacement)
            {
                ++b;
                continue;
            }

            if (bot && pending)
                DequeueBotFromBg(bot, fill.queueType);
            ForgetBot(b->guid);
            b = fill.bots.erase(b);
        }

        if (!cfg.backfill || !bg || bg->GetStatus() != STATUS_IN_PROGRESS)
            return;

        uint64& next = g_backfillAt[human->GetGUID()];
        if (now < next)
            return;
        next = now + TimeMs::SecsToMs(cfg.backfillIntervalSecs);

        BattlegroundTypeId const bgTypeId = bg->GetBgTypeID();
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
            return;
        PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), human->GetLevel());
        if (!bracket)
            return;
        uint8 const arenaType = bg->isArena() ? bg->GetArenaType() : 0;

        std::set<ObjectGuid> taken;
        for (Fill::Bot const& b : fill.bots)
            taken.insert(b.guid);

        for (uint8 t = 0; t < 2; ++t)
        {
            TeamId const team = static_cast<TeamId>(t);

            uint32 mineTotal = 0;
            uint32 mineHere = 0;
            for (Fill::Bot const& b : fill.bots)
                if (b.intendedTeam == team)
                {
                    ++mineTotal;
                    if (b.entered)
                        ++mineHere;
                }
            if (!mineTotal)
                continue;

            uint32 const want = std::min<uint32>({ mineTotal > mineHere ? mineTotal - mineHere : 0u,
                                                   bg->GetFreeSlotsForTeam(team), 2u });
            if (!want)
                continue;

            for (Player* bot : CollectBots(want, bracket->minLevel, bracket->maxLevel, team, 0, nullptr, &taken))
            {
                BotClaims::Lease claim(bot->GetGUID().GetCounter(), OWNER);
                if (!claim || !IsFreeBot(bot))
                    continue;
                if (!QueueBotForBg(bot, bgTemplate, bgTypeId, fill.queueType, arenaType))
                    continue;

                claim.Keep();
                Fill::Bot slot;
                slot.guid = bot->GetGUID();
                slot.intendedTeam = team;
                slot.queuedMs = now;
                fill.bots.push_back(slot);
                g_humanOf[bot->GetGUID()] = human->GetGUID();
                taken.insert(bot->GetGUID());
                LOG_INFO("module", "[queue-bots] Repuesto {} ({}) a la partida de {}.",
                         bot->GetName(), team == TEAM_ALLIANCE ? "Alianza" : "Horda", human->GetName());
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Configuración
// ─────────────────────────────────────────────────────────────────────────────
class mod_queue_bots_config : public WorldScript
{
public:
    mod_queue_bots_config() : WorldScript("mod_queue_bots_config", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        cfg.enabled        = sConfigMgr->GetOption<bool>("QueueBots.Enable", true);
        cfg.arena1v1       = sConfigMgr->GetOption<bool>("QueueBots.Arena1v1", true);
        cfg.battlegrounds  = sConfigMgr->GetOption<bool>("QueueBots.Battlegrounds", true);
        cfg.arenas         = sConfigMgr->GetOption<bool>("QueueBots.Arenas", true);
        cfg.dungeons       = sConfigMgr->GetOption<bool>("QueueBots.Dungeons", true);
        cfg.raidBrowser    = sConfigMgr->GetOption<bool>("QueueBots.RaidBrowser", true);
        cfg.announce       = sConfigMgr->GetOption<bool>("QueueBots.Announce", true);
        cfg.forceAccept    = sConfigMgr->GetOption<bool>("QueueBots.ForceAccept", true);
        cfg.delaySeconds   = sConfigMgr->GetOption<uint32>("QueueBots.Delay", 0);
        cfg.forceAfterSecs = sConfigMgr->GetOption<uint32>("QueueBots.ForceAcceptAfter", 5);
        cfg.maxRaidBots    = sConfigMgr->GetOption<uint32>("QueueBots.MaxRaidBots", 39);
        // El .conf.dist promete que no pasa de AiPlayerbot.MaxAddedBots: sin este
        // tope una banda pedida por config podía exceder lo que playerbots deja
        // añadir a un jugador (B_maxRaid).
        if (uint32 const addedCap = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxAddedBots", 40))
            cfg.maxRaidBots = std::min(cfg.maxRaidBots, addedCap);
        // Gate principal del ritmo de rellenado (g_nextScanOf). Estaba declarado
        // con default 2000 pero nunca se leia de config ni aparecia en el .dist.
        cfg.scanIntervalMs = std::max<uint32>(sConfigMgr->GetOption<uint32>("QueueBots.ScanIntervalMs", 2000), 250);
        cfg.wakeCooldownSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("QueueBots.WakeCooldownSecs", 15), 1);
        cfg.raidSize       = sConfigMgr->GetOption<uint32>("QueueBots.RaidSize", 0);
        cfg.raidTanks      = sConfigMgr->GetOption<uint32>("QueueBots.RaidTanks", 0);
        cfg.raidHealers    = sConfigMgr->GetOption<uint32>("QueueBots.RaidHealers", 0);
        cfg.raidLevelBelow = sConfigMgr->GetOption<uint32>("QueueBots.RaidLevelBelow", 5);
        cfg.wakeBots       = sConfigMgr->GetOption<bool>("QueueBots.WakeBots", true);
        cfg.wakeMax        = sConfigMgr->GetOption<uint32>("QueueBots.WakeMax", 40);
        cfg.raidSummonSecs = sConfigMgr->GetOption<uint32>("QueueBots.RaidSummonDelay", 8);
        cfg.raidSummonGiveUpSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("QueueBots.RaidSummonGiveUpSecs", 60), 5);
        cfg.dcAuto         = sConfigMgr->GetOption<bool>("QueueBots.DungeonClearAuto", true);
        cfg.dcDelaySecs    = sConfigMgr->GetOption<uint32>("QueueBots.DungeonClearDelay", 10);
        cfg.dcTries        = std::max<uint32>(sConfigMgr->GetOption<uint32>("QueueBots.DungeonClearTries", 3), 1);
        cfg.dcReviveDelaySecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("QueueBots.DungeonClearReviveDelay", 8), 1);
        cfg.dungeonTanks   = sConfigMgr->GetOption<uint32>("QueueBots.DungeonTanks", 1);
        cfg.dungeonHealers = sConfigMgr->GetOption<uint32>("QueueBots.DungeonHealers", 1);
        cfg.dungeonDamage  = sConfigMgr->GetOption<uint32>("QueueBots.DungeonDamage", 3);
        cfg.classVariety   = sConfigMgr->GetOption<bool>("QueueBots.ClassVariety", true);
        cfg.bgFillToFull   = sConfigMgr->GetOption<bool>("QueueBots.BattlegroundFillToFull", false);
        cfg.backfill       = sConfigMgr->GetOption<bool>("QueueBots.Backfill", true);
        cfg.backfillIntervalSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("QueueBots.BackfillIntervalSecs", 20), 3);
        cfg.backfillGiveUpSecs   = std::max<uint32>(sConfigMgr->GetOption<uint32>("QueueBots.BackfillGiveUpSecs", 45), 10);
        cfg.gear           = BotGear::LoadSettings("QueueBots");
        BotPopulationCoordinator::LoadSettings();
        cfg.tokenTurnIn    = sConfigMgr->GetOption<bool>("QueueBots.TokenTurnIn", true);
        cfg.tokenDelaySecs = sConfigMgr->GetOption<uint32>("QueueBots.TokenTurnInDelay", 90);

        // Sin mod-token-turnin no existe el comando: se apaga en silencio.
        if (cfg.tokenTurnIn && !sConfigMgr->GetOption<bool>("TokenTurnIn.Enable", false))
        {
            LOG_INFO("module", "[queue-bots] mod-token-turnin no esta instalado o esta apagado: no se canjearan tokens solos.");
            cfg.tokenTurnIn = false;
        }

        // Sin mod-dungeon-clear instalado no existe su clave, y no hay a quién
        // mandarle ".dc on": se apaga en silencio.
        if (cfg.dcAuto && !sConfigMgr->GetOption<bool>("DungeonClear.Enable", false))
        {
            LOG_INFO("module", "[queue-bots] mod-dungeon-clear no esta instalado o esta apagado: no se activara solo.");
            cfg.dcAuto = false;
        }

        // El 1c1 hereda el nivel mínimo de mod-1v1-arena: de nada sirve encolar
        // a un bot al que ese módulo no dejaría entrar.
        cfg.arena1v1Level = sConfigMgr->GetOption<uint32>("Arena1v1.MinLevel", 80);

        if (cfg.arena1v1 && !sConfigMgr->GetOption<bool>("Arena1v1.Enable", false))
        {
            LOG_INFO("module", "[queue-bots] mod-1v1-arena esta desactivado: no se rellenara la cola 1c1.");
            cfg.arena1v1 = false;
        }

        // Espejo atómico para el hook de hilo de mapa (T7).
        g_enabledHot.store(cfg.enabled, std::memory_order_relaxed);
        g_tokenTurnInHot.store(cfg.tokenTurnIn, std::memory_order_relaxed);
        g_cleanedWhileOff.store(false, std::memory_order_relaxed);   // rearmar el latch de T8
        g_auxCleanedWhileOff = false;                                 // y el de M06
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  El rellenador
// ─────────────────────────────────────────────────────────────────────────────
class mod_queue_bots_player_events : public PlayerScript
{
public:
    mod_queue_bots_player_events() : PlayerScript("mod_queue_bots_player_events",
        { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        if (!session)
            return;

        if (!session->IsHeadless())
        {
            PushPlayerRequest(player->GetGUID(), REQ_HUMAN_UPDATE);
            return;
        }

        // La inmensa mayoria de bots esta deambulando sin participar en nada
        // gestionado por este modulo. HandleBot los descartaba igualmente, pero
        // antes todos publicaban una peticion protegida por mutex en cada tick.
        if (player->GetGroup() || player->InBattleground() || player->InBattlegroundQueue()
            || sLFGMgr->GetState(player->GetGUID()) != LFG_STATE_NONE)
            PushPlayerRequest(player->GetGUID(), REQ_BOT_UPDATE);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            PushPlayerRequest(player->GetGUID(), REQ_LOGOUT);
    }
};

// Helper sin hooks: solo lo llama el WorldScript propietario.
class QueueBotsFiller
{
public:
    void ProcessRequests()
    {
        std::unordered_map<ObjectGuid, uint8> requests;
        {
            std::lock_guard<std::mutex> lock(g_playerRequestsLock);
            requests.swap(g_playerRequests);
        }

        if (!cfg.enabled)
        {
            // Solo una vez por apagado: BotClaims::ReleaseAll escanea el mapa
            // global de claims bajo el candado compartido por 5 módulos; hacerlo
            // en cada tick con el módulo apagado en caliente es puro coste (T8).
            if (!g_cleanedWhileOff.exchange(true))
            {
                while (!g_fills.empty())
                    CancelFill(g_fills.begin()->first);
                g_waitingSince.clear();
                g_humanOf.clear();
                g_forceAt.clear();
                g_raidNextAdd.clear();
                g_raidSummons.clear();
                g_backfillAt.clear();
                g_dcState.clear();
                g_nextScanOf.clear();
                g_nextWakeOf.clear();
                BotClaims::ReleaseAll(OWNER);
            }
            return;
        }

        for (auto const& request : requests)
        {
            if (request.second == REQ_LOGOUT)
            {
                HandleLogout(request.first);
                continue;
            }

            Player* player = ObjectAccessor::FindPlayer(request.first);
            WorldSession* session = player ? player->GetSession() : nullptr;
            if (!session || !player->IsInWorld())
                continue;

            if (request.second == REQ_BOT_UPDATE && session->IsHeadless())
                HandleBot(player);
            else if (request.second == REQ_HUMAN_UPDATE && !session->IsHeadless())
                HandleHuman(player);
        }
    }

    // ─── Comandos .queuebots ────────────────────────────────────────────────
    void RunCommands()
    {
        std::vector<QbCmd> cmds;
        {
            std::lock_guard<std::mutex> lock(g_qbCmdLock);
            cmds.swap(g_qbCmd);
        }

        for (QbCmd const& cmd : cmds)
        {
            Player* player = ObjectAccessor::FindPlayer(cmd.human);
            if (!player || !player->GetSession())
                continue;

            ChatHandler handler(player->GetSession());
            if (!cfg.enabled)
            {
                handler.SendSysMessage("mod-queue-bots esta desactivado.");
                continue;
            }

            switch (cmd.kind)
            {
                case QB_CMD_STATUS:
                    ReportStatus(player, handler);
                    break;
                case QB_CMD_TOPUP:
                    // Se borran los sellos de throttle para que la próxima pasada
                    // de HandleHuman/FillRaid vuelva a mirar la cola de inmediato.
                    g_nextScanOf.erase(cmd.human);
                    g_waitingSince.erase(cmd.human);
                    g_raidNextAdd.erase(cmd.human);
                    handler.SendSysMessage("Se revisara tu cola en la proxima pasada de rellenado.");
                    break;
                case QB_CMD_LEAVE:
                {
                    auto fill = g_fills.find(cmd.human);
                    if (fill == g_fills.end() || fill->second.bots.empty())
                    {
                        handler.SendSysMessage("No hay bots de este modulo en tu cola.");
                        break;
                    }
                    uint32 const gone = static_cast<uint32>(fill->second.bots.size());
                    CancelFill(cmd.human);
                    handler.PSendSysMessage("Se han sacado {} bots de tu cola (tu sigues en ella).", gone);
                    break;
                }
            }
        }

        // Acciones del panel (mod-bot-operations, via BotOperations.h):
        // "adelanta la proxima pasada" es lo mismo que QB_CMD_TOPUP pero para
        // todos los humanos en cola, no solo uno: se borran los sellos de
        // throttle de todos para que la proxima pasada de HandleHuman/FillRaid
        // los mire de inmediato.
        // Apagado (M07): lo que hubiera en el buzón vuelve como fallido y no
        // se aceptan más; antes se contestaba "adelantada" sin hacer nada.
        if (!cfg.enabled)
        {
            BotOperations::SetTargetOffline(BotOperations::Target::QueueBots, TimeMs::NowMs(),
                                            "queue-bots esta desactivado.");
            return;
        }

        for (BotOperations::ActionRequest const& request : BotOperations::TakeRequests(BotOperations::Target::QueueBots, TimeMs::NowMs()))
        {
            if (request.type == BotOperations::ActionType::QueueBotsPass)
            {
                g_nextScanOf.clear();
                g_waitingSince.clear();
                g_raidNextAdd.clear();
                BotOperations::ReportOutcome(request.id, true, "Proxima pasada de relleno de colas adelantada.", TimeMs::NowMs());
            }
            else
                BotOperations::ReportOutcome(request.id, false, "Accion no reconocida por queue-bots.", TimeMs::NowMs());
        }
    }

    // Estadisticas para el panel (pestaña "Colas y grupos"). Solo cuenta
    // huecos totales por tipo de cola: Fill no guarda con que rol se metio
    // cada bot (ver comentario de QueueStats en BotOperations.h).
    void PublishStats(uint64_t now)
    {
        BotOperations::QueueStats stats;
        stats.enabled = cfg.enabled;
        stats.waitingHumans = static_cast<uint32_t>(g_fills.size());
        for (auto const& pair : g_fills)
        {
            Fill const& fill = pair.second;
            stats.botsFilled += static_cast<uint32_t>(fill.bots.size());
            uint32_t const missing = fill.needed > fill.bots.size()
                ? fill.needed - static_cast<uint32_t>(fill.bots.size()) : 0;
            switch (fill.kind)
            {
                case FILL_ARENA_1V1:    stats.arena1v1Missing += missing; break;
                case FILL_BATTLEGROUND: stats.battlegroundMissing += missing; break;
                case FILL_ARENA:        stats.arenaMissing += missing; break;
                case FILL_DUNGEON:      stats.dungeonMissing += missing; break;
                case FILL_RAID:         stats.raidMissing += missing; break;
                default: break;
            }
        }
        stats.updatedAtMs = now;
        BotOperations::PublishQueueStats(std::move(stats));
    }

private:
    void ReportStatus(Player* player, ChatHandler& handler)
    {
        auto fill = g_fills.find(player->GetGUID());
        if (fill == g_fills.end() || fill->second.bots.empty())
        {
            handler.SendSysMessage("Sin bots de este modulo en cola. Ponte en una cola (mazmorra, banda, "
                                   "campo de batalla, arena) y se rellenara sola.");
            return;
        }

        Fill const& f = fill->second;
        char const* kindName = "cola";
        switch (f.kind)
        {
            case FILL_ARENA_1V1:    kindName = "arena 1c1";       break;
            case FILL_BATTLEGROUND: kindName = "campo de batalla"; break;
            case FILL_ARENA:        kindName = "arena";            break;
            case FILL_DUNGEON:      kindName = "mazmorra";         break;
            case FILL_RAID:         kindName = "banda";            break;
            default: break;
        }

        uint32 tanks = 0, healers = 0, damage = 0;
        for (Fill::Bot const& assignment : f.bots)
            if (Player* bot = ObjectAccessor::FindPlayer(assignment.guid))
            {
                uint8 const role = BotRole(bot);
                if (role == PLAYER_ROLE_TANK)        ++tanks;
                else if (role == PLAYER_ROLE_HEALER) ++healers;
                else                                 ++damage;
            }

        handler.PSendSysMessage("Cola de {}: {}/{} bots ({} tanque, {} sanador, {} dano).",
                                kindName, static_cast<uint32>(f.bots.size()), f.needed, tanks, healers, damage);

        uint32 const missing = f.needed > f.bots.size() ? f.needed - static_cast<uint32>(f.bots.size()) : 0;
        if (missing)
            handler.PSendSysMessage("Faltan {} por entrar. '.queuebots traer' fuerza otra pasada; "
                                    "'.queuebots salir' saca los bots.", missing);
        else
            handler.SendSysMessage("Cola completa. '.queuebots salir' saca los bots.");
    }

    void HandleLogout(ObjectGuid guid)
    {
        // Un bot de banda que se desconecta antes de que TrySummon lo traiga
        // dejaba su entrada huerfana en g_raidSummons (el bucle solo compara
        // summon.human). Se borra siempre, sea GUID de bot o de humano.
        g_raidSummons.erase(guid);

        auto paired = g_humanOf.find(guid);
        if (paired != g_humanOf.end())
        {
            RemoveBotFromFill(paired->second, guid);
            return;
        }

        CancelFill(guid);

        g_raidNextAdd.erase(guid);
        g_dcState.erase(guid);
        g_nextScanOf.erase(guid);
        g_backfillAt.erase(guid);
        for (auto summon = g_raidSummons.begin(); summon != g_raidSummons.end();)
            summon = summon->second.human == guid ? g_raidSummons.erase(summon) : std::next(summon);

        // Un canje pendiente de un jugador que se desconecta antes del plazo
        // no debe sobrevivir hasta vencer para descartarse en silencio.
        {
            std::lock_guard<std::mutex> lock(g_bossLock);
            g_bossKills.erase(std::remove_if(g_bossKills.begin(), g_bossKills.end(),
                [guid](BossKill const& kill) { return kill.human == guid; }), g_bossKills.end());
        }
    }

    void CancelFill(ObjectGuid humanGuid)
    {
        auto fill = g_fills.find(humanGuid);
        if (fill == g_fills.end())
            return;

        for (Fill::Bot const& assignment : fill->second.bots)
        {
            Player* bot = ObjectAccessor::FindPlayer(assignment.guid);
            if (!bot)
                continue;

            if (fill->second.kind == FILL_DUNGEON)
            {
                if (sLFGMgr->GetState(assignment.guid) != LFG_STATE_NONE && !bot->GetGroup())
                    sLFGMgr->LeaveLfg(assignment.guid);
            }
            else if (fill->second.kind == FILL_RAID)
            {
                // El humano ya no forma banda (logout, salio del buscador): los
                // bots no pueden quedarse en un grupo con lider ausente (T5).
                if (Group* group = bot->GetGroup())
                    if (!bot->IsInCombat() && !bot->IsBeingTeleported())
                        group->RemoveMember(bot->GetGUID());
            }
            else if (!bot->InBattleground())
                DequeueBotFromBg(bot, fill->second.queueType);
        }
        ForgetFill(humanGuid);
    }

    // ─── Lado del jugador ───────────────────────────────────────────────────
    void HandleHuman(Player* player)
    {
        ObjectGuid const guid = player->GetGUID();

        // ¿En qué cola está? Se mira en orden: primero las de batalla, que son
        // las más baratas de comprobar, y luego el buscador.
        //
        // Solo se atiende la PRIMERA cola de batalla no vacía. Encolarse a dos
        // BG/arenas a la vez (poco habitual en un servidor de una persona) deja
        // la segunda sin bots: g_fills es de una entrada por humano y separarlo
        // por cola exige un rediseño (B8, diferido).
        BattlegroundQueueTypeId queueType = BATTLEGROUND_QUEUE_NONE;
        for (uint32 slot = 0; slot < PLAYER_MAX_BATTLEGROUND_QUEUES; ++slot)
        {
            BattlegroundQueueTypeId const candidate = player->GetBattlegroundQueueTypeId(slot);
            if (candidate != BATTLEGROUND_QUEUE_NONE)
            {
                queueType = candidate;
                break;
            }
        }

        LfgState const lfgState = sLFGMgr->GetState(guid);

        // Una banda del buscador ya en marcha sigue rellenándose aunque el
        // núcleo limpie LFG_STATE_RAIDBROWSER en cuanto el jugador tiene
        // grupo (LFGMgr no distingue "sigo buscando" de "ya tengo compañeros
        // y quiero más"; se vio en vivo, M48: el estado pasaba a
        // LFG_STATE_NONE justo después de que entraran los primeros 2-3
        // bots). Sin este atajo, FillRaid dejaba de llamarse para siempre en
        // cuanto se formaba el primer grupito, y la banda se quedaba ahí
        // aunque hubiera bots de sobra para completarla. FillRaid no mira
        // lfgState: sólo el grupo y el presupuesto, así que es seguro
        // saltarse aquí el resto de la comprobación de colas.
        if (queueType == BATTLEGROUND_QUEUE_NONE && lfgState == LFG_STATE_NONE)
        {
            if (auto raidFill = g_fills.find(guid); raidFill != g_fills.end() && raidFill->second.kind == FILL_RAID)
            {
                Group* group = player->GetGroup();
                uint32 const current = group ? group->GetMembersCount() : 1;
                Difficulty const raidDifficulty = player->GetRaidDifficulty();
                bool const is25Plus = raidDifficulty == RAID_DIFFICULTY_25MAN_NORMAL || raidDifficulty == RAID_DIFFICULTY_25MAN_HEROIC;
                if (QueueBotsPolicy::ComputeRaidBudget(cfg.raidSize, is25Plus, cfg.raidTanks, cfg.raidHealers,
                                                        current, cfg.maxRaidBots, 0, 0).shouldFill)
                {
                    FillRaid(player);
                    return;
                }
            }

            // En una partida en curso (ya no hay cola, pero sí BG): se reponen
            // los bots que se hayan caído a mitad.
            if (player->InBattleground())
            {
                BackfillBattle(player);
                return;
            }

            // Ya no espera nada. Los bots se los quita de encima cada uno desde
            // su propio lado, que es quien sabe si la partida ha empezado; aquí
            // se tira el registro en cuanto ninguno sigue a nuestro cargo.
            // Sin esto el registro se queda "completo" para siempre y la
            // siguiente cola de este jugador no se rellena nunca.
            auto fill = g_fills.find(guid);
            if (fill != g_fills.end())
            {
                bool stillOurs = false;
                bool stillPending = false;
                for (Fill::Bot const& assignment : fill->second.bots)
                    if (g_humanOf.count(assignment.guid))
                    {
                        stillOurs = true;
                        if (!assignment.entered)
                            stillPending = true;
                    }

                bool const battle = fill->second.kind == FILL_BATTLEGROUND || fill->second.kind == FILL_ARENA;
                if (!stillOurs)
                    g_fills.erase(fill);
                else if (battle && !stillPending)
                    // Sólo quedan bots que ya jugaron y ahora están fuera del BG:
                    // la partida ha terminado, se sueltan.
                    CancelFill(guid);
            }

            g_waitingSince.erase(guid);
            g_raidNextAdd.erase(guid);
            g_nextScanOf.erase(guid);
            g_backfillAt.erase(guid);
            return;
        }

        uint64_t const now = TimeMs::NowMs();

        auto since = g_waitingSince.find(guid);
        if (since == g_waitingSince.end())
        {
            g_waitingSince[guid] = now;
            return;
        }

        if (now - since->second < TimeMs::SecsToMs(cfg.delaySeconds))
            return;

        // El buscador de bandas no es una cola: se atiende aparte y con su
        // propio ritmo, porque montar el grupo es caro.
        if (lfgState == LFG_STATE_RAIDBROWSER)
        {
            FillRaid(player);
            return;
        }

        // Mientras falten bots se vuelve a intentar: en una pasada puede que no
        // hubiera suficientes libres, y rendirse ahí dejaba la cola a medias
        // esperando para siempre.
        auto filled = g_fills.find(guid);
        if (filled != g_fills.end())
        {
            // Un registro de OTRA cola no vale: si te apuntaste a un campo de
            // batalla después de una arena, el de la arena diría "completo" y no
            // se rellenaría nada.
            bool const otherQueue = (queueType != BATTLEGROUND_QUEUE_NONE)
                                        ? filled->second.queueType != queueType
                                        : filled->second.kind != FILL_DUNGEON;

            if (otherQueue)
            {
                CancelFill(guid);
                return;                 // en el siguiente tick se rellena de cero
            }

            // Para mazmorras no basta con mirar el tamaño: un bot puede caerse de
            // la cola LFG (propuesta rechazada, otro dueño se lo lleva...) sin
            // dejar de existir como jugador, y entonces "completo" es mentira
            // para siempre porque aquí ya no se vuelve a entrar. FillDungeon poda
            // esas entradas cada vez que corre; dejar que corra es barato (a lo
            // sumo cinco bots) frente a dejar la cola atascada.
            if (filled->second.bots.size() >= filled->second.needed
                && filled->second.kind != FILL_DUNGEON)
                return;
        }

        uint64& nextScan = g_nextScanOf[guid];   // sello por jugador, no global (B6)
        if (now < nextScan)                 // no barrer la lista de jugadores en cada tick
            return;
        nextScan = now + cfg.scanIntervalMs;

        if (queueType != BATTLEGROUND_QUEUE_NONE)
            FillBattleQueue(player, queueType);
        else if (lfgState == LFG_STATE_QUEUED)
            FillDungeon(player);
    }

    // ─── Campos de batalla, arenas y 1c1 ────────────────────────────────────
    void FillBattleQueue(Player* player, BattlegroundQueueTypeId queueType)
    {
        uint8 const arenaType = (queueType == BG_QUEUE_1V1) ? ARENA_TYPE_1V1
                                                            : BattlegroundMgr::BGArenaType(queueType);
        bool const isArena = arenaType != 0;

        if (isArena && queueType == BG_QUEUE_1V1 && !cfg.arena1v1)
            return;
        if (isArena && queueType != BG_QUEUE_1V1 && !cfg.arenas)
            return;
        if (!isArena && !cfg.battlegrounds)
            return;

        BattlegroundTypeId const bgTypeId = (queueType == BG_QUEUE_1V1) ? BATTLEGROUND_AA
                                                                        : BattlegroundMgr::BGTemplateId(queueType);
        Battleground* bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgTemplate)
        {
            LOG_INFO("module", "[queue-bots] Sin plantilla para la cola {} (tipo {}): no se puede rellenar.",
                     static_cast<uint32>(queueType), static_cast<uint32>(bgTypeId));
            return;
        }

        PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(bgTemplate->GetMapId(), player->GetLevel());
        if (!bracket)
        {
            LOG_INFO("module", "[queue-bots] El mapa {} de la cola {} no tiene tramo para el nivel {}: "
                                "no se puede encolar a nadie.",
                     bgTemplate->GetMapId(), static_cast<uint32>(queueType), player->GetLevel());
            return;
        }

        uint32 const minLevel = bracket->minLevel;
        uint32 const maxLevel = bracket->maxLevel;

        // Se completa lo que ya haya: en una pasada anterior puede que no
        // hubiera bots libres suficientes.
        Fill& fill = g_fills[player->GetGUID()];
        fill.queueType = queueType;

        // Un logout o abandono puede llegar entre dos pasadas. Se quitan los
        // apuntes muertos antes de recalcular para no dar la cola por completa.
        for (auto it = fill.bots.begin(); it != fill.bots.end();)
        {
            Player* bot = ObjectAccessor::FindPlayer(it->guid);
            if (!bot || (!bot->InBattleground() && !bot->InBattlegroundQueueForBattlegroundQueueType(queueType)))
            {
                ForgetBot(it->guid);
                it = fill.bots.erase(it);
            }
            else
                ++it;
        }

        uint32 const before = static_cast<uint32>(fill.bots.size());
        struct Candidate
        {
            Player* bot;
            TeamId intendedTeam;
        };
        std::vector<Candidate> chosen;

        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueType);
        GroupQueueInfo humanInfo;
        uint32 queuedWithHuman = 1;
        TeamId mine = player->GetTeamId();
        if (queue.GetPlayerGroupInfoData(player->GetGUID(), &humanInfo))
        {
            queuedWithHuman = std::max<uint32>(1, static_cast<uint32>(humanInfo.Players.size()));
            mine = humanInfo.teamId;
        }
        TeamId const theirs = mine == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE;

        if (isArena)
        {
            // Una arena necesita arenaType jugadores en cada lado. El grupo
            // que se encolo con el humano ya aporta todo o parte de su equipo;
            // los bots se asignan expresamente como aliados o rivales.
            fill.kind = (queueType == BG_QUEUE_1V1) ? FILL_ARENA_1V1 : FILL_ARENA;
            uint32 const floorLevel = (queueType == BG_QUEUE_1V1)
                                          ? std::max<uint32>(minLevel, cfg.arena1v1Level)
                                          : minLevel;

            // Se prefiere un rival de la facción contraria, que es el camino
            // normal del núcleo: cada uno espera en la cola de su bando. Si no
            // hay ninguno vale uno de tu misma facción, porque el núcleo sabe
            // pasar a uno de los dos al otro equipo
            // (CheckSkirmishForSameFaction, BattlegroundQueue.cpp:762).
            uint32 haveMine = std::min<uint32>(arenaType, queuedWithHuman);
            uint32 haveTheirs = 0;
            std::set<ObjectGuid> taken;
            for (Fill::Bot const& assignment : fill.bots)
            {
                (assignment.intendedTeam == mine ? haveMine : haveTheirs) += 1;
                taken.insert(assignment.guid);
            }

            ArenaNeeds const needs = MissingArenaPlayers(arenaType, haveMine, haveTheirs);
            fill.needed = before + needs.mine + needs.theirs;

            auto append = [&](std::vector<Player*> const& bots, TeamId intended)
            {
                for (Player* bot : bots)
                {
                    chosen.push_back({ bot, intended });
                    taken.insert(bot->GetGUID());
                }
            };

            append(CollectBots(needs.mine, floorLevel, maxLevel, mine, 0, nullptr, &taken), mine);
            uint32 const gotMine = static_cast<uint32>(chosen.size());

            std::vector<Player*> enemies = CollectBots(needs.theirs, floorLevel, maxLevel, theirs, 0, nullptr, &taken);
            append(enemies, theirs);
            uint32 const gotEnemies = static_cast<uint32>(enemies.size());

            // Para refriegas de una sola faccion el nucleo mueve el segundo
            // conjunto al otro equipo. Conservamos aqui el bando pretendido.
            if (gotEnemies < needs.theirs)
                append(CollectBots(needs.theirs - gotEnemies, floorLevel, maxLevel,
                                   TEAM_NEUTRAL, 0, nullptr, &taken), theirs);

            if (gotMine < needs.mine)
                WakeBots(needs.mine - gotMine, floorLevel, maxLevel, mine);
            uint32 const gotTheirs = static_cast<uint32>(chosen.size()) - gotMine;
            if (gotTheirs < needs.theirs)
                WakeBots(needs.theirs - gotTheirs, floorLevel, maxLevel, theirs);
        }
        else
        {
            // Un campo de batalla necesita los DOS bandos: la facción de un bot
            // la fija su raza, así que se buscan por separado.
            fill.kind = FILL_BATTLEGROUND;

            // Cuántos hay ya de cada bando, para pedir sólo los que faltan.
            uint32 haveMine = 0;
            uint32 haveTheirs = 0;
            for (Fill::Bot const& assignment : fill.bots)
                (assignment.intendedTeam == mine ? haveMine : haveTheirs) += 1;

            QueueBotsPolicy::BattlegroundNeeds const needs = QueueBotsPolicy::ComputeBattlegroundNeeds(
                bgTemplate->GetMaxPlayersPerTeam(), bgTemplate->GetMinPlayersPerTeam(), cfg.bgFillToFull,
                queuedWithHuman, haveMine, haveTheirs);
            fill.needed = needs.totalNeeded;

            std::vector<Player*> allies = CollectBots(needs.needMine, minLevel, maxLevel, mine);
            std::vector<Player*> enemies = CollectBots(needs.needTheirs, minLevel, maxLevel, theirs);

            // Lo que no haya despierto, se despierta de la reserva. Cada bando
            // por su cuenta: la facción de un bot la fija su raza.
            if (allies.size() < needs.needMine)
                WakeBots(needs.needMine - static_cast<uint32>(allies.size()), minLevel, maxLevel, mine);
            if (enemies.size() < needs.needTheirs)
                WakeBots(needs.needTheirs - static_cast<uint32>(enemies.size()), minLevel, maxLevel, theirs);

            for (Player* bot : allies)
                chosen.push_back({ bot, mine });
            for (Player* bot : enemies)
                chosen.push_back({ bot, theirs });
        }

        uint32 addedMine = 0;
        uint32 addedTheirs = 0;
        for (Candidate const& candidate : chosen)
        {
            Player* bot = candidate.bot;
            BotClaims::Lease claim(bot->GetGUID().GetCounter(), OWNER);
            if (!claim || !IsFreeBot(bot))
                continue;

            if (!QueueBotForBg(bot, bgTemplate, bgTypeId, queueType, arenaType))
                continue;

            claim.Keep();
            fill.bots.push_back({ bot->GetGUID(), candidate.intendedTeam });
            g_humanOf[bot->GetGUID()] = player->GetGUID();
            (candidate.intendedTeam == mine ? addedMine : addedTheirs) += 1;
        }

        uint32 const added = static_cast<uint32>(fill.bots.size()) - before;
        if (!added)
        {
            uint32 const missing = fill.needed > fill.bots.size()
                                       ? fill.needed - static_cast<uint32>(fill.bots.size()) : 0;
            LOG_DEBUG("module", "[queue-bots] Faltan {} bots libres de nivel {}-{} para la cola de {}.",
                      missing, minLevel, maxLevel, player->GetName());
            return;
        }

        LOG_INFO("module", "[queue-bots] {} bots a la cola de {} con {}: "
                           "{} aliados y {} rivales ({}/{}, niveles {}-{}).",
                 added, isArena ? "arena" : "campo de batalla", player->GetName(),
                 addedMine, addedTheirs, fill.bots.size(), fill.needed, minLevel, maxLevel);

        if (cfg.announce)
            ChatHandler(player->GetSession()).PSendSysMessage("Se han apuntado {} contrincantes a tu cola ({}/{}).",
                                                              added, static_cast<uint32>(fill.bots.size()),
                                                              fill.needed);
    }

    // ─── Buscador de mazmorras ──────────────────────────────────────────────
    void FillDungeon(Player* player)
    {
        if (!cfg.dungeons)
            return;

        ObjectGuid const guid = player->GetGUID();

        LfgDungeonSet const& selected = sLFGMgr->GetSelectedDungeons(guid);
        if (selected.empty())
        {
            LOG_INFO("module", "[queue-bots] {} está en el buscador pero sin mazmorra elegida.", player->GetName());
            return;
        }

        // El tramo sale de la ficha que maneja el núcleo (LFGDungeonData), no del
        // DBC en crudo: ahí están ya aplicadas las correcciones de
        // lfg_dungeon_template. Para las mazmorras aleatorias ese rango puede
        // venir a cero, y entonces se usa el nivel del jugador.
        // Resuelto por QueueBotsPolicy::ResolveDungeonLevelRange política pura, probada con static_assert aparte.
        uint32 dungeonMinLevel = 0;
        uint32 dungeonMaxLevel = 0;
        if (LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(*selected.begin()))
        {
            dungeonMinLevel = dungeon->minlevel;
            dungeonMaxLevel = dungeon->maxlevel;
        }

        QueueBotsPolicy::DungeonLevelRange const range =
            QueueBotsPolicy::ResolveDungeonLevelRange(dungeonMinLevel, dungeonMaxLevel, player->GetLevel());
        uint32 const minLevel = range.minLevel;
        uint32 const maxLevel = range.maxLevel;
        if (!dungeonMinLevel || dungeonMaxLevel < dungeonMinLevel)
            LOG_INFO("module", "[queue-bots] La mazmorra no declara tramo de nivel: se buscan bots de {} a {}.",
                     minLevel, maxLevel);

        // Reparto de roles (M15, QueueBotsPolicy::PlanDungeonRoles): cada
        // participante —el jugador, sus compañeros de grupo y los bots ya
        // encolados— ocupa UNA plaza de un rol que haya elegido. El jugador y
        // sus compañeros cuentan con lo marcado en el buscador; un compañero
        // sin roles en el buscador (bot de party-here), con su especialidad.
        static_assert(PLAYER_ROLE_TANK == QueueBotsPolicy::ROLE_TANK && PLAYER_ROLE_HEALER == QueueBotsPolicy::ROLE_HEALER
                      && PLAYER_ROLE_DAMAGE == QueueBotsPolicy::ROLE_DAMAGE);
        uint8 const roleMask = QueueBotsPolicy::ROLE_ANY;

        std::vector<QueueBotsPolicy::DungeonParticipant> participants;
        participants.push_back({ static_cast<uint8>(sLFGMgr->GetRoles(guid) & roleMask), false });
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == player)
                    continue;

                uint8 roles = sLFGMgr->GetRoles(member->GetGUID()) & roleMask;
                if (!roles)
                    roles = BotRole(member);
                participants.push_back({ roles, false });
            }
        }

        // Y lo que ya se metió en pasadas anteriores, tampoco.
        Fill& fill = g_fills[guid];
        fill.kind = FILL_DUNGEON;

        // Un bot puede caerse de la cola LFG entre dos pasadas (desconexión,
        // propuesta rechazada, otro dueño se lo lleva...) sin que desaparezca
        // como jugador. Si no se quita aquí, el hueco se da por cubierto para
        // siempre: el "X/Y" que ve el jugador deja de ser cierto y el barrido
        // de más arriba (bots.size() >= needed) ya no vuelve a llamar aquí.
        for (auto it = fill.bots.begin(); it != fill.bots.end();)
        {
            Player* bot = ObjectAccessor::FindPlayer(it->guid);
            if (!bot || sLFGMgr->GetState(it->guid) == LFG_STATE_NONE)
            {
                ForgetBot(it->guid);
                it = fill.bots.erase(it);
            }
            else
                ++it;
        }

        // Los encolados, con el rol con el que entraron (el que ve el
        // emparejador del núcleo), no con el que diga hoy su especialidad.
        size_t const firstQueued = participants.size();
        for (Fill::Bot const& assignment : fill.bots)
        {
            uint8 role = assignment.role;
            if (!role)
                if (Player* bot = ObjectAccessor::FindPlayer(assignment.guid))
                    role = BotRole(bot);
            participants.push_back({ role, true });
        }

        // Los bots libres por rol sólo deciden si alguien tiene más de un rol
        // posible: no se recorre la lista de jugadores si no hace falta.
        bool flexible = false;
        for (size_t i = 0; i < firstQueued; ++i)
        {
            uint8 const roles = participants[i].roles ? participants[i].roles : roleMask;
            if (roles & (roles - 1))
                flexible = true;
        }
        RoleCounts const avail = flexible ? CountFreeByRole(minLevel, maxLevel) : RoleCounts{};

        QueueBotsPolicy::DungeonRolePlan const plan = QueueBotsPolicy::PlanDungeonRoles(
            cfg.dungeonTanks, cfg.dungeonHealers, cfg.dungeonDamage,
            participants.data(), static_cast<uint32>(participants.size()),
            avail.tanks, avail.healers, avail.damage);

        if (!plan.feasible)
        {
            // Los roles del jugador y de su grupo no caben en la composición
            // (dos que sólo tanquean en un 1/1/3): el núcleo tampoco formará
            // grupo, y buscar bots no lo arregla. Se avisa una vez.
            if (!fill.rolesWarned)
            {
                fill.rolesWarned = true;
                LOG_INFO("module", "[queue-bots] Los roles de {} y su grupo no caben en una mazmorra {}/{}/{}: no se buscan bots.",
                         player->GetName(), cfg.dungeonTanks, cfg.dungeonHealers, cfg.dungeonDamage);
                ChatHandler(player->GetSession()).SendSysMessage(
                    "Los roles elegidos en tu grupo no caben en una mazmorra: cambialos para que se busquen companeros.");
            }
            fill.needed = static_cast<uint32>(fill.bots.size());
            return;
        }
        fill.rolesWarned = false;

        // Bots encolados que ya no tienen plaza (el jugador cambió de roles o
        // entró alguien al grupo): fuera de la cola, no se reclasifican.
        std::vector<ObjectGuid> surplus;
        for (size_t i = firstQueued; i < participants.size(); ++i)
            if (i >= QueueBotsPolicy::MAX_DUNGEON_PARTICIPANTS || plan.assigned[i] == 0)
                surplus.push_back(fill.bots[i - firstQueued].guid);
        for (ObjectGuid const& botGuid : surplus)
        {
            Player* bot = ObjectAccessor::FindPlayer(botGuid);
            if (bot && sLFGMgr->GetState(botGuid) != LFG_STATE_NONE && !bot->GetGroup())
                sLFGMgr->LeaveLfg(botGuid);
            LOG_INFO("module", "[queue-bots] {} sobra en la mazmorra de {}: sale del buscador.",
                     bot ? bot->GetName() : std::to_string(botGuid.GetCounter()), player->GetName());
            ForgetBot(botGuid);
            fill.bots.erase(std::remove_if(fill.bots.begin(), fill.bots.end(),
                [&botGuid](Fill::Bot const& b) { return b.guid == botGuid; }), fill.bots.end());
        }

        uint32 const tanks = plan.tanks;
        uint32 const healers = plan.healers;
        uint32 const damage = plan.damage;
        fill.needed = static_cast<uint32>(fill.bots.size()) + tanks + healers + damage;

        struct Need { uint8 role; uint32 count; };
        Need const needs[] = {
            { PLAYER_ROLE_TANK,   tanks   },
            { PLAYER_ROLE_HEALER, healers },
            { PLAYER_ROLE_DAMAGE, damage  }
        };

        uint32 const before = static_cast<uint32>(fill.bots.size());
        LfgDungeonSet dungeons = selected;   // JoinLfg lo quiere por referencia no constante

        // Clases que ya van en el grupo: en una mazmorra de cinco la variedad se
        // nota todavía más que en una banda.
        std::set<uint8> classes;
        classes.insert(player->getClass());
        if (Group* group = player->GetGroup())
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    classes.insert(member->getClass());

        for (Fill::Bot const& assignment : fill.bots)
            if (Player* bot = ObjectAccessor::FindPlayer(assignment.guid))
                classes.insert(bot->getClass());

        for (Need const& need : needs)
        {
            if (!need.count)
                continue;

            std::vector<Player*> found = CollectBots(need.count, minLevel, maxLevel, TEAM_NEUTRAL, need.role,
                                                    cfg.classVariety ? &classes : nullptr);
            if (found.empty())
                found = CollectBots(need.count, minLevel, maxLevel, TEAM_NEUTRAL, need.role);

            for (Player* bot : found)
            {
                BotClaims::Lease claim(bot->GetGUID().GetCounter(), OWNER);
                if (!claim || !IsFreeBot(bot))
                    continue;

                sLFGMgr->JoinLfg(bot, need.role, dungeons, "");

                if (sLFGMgr->GetState(bot->GetGUID()) == LFG_STATE_NONE)
                {
                    LOG_DEBUG("module", "[queue-bots] {} no pudo entrar al buscador de mazmorras.", bot->GetName());
                    continue;
                }

                claim.Keep();
                Fill::Bot queued;
                queued.guid = bot->GetGUID();
                queued.role = need.role;
                fill.bots.push_back(queued);
                g_humanOf[bot->GetGUID()] = guid;
                classes.insert(bot->getClass());
            }
        }

        uint32 const added = static_cast<uint32>(fill.bots.size()) - before;
        uint32 const missing = tanks + healers + damage > added ? tanks + healers + damage - added : 0;

        // No se puede pedir un rol concreto a la reserva (el rol sale de los
        // talentos, y eso no está en la tabla de personajes), así que se
        // despierta el doble y ya se elige entre los que entren.
        if (missing)
            WakeBots(missing * 2, minLevel, maxLevel, player->GetTeamId());

        if (!added)
        {
            LOG_INFO("module", "[queue-bots] Sin bots libres de nivel {}-{} para la mazmorra de {}: "
                               "faltan {} tanques, {} sanadores y {} de dano.",
                     minLevel, maxLevel, player->GetName(), tanks, healers, damage);
            return;
        }

        LOG_INFO("module", "[queue-bots] {} bots al buscador de mazmorras con {} ({}/{}, niveles {}-{}).",
                 added, player->GetName(), fill.bots.size(), fill.needed, minLevel, maxLevel);

        if (cfg.announce)
            ChatHandler(player->GetSession()).PSendSysMessage("Se han apuntado {} companeros a tu mazmorra ({}/{}).",
                                                              added, static_cast<uint32>(fill.bots.size()),
                                                              fill.needed);
    }

    // ─── Buscador de bandas ─────────────────────────────────────────────────
    // El tablón de bandas de 3.3.5 no empareja a nadie ni teletransporta: sólo
    // lista quién busca. Así que aquí no se rellena una cola, se te forma el
    // grupo: se meten bots en tu banda hasta el tamaño que toque.
    void FillRaid(Player* player)
    {
        if (!cfg.raidBrowser)
            return;

        ObjectGuid const guid = player->GetGUID();
        uint64_t const now = TimeMs::NowMs();

        auto next = g_raidNextAdd.find(guid);
        if (next != g_raidNextAdd.end() && now < next->second)
            return;

        // Un registro de OTRA cola (venias de un buscador de mazmorra) no vale:
        // se cancela antes de reutilizar la entrada como FILL_RAID.
        auto existing = g_fills.find(guid);
        if (existing != g_fills.end() && existing->second.kind != FILL_RAID)
            CancelFill(guid);

        // ⚠️ El tablón NO guarda a qué banda te has apuntado: LFGMgr::JoinLfg
        // mete la entrada en RaidBrowserStore (privado) y retorna ANTES del
        // SetSelectedDungeons. Así que no hay forma de saber si querías
        // Naxxramas de 10 o de 25, y se usa tu ajuste de dificultad de banda.
        Difficulty const raidDifficulty = player->GetRaidDifficulty();
        bool const is25PlusDifficulty = raidDifficulty == RAID_DIFFICULTY_25MAN_NORMAL || raidDifficulty == RAID_DIFFICULTY_25MAN_HEROIC;

        Group* group = player->GetGroup();

        // Si ya estás en el grupo de otro, no es cosa nuestra.
        if (group && group->GetLeaderGUID() != guid)
        {
            g_raidNextAdd[guid] = now + 30000;
            return;
        }

        uint32 const current = group ? group->GetMembersCount() : 1;

        // Composición y presupuesto (cuántos tanques/sanadores/daño hacen
        // falta) los decide QueueBotsPolicy::ComputeRaidBudget política pura, probada con static_assert aparte. Antes de
        // recorrer el grupo para contar tanques/sanadores (caro), una
        // comprobación barata con 0/0: si ya no hiciera falta rellenar, da
        // igual cuántos haya de cada — shouldFill ya sale en false.
        if (!QueueBotsPolicy::ComputeRaidBudget(cfg.raidSize, is25PlusDifficulty, cfg.raidTanks, cfg.raidHealers,
                                                 current, cfg.maxRaidBots, 0, 0).shouldFill)
        {
            g_raidNextAdd[guid] = now + 30000;
            return;
        }

        // Nivel EXACTO primero: en una banda, un bot por debajo es carne de
        // cañón y le resta al grupo. Pero con poca población en tu nivel
        // exacto (M48: mod-world-bots reparte los bots entre muchos niveles
        // y una banda de 56+ podía quedarse en 0-2 aunque el servidor tuviera
        // de sobra unos niveles más abajo) se admite bajar hasta
        // QueueBots.RaidLevelBelow niveles; nunca por encima del tuyo.
        uint32 const level = player->GetLevel();
        uint32 const minLevel = cfg.raidLevelBelow < level ? level - cfg.raidLevelBelow : 1;
        TeamId const team = player->GetTeamId();

        uint32 haveTanks = 0;
        uint32 haveHealers = 0;
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member)
                    continue;

                uint8 const role = BotRole(member);
                if (role == PLAYER_ROLE_TANK)        ++haveTanks;
                else if (role == PLAYER_ROLE_HEALER) ++haveHealers;
            }
        }
        else
        {
            uint8 const role = BotRole(player);
            if (role == PLAYER_ROLE_TANK)        ++haveTanks;
            else if (role == PLAYER_ROLE_HEALER) ++haveHealers;
        }

        QueueBotsPolicy::RaidBudget const budget = QueueBotsPolicy::ComputeRaidBudget(
            cfg.raidSize, is25PlusDifficulty, cfg.raidTanks, cfg.raidHealers,
            current, cfg.maxRaidBots, haveTanks, haveHealers);
        uint32 const size = budget.size;

        // Clases que ya están en el grupo, para no repetir mientras haya de
        // sobra: un grupo con tres cazadores es peor grupo.
        std::set<uint8> classes;
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    classes.insert(member->getClass());
        }
        else
            classes.insert(player->getClass());

        // De tres en tres: uno por pasada tardaba un minuto en formar una banda
        // de diez, y la mayor parte era espera. Primero lo que más falta.
        uint32 const room = budget.room;
        std::set<ObjectGuid> taken;
        std::vector<Player*> chosen;

        auto take = [&](uint8 role, uint32 count)
        {
            while (count && chosen.size() < room)
            {
                uint32 const want = std::min<uint32>(count, room - static_cast<uint32>(chosen.size()));

                // Primero tu nivel exacto, sin repetir clase si hay alternativa;
                // si no hay nadie así, se relaja primero la clase y luego el
                // nivel (hasta minLevel, M48), pero nunca por encima del tuyo.
                std::vector<Player*> found = CollectBots(want, level, level, team, role,
                                                        cfg.classVariety ? &classes : nullptr, &taken);
                if (found.empty())
                    found = CollectBots(want, level, level, team, role, nullptr, &taken);
                if (found.empty() && minLevel < level)
                    found = CollectBots(want, minLevel, level, team, role,
                                        cfg.classVariety ? &classes : nullptr, &taken);
                if (found.empty() && minLevel < level)
                    found = CollectBots(want, minLevel, level, team, role, nullptr, &taken);

                if (found.empty())
                    return;

                for (Player* bot : found)
                {
                    chosen.push_back(bot);
                    taken.insert(bot->GetGUID());
                    classes.insert(bot->getClass());
                    --count;
                }
            }
        };

        take(PLAYER_ROLE_TANK,   budget.neededTanks);
        take(PLAYER_ROLE_HEALER, budget.neededHealers);
        take(PLAYER_ROLE_DAMAGE, budget.neededDps);

        // Lo que falte tras agotar a los ya conectados se despierta de la
        // reserva dormida en el mismo tramo [minLevel, level] (M48): antes
        // sólo se despertaba con la banda entera a cero, así que una banda
        // que encontraba 1-2 bots conectados se quedaba ahí para siempre sin
        // tocar la reserva.
        if (chosen.size() < room)
        {
            LOG_INFO("module", "[queue-bots] {}/{} bots conectados de nivel {}-{} para la banda de {} "
                               "(faltan {} tanques y {} sanadores): se despiertan mas.",
                     static_cast<uint32>(chosen.size()), room, minLevel, level, player->GetName(),
                     budget.neededTanks, budget.neededHealers);
            WakeBots(room - static_cast<uint32>(chosen.size()), minLevel, level, team);
        }
        if (chosen.empty())
        {
            g_raidNextAdd[guid] = now + 10000;
            return;
        }

        // Se monta el grupo igual que hace el núcleo al aceptar una invitación:
        // Create, registrarlo en el gestor y luego los miembros. Y a banda antes
        // del sexto, que un grupo normal no pasa de cinco.
        if (!group)
        {
            group = new Group();
            if (!group->Create(player))
            {
                delete group;
                LOG_INFO("module", "[queue-bots] No se pudo crear la banda de {}.", player->GetName());
                g_raidNextAdd[guid] = now + 30000;
                return;
            }

            sGroupMgr->AddGroup(group);
        }

        if (size > 5 && !group->isRaidGroup())
            group->ConvertToRaid();

        Fill& fill = g_fills[guid];
        fill.kind = FILL_RAID;

        uint32 joined = 0;
        for (Player* bot : chosen)
        {
            if (group->GetMembersCount() >= size)
                break;

            BotClaims::Lease claim(bot->GetGUID().GetCounter(), OWNER);
            if (!claim || !IsFreeBot(bot))
                continue;

            if (!group->AddMember(bot))
            {
                LOG_INFO("module", "[queue-bots] {} no ha podido entrar en la banda de {}.",
                         bot->GetName(), player->GetName());
                continue;
            }

            // La reserva se conserva (antes el destructor la soltaba al salir de
            // la iteracion y solo IsFreeBot via GetGroup protegia al bot), y el
            // bot queda registrado como los de BG/mazmorra para que la limpieza
            // sea uniforme (HandleRaidBot, CancelFill, HandleLogout).
            claim.Keep();
            g_humanOf[bot->GetGUID()] = guid;
            fill.bots.push_back({ bot->GetGUID(), TEAM_NEUTRAL });

            // Sin esto el bot esta en el grupo pero su IA no sabe que tiene amo.
            AdoptRaidMaster(bot, player);

            ++joined;
            LOG_INFO("module", "[queue-bots] {} se une a la banda de {} ({}/{}, tamaño por tu dificultad de banda).",
                     bot->GetName(), player->GetName(), group->GetMembersCount(), size);

            // Equipo a la altura del jugador y de su fase, no de Naxxramas.
            BotGear::Schedule(bot->GetGUID(), BotGear::TargetItemLevel(player, cfg.gear), cfg.gear, OWNER,
                              BotGear::PRIORITY_GROUP, BotGear::InGroupOf(guid, &cfg.gear));

            // A veces el bot no viene solo (está en otro continente, o su IA no
            // arranca el "seguir"). Se le apunta para traerlo a tu lado pasados
            // unos segundos, con un poco de azar para que no lleguen en bloque.
            if (cfg.raidSummonSecs)
            {
                Summon& summon = g_raidSummons[bot->GetGUID()];
                summon.human    = guid;
                summon.dueMs    = now + TimeMs::SecsToMs(cfg.raidSummonSecs) + urand(0, 3000);
                summon.giveUpMs = summon.dueMs + TimeMs::SecsToMs(cfg.raidSummonGiveUpSecs);
            }
        }

        g_raidNextAdd[guid] = now + (joined ? 1500 : 10000);

        // No dejar un Fill vacio si esta pasada no metio a nadie (el resto del
        // codigo asume que un Fill con kind != NONE tiene bots o esta a punto).
        if (fill.bots.empty())
            g_fills.erase(guid);

        if (cfg.announce && group->GetMembersCount() == 2)
            ChatHandler(player->GetSession()).PSendSysMessage("Formando banda de {} con los bots disponibles.", size);
    }

    // ─── Lado del bot ───────────────────────────────────────────────────────
    void HandleBot(Player* bot)
    {
        ObjectGuid const guid = bot->GetGUID();

        auto pairedWith = g_humanOf.find(guid);
        if (pairedWith == g_humanOf.end())
            return;

        ObjectGuid const humanGuid = pairedWith->second;

        auto fill = g_fills.find(humanGuid);
        if (fill == g_fills.end())
        {
            ForgetBot(guid);
            return;
        }

        if (fill->second.kind == FILL_DUNGEON)
        {
            HandleDungeonBot(bot, humanGuid);
            return;
        }

        if (fill->second.kind == FILL_RAID)
        {
            HandleRaidBot(bot, humanGuid);
            return;
        }

        HandleBattleBot(bot, humanGuid, fill->second.queueType);
    }

    // Un bot de banda deja de ser nuestro cuando ya no comparte grupo con el
    // humano (lo echaste, se disolvio, alguien lo movio). RemoveBotFromFill le
    // quita el amo, resetea su IA y suelta la reserva.
    void HandleRaidBot(Player* bot, ObjectGuid humanGuid)
    {
        Player* human = ObjectAccessor::FindPlayer(humanGuid);
        Group* group = human ? human->GetGroup() : nullptr;
        if (group && bot->GetGroup() == group)
            return;   // sigue en la banda: nada que hacer

        LOG_INFO("module", "[queue-bots] {} deja la banda de {}: se le suelta.",
                 bot->GetName(), human ? human->GetName() : "?");
        RemoveBotFromFill(humanGuid, bot->GetGUID());
        g_raidSummons.erase(bot->GetGUID());
    }

    void HandleDungeonBot(Player* bot, ObjectGuid humanGuid)
    {
        Player* human = ObjectAccessor::FindPlayer(humanGuid);
        LfgState const humanState = human ? sLFGMgr->GetState(humanGuid) : LFG_STATE_NONE;

        // Mientras el jugador siga buscando o ya estén dentro, no se toca nada.
        if (humanState != LFG_STATE_NONE)
            return;

        // El jugador se ha ido del buscador: todos los bots que metimos sobran.
        // CancelFill los saca antes de liberar sus reservas.
        LOG_INFO("module", "[queue-bots] {} sale del buscador: su grupo ya no busca.", bot->GetName());
        CancelFill(humanGuid);
    }

    void HandleBattleBot(Player* bot, ObjectGuid humanGuid, BattlegroundQueueTypeId queueType)
    {
        ObjectGuid const guid = bot->GetGUID();

        // Su hueco en el fill: en un BG/arena se conserva tras entrar para que
        // BackfillBattle sepa cuántos de los nuestros siguen dentro.
        Fill::Bot* slot = nullptr;
        bool backfillKind = false;
        if (auto f = g_fills.find(humanGuid); f != g_fills.end())
        {
            backfillKind = f->second.kind == FILL_BATTLEGROUND || f->second.kind == FILL_ARENA;
            for (Fill::Bot& b : f->second.bots)
                if (b.guid == guid) { slot = &b; break; }
        }

        if (bot->InBattleground())          // dentro: a partir de aquí manda su IA
        {
            // Sólo la primera vez: en backfill el hueco se conserva y esta rama
            // se repite cada pasada mientras el bot siga dentro; reconstruir las
            // estrategias cada vez inundaba el log y le reiniciaba la IA.
            if (!(backfillKind && slot && slot->entered))
                GiveBattleBehaviour(bot);
            if (backfillKind && slot)
            {
                slot->entered = true;       // se conserva: BackfillBattle lo cuenta
                g_forceAt.erase(guid);
            }
            else
                ForgetBot(guid);
            return;
        }

        // Un bot que ya jugó y ha salido del BG: la partida terminó para él.
        if (backfillKind && slot && slot->entered)
        {
            RemoveBotFromFill(humanGuid, guid);
            return;
        }

        if (!bot->InBattlegroundQueueForBattlegroundQueueType(queueType))
        {
            ForgetBot(guid);
            return;
        }

        GroupQueueInfo ginfo;
        bool const invited = sBattlegroundMgr->GetBattlegroundQueue(queueType).GetPlayerGroupInfoData(guid, &ginfo)
                             && ginfo.IsInvitedToBGInstanceGUID != 0;

        Player* human = ObjectAccessor::FindPlayer(humanGuid);
        bool const humanWaiting = human && (human->InBattlegroundQueueForBattlegroundQueueType(queueType)
                                            || human->InBattleground());

        // Con invitación pendiente no se toca: es el instante justo en el que
        // los dos salen de la cola para entrar a la partida.
        if (!invited && !humanWaiting)
        {
            DequeueBotFromBg(bot, queueType);
            ForgetBot(guid);
            LOG_INFO("module", "[queue-bots] {} sale de la cola: su rival ya no esta.", bot->GetName());
            return;
        }

        if (!invited || !cfg.forceAccept)
        {
            g_forceAt.erase(guid);
            return;
        }

        uint64_t const now = TimeMs::NowMs();

        auto forceAt = g_forceAt.find(guid);
        if (forceAt == g_forceAt.end())
        {
            g_forceAt[guid] = now + TimeMs::SecsToMs(cfg.forceAfterSecs);
            return;
        }

        if (now < forceAt->second)
            return;

        // Su IA no ha aceptado la invitación. Se le manda el mismo paquete que
        // mandaría su cliente al pulsar "Entrar en la batalla".
        uint8 const arenaType = (queueType == BG_QUEUE_1V1) ? ARENA_TYPE_1V1
                                                            : BattlegroundMgr::BGArenaType(queueType);
        BattlegroundTypeId const bgTypeId = (queueType == BG_QUEUE_1V1) ? BATTLEGROUND_AA
                                                                        : BattlegroundMgr::BGTemplateId(queueType);

        WorldPacket data(CMSG_BATTLEFIELD_PORT, 20);
        data << uint8(arenaType) << uint8(0) << uint32(bgTypeId) << uint16(0) << uint8(1);
        bot->GetSession()->QueuePacket(new WorldPacket(data));

        forceAt->second = now + 3000;       // por si el paquete se pierde
        LOG_INFO("module", "[queue-bots] {} entra a la partida a la fuerza: su IA no acepto la invitacion.",
                 bot->GetName());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Traer a tu lado a los bots de la banda
//
//  Va en OnUpdate del mundo y no en el hook del bot porque teletransportar a
//  un jugador de un mapa junto a otro de otro mapa, desde el hilo de uno de los
//  dos mapas, es una carrera con MapUpdate.Threads = 4. Aquí no corre ningún
//  hilo de mapas. La secuencia es la del comando "summon" de playerbots
//  (UseMeetingStoneAction.cpp): sitio con línea de visión, nada en combate,
//  revivir si está muerto, limpiar el movimiento y las auras, y la mascota
//  detrás.
// ─────────────────────────────────────────────────────────────────────────────
class mod_queue_bots_world : public WorldScript
{
public:
    mod_queue_bots_world() : WorldScript("mod_queue_bots_world", { WORLDHOOK_ON_UPDATE }) { }

    void OnUpdate(uint32 /*diff*/) override
    {
        uint32 const t0 = getMSTime();
        DoUpdate();
        SlowTick::WarnIfSlow("queue-bots", "OnUpdate", t0);
    }

private:
    void DoUpdate()
    {
        _filler.ProcessRequests();
        _filler.RunCommands();

        uint64_t const now = TimeMs::NowMs();
        _filler.PublishStats(now);

        // apagado, este módulo no inicia trabajo propio. ProcessRequests
        // ya vació rellenos, convocatorias y estado de mazmorra; aquí se
        // descartan los canjes de jefe pendientes y los reequipados que pidió
        // él. Sigue atendiendo la cola COMPARTIDA de BotGear: esos trabajos
        // son de otros módulos, con su propio contexto, y el presupuesto es
        // global (no se reequipa más rápido por llamar desde más sitios).
        if (!cfg.enabled)
        {
            if (!g_auxCleanedWhileOff)
            {
                g_auxCleanedWhileOff = true;
                {
                    std::lock_guard<std::mutex> lock(g_bossLock);
                    g_bossKills.clear();
                }
                g_raidSummons.clear();
                g_dcState.clear();
                std::size_t const cancelled = BotGear::CancelOwnedBy(OWNER);
                LOG_INFO("module", "[queue-bots] Desactivado: sin mazmorras, canjes ni convocatorias; {} reequipado(s) propios cancelados.",
                         cancelled);
            }
            BotGear::ProcessOne();
            return;
        }

        if (now >= g_nextDcScan)
        {
            g_nextDcScan = now + 5000;
            AutoDungeonClear(now);
        }

        // Presupuesto compartido con los demás módulos que llaman a ProcessOne
        // (ver BotGear.h): cuesta decenas de milisegundos reequipar a un bot.
        BotGear::ProcessOne();

        TokenTurnIn(now);

        if (g_raidSummons.empty())
            return;

        for (auto it = g_raidSummons.begin(); it != g_raidSummons.end();)
        {
            if (now < it->second.dueMs)
            {
                ++it;
                continue;
            }

            switch (TrySummon(it->first, it->second, now))
            {
                case SUMMON_LATER:
                    it->second.dueMs = now + 3000;
                    ++it;
                    break;
                default:
                    it = g_raidSummons.erase(it);
                    break;
            }
        }
    }

private:
    QueueBotsFiller _filler;

    // ─── mod-dungeon-clear, activado solo ────────────────────────────────────
    // Ese módulo sólo arranca con ".dc on" desde dentro de la mazmorra, y su
    // propia batería de pruebas lo activa reintentando hasta que prende. Aquí
    // se hace lo mismo en nombre del jugador: cuando todo el grupo está en la
    // misma instancia, hay un tanque bot y el jugador no es el tanque (la IA
    // lleva al tanque: no puede ser él), se le manda el comando por su sesión.
    // Tres intentos cada 15 s, por si el módulo aún no tiene la lista de jefes.
    // Además, una vez activado, vigila si el grupo se repone de una muerte
    // (mod-dungeon-clear se autodesactiva tras un wipe/rez fallido y pide ".dc
    // on" manual) y reabre la misma tanda de reintentos — ver el comentario
    // junto a `state.anyDead` más abajo.
    void AutoDungeonClear(uint64_t now)
    {
        if (!cfg.enabled)   // nunca crea estado con el módulo apagado
            return;

        std::set<ObjectGuid> seen;

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* human = pair.second;
            WorldSession* session = human ? human->GetSession() : nullptr;
            if (!session || session->IsHeadless() || !human->IsInWorld())
                continue;

            Map* map = human->GetMap();
            Group* group = human->GetGroup();
            if (!map || !map->IsDungeon() || map->IsBattlegroundOrArena() || !group)
                continue;

            seen.insert(human->GetGUID());

            DcState& state = g_dcState[human->GetGUID()];
            if (state.instanceId != map->GetInstanceId())
            {
                state = DcState();
                state.instanceId = map->GetInstanceId();
                state.enteredMs = now;
                state.nextTryMs = now + TimeMs::SecsToMs(cfg.dcDelaySecs);
            }

            // Reactivación tras una muerte. mod-dungeon-clear se apaga solo tras
            // un wipe, una muerte que nadie puede resucitar o un rez que se agota
            // (DcActionShared::DisableDungeonClear en ese módulo) y pide ".dc on"
            // manual para retomar. A diferencia de la entrada a la mazmorra,
            // revivir dentro de la MISMA instancia no dispara ningún evento de
            // "entrada" con el que enganchar esto (por eso detectar sólo la
            // entrada, como se planteó al principio, no basta). Se vigila en su
            // lugar si alguien del grupo presente en este mapa está muerto; en
            // cuanto todos vuelven a tener vida se reabre una tanda de
            // reintentos igual a la de la entrada.
            //
            // Un compañero que muere y su propio rezzer lo resucita en segundos
            // es el caso normal, que mod-dungeon-clear resuelve solo sin
            // apagarse (sólo llega a apagarse tras un wipe entero, sin nadie que
            // pueda resucitar, o agotando DungeonClear.PostCombatRezTimeoutSecs,
            // 180 s por defecto): exigir que alguien lleve muerto un buen rato
            // antes de contar la recuperación evita mandar un ".dc on" de más
            // en el caso normal, que le resetearía el tirón en curso si el
            // tanque estaba a mitad de un pull avanzado (DcOnAction reinicia el
            // estado transitorio del pull al (re)activarse).
            //
            // Esto NO distingue la causa del apagado de un ".dc off" manual
            // tuyo: no hay forma de ver ese comando desde aquí sin acoplarse al
            // módulo de terceros (no expone ningún hook ni valor compartido
            // para ello), así que si lo apagas tú mismo justo al reponerte de
            // una muerte larga, esto lo reencenderá.
            if (cfg.dcAuto && state.dcApplicable && !map->IsRaid())
            {
                constexpr uint64 DC_REVIVE_MIN_DEAD_MS = 60000;

                bool anyDeadNow = false;
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && member->IsInWorld() && member->GetMap() == map && !member->IsAlive())
                    {
                        anyDeadNow = true;
                        break;
                    }
                }

                if (anyDeadNow)
                {
                    if (!state.anyDead)
                        state.deadSinceMs = now;
                    state.anyDead = true;
                }
                else if (state.anyDead)
                {
                    state.anyDead = false;
                    if (now - state.deadSinceMs >= DC_REVIVE_MIN_DEAD_MS)
                    {
                        state.done      = false;
                        state.tries     = 0;
                        state.nextTryMs = now + TimeMs::SecsToMs(cfg.dcReviveDelaySecs);
                        LOG_INFO("module", "[queue-bots] Grupo de {} repuesto tras una muerte (mapa {}): reintentando '.dc on'.",
                                 human->GetName(), map->GetId());
                    }
                }
            }

            if (state.done || now < state.nextTryMs)
                continue;

            // Todos dentro, y un tanque bot entre ellos.
            bool allHere = true;
            bool botTank = false;
            std::vector<Player*> missingBots;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || !member->IsInWorld() || member->GetMap() != map)
                {
                    allHere = false;
                    if (member && member->IsInWorld() && member->GetSession() && member->GetSession()->IsHeadless())
                        missingBots.push_back(member);
                    continue;
                }
                if (member != human && member->GetSession() && member->GetSession()->IsHeadless()
                    && BotRole(member) == PLAYER_ROLE_TANK)
                    botTank = true;
            }

            // Al grupo le puede faltar alguien un rato (teleportes escalonados);
            // pasados dos minutos se intenta con los que haya. El buscador de
            // mazmorras a veces deja algún bot fuera (no acepta la propuesta a
            // tiempo, o su teletransporte se pierde): en vez de esperar sin más
            // se le trae con el mismo mecanismo del "summon" de banda
            // (TrySummon/g_raidSummons), para no tener que sacarlo a mano con
            // ".summon". Sólo bots: a un jugador real no se le arrastra sin
            // avisar. Si cfg.raidSummonSecs es 0 ("nunca"), se deja como antes:
            // sólo esperar.
            if (!allHere)
            {
                if (cfg.raidSummonSecs)
                    for (Player* bot : missingBots)
                        if (!g_raidSummons.count(bot->GetGUID()))
                        {
                            Summon& summon = g_raidSummons[bot->GetGUID()];
                            summon.human    = human->GetGUID();
                            summon.dueMs    = now + TimeMs::SecsToMs(cfg.raidSummonSecs);
                            summon.giveUpMs = summon.dueMs + TimeMs::SecsToMs(cfg.raidSummonGiveUpSecs);
                            LOG_INFO("module", "[queue-bots] {} se queda fuera de la mazmorra de {}: se le trae.",
                                     bot->GetName(), human->GetName());
                        }

                if (now - state.enteredMs < 120000)
                {
                    state.nextTryMs = now + 5000;
                    continue;
                }
            }

            // A partir de aquí se sigue con quien haya (todos, o los que quedan
            // tras los dos minutos): cualquier intento de traer a un rezagado
            // que siga pendiente se cancela. Si llegara de todas formas tarde
            // se sumaría al grupo ya con ".dc on" activo, y ese "alguien entra"
            // es justo lo que mod-dungeon-clear no distingue de una razón para
            // desactivarse (ver el comentario de más arriba sobre `anyDead`).
            for (Player* bot : missingBots)
                g_raidSummons.erase(bot->GetGUID());

            // Ya están todos dentro: es el momento de topar el equipo de los
            // bots del grupo al del jugador y a su fase (mazmorras y bandas,
            // vengan del buscador, del tablón o de mod-party-here).
            if (!state.gearDone)
            {
                state.gearDone = true;
                uint32 const target = BotGear::TargetItemLevel(human, cfg.gear);
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && member != human && member->GetSession() && member->GetSession()->IsHeadless())
                        BotGear::Schedule(member->GetGUID(), target, cfg.gear, OWNER,
                                          BotGear::PRIORITY_GROUP, BotGear::InGroupOf(human->GetGUID(), &cfg.gear));
                }
                if (target)
                    LOG_INFO("module", "[queue-bots] Equipo de los bots del grupo de {} topado a nivel de objeto {} (mapa {}).",
                             human->GetName(), target, map->GetId());
            }

            // Lo de dungeon-clear sólo en mazmorras de cinco, y sólo si está.
            if (!cfg.dcAuto || map->IsRaid())
            {
                state.done = true;
                continue;
            }

            if (!botTank || BotRole(human) == PLAYER_ROLE_TANK)
            {
                LOG_INFO("module", "[queue-bots] No se activa mod-dungeon-clear para {}: {}.", human->GetName(),
                         botTank ? "el jugador es el tanque y la IA no puede llevarle" : "no hay tanque bot en el grupo");
                state.done = true;
                continue;
            }

            // A partir de aquí el grupo cualifica para el auto ".dc on" (tanque
            // bot presente, tú no eres el tanque): arma la vigilancia de
            // "reponerse tras una muerte" de más arriba.
            state.dcApplicable = true;

            // ParseCommands desde el hilo del mundo (aquí no corre ningún hilo de
            // mapa). Funciona porque el manejador de mod-dungeon-clear es
            // agnóstico al hilo; si algún día deja de serlo habría que encolar el
            // comando para su propio OnUpdate (B10).
            ChatHandler(human->GetSession()).ParseCommands(".dc on");
            ++state.tries;
            LOG_INFO("module", "[queue-bots] '.dc on' en nombre de {} (mapa {}, intento {}/{}).",
                     human->GetName(), map->GetId(), state.tries, cfg.dcTries);

            if (state.tries >= cfg.dcTries)
                state.done = true;
            else
                state.nextTryMs = now + 15000;
        }

        // Olvidar a los que ya no están en una mazmorra.
        for (auto it = g_dcState.begin(); it != g_dcState.end();)
            it = seen.count(it->first) ? std::next(it) : g_dcState.erase(it);
    }

    // ─── Canjear los tokens de los bots tras cada jefe ──────────────────────
    // Los bots sólo tiran codicia sobre los tokens que pueden usar; los que tú
    // pasas se los llevan y se quedan muertos en su bolsa. mod-token-turnin
    // los cambia por la pieza de su especialización, pero es un comando
    // manual: aquí se manda en tu nombre pasado el margen de las tiradas.
    void TokenTurnIn(uint64_t now)
    {
        if (!cfg.enabled || !cfg.tokenTurnIn)   // la opción particular no basta
            return;

        std::vector<BossKill> due;
        {
            std::lock_guard<std::mutex> lock(g_bossLock);
            for (auto it = g_bossKills.begin(); it != g_bossKills.end();)
            {
                if (now >= it->dueMs)
                {
                    due.push_back(*it);
                    it = g_bossKills.erase(it);
                }
                else
                    ++it;
            }
        }

        for (BossKill const& kill : due)
        {
            Player* human = ObjectAccessor::FindPlayer(kill.human);
            if (!human || !human->IsInWorld() || !human->GetSession() || !human->GetGroup())
                continue;

            // Igual que en AutoDungeonClear: el manejador de mod-token-turnin se
            // asume agnóstico al hilo (B10).
            ChatHandler(human->GetSession()).ParseCommands(".tokenturnin redeem");
            LOG_INFO("module", "[queue-bots] '.tokenturnin redeem' en nombre de {} tras matar un jefe.", human->GetName());
        }
    }

    enum Outcome { SUMMON_DONE, SUMMON_DROP, SUMMON_LATER };

    Outcome TrySummon(ObjectGuid botGuid, Summon const& summon, uint64_t now)
    {
        Player* bot = ObjectAccessor::FindPlayer(botGuid);
        Player* human = ObjectAccessor::FindPlayer(summon.human);
        if (!bot || !human || !bot->IsInWorld() || !human->IsInWorld())
            return SUMMON_DROP;

        // Sólo mientras siga en TU banda.
        Group* group = human->GetGroup();
        if (!group || bot->GetGroup() != group)
            return SUMMON_DROP;

        // Ya ha llegado por su cuenta.
        if (bot->GetMapId() == human->GetMapId() && bot->IsWithinDist(human, 40.0f, false))
            return SUMMON_DROP;

        if (now > summon.giveUpMs)
        {
            LOG_INFO("module", "[queue-bots] {} no se ha podido traer junto a {} en un minuto: se deja donde esta.",
                     bot->GetName(), human->GetName());
            return SUMMON_DROP;
        }

        // Momentos en los que no se toca a nadie: se vuelve a mirar en 3 s.
        if (bot->IsBeingTeleported() || human->IsBeingTeleported() || bot->IsInFlight())
            return SUMMON_LATER;
        if (human->IsInCombat() || bot->IsInCombat() || !human->IsAlive())
            return SUMMON_LATER;
        if (human->InBattleground() || human->InArena() || human->GetVehicle() || bot->GetVehicle())
            return SUMMON_LATER;

        Map* map = human->GetMap();
        if (!map)
            return SUMMON_LATER;

        // Un sitio a tu alrededor con línea de visión, como hace playerbots.
        uint32 const mapId = human->GetMapId();
        float const dist = 4.0f;
        float x = 0.f, y = 0.f, z = 0.f;
        bool found = false;
        float const start = frand(0.0f, 2.0f * static_cast<float>(M_PI));
        for (float angle = start; angle < start + 2.0f * static_cast<float>(M_PI); angle += static_cast<float>(M_PI) / 4.0f)
        {
            x = human->GetPositionX() + std::cos(angle) * dist;
            y = human->GetPositionY() + std::sin(angle) * dist;
            z = human->GetPositionZ();
            float const ground = map->GetHeight(human->GetPhaseMask(), x, y, z + 2.0f);
            if (ground > INVALID_HEIGHT && std::fabs(ground - z) < 5.0f)
                z = ground + 0.05f;
            if (human->IsWithinLOS(x, y, z))
            {
                found = true;
                break;
            }
        }
        if (!found)
            return SUMMON_LATER;

        if (bot->isDead())
        {
            bot->ResurrectPlayer(1.0f, false);
            bot->SpawnCorpseBones();
        }

        bot->GetMotionMaster()->Clear();
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        if (!bot->TeleportTo(mapId, x, y, z, human->GetOrientation()))
        {
            LOG_INFO("module", "[queue-bots] {} no ha podido teletransportarse junto a {} (mapa {}): se reintenta.",
                     bot->GetName(), human->GetName(), mapId);
            return SUMMON_LATER;
        }

        if (Pet* pet = bot->GetPet())
            pet->NearTeleportTo(x, y, z, human->GetOrientation());

        LOG_INFO("module", "[queue-bots] {} llega junto a {} ({}).", bot->GetName(), human->GetName(),
                 bot->GetMapId() == mapId ? "mismo mapa" : "desde otro mapa");
        return SUMMON_DONE;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Jefes muertos (hilo del mapa: sólo se apunta; el canje lo hace OnUpdate)
// ─────────────────────────────────────────────────────────────────────────────
class mod_queue_bots_bosses : public PlayerScript
{
public:
    mod_queue_bots_bosses() : PlayerScript("mod_queue_bots_bosses", { PLAYERHOOK_ON_CREATURE_KILL }) { }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        // Hilo de mapa: se leen los espejos atómicos, no el POD `cfg` (T7).
        if (!g_enabledHot.load(std::memory_order_relaxed)
            || !g_tokenTurnInHot.load(std::memory_order_relaxed) || !killer || !killed)
            return;

        WorldSession* session = killer->GetSession();
        if (!session || session->IsHeadless() || !killer->GetGroup())
            return;

        if (!killed->IsDungeonBoss() && !killed->isWorldBoss())
            return;

        uint64_t const due = TimeMs::NowMs() + TimeMs::SecsToMs(cfg.tokenDelaySecs);

        std::lock_guard<std::mutex> lock(g_bossLock);
        for (BossKill& kill : g_bossKills)
            if (kill.human == killer->GetGUID())
            {
                // Un segundo jefe con el canje del primero aún pendiente no se
                // descarta: el último jefe cierra la ventana. Sigue habiendo
                // como máximo un trabajo por humano.
                kill.dueMs = std::max(kill.dueMs, due);
                return;
            }
        g_bossKills.push_back({ killer->GetGUID(), due });
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  .queuebots — estado del rellenado, forzar una pasada, sacar los bots
//
//  El handler corre en el hilo de despacho: sólo encola la petición y la
//  atiende mod_queue_bots_world::OnUpdate, dueño de g_fills (regla 5 del README,
//  igual que .grupo de mod-party-here y .hermandad de mod-home-guild).
// ─────────────────────────────────────────────────────────────────────────────
class mod_queue_bots_command : public CommandScript
{
public:
    mod_queue_bots_command() : CommandScript("mod_queue_bots_command") { }

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        using namespace Acore::ChatCommands;
        static ChatCommandTable queueBotsTable =
        {
            { "estado", HandleStatus, SEC_PLAYER, Console::No },
            { "traer",  HandleTopUp,  SEC_PLAYER, Console::No },
            { "salir",  HandleLeave,  SEC_PLAYER, Console::No },
            { "",       HandleStatus, SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "queuebots", queueBotsTable },
        };
        return commandTable;
    }

private:
    static bool Enqueue(ChatHandler* handler, uint8 kind)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return true;
        PushQbCmd(player->GetGUID(), kind);
        return true;
    }

    static bool HandleStatus(ChatHandler* handler) { return Enqueue(handler, QB_CMD_STATUS); }
    static bool HandleTopUp(ChatHandler* handler)  { return Enqueue(handler, QB_CMD_TOPUP); }
    static bool HandleLeave(ChatHandler* handler)  { return Enqueue(handler, QB_CMD_LEAVE); }
};

void AddSC_mod_queue_bots()
{
    new mod_queue_bots_config();
    new mod_queue_bots_player_events();
    new mod_queue_bots_world();
    new mod_queue_bots_bosses();
    new mod_queue_bots_command();
}
