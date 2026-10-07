// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-party-here — grupo donde estás, sin cola.
 *
 * EL PROBLEMA
 * mod-queue-bots rellena COLAS: campos de batalla, arenas, el buscador de
 * mazmorras y el tablón de bandas. Pero una misión de grupo ("Hogger", los
 * elites de Bosque del Ocaso, cualquier "(Grupo)" del diario) o una mazmorra
 * a la que quieres entrar andando con la misión en el diario no tienen cola:
 * te quedas solo delante de la puerta. mod-quest-mates da compañeros que hacen
 * tu misión EN PARALELO, sin agruparse contigo.
 *
 * LA SOLUCIÓN
 * Un comando (".grupo", ".grupo banda 25", ".grupo fuera") que forma un grupo
 * de bots de tu nivel y tu facción, con tanque y sanador de verdad, y los trae
 * a tu lado. Y en automático: al aceptar una misión con "jugadores sugeridos"
 * se invita a los que falten, y se van solos al entregarla o al cambiar de
 * zona. El grupo se monta con la API del núcleo (Group::Create, AddMember),
 * igual que hace mod-queue-bots con las bandas del tablón.
 *
 * QUÉ PASA DESPUÉS
 * Un bot en tu grupo queda fuera del alcance de los demás módulos (todos
 * excluyen a los agrupados). Al entrar en una instancia, mod-queue-bots hace
 * lo suyo con cualquier grupo: topa el equipo de los bots a tu fase, activa
 * mod-dungeon-clear si hay tanque bot y canjea los tokens tras cada jefe.
 * Los bots de tu hermandad (mod-home-guild) van primero al elegir.
 *
 * SP05: mientras un bot sea tu compañero (manual o por misión de grupo), se
 * le dan también tus misiones activas que pueda coger -incluidas mazmorra y
 * banda, a diferencia de mod-quest-mates, porque este bot ya está dentro
 * contigo- para que aproveche sus propias copias de botín blanco de misión
 * de mod-quest-loot-party. `PartyHere.SyncQuestsToGroup` lo apaga.
 *
 * DÓNDE SE HACE EL TRABAJO
 * El comando y los hooks de misión sólo apuntan la petición; la atiende
 * WorldScript::OnUpdate, cuando ningún mapa se está actualizando
 * (modules/README.md, regla 5).
 *
 * DEPENDENCIA CON PLAYERBOTS (entre guardas)
 * IsTank/IsHeal por especialización (el rol), IsRandomBot (nunca el alt de
 * alguien), AddPlayerBot/LogoutPlayerBot (despertar dormidos, como queue-bots)
 * y, al meter un bot en el grupo, lo que hace su propia AcceptInvitationAction:
 * SetMaster, ResetStrategies, ChangeStrategy("+follow,-lfg,-bg") y Reset. Sin
 * eso el bot entra en el grupo pero su IA no sabe que tiene amo. Sin
 * playerbots, compila y no hace nada.
 */

#include "BotClaims.h"
#include "BotEligibility.h"
#include "BotGear.h"
#include "BotOperations.h"
#include "BotPopulationCoordinator.h"
#include "BotWorldAge.h"
#include "BotWake.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Containers.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "ModLocale.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Optional.h"
#include "party_here_locale.h"
#include "Pet.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SlowTick.h"
#include "SpellDefines.h"
#include "StringFormat.h"
#include "TimeMs.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// PlayerbotAIConfig/PlayerbotFactory (M26): cambiar la especialización de un
// bot aleatorio al rol que falta es exactamente lo que hace su comando
// "talents spec <nombre>" (ChangeTalentsAction::SpecPick); no hay otra vía
// pública para elegir la rama de talentos de un bot.
#if defined(__has_include)
#  if __has_include("Playerbots.h") && __has_include("PlayerbotAI.h") &&       __has_include("PlayerbotAIConfig.h") && __has_include("PlayerbotFactory.h")
#    include "PlayerbotAI.h"
#    include "PlayerbotAIConfig.h"
#    include "PlayerbotFactory.h"
#    include "Playerbots.h"
#    include "RandomPlayerbotMgr.h"
#    define PARTY_HERE_WITH_PLAYERBOTS 1
#  endif
#endif

using namespace Acore::ChatCommands;

namespace
{
    // Un único propietario para todas las parties. Con dos humanos y un bot que
    // por un hueco de estado queda sin grupo pero reservado, el segundo humano
    // podría llevárselo (Lease devuelve AlreadyOwned) y el Tick del primero
    // soltaría el claim del segundo (§4.2 P3). Inalcanzable en un servidor de
    // una persona; owners por-humano ("party-here:"+guid) lo cerrarían del todo.
    char const* const OWNER = "party-here";

    struct Config
    {
        bool   enabled       = true;
        uint32 levelBelow    = 3;
        uint32 levelAbove    = 0;
        uint32 maxRaidBots   = 39;
        bool   autoQuests    = true;
        uint32 autoMaxBots   = 2;
        bool   syncQuestsToGroup = true; // SP05: dar tambien tus misiones activas a tus companeros
        uint32 lingerSecs    = 120;
        uint32 summonSecs    = 3;
        bool   wakeBots      = true;
        uint32 wakeMax       = 20;
        uint32 wakeThrottleSecs = 15;  // margen entre tandas de WakeBots
        bool   announce      = true;
        bool   announceErrors = true;  // los mensajes de fallo (aparte de Announce)
        uint32 loadGraceSecs = 30;   // margen mientras el amo carga de mapa
        uint32 botMinWorldSecs = 60; // no traer una sesion recien conectada
        bool   dismissAutoOnZoneChange = false; // echar automaticos al cambiar de zona aunque la mision siga activa
        uint32 scanIntervalMs = 3000;  // cada cuanto se recorren las parties
        uint32 summonRetrySecs = 3;    // reintento de summon
        uint32 summonGiveUpSecs = 60;  // se deja de intentar traer al bot tras esto
        float  summonDistance = 40.0f; // "ya esta cerca": no se le trae
        float  summonPlaceRadius = 4.0f; // radio de colocacion alrededor del amo
        uint32 maxGroupBots = 0;       // tope para ".grupo N"; 0 = MaxRaidBots
        uint32 tanks   = 0;            // composicion; 0 = segun el tamano
        uint32 healers = 0;
        uint32 autoQuestMinSuggested = 0; // umbral de "jugadores sugeridos"; 0 = cualquiera > 0
        bool   resurrectOnSummon = true;
        bool   replenish       = true;    // reponer companheros que se van de un grupo manual
        uint32 replenishThrottleSecs = 15;
        uint32 persistMinutes  = 10;      // recomponer el grupo manual tras un relogin (0 = no)
        uint32 pendingTimeoutSecs = 180;  // una peticion sin completar caduca tras esto (M17)
        uint32 pendingRetrySecs   = 5;    // entre intentos de completarla
        uint32 pendingWakeSecs    = 45;   // entre tandas de despertar para la misma peticion
        uint32 roleWaitSecs       = 45;   // grupo manual: esperar tanque/sanador antes de cubrir con cualquiera
        bool   roleRespec         = false; // pasada la espera, especializar a un bot capaz en el rol que falta
        uint32 excludeMinutes     = 10;   // un bot echado o cambiado no vuelve por reposicion en este tiempo (M16)
        BotGear::Settings gear;
    };

    Config cfg;

    // Espejos atómicos de los campos que leen los handlers de comando y los
    // hooks de misión (T7). Con el core fijado no hacen falta: ".reload config"
    // y los comandos corren en el hilo del mundo con los mapas parados
    // (modules/README.md, regla 5; M05, 24/09/2026). Se conservan porque no
    // cuestan nada y protegen si el core cambiara ese despacho.
    std::atomic<bool>   g_enabledHot{true};
    std::atomic<bool>   g_autoQuestsHot{true};
    std::atomic<uint32> g_maxRaidBotsHot{39};

    // Latch "ya limpiado con el módulo apagado": sin él, la rama de OnUpdate con
    // !enabled hacía BotClaims::ReleaseAll(OWNER) —un escaneo del mapa entero de
    // claims bajo el candado global de 5 módulos— en CADA tick del mundo (T8).
    std::atomic<bool>   g_cleanedWhileOff{false};

    // ─── Peticiones (se apuntan desde el hilo que sea; las atiende OnUpdate) ─
    enum RequestKind : uint8
    {
        REQ_GROUP = 0,      // ".grupo [n]" / ".grupo mazmorra"
        REQ_RAID,           // ".grupo banda [tamaño]"
        REQ_DISMISS,        // ".grupo fuera" (todos)
        REQ_STATUS,         // ".grupo estado"
        REQ_QUEST_ACCEPT,   // misión aceptada
        REQ_QUEST_DONE,     // misión entregada o abandonada
        REQ_LOGOUT,         // jugador desconectado
        REQ_DISMISS_ONE,    // ".grupo fuera <nombre>"
        REQ_HOLD,           // ".grupo quieto [<nombre>]"
        REQ_FOLLOW,         // ".grupo sigue [<nombre>]"
        REQ_RESTORE,        // recomponer el grupo tras un relogin
        REQ_REPLACE_ONE,    // ".grupo cambia <nombre>"
        REQ_KEEP            // ".grupo conserva [<nombre>]" (M19)
    };

    struct Request
    {
        ObjectGuid  human;
        uint8       kind  = REQ_GROUP;
        uint32      value = 0;       // tamaño pedido, o id de misión
        std::string text = {};      // nombre de un companhero (comandos por bot)
    };

    // Productores: comandos y hooks de jugador/mapa. Consumidor: exclusivamente
    // mod_party_here_world::OnUpdate. La cola es el unico estado compartido.
    std::mutex          g_pendingLock;
    std::deque<Request> g_pending;

    // Lo que le hemos dado a cada jugador.
    struct Companion
    {
        ObjectGuid bot;
        bool       automatic = false;   // vino por una misión de grupo
        uint32     zone      = 0;       // dónde se le invitó (los automáticos)
        uint64     summonMs  = 0;       // cuándo traerlo (0 = ya)
        uint64     giveUpMs  = 0;
        // Despedida pedida por el jugador mientras el bot estaba en combate o
        // viajando (M16): se va en cuanto termine, no se le trae ni reequipa.
        bool       dismissPending = false;
        bool       replaceOnLeave = false;   // ".grupo cambia": al irse, viene otro
        // ".grupo quieto" (M20): se planta donde está y no se le trae aunque
        // venza summonMs. Se guarda aquí (no basta con tocar la IA del bot)
        // porque Tick decide el summon con este estado, no preguntándole a
        // playerbots su estrategia actual.
        bool       held           = false;
        // SP05: misiones tuyas que le ha dado ESTA sincronización mientras es
        // tu compañero. Sólo se toca lo que hay aquí (nunca una misión que el
        // bot tuviera por otra causa), igual que Mate::quests de
        // mod-quest-mates. Se vacía en cada punto de salida de un Companion
        // (ver ReleaseSyncedQuests).
        std::set<uint32> syncedQuests;
    };

    struct Party
    {
        std::vector<Companion> bots;
        std::set<uint32>       autoQuests;      // misiones de grupo activas
        uint64                 lingerUntilMs = 0; // 0 = sin cuenta atrás
        uint64                 graceUntilMs  = 0; // amo cargando de mapa: 0 = no
        uint32                 wantedSize    = 0; // tamaño del último ".grupo N/banda" (para reponer y recomponer)
        uint64                 nextReplenishMs = 0;
    };

    // Lo que había en el grupo manual de un jugador que se desconecta: se
    // recompone al volver, si es pronto (PartyHere.PersistMinutes).
    struct Persisted
    {
        uint32 wantedSize = 0;
        uint64 savedMs    = 0;
    };

    // Petición de compañeros que no se pudo completar al momento (M17): el
    // arranque en frío despierta bots que tardan en entrar, y antes la orden se
    // perdía y había que repetirla. Una por jugador; se reintenta espaciada
    // hasta completarse, caducar o cancelarse por una orden del jugador.
    enum DemandReason : uint8
    {
        DEMAND_COMMAND = 0,   // ".grupo N" / ".grupo banda"
        DEMAND_QUEST,         // misión con jugadores sugeridos
        DEMAND_RESTORE,       // recomponer tras un relogin
        DEMAND_REPLACE        // ".grupo cambia <nombre>"
    };

    struct Demand
    {
        uint32           size      = 0;       // miembros en total, tú incluido
        bool             automatic = false;   // compañeros de misión (se van al terminarla)
        uint8            reason    = DEMAND_COMMAND;
        std::set<uint32> quests;              // automática: misiones que la sostienen
        WorldSession*    session   = nullptr; // sólo identidad: otra sesión = petición caducada
        bool             hadGroup  = false;   // ya había grupo: si desaparece, el jugador lo dejó
        uint64           expiresMs = 0;
        uint64           nextTryMs = 0;
        uint64           nextWakeMs = 0;
        uint64           roleDeadlineMs = 0;  // hasta aquí se guardan las plazas de tanque/sanador
        uint32           attempts  = 0;
        bool             announced = false;   // ya se dijo "preparando compañeros"
    };

    // Propietario exclusivo: mod_party_here_world::OnUpdate (hilo del mundo).
    std::unordered_map<ObjectGuid, Party>     g_parties;
    std::unordered_map<ObjectGuid, Persisted> g_persisted;
    std::unordered_map<ObjectGuid, Demand>    g_demands;
    // humano -> (bot -> ms hasta el que no vuelve): los que echó o cambió (M16).
    std::unordered_map<ObjectGuid, std::unordered_map<ObjectGuid, uint64>> g_excluded;
    uint64 g_nextScan = 0;
    uint64 g_nextWake = 0;

    using BotEligibility::IsHuman;

    bool IsRandomBot(Player* bot)
    {
#ifdef PARTY_HERE_WITH_PLAYERBOTS
        return sRandomPlayerbotMgr.IsRandomBot(bot);
#else
        (void)bot;
        return false;
#endif
    }

    void Push(Request const& request)
    {
        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending.push_back(request);
    }

    void Tell(Player* player, std::string const& text)
    {
        if (cfg.announce && player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    // Mensajes de fallo ("no hay bots", "ya estas en otro grupo"): se ven
    // siempre, aunque PartyHere.Announce esté a 0 (§4.2 P4). Announce solo silencia
    // los avisos de cortesía, no los errores.
    void TellError(Player* player, std::string const& text)
    {
        if (cfg.announceErrors && player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    // ─── Bots libres (mismo criterio base que mod-queue-bots y mod-world-bots,
    // centralizado en BotEligibility.h) ──────────────────────────────────────
    bool IsFreeBot(Player* bot)
    {
        if (!BotEligibility::IsAvailable(bot))
            return false;

        if (BotClaims::IsClaimedByOther(bot->GetGUID().GetCounter(), OWNER))
            return false;

        return IsRandomBot(bot);
    }

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

    // El rol, con el criterio de playerbots (IsTank/IsHeal por especialización).
    uint8 BotRole(Player* bot)
    {
#ifdef PARTY_HERE_WITH_PLAYERBOTS
        if (PlayerbotAI::IsTank(bot, true))
            return lfg::PLAYER_ROLE_TANK;
        if (PlayerbotAI::IsHeal(bot, true))
            return lfg::PLAYER_ROLE_HEALER;
        return lfg::PLAYER_ROLE_DAMAGE;
#else
        uint8 const spec = SpecTab(bot);
        switch (bot->getClass())
        {
            case CLASS_DRUID:        return spec == 2 ? lfg::PLAYER_ROLE_HEALER : lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_PALADIN:      return spec == 1 ? lfg::PLAYER_ROLE_TANK : (spec == 0 ? lfg::PLAYER_ROLE_HEALER : lfg::PLAYER_ROLE_DAMAGE);
            case CLASS_PRIEST:       return spec != 2 ? lfg::PLAYER_ROLE_HEALER : lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_SHAMAN:       return spec == 2 ? lfg::PLAYER_ROLE_HEALER : lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_WARRIOR:      return spec == 2 ? lfg::PLAYER_ROLE_TANK : lfg::PLAYER_ROLE_DAMAGE;
            case CLASS_DEATH_KNIGHT: return spec == 0 ? lfg::PLAYER_ROLE_TANK : lfg::PLAYER_ROLE_DAMAGE;
            default:                 return lfg::PLAYER_ROLE_DAMAGE;
        }
#endif
    }

    // Hasta 'wanted' bots libres del tramo, facción y rol pedidos. Los de tu
    // hermandad primero; el resto barajado; sin repetir clase mientras haya.
    std::vector<Player*> CollectBots(uint32 wanted, uint32 minLevel, uint32 maxLevel, TeamId team,
                                     uint8 role, std::set<uint8> const* avoidClasses,
                                     std::set<ObjectGuid> const& taken)
    {
        std::vector<Player*> candidates;
        if (!wanted)
            return candidates;

        // El tope de recogida se cuenta solo sobre los bots que NO son de la
        // hermandad: los de tu hermandad se recogen siempre, aunque aparezcan
        // después del bot número N en ObjectAccessor. M21: el tope ya no corta
        // el recorrido (el `break` dejaba sin ver a los de tu hermandad que
        // venían detrás); sólo deja de admitir externos, y a partir de ahí
        // cada bot cuesta una búsqueda en BotClaims antes de los filtros caros.
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
            if (bot->GetTeamId() != team)
                continue;
            if (role && !(BotRole(bot) & role))
                continue;
            if (avoidClasses && avoidClasses->count(bot->getClass()))
                continue;
            if (taken.count(bot->GetGUID()))
                continue;

            candidates.push_back(bot);
            if (!home)
                ++nonHome;
        }

        Acore::Containers::RandomShuffle(candidates);
        std::stable_partition(candidates.begin(), candidates.end(), [](Player* bot)
        {
            return BotClaims::IsHomeGuildBot(bot->GetGUID().GetCounter());
        });
        if (candidates.size() > wanted)
            candidates.resize(wanted);

        return candidates;
    }

    // ─── Despertar dormidos (como mod-queue-bots) ────────────────────────────
    uint32 MakeRoom(uint32 wanted, uint32 keepMin, uint32 keepMax)
    {
        uint32 freed = 0;
#ifdef PARTY_HERE_WITH_PLAYERBOTS
        // LogoutPlayerBot es síncrono (LogoutPlayer -> RemoveFromWorld ->
        // ObjectAccessor::RemoveObject): llamarlo dentro del range-for sobre
        // GetPlayers() muta el contenedor en plena iteración (T1). Se recogen los
        // GUID y se desconectan tras cerrar el bucle.
        std::vector<ObjectGuid> victims;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            if (victims.size() >= wanted)
                break;
            Player* bot = pair.second;
            if (!IsFreeBot(bot))
                continue;
            if (bot->GetLevel() >= keepMin && bot->GetLevel() <= keepMax)
                continue;
            if (BotClaims::IsHomeGuildBot(bot->GetGUID().GetCounter()))
                continue;
            victims.push_back(bot->GetGUID());
        }
        for (ObjectGuid const& guid : victims)
        {
            sRandomPlayerbotMgr.LogoutPlayerBot(guid);
            ++freed;
        }
#else
        (void)wanted; (void)keepMin; (void)keepMax;
#endif
        return freed;
    }

    // Clases que pueden hacer de tanque o de sanador, para despertar sólo a
    // ésas mientras una petición espera ese rol (M26). El druida no cuenta
    // como tanque: PlayerbotAI::IsTank sólo lo reconoce en forma de oso. El
    // caballero de la Muerte sólo existe desde el nivel 55.
    char const* RoleClasses(uint8 role, uint32 maxLevel)
    {
        if (role == lfg::PLAYER_ROLE_TANK)
            return maxLevel >= 55 ? "1,2,6" : "1,2";
        if (role == lfg::PLAYER_ROLE_HEALER)
            return "2,5,7,11";
        return nullptr;
    }

    char const* RoleName(uint8 role)
    {
        return role == lfg::PLAYER_ROLE_TANK ? "tanque" : (role == lfg::PLAYER_ROLE_HEALER ? "sanador" : "dano");
    }

    // tanks/healers (M26): cuántas plazas de ese rol siguen sin candidato en
    // un grupo manual. Se despierta el doble de clases capaces de cada una
    // COMO EXTRA sobre el lote normal: sólo con el filtro, un arranque en frío
    // traía guerreros y nadie más.
    uint32 WakeBots(uint32 wanted, uint32 minLevel, uint32 maxLevel, TeamId team,
                    uint32 tanks = 0, uint32 healers = 0)
    {
        if (!wanted || !cfg.wakeBots)
            return 0;
#ifdef PARTY_HERE_WITH_PLAYERBOTS
        // La etapa de progresión activa (mod-world-bots) manda sobre cualquier
        // rango pedido aquí: si no cabe nada por debajo del tope, no se despierta.
        maxLevel = std::min(maxLevel, BotPopulationCoordinator::StageCap());
        if (minLevel > maxLevel)
            return 0;

        uint64_t const now = TimeMs::NowMs();
        if (now < g_nextWake)
            return 0;
        g_nextWake = now + TimeMs::SecsToMs(cfg.wakeThrottleSecs);

        wanted = std::min(wanted, cfg.wakeMax);

        // Recuento desde la foto cacheada del coordinador (un solo escaneo de
        // ObjectAccessor por pasada, compartido con los Reserve de abajo).
        uint32 const cap = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxRandomBots", 200);
        uint32 const online = BotPopulationCoordinator::OnlineCount(now);
        if (online + wanted > cap)
            MakeRoom(online + wanted - cap, minLevel, maxLevel);

        // medir el coste real de BotWake (hasta dos consultas
        // síncronas por selección) antes de decidir si hace falta precargar
        // candidatos de forma asíncrona.
        uint32 const tWake0 = getMSTime();
        bool const alliance = team == TEAM_ALLIANCE;
        std::vector<uint32_t> wakeCandidates;
        std::set<uint32_t> seen;
        auto add = [&](std::vector<uint32_t> const& found)
        {
            for (uint32_t const lowGuid : found)
                if (seen.insert(lowGuid).second)
                    wakeCandidates.push_back(lowGuid);
        };
        uint32 const tankWake = std::min<uint32>(tanks * 2, 4);
        uint32 const healWake = std::min<uint32>(healers * 2, 4);
        if (tankWake)
            add(BotWake::SelectOfflineCandidates(tankWake, minLevel, maxLevel, alliance,
                                                 RoleClasses(lfg::PLAYER_ROLE_TANK, maxLevel)));
        if (healWake)
            add(BotWake::SelectOfflineCandidates(healWake, minLevel, maxLevel, alliance,
                                                 RoleClasses(lfg::PLAYER_ROLE_HEALER, maxLevel)));
        add(BotWake::SelectOfflineCandidates(wanted, minLevel, maxLevel, alliance));
        wanted += tankWake + healWake;
        SlowTick::WarnIfSlow("party-here", "BotWake::SelectOfflineCandidates", tWake0);

        uint32 woken = 0;
        for (uint32 const lowGuid : wakeCandidates)
        {
            if (!sRandomPlayerbotMgr.IsRandomBot(lowGuid))
                continue;
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
            LOG_INFO("module", "[party-here] Despertando {} bots dormidos de nivel {}-{} ({}){}.",
                     woken, minLevel, maxLevel, team == TEAM_ALLIANCE ? "Alianza" : "Horda",
                     tankWake || healWake ? Acore::StringFormat(", buscando {} tanque(s) y {} sanador(es) entre clases capaces",
                                                                tankWake, healWake) : std::string());
        return woken;
#else
        (void)minLevel; (void)maxLevel; (void)team; (void)tanks; (void)healers;
        return 0;
#endif
    }

    // ─── Rol que falta: otra especialización (M26) ──────────────────────────
    // Con pocos bots en el tramo (tras una reinstalación casi todos son de
    // nivel 1) no aparece ningún tanque o sanador por mucho que se espere, y
    // la petición acababa en un grupo de cinco dps con el que queue-bots no
    // arranca dungeon-clear. Pasada RoleWaitSeconds, con PartyHere.RoleRespec,
    // un bot libre de una clase capaz cambia su rama de talentos a la del rol.

    // Rama de talentos (0..2) que da el rol a esa clase con el criterio de
    // PlayerbotAI::IsTank/IsHeal; -1 si la clase no puede hacerlo (o, como el
    // druida tanque, sólo lo es según la forma que tenga en ese momento).
    int RoleTab(uint8 cls, uint8 role)
    {
        if (role == lfg::PLAYER_ROLE_TANK)
            switch (cls)
            {
                case CLASS_WARRIOR:      return 2;
                case CLASS_PALADIN:      return 1;
                case CLASS_DEATH_KNIGHT: return 0;
                default:                 return -1;
            }
        if (role == lfg::PLAYER_ROLE_HEALER)
            switch (cls)
            {
                case CLASS_PALADIN: return 0;
                case CLASS_PRIEST:  return 1;
                case CLASS_SHAMAN:  return 2;
                case CLASS_DRUID:   return 2;
                default:            return -1;
            }
        return -1;
    }

#ifdef PARTY_HERE_WITH_PLAYERBOTS
    // La plantilla de talentos de playerbots (AiPlayerbot.PremadeSpecName.*)
    // que pone más puntos en esa rama; -1 si no hay ninguna.
    int PremadeSpecFor(uint8 cls, int tab)
    {
        for (int specNo = 0; specNo < MAX_SPECNO; ++specNo)
        {
            if (sPlayerbotAIConfig.premadeSpecName[cls][specNo].empty())
                break;
            uint32 points[3] = { 0, 0, 0 };
            for (std::vector<uint32> const& step : sPlayerbotAIConfig.parsedSpecLinkOrder[cls][specNo][80])
                if (step.size() >= 4 && step[0] < 3)
                    points[step[0]] += step[3];
            int const top = static_cast<int>(std::max_element(points, points + 3) - points);
            if (top == tab && points[top])
                return specNo;
        }
        return -1;
    }
#endif

    // Cambia la especialización del bot al rol pedido. Devuelve si ahora lo
    // tiene según BotRole. El cambio se queda: el bot es aleatorio y
    // playerbots ya le reparte talentos por su cuenta al subir de nivel.
    bool Respec(Player* bot, uint8 role)
    {
#ifdef PARTY_HERE_WITH_PLAYERBOTS
        int const tab = RoleTab(bot->getClass(), role);
        if (tab < 0)
            return false;
        int const specNo = PremadeSpecFor(bot->getClass(), tab);
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (specNo < 0 || !botAI)
            return false;

        PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitGlyphs(false);
        botAI->ResetStrategies();
        return (BotRole(bot) & role) != 0;
#else
        (void)bot; (void)role;
        return false;
#endif
    }

    // ─── Meter un bot en el grupo del jugador ────────────────────────────────
    // Lo que hace la propia AcceptInvitationAction de playerbots al aceptar
    // una invitación: tomar al jugador como amo, reconstruir estrategias,
    // "seguir", y reiniciar la IA. Sin esto el bot está en el grupo pero su
    // IA no sabe que tiene amo y sigue a lo suyo.
    void AdoptMaster(Player* bot, Player* human)
    {
#ifdef PARTY_HERE_WITH_PLAYERBOTS
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

    // Quita el amo, resetea la IA y suelta la reserva de un companion. Simétrico
    // de AdoptMaster: sin esto un bot echado seguía con amo y "+follow" hasta que
    // playerbots lo notaba (§4.2 P4). Con removeFromGroup además lo saca del
    // grupo del amo (DissolveParty).
    void DetachBot(ObjectGuid botGuid, bool removeFromGroup)
    {
        if (Player* bot = ObjectAccessor::FindPlayer(botGuid))
        {
            if (removeFromGroup)
                if (Group* group = bot->GetGroup())
                    group->RemoveMember(bot->GetGUID());
#ifdef PARTY_HERE_WITH_PLAYERBOTS
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                botAI->SetMaster(nullptr);
                botAI->ResetStrategies();
            }
#endif
        }
        BotClaims::Release(botGuid.GetCounter(), OWNER);
    }

    bool NameEq(std::string const& a, std::string const& b)
    {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(),
                          [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) ==
                                                      std::tolower(static_cast<unsigned char>(y)); });
    }

    // ─── SP05: sincronizar tus misiones activas a tus companeros ────────────
    // Sin cabecera compartida con mod-quest-mates: no
    // hay un segundo consumidor real y las versiones no serian identicas de
    // todas formas (ver mas abajo).

    // A diferencia de IsSuitableQuest de mod-quest-mates, aqui SI se admiten
    // mazmorra/heroica/banda: el bot ya esta agrupado y dentro contigo, que es
    // precisamente lo que permite aprovechar tambien alli las copias de botin
    // de mision de mod-quest-loot-party (SP04). Se mantiene la exclusion de
    // JcJ, evento de mundo, legendaria y escolta (misma razon que alli: la
    // escolta es guionizada y un segundo poseedor de la mision puede atascar
    // el script) y de diaria/repetible/estacional/autocompletable (evita
    // churn de la reserva por algo que se repite solo).
    bool IsSyncableQuestType(Quest const* quest)
    {
        switch (quest->GetType())
        {
            case QUEST_TYPE_PVP:
            case QUEST_TYPE_WORLD_EVENT:
            case QUEST_TYPE_LEGENDARY:
            case QUEST_TYPE_ESCORT:
                return false;
            default:
                break;
        }

        if (quest->IsDailyOrWeekly() || quest->IsRepeatable() || quest->IsSeasonal() || quest->IsAutoComplete())
            return false;

        return true;
    }

    // ¿Puede este bot coger la misión ahora mismo? Lo decide el núcleo con los
    // mismos criterios que a un jugador: nivel, raza, clase, requisitos
    // previos, hueco en el diario. Y que no la tenga ya ni la haya entregado.
    bool QuestSyncCanBotTake(Player* bot, Quest const* quest)
    {
        uint32 const questId = quest->GetQuestId();
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE || bot->GetQuestRewardStatus(questId))
            return false;

        return bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false);
    }

    // A diferencia de GiveQuest de mod-quest-mates, ESTO NUNCA empuja
    // "+new rpg": el bot debe seguir con "+follow" tal y como lo dejó
    // AdoptMaster, no irse a hacer la misión por su cuenta. Sólo se le mete
    // en el diario para que comparta el crédito de grupo y las copias de
    // botín de SP04; toda la mecánica de acompañar sigue siendo la de
    // siempre.
    void QuestSyncGiveQuest(Player* bot, Quest const* quest)
    {
        bot->AddQuestAndCheckCompletion(quest, nullptr);
    }

    // Misma secuencia que WorldSession::HandleQuestLogRemoveQuest. Sólo se
    // llama sobre misiones que Companion::syncedQuests dice que dio esta
    // sincronización (nunca una que el bot tuviera por otra causa).
    void QuestSyncAbandonBotQuest(Player* bot, uint32 questId)
    {
        uint16 const slot = bot->FindQuestSlot(questId);
        bot->TakeQuestSourceItem(questId, false);
        bot->AbandonQuest(questId);
        bot->RemoveActiveQuest(questId);
        if (slot < MAX_QUEST_LOG_SIZE)
            bot->SetQuestSlot(slot, 0);
    }

    // Se llama en cada punto de salida de un Companion (deja de ser tu
    // compañero por cualquier vía). Mejor esfuerzo: si el bot ya no se puede
    // resolver (se desconectó a la vez que se le saca del grupo), la copia se
    // queda en su diario sin reclamar el hueco; es la misma imprecisión que ya
    // asume mod-quest-mates en su CleanupMates con un bot no resuelto.
    void ReleaseSyncedQuests(ObjectGuid botGuid, std::set<uint32> const& synced)
    {
        if (synced.empty())
            return;
        Player* bot = ObjectAccessor::FindPlayer(botGuid);
        if (!bot)
            return;
        for (uint32 const questId : synced)
        {
            QuestStatus const status = bot->GetQuestStatus(questId);
            if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
                QuestSyncAbandonBotQuest(bot, questId);
        }
    }

    // "quieto"/"sigue": el bot se planta donde está o vuelve a seguirte. Es lo
    // que hacen "co -follow,+stay" y "co +follow,-stay" de playerbots.
    bool SetBotHold(ObjectGuid botGuid, bool hold)
    {
#ifdef PARTY_HERE_WITH_PLAYERBOTS
        if (Player* bot = ObjectAccessor::FindPlayer(botGuid))
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                char const* toggle = hold ? "-follow,+stay" : "+follow,-stay";
                botAI->ChangeStrategy(toggle, BOT_STATE_NON_COMBAT);
                botAI->ChangeStrategy(toggle, BOT_STATE_COMBAT);
                return true;
            }
#endif
        (void)botGuid; (void)hold;
        return false;
    }

    // Deshace la party: saca a cada companion del grupo del amo y le quita el
    // amo antes de soltar la reserva. Sin esto, al desconectarse el amo los bots
    // quedaban en un Group con lider offline (playerbots NO lo disuelve: solo
    // hace SetMaster(nullptr)), y como los claims ya estaban sueltos ningun
    // modulo los recuperaba -> GetGroup() != nullptr permanente, excluidos de
    // IsFreeBot para siempre (T5).
    void DissolveParty(Party& party)
    {
        for (Companion const& companion : party.bots)
        {
            ReleaseSyncedQuests(companion.bot, companion.syncedQuests);
            DetachBot(companion.bot, true);
        }
        party.bots.clear();
    }

    // Composición: cuántos tanques y sanadores quiere un grupo de 'size'.
    void Composition(uint32 size, uint32& tanks, uint32& healers)
    {
        // Un grupo de dos (".grupo 1") no fuerza rol: pedir "1 tanque" te daba
        // preferentemente un tanque en solitario (§4.2 P4).
        if (size <= 2)       { tanks = 0; healers = 0; }
        else if (size <= 5)  { tanks = 1; healers = 1; }
        else if (size <= 10) { tanks = 2; healers = 3; }
        else if (size <= 25) { tanks = 3; healers = 6; }
        else                 { tanks = 5; healers = 12; }

        // PartyHere.Tanks / PartyHere.Healers fijan la composición si no son 0
        // (útil para heroicas o bandas con dos sanadores). No se aplican al
        // grupo de dos.
        if (size > 2)
        {
            if (cfg.tanks)   tanks   = std::min(cfg.tanks, size);
            if (cfg.healers) healers = std::min(cfg.healers, size > tanks ? size - tanks : 0u);
        }
    }

    // En combate, a medio viaje o en una pantalla de carga: no se le puede
    // sacar del grupo ahora (M16).
    bool IsBusy(Player* bot)
    {
        return !bot->IsInWorld() || bot->IsInCombat() || bot->IsBeingTeleported();
    }

    struct BuildOptions
    {
        bool automatic = false;                    // vienen por misión de grupo
        std::set<uint32> const* quests = nullptr;  // misiones que los sostienen (automáticos)
        bool anyRole = true;   // cubrir con quien haya las plazas de tanque/sanador sin candidato
        bool wake    = true;   // despertar dormidos si faltan
    };

    struct BuildResult
    {
        uint32 joined  = 0;
        uint32 woken   = 0;
        bool   refused = false;   // estás en el grupo de otro
        uint32 missingTanks   = 0;   // plazas de rol que se han cubierto con otro rol (M26)
        uint32 missingHealers = 0;
        uint32 respecced      = 0;   // bots que han cambiado de especialización (M26)
    };

    // Forma o completa el grupo del jugador hasta 'size' miembros.
    BuildResult BuildGroup(Player* player, uint32 size, BuildOptions const& opts)
    {
        BuildResult result;
        ObjectGuid const guid = player->GetGUID();
        uint64_t const now = TimeMs::NowMs();

        Group* group = player->GetGroup();
        if (group && group->GetLeaderGUID() != guid)
        {
            TellError(player, ModLocale::L(player, "Ya estas en el grupo de otro: no se te forma ninguno."));
            result.refused = true;
            return result;
        }

        uint32 const current = group ? group->GetMembersCount() : 1;
        if (current >= size)
            return result;

        uint32 const level = player->GetLevel();
        uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        uint32 const minLevel = level > cfg.levelBelow ? level - cfg.levelBelow : 1;
        uint32 const topLevel = std::min(level + cfg.levelAbove, maxLevel);
        TeamId const team = player->GetTeamId();

        uint32 wantTanks = 0, wantHealers = 0;
        Composition(size, wantTanks, wantHealers);

        // Lo que ya hay (tú incluido).
        uint32 haveTanks = 0, haveHealers = 0;
        std::set<uint8> classes;
        auto count = [&](Player* member)
        {
            classes.insert(member->getClass());
            uint8 const role = BotRole(member);
            if (role == lfg::PLAYER_ROLE_TANK)        ++haveTanks;
            else if (role == lfg::PLAYER_ROLE_HEALER) ++haveHealers;
        };
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    count(member);
        }
        else
            count(player);

        uint32 const wantDps = size > wantTanks + wantHealers ? size - wantTanks - wantHealers : 0;
        uint32 const haveDps = current > haveTanks + haveHealers ? current - haveTanks - haveHealers : 0;

        uint32 const room = size - current;
        std::vector<Player*> chosen;

        // Los que el jugador echó o cambió hace poco no vuelven por esta
        // demanda (M16).
        std::set<ObjectGuid> taken;
        if (auto excluded = g_excluded.find(guid); excluded != g_excluded.end())
            for (auto const& [botGuid, untilMs] : excluded->second)
                if (now < untilMs)
                    taken.insert(botGuid);

        auto take = [&](uint8 role, uint32 wanted) -> uint32
        {
            uint32 got = 0;
            while (wanted && chosen.size() < room)
            {
                uint32 const want = std::min<uint32>(wanted, room - static_cast<uint32>(chosen.size()));
                std::vector<Player*> found = CollectBots(want, minLevel, topLevel, team, role, &classes, taken);
                if (found.empty())
                    found = CollectBots(want, minLevel, topLevel, team, role, nullptr, taken);
                if (found.empty())
                    break;
                for (Player* bot : found)
                {
                    chosen.push_back(bot);
                    taken.insert(bot->GetGUID());
                    classes.insert(bot->getClass());
                    --wanted;
                    ++got;
                }
            }
            return got;
        };

        // Pasada la espera de rol (M26): un bot libre de clase capaz cambia
        // de especialización. Sólo en grupos pedidos por el jugador; los de
        // misión son de paso y no justifican tocar los talentos de nadie.
        auto convert = [&](uint8 role, uint32 wanted) -> uint32
        {
            if (!wanted || !cfg.roleRespec || opts.automatic || !opts.anyRole)
                return 0;
            std::vector<Player*> pool;
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* bot = pair.second;
                if (!bot || taken.count(bot->GetGUID()) || RoleTab(bot->getClass(), role) < 0)
                    continue;
                if (bot->GetLevel() < std::max<uint32>(minLevel, 10) || bot->GetLevel() > topLevel)
                    continue;   // sin puntos de talento no hay rama que elegir
                if (bot->GetTeamId() != team || !IsFreeBot(bot))
                    continue;
                pool.push_back(bot);
            }
            Acore::Containers::RandomShuffle(pool);
            // Mejor uno de daño que uno que ya cubre el otro rol que falta.
            std::stable_partition(pool.begin(), pool.end(), [role](Player* bot)
            {
                uint8 const current = BotRole(bot);
                return current == lfg::PLAYER_ROLE_DAMAGE || current == role;
            });

            uint32 got = 0;
            for (Player* bot : pool)
            {
                if (got >= wanted || chosen.size() >= room)
                    break;
                // Comprobar -> reservar -> revalidar -> actuar.
                BotClaims::Lease claim(bot->GetGUID().GetCounter(), OWNER);
                if (!claim || !IsFreeBot(bot))
                    continue;
                uint8 const before = BotRole(bot);
                if (!Respec(bot, role))
                {
                    LOG_INFO("module", "[party-here] {} ({}, {}) no ha podido pasar a {}.",
                             bot->GetName(), bot->GetLevel(), RoleName(before), RoleName(role));
                    continue;
                }
                LOG_INFO("module", "[party-here] {} ({}) cambia de {} a {} para el grupo de {}: no habia {} libre de nivel {}-{}.",
                         bot->GetName(), bot->GetLevel(), RoleName(before), RoleName(role),
                         player->GetName(), RoleName(role), minLevel, topLevel);
                chosen.push_back(bot);
                taken.insert(bot->GetGUID());
                classes.insert(bot->getClass());
                ++got;
                ++result.respecced;
            }
            return got;
        };

        uint32 const needTanks   = wantTanks   > haveTanks   ? wantTanks   - haveTanks   : 0;
        uint32 const needHealers = wantHealers > haveHealers ? wantHealers - haveHealers : 0;
        uint32 gotTanks          = take(lfg::PLAYER_ROLE_TANK,   needTanks);
        uint32 gotHealers        = take(lfg::PLAYER_ROLE_HEALER, needHealers);
        gotTanks   += convert(lfg::PLAYER_ROLE_TANK,   needTanks   - gotTanks);
        gotHealers += convert(lfg::PLAYER_ROLE_HEALER, needHealers - gotHealers);
        result.missingTanks   = needTanks   - gotTanks;
        result.missingHealers = needHealers - gotHealers;
        take(lfg::PLAYER_ROLE_DAMAGE, wantDps > haveDps ? wantDps - haveDps : 0);
        // Lo que quede, cualquiera (grupo pequeño, o faltan bots de daño). Las
        // plazas de tanque/sanador sin candidato se guardan mientras la
        // petición espera a que llegue ese rol (M17), salvo con anyRole.
        uint32 const reserved = opts.anyRole ? 0 : (needTanks - gotTanks) + (needHealers - gotHealers);
        uint32 const left = room - static_cast<uint32>(chosen.size());
        take(0, left > reserved ? left - reserved : 0);

        if (chosen.empty())
        {
            LOG_INFO("module", "[party-here] Sin bots {}libres de nivel {}-{} ({}) para el grupo de {}{}.",
                     reserved ? "del rol que falta " : "", minLevel, topLevel,
                     team == TEAM_ALLIANCE ? "Alianza" : "Horda", player->GetName(),
                     opts.wake ? ": se despiertan" : "");
            if (opts.wake)
                result.woken = WakeBots(room * 2, minLevel, topLevel, team,
                                        opts.automatic ? 0 : result.missingTanks, opts.automatic ? 0 : result.missingHealers);
            return result;
        }

        if (!group)
        {
            group = new Group();
            if (!group->Create(player))
            {
                delete group;
                LOG_INFO("module", "[party-here] No se pudo crear el grupo de {}.", player->GetName());
                return result;
            }
            sGroupMgr->AddGroup(group);
        }

        if (size > 5 && !group->isRaidGroup())
            group->ConvertToRaid();

        Party& party = g_parties[guid];
        if (!opts.automatic)
            party.wantedSize = std::max(party.wantedSize, size);   // para reponer y recomponer tras relogin
        uint32 joined = 0;
        std::string names;
        for (Player* bot : chosen)
        {
            if (group->GetMembersCount() >= size)
                break;

            // Comprobar -> reservar -> revalidar -> actuar. El guard libera la
            // reserva nueva si cualquiera de los pasos posteriores falla.
            BotClaims::Lease claim(bot->GetGUID().GetCounter(), OWNER);
            if (!claim || !IsFreeBot(bot))
                continue;

            if (!group->AddMember(bot))
            {
                LOG_INFO("module", "[party-here] {} no ha podido entrar en el grupo de {}.", bot->GetName(), player->GetName());
                continue;
            }

            claim.Keep();

            AdoptMaster(bot, player);
            // Al consumirse (M02): sigue siendo compañero de ESTA party y va en
            // el grupo de su amo; si no, el trabajo se cancela.
            BotGear::Schedule(bot->GetGUID(), BotGear::TargetItemLevel(player, cfg.gear), cfg.gear, OWNER,
                BotGear::PRIORITY_GROUP,
                [human = guid, inGroup = BotGear::InGroupOf(guid, &cfg.gear)](Player* member, uint32& ilvl)
                {
                    // Una despedida pendiente (M16) también lo cancela.
                    auto partyIt = g_parties.find(human);
                    if (partyIt == g_parties.end() || std::none_of(partyIt->second.bots.begin(), partyIt->second.bots.end(),
                            [member](Companion const& c) { return c.bot == member->GetGUID() && !c.dismissPending; }))
                        return BotGear::Decision::Cancel;
                    return inGroup(member, ilvl);
                });

            Companion companion;
            companion.bot = bot->GetGUID();
            companion.automatic = opts.automatic;
            companion.zone = player->GetZoneId();
            companion.summonMs = cfg.summonSecs ? now + TimeMs::SecsToMs(cfg.summonSecs) + urand(0, 2000) : 0;
            companion.giveUpMs = now + TimeMs::SecsToMs(cfg.summonGiveUpSecs);
            party.bots.push_back(companion);

            ++joined;
            names += (names.empty() ? "" : ", ") + bot->GetName();
            LOG_INFO("module", "[party-here] {} ({}, {}) se une al grupo de {} ({}/{}{}).",
                     bot->GetName(), bot->GetLevel(),
                     BotRole(bot) == lfg::PLAYER_ROLE_TANK ? "tanque" : (BotRole(bot) == lfg::PLAYER_ROLE_HEALER ? "sanador" : "dano"),
                     player->GetName(), group->GetMembersCount(), size,
                     opts.automatic ? ", por mision de grupo" : "");
        }

        if (opts.automatic && opts.quests)
            party.autoQuests.insert(opts.quests->begin(), opts.quests->end());
        party.lingerUntilMs = 0;
        result.joined = joined;

        if (joined)
            Tell(player, Acore::StringFormat(ModLocale::L(player, "Se unen a tu grupo: {} ({}/{})."), names, group->GetMembersCount(), size));

        if (joined < room && opts.wake)
        {
            uint32 const missing = room - joined;
            LOG_INFO("module", "[party-here] Faltan {} para el grupo de {} (nivel {}-{}): se despiertan bots.",
                     missing, player->GetName(), minLevel, topLevel);
            result.woken = WakeBots(missing * 2, minLevel, topLevel, team,
                                    opts.automatic ? 0 : result.missingTanks, opts.automatic ? 0 : result.missingHealers);
        }

        return result;
    }

    // Saca del grupo a los bots que dio este módulo (todos, o sólo los
    // automáticos). Los que están en combate o viajando quedan con la
    // despedida pendiente y se van en cuanto terminen (M16): antes se
    // saltaban y, en un grupo manual, nadie volvía a pedir que se fueran.
    uint32 Dismiss(Player* player, Party& party, bool onlyAutomatic, char const* why)
    {
        Group* group = player ? player->GetGroup() : nullptr;
        uint32 gone = 0;
        uint32 pending = 0;

        for (auto it = party.bots.begin(); it != party.bots.end();)
        {
            if (onlyAutomatic && !it->automatic)
            {
                ++it;
                continue;
            }

            Player* bot = ObjectAccessor::FindConnectedPlayer(it->bot);
            if (bot && group && bot->GetGroup() == group)
            {
                if (IsBusy(bot))
                {
                    it->dismissPending = true;
                    it->replaceOnLeave = false;
                    it->summonMs = 0;
                    ++pending;
                    ++it;
                    continue;
                }
                ++gone;
                LOG_INFO("module", "[party-here] {} deja el grupo de {} ({}).", bot->GetName(), player->GetName(), why);
            }

            // DetachBot lo saca del grupo (si sigue), le quita el amo y resetea
            // la IA antes de soltar la reserva (§4.2 P4).
            ReleaseSyncedQuests(it->bot, it->syncedQuests);
            DetachBot(it->bot, true);
            it = party.bots.erase(it);
        }

        party.autoQuests.clear();
        party.lingerUntilMs = 0;

        if (gone)
            Tell(player, Acore::StringFormat(ModLocale::L(player, "{} companero(s) se despiden ({})."), gone, ModLocale::L(player, why)));
        if (pending)
            TellError(player, Acore::StringFormat(ModLocale::L(player, "{} companero(s) estan en combate o de viaje: se iran en cuanto terminen."), pending));

        return gone;
    }

    // ─── Peticiones pendientes (M17) ─────────────────────────────────────────
    char const* DemandReasonText(uint8 reason)
    {
        switch (reason)
        {
            case DEMAND_QUEST:   return "mision de grupo";
            case DEMAND_RESTORE: return "grupo de antes";
            case DEMAND_REPLACE: return "cambio de companero";
            default:             return "orden .grupo";
        }
    }

    enum class DemandStep { Wait, Done, Cancel };

    // Un intento de completar la petición. Revalida primero todo lo que pudo
    // cambiar desde que se pidió: la sesión, el grupo, las colas, la misión.
    DemandStep ServeDemand(ObjectGuid guid, Demand& demand, uint64 now, std::string& why)
    {
        Player* human = ObjectAccessor::FindConnectedPlayer(guid);
        WorldSession* session = human ? human->GetSession() : nullptr;
        if (!session || session->PlayerLogout() || session != demand.session)
            return DemandStep::Cancel;   // se fue: no hay a quién avisar

        if (now >= demand.expiresMs)
        {
            why = Acore::StringFormat("no han llegado companeros en {} s", cfg.pendingTimeoutSecs);   // ver LocalizedWhy
            return DemandStep::Cancel;
        }

        if (!human->IsInWorld() || session->PlayerLoading())
            return DemandStep::Wait;     // pantalla de carga: se sigue esperando

        Group* group = human->GetGroup();
        if (group && group->GetLeaderGUID() != guid)
        {
            why = "estas en el grupo de otro";
            return DemandStep::Cancel;
        }
        if (demand.hadGroup && !group)
        {
            why = "has dejado el grupo";
            return DemandStep::Cancel;
        }
        if (human->InBattleground() || human->InBattlegroundQueue() || human->InArena()
            || sLFGMgr->GetState(guid) != lfg::LFG_STATE_NONE)
        {
            why = "estas en una cola o en un campo de batalla";
            return DemandStep::Cancel;
        }

        if (demand.automatic)
        {
            // El evento de aceptar puede estar obsoleto: sólo cuentan las
            // misiones que siguen en el diario.
            for (auto q = demand.quests.begin(); q != demand.quests.end();)
            {
                QuestStatus const status = human->GetQuestStatus(*q);
                q = (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE) ? std::next(q) : demand.quests.erase(q);
            }
            if (demand.quests.empty())
            {
                why = "la mision de grupo ya no esta en tu diario";
                return DemandStep::Cancel;
            }
        }

        if ((group ? group->GetMembersCount() : 1) >= demand.size)
            return DemandStep::Done;

        if (now < demand.nextTryMs)
            return DemandStep::Wait;
        demand.nextTryMs = now + TimeMs::SecsToMs(cfg.pendingRetrySecs);

        // Los automáticos no se forman dentro de instancias (como antes): se
        // espera a que salga.
        if (demand.automatic)
            if (Map* map = human->GetMap(); !map || map->IsDungeon() || map->IsBattlegroundOrArena())
                return DemandStep::Wait;

        BuildOptions opts;
        opts.automatic = demand.automatic;
        opts.quests = &demand.quests;
        opts.anyRole = demand.automatic || now >= demand.roleDeadlineMs;
        opts.wake = now >= demand.nextWakeMs;
        BuildResult const result = BuildGroup(human, demand.size, opts);
        ++demand.attempts;
        if (result.refused)
            return DemandStep::Cancel;   // BuildGroup ya lo ha dicho

        if (result.woken)
            demand.nextWakeMs = now + TimeMs::SecsToMs(cfg.pendingWakeSecs);

        group = human->GetGroup();
        if (group)
            demand.hadGroup = true;
        return (group ? group->GetMembersCount() : 1) >= demand.size ? DemandStep::Done : DemandStep::Wait;
    }

    // con la composición REAL del grupo al terminar una petición manual
    // (completa o caducada), dice si falta tanque o sanador. Antes se
    // completaba en silencio con dps y la mazmorra no arrancaba; mirar el
    // resultado de cada intento avisaba de más (un sanador que llega no
    // significa que el tanque no vaya a llegar después).
    void WarnMissingRoles(Player* human, Demand const& demand, bool completed)
    {
        if (!human || demand.automatic || demand.size <= 2)
            return;
        uint32 wantTanks = 0, wantHealers = 0;
        Composition(demand.size, wantTanks, wantHealers);
        uint32 tanks = 0, healers = 0;
        if (Group* group = human->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                {
                    uint8 const role = BotRole(member);
                    if (role == lfg::PLAYER_ROLE_TANK)        ++tanks;
                    else if (role == lfg::PLAYER_ROLE_HEALER) ++healers;
                }
        }
        else
        {
            uint8 const role = BotRole(human);
            tanks = role == lfg::PLAYER_ROLE_TANK;
            healers = role == lfg::PLAYER_ROLE_HEALER;
        }
        bool const noTank = tanks < wantTanks;
        bool const noHealer = healers < wantHealers;
        if (!noTank && !noHealer)
            return;

        uint32 const level = human->GetLevel();
        uint32 const minLevel = level > cfg.levelBelow ? level - cfg.levelBelow : 1;
        uint32 const topLevel = std::min(level + cfg.levelAbove, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        char const* const missing = noTank && noHealer ? "tanque ni sanador" : (noTank ? "tanque" : "sanador");
        LOG_INFO("module", "[party-here] El grupo de {} {} sin {}: no hay bots de ese rol libres de nivel {}-{}{}.",
                 human->GetName(), completed ? "se completa" : "se queda", missing, minLevel, topLevel,
                 cfg.roleRespec ? " ni de una clase que pueda cambiar" : " (PartyHere.RoleRespec desactivado)");
        TellError(human, Acore::StringFormat(
            ModLocale::L(human, "No hay {} libre de nivel {}-{}: tu grupo {} sin el. Una mazmorra necesita tanque y sanador; "
            "prueba '.grupo cambia <nombre>' mas tarde o el buscador de mazmorras."),
            ModLocale::L(human, missing), minLevel, topLevel,
            completed ? ModLocale::L(human, "se completa") : ModLocale::L(human, "se queda")));
    }

    // El motivo de cancelar una petición se queda en español (la lógica busca "no han
    // llegado" y el log lo escribe tal cual); aquí se traduce sólo para el jugador.
    std::string LocalizedWhy(Player* human, std::string const& why)
    {
        static std::string const timeout = "no han llegado companeros en ";
        if (why.compare(0, timeout.size(), timeout) == 0)
            return Acore::StringFormat(ModLocale::L(human, "no han llegado companeros en {} s"), cfg.pendingTimeoutSecs);
        return ModLocale::L(human, why);
    }

    void PumpDemand(ObjectGuid guid, uint64 now)
    {
        auto it = g_demands.find(guid);
        if (it == g_demands.end())
            return;

        Demand& demand = it->second;
        std::string why;
        DemandStep const step = ServeDemand(guid, demand, now, why);
        Player* human = ObjectAccessor::FindConnectedPlayer(guid);
        Group* group = human ? human->GetGroup() : nullptr;
        uint32 const current = group ? group->GetMembersCount() : 1;

        if (step == DemandStep::Done)
        {
            LOG_INFO("module", "[party-here] Peticion de {} completada ({}/{}, {} intento(s)).",
                     human ? human->GetName() : "?", current, demand.size, demand.attempts);
            if (demand.announced && human)
                Tell(human, Acore::StringFormat(ModLocale::L(human, "Tu grupo esta completo ({}/{})."), current, demand.size));
            WarnMissingRoles(human, demand, true);
            g_demands.erase(it);
            return;
        }

        if (step == DemandStep::Cancel)
        {
            LOG_INFO("module", "[party-here] Peticion de companeros de {} cancelada{}{}.",
                     human ? human->GetName() : std::to_string(guid.GetCounter()), why.empty() ? "" : ": ", why);
            if (human && !why.empty())
                TellError(human, Acore::StringFormat(ModLocale::L(human, "Se cancela la peticion de companeros ({}/{}): {}."), current, demand.size, LocalizedWhy(human, why)));
            // Sólo si la petición caducó con compañeros dentro: si la canceló
            // el jugador (salió del grupo, entró en una cola) no hace falta.
            if (current > 1 && why.find("no han llegado") != std::string::npos)
                WarnMissingRoles(human, demand, false);
            g_demands.erase(it);
            return;
        }

        // Un solo aviso de espera; el progreso lo cuentan los "Se unen a tu
        // grupo" de cada llegada.
        if (!demand.announced && demand.attempts && human && human->IsInWorld())
        {
            demand.announced = true;
            TellError(human, Acore::StringFormat(
                ModLocale::L(human, "Preparando companeros ({}/{}): se estan despertando bots. Te aviso cuando esten (hasta {} s); '.grupo fuera' lo cancela."),
                current, demand.size, cfg.pendingTimeoutSecs));
        }
    }

    // Apunta (o actualiza) la petición del jugador y hace el primer intento.
    // Una manual sustituye a cualquier otra; una automática no pisa a una
    // manual pendiente (ésta ya da compañeros) y se suma a otra automática.
    void StartDemand(Player* player, uint32 size, uint8 reason, bool automatic,
                     std::set<uint32> const& quests, uint64 now)
    {
        ObjectGuid const guid = player->GetGUID();
        auto existing = g_demands.find(guid);
        if (automatic && existing != g_demands.end())
        {
            Demand& demand = existing->second;
            if (!demand.automatic)
                return;
            demand.size = std::max(demand.size, size);
            demand.quests.insert(quests.begin(), quests.end());
            demand.expiresMs = now + TimeMs::SecsToMs(cfg.pendingTimeoutSecs);
            demand.nextTryMs = 0;
            PumpDemand(guid, now);
            return;
        }

        Demand demand;
        demand.size = size;
        demand.automatic = automatic;
        demand.reason = reason;
        demand.quests = quests;
        demand.session = player->GetSession();
        demand.hadGroup = player->GetGroup() != nullptr;
        demand.expiresMs = now + TimeMs::SecsToMs(cfg.pendingTimeoutSecs);
        demand.roleDeadlineMs = now + TimeMs::SecsToMs(cfg.roleWaitSecs);
        g_demands[guid] = std::move(demand);
        PumpDemand(guid, now);
    }

    // Tras irse un compañero por ".grupo cambia", pedir otro: mismo tamaño y
    // misma naturaleza (automático o manual) que el que se fue.
    void StartReplacement(Player* player, Party& party, bool automatic, uint64 now)
    {
        Group* group = player->GetGroup();
        uint32 const current = group ? group->GetMembersCount() : 1;
        uint32 const size = (!automatic && party.wantedSize) ? party.wantedSize : current + 1;
        std::set<uint32> const quests = automatic ? party.autoQuests : std::set<uint32>();
        StartDemand(player, size, DEMAND_REPLACE, automatic, quests, now);
    }

    // ".grupo conserva [<nombre>]" (M19): convierte a los compañeros
    // automáticos ya presentes en manuales, sin tocar a los que ya lo eran.
    // Cancela cualquier despedida pendiente sobre ellos (fin de misión,
    // cambio de zona) y fija wantedSize al tamaño actual para que Replenish
    // los mantenga sin traer bots de más: sólo se conservan los que había, no
    // se amplía el grupo. La opción de ayuda temporal (compañeros de misión
    // nuevos) sigue disponible después: esto no toca cfg.autoQuests ni ningún
    // ajuste del módulo, sólo el estado de ESTA party.
    void KeepAsManual(Player* player, Party& party, std::string const& name, uint64 /*now*/)
    {
        Group* group = player->GetGroup();
        uint32 kept = 0;
        std::string names;
        for (Companion& companion : party.bots)
        {
            if (!companion.automatic)
                continue;   // ya es manual: nada que hacer
            if (!name.empty())
            {
                Player* bot = ObjectAccessor::FindPlayer(companion.bot);
                if (!bot || !NameEq(bot->GetName(), name))
                    continue;
            }
            companion.automatic = false;
            companion.dismissPending = false;
            companion.replaceOnLeave = false;
            Player* bot = ObjectAccessor::FindConnectedPlayer(companion.bot);
            names += (names.empty() ? "" : ", ") + (bot ? bot->GetName() : std::to_string(companion.bot.GetCounter()));
            ++kept;
        }

        if (!kept)
        {
            TellError(player, name.empty() ? std::string(ModLocale::L(player, "No tienes companeros automaticos que conservar."))
                                            : Acore::StringFormat(ModLocale::L(player, "'{}' no es un companero automatico que conservar."), name));
            return;
        }

        bool const anyAutomaticLeft = std::any_of(party.bots.begin(), party.bots.end(),
                                                  [](Companion const& c) { return c.automatic; });
        if (!anyAutomaticLeft)
            party.autoQuests.clear();   // ya no queda nadie que esas misiones sostengan

        uint32 const current = group ? group->GetMembersCount() : 1;
        party.wantedSize = std::max(party.wantedSize, current);   // no traer bots de mas
        party.lingerUntilMs = 0;   // cancela la cuenta atras de despedida por mision/zona

        Tell(player, Acore::StringFormat(ModLocale::L(player, "Se quedan como grupo manual: {} ({}/{})."), names, current, party.wantedSize));
    }

    // ".grupo fuera <nombre>" y ".grupo cambia <nombre>" (M16). Echar a uno
    // es una decisión del jugador: baja el tamaño que se repone y la petición
    // pendiente, y el bot no vuelve por esa demanda. Cambiarlo conserva el
    // tamaño y pide otro. Si está ocupado, la orden queda pendiente y visible.
    void DismissOne(Player* player, Party& party, std::vector<Companion>::iterator companion, bool replace, uint64 now)
    {
        ObjectGuid const guid = player->GetGUID();
        ObjectGuid const botGuid = companion->bot;
        bool const automatic = companion->automatic;
        Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
        std::string const name = bot ? bot->GetName() : std::to_string(botGuid.GetCounter());

        // Repetir la orden no vuelve a bajar el tamaño.
        if (companion->dismissPending)
        {
            TellError(player, Acore::StringFormat(ModLocale::L(player, "{} ya se va en cuanto termine."), name));
            return;
        }

        if (cfg.excludeMinutes)
            g_excluded[guid][botGuid] = now + TimeMs::MinsToMs(cfg.excludeMinutes);

        if (!replace)
        {
            if (!automatic && party.wantedSize)
                party.wantedSize = party.wantedSize > 2 ? party.wantedSize - 1 : 0;
            if (auto demand = g_demands.find(guid); demand != g_demands.end() && demand->second.automatic == automatic)
            {
                if (demand->second.size > 2)
                    --demand->second.size;
                else
                    g_demands.erase(demand);
            }
        }

        if (bot && IsBusy(bot))
        {
            companion->dismissPending = true;
            companion->replaceOnLeave = replace;
            companion->summonMs = 0;
            TellError(player, Acore::StringFormat(ModLocale::L(player, "{} esta en combate o de viaje: se ira en cuanto termine{}."),
                                                  name, replace ? ModLocale::L(player, " y vendra otro") : ""));
            return;
        }

        ReleaseSyncedQuests(botGuid, companion->syncedQuests);
        DetachBot(botGuid, true);
        party.bots.erase(companion);
        Tell(player, Acore::StringFormat(ModLocale::L(player, "{} se despide{}."), name, replace ? ModLocale::L(player, ": se busca otro companero") : ""));
        if (replace)
            StartReplacement(player, party, automatic, now);
    }

    // ─── Traer al bot a tu lado (la secuencia del "summon" de playerbots) ──
    enum Outcome { SUMMON_DONE, SUMMON_DROP, SUMMON_LATER };

    // Único punto que decide "me rindo": tras un minuto (companion.giveUpMs)
    // deja de reintentar y lo dice en el log; el nombre puede venir del bot
    // (si lo tenemos) o del GUID guardado (si ni eso: ver más abajo por qué).
    bool GiveUp(Player* human, Player* bot, ObjectGuid const& botGuid, Companion const& companion, uint64 now)
    {
        if (now <= companion.giveUpMs)
            return false;
        LOG_INFO("module", "[party-here] {} no se ha podido traer junto a {} en un minuto: se deja donde esta.",
                 bot ? bot->GetName() : Acore::StringFormat("bot#{}", botGuid.GetCounter()),
                 human ? human->GetName() : "?");
        return true;
    }

    Outcome TrySummon(Player* human, Companion& companion, uint64 now)
    {
        Player* bot = ObjectAccessor::FindPlayer(companion.bot);

        // Ausente/fuera de su grupo puede ser transitorio: un bot en pleno
        // teletransporte lejano (asíncrono, HandleTeleportAck) no está "en el
        // mundo" ni en su grupo durante un instante. Antes esto cortaba el
        // summon para siempre sin ningún aviso ("Jassanvy"/"Domdy" nunca
        // llegaban ni se veían en el log); ahora reintenta como cualquier otro
        // obstáculo temporal y sólo se rinde -con el mismo aviso de siempre-
        // al vencer companion.giveUpMs.
        if (!bot || !human || !bot->IsInWorld() || !human->IsInWorld())
            return GiveUp(human, bot, companion.bot, companion, now) ? SUMMON_DROP : SUMMON_LATER;

        Group* group = human->GetGroup();
        if (!group || bot->GetGroup() != group)
            return GiveUp(human, bot, companion.bot, companion, now) ? SUMMON_DROP : SUMMON_LATER;

        if (!BotWorldAge::IsMature(bot->GetGUID().GetCounter(), bot->GetSession(),
                                   now, cfg.botMinWorldSecs))
        {
            // La espera de asentamiento no consume el minuto disponible para
            // encontrar suelo y completar el summon.
            companion.giveUpMs = std::max(companion.giveUpMs, now + TimeMs::SecsToMs(cfg.summonGiveUpSecs));
            return SUMMON_LATER;
        }

        if (bot->GetMapId() == human->GetMapId() && bot->IsWithinDist(human, cfg.summonDistance, false))
            return SUMMON_DROP;

        if (GiveUp(human, bot, companion.bot, companion, now))
            return SUMMON_DROP;

        if (bot->IsBeingTeleported() || human->IsBeingTeleported() || bot->IsInFlight())
            return SUMMON_LATER;
        if (human->IsInCombat() || bot->IsInCombat() || !human->IsAlive())
            return SUMMON_LATER;
        if (human->InBattleground() || human->InArena() || human->GetVehicle() || bot->GetVehicle())
            return SUMMON_LATER;

        Map* map = human->GetMap();
        if (!map)
            return SUMMON_LATER;

        uint32 const mapId = human->GetMapId();

        // Hueco con línea de visión alrededor del jugador (M49: antes sólo
        // probaba 8 puntos a un único radio, y en terreno recortado -esquinas,
        // interiores, cornisas- podía fallar las veinte rondas de un minuto
        // entero de QueueBots.SummonGiveUpSeconds antes de rendirse, y el
        // rearme podía repetir el ciclo dos o tres veces (>90 s en total, sin
        // que el jugador se hubiera movido ni estuviera en combate). Ahora se
        // prueban dos radios -el pedido y uno más cercano- con el doble de
        // puntos por vuelta; si ninguno vale, la posición EXACTA del jugador
        // siempre es suelo válido (es donde está de pie ahora mismo), así que
        // se usa esa en vez de seguir esperando por un destino que ya se
        // conoce.
        float x = human->GetPositionX(), y = human->GetPositionY(), z = human->GetPositionZ();
        bool found = false;
        for (float const radius : { cfg.summonPlaceRadius, cfg.summonPlaceRadius * 0.4f })
        {
            float const start = frand(0.0f, 2.0f * static_cast<float>(M_PI));
            for (float angle = start; angle < start + 2.0f * static_cast<float>(M_PI); angle += static_cast<float>(M_PI) / 8.0f)
            {
                float const px = human->GetPositionX() + std::cos(angle) * radius;
                float const py = human->GetPositionY() + std::sin(angle) * radius;
                float pz = human->GetPositionZ();
                float const ground = map->GetHeight(human->GetPhaseMask(), px, py, pz + 2.0f);
                if (ground > INVALID_HEIGHT && std::fabs(ground - pz) < 5.0f)
                    pz = ground + 0.05f;
                if (human->IsWithinLOS(px, py, pz))
                {
                    x = px; y = py; z = pz;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }

        if (bot->isDead() && cfg.resurrectOnSummon)
        {
            bot->ResurrectPlayer(1.0f, false);
            bot->SpawnCorpseBones();
        }

        bot->GetMotionMaster()->Clear();
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        if (!bot->TeleportTo(mapId, x, y, z, human->GetOrientation()))
            return SUMMON_LATER;

        if (Pet* pet = bot->GetPet())
            pet->NearTeleportTo(x, y, z, human->GetOrientation());

        LOG_INFO("module", "[party-here] {} llega junto a {}.", bot->GetName(), human->GetName());
        return SUMMON_DONE;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Configuración y trabajo (OnUpdate del mundo)
// ─────────────────────────────────────────────────────────────────────────────
class mod_party_here_world : public WorldScript
{
public:
    mod_party_here_world() : WorldScript("mod_party_here_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        cfg.enabled     = sConfigMgr->GetOption<bool>("PartyHere.Enable", true);
        cfg.levelBelow  = sConfigMgr->GetOption<uint32>("PartyHere.LevelBelow", 3);
        cfg.levelAbove  = sConfigMgr->GetOption<uint32>("PartyHere.LevelAbove", 0);
        cfg.maxRaidBots = sConfigMgr->GetOption<uint32>("PartyHere.MaxRaidBots", 39);
        cfg.autoQuests  = sConfigMgr->GetOption<bool>("PartyHere.AutoGroupQuests", true);
        cfg.syncQuestsToGroup = sConfigMgr->GetOption<bool>("PartyHere.SyncQuestsToGroup", true);
        cfg.autoMaxBots = sConfigMgr->GetOption<uint32>("PartyHere.AutoMaxBots", 2);
        cfg.lingerSecs  = sConfigMgr->GetOption<uint32>("PartyHere.AutoLingerSeconds", 120);
        cfg.summonSecs  = sConfigMgr->GetOption<uint32>("PartyHere.SummonDelay", 3);
        cfg.wakeBots    = sConfigMgr->GetOption<bool>("PartyHere.WakeBots", true);
        cfg.wakeMax     = sConfigMgr->GetOption<uint32>("PartyHere.WakeMax", 20);
        cfg.wakeThrottleSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("PartyHere.WakeThrottleSeconds", 15), 1);
        cfg.announce    = sConfigMgr->GetOption<bool>("PartyHere.Announce", true);
        cfg.announceErrors = sConfigMgr->GetOption<bool>("PartyHere.AnnounceErrors", true);
        cfg.loadGraceSecs = sConfigMgr->GetOption<uint32>("PartyHere.LoadGraceSeconds", 30);
        cfg.botMinWorldSecs = sConfigMgr->GetOption<uint32>("PartyHere.BotMinWorldSeconds", 60);
        cfg.dismissAutoOnZoneChange = sConfigMgr->GetOption<bool>("PartyHere.DismissAutoOnZoneChange", false);
        cfg.scanIntervalMs = std::max<uint32>(sConfigMgr->GetOption<uint32>("PartyHere.ScanIntervalMs", 3000), 500);
        cfg.summonRetrySecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("PartyHere.SummonRetrySeconds", 3), 1);
        cfg.summonGiveUpSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("PartyHere.SummonGiveUpSeconds", 60), 5);
        cfg.summonDistance = std::max(sConfigMgr->GetOption<float>("PartyHere.SummonDistance", 40.0f), 5.0f);
        cfg.summonPlaceRadius = std::max(sConfigMgr->GetOption<float>("PartyHere.SummonPlaceRadius", 4.0f), 1.0f);
        cfg.maxGroupBots = sConfigMgr->GetOption<uint32>("PartyHere.MaxGroupBots", 0);
        cfg.tanks       = sConfigMgr->GetOption<uint32>("PartyHere.Tanks", 0);
        cfg.healers     = sConfigMgr->GetOption<uint32>("PartyHere.Healers", 0);
        cfg.autoQuestMinSuggested = sConfigMgr->GetOption<uint32>("PartyHere.AutoQuestMinSuggested", 0);
        cfg.resurrectOnSummon = sConfigMgr->GetOption<bool>("PartyHere.ResurrectOnSummon", true);
        cfg.replenish   = sConfigMgr->GetOption<bool>("PartyHere.Replenish", true);
        cfg.replenishThrottleSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("PartyHere.ReplenishThrottleSeconds", 15), 3);
        cfg.persistMinutes = sConfigMgr->GetOption<uint32>("PartyHere.PersistMinutes", 10);
        cfg.pendingTimeoutSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("PartyHere.PendingTimeoutSeconds", 180), 10);
        cfg.pendingRetrySecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("PartyHere.PendingRetrySeconds", 5), 1);
        cfg.pendingWakeSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("PartyHere.PendingWakeSeconds", 45), 5);
        cfg.roleWaitSecs = sConfigMgr->GetOption<uint32>("PartyHere.RoleWaitSeconds", 45);
        cfg.roleRespec   = sConfigMgr->GetOption<bool>("PartyHere.RoleRespec", false);
        cfg.excludeMinutes = sConfigMgr->GetOption<uint32>("PartyHere.DismissExcludeMinutes", 10);
        cfg.gear        = BotGear::LoadSettings("PartyHere");
        BotPopulationCoordinator::LoadSettings();

        // Normalizado ANTES de cualquier uso: size = std::clamp(size, 6, ...+1)
        // y n = std::clamp(*count, 1, ...) exigen maxRaidBots >= 5, o el clamp
        // es UB (lo == hi con lo > hi). El techo de MaxAddedBots sólo se aplica
        // si es un valor real: con 0 (config a medio poner) se ignora en vez de
        // forzar maxRaidBots a 0 y romper esa misma cota.
        cfg.maxRaidBots = std::clamp<uint32>(cfg.maxRaidBots, 5, 40);
        uint32 const maxAdded = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxAddedBots", 40);
        if (maxAdded > 0 && cfg.maxRaidBots > maxAdded)
            cfg.maxRaidBots = std::max<uint32>(maxAdded, 5);

#ifndef PARTY_HERE_WITH_PLAYERBOTS
        if (cfg.enabled)
        {
            LOG_INFO("module", "[party-here] Compilado sin mod-playerbots: no hay bots, el modulo no hace nada.");
            cfg.enabled = false;
        }
#endif

        // Espejos atómicos para los handlers de comando y hooks de misión (T7).
        g_enabledHot.store(cfg.enabled, std::memory_order_relaxed);
        g_autoQuestsHot.store(cfg.autoQuests, std::memory_order_relaxed);
        g_maxRaidBotsHot.store(cfg.maxRaidBots, std::memory_order_relaxed);
        g_cleanedWhileOff.store(false, std::memory_order_relaxed);   // rearmar el latch de T8
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        uint32 const t0 = getMSTime();
        DoUpdate();
        SlowTick::WarnIfSlow("party-here", "OnUpdate", t0);
    }

private:
    void DoUpdate()
    {
        if (!cfg.enabled)
        {
            if (!g_cleanedWhileOff.exchange(true))   // solo una vez por apagado (T8)
            {
                {
                    std::lock_guard<std::mutex> lock(g_pendingLock);
                    g_pending.clear();
                }
                for (auto const& party : g_parties)
                    for (Companion const& companion : party.second.bots)
                        BotClaims::Release(companion.bot.GetCounter(), OWNER);
                g_parties.clear();
                g_persisted.clear();
                g_demands.clear();
                g_excluded.clear();
                BotClaims::ReleaseAll(OWNER);

                // Última estadística con enabled=false (M07): sin ella el
                // panel seguía mostrando los grupos de antes del apagado.
                BotOperations::GroupStats stats;
                stats.updatedAtMs = TimeMs::NowMs();
                BotOperations::PublishGroupStats(std::move(stats));
            }
            BotOperations::SetTargetOffline(BotOperations::Target::PartyHere, TimeMs::NowMs(),
                                            "party-here esta desactivado.");
            return;
        }

        uint64_t const now = TimeMs::NowMs();

        std::deque<Request> requests;
        {
            std::lock_guard<std::mutex> lock(g_pendingLock);
            requests.swap(g_pending);
        }
        SlowTick::WarnIfDeep("party-here", "g_pending", requests.size());
        for (Request const& request : requests)
            Handle(request, now);

        // Acciones del panel (mod-bot-operations, via BotOperations.h):
        // "adelanta la proxima pasada" resetea el mismo throttle global que
        // el bucle de abajo respeta con g_nextScan.
        for (BotOperations::ActionRequest const& request : BotOperations::TakeRequests(BotOperations::Target::PartyHere, now))
        {
            if (request.type == BotOperations::ActionType::PartyHerePass)
            {
                g_nextScan = now;
                BotOperations::ReportOutcome(request.id, true, "Proximo cuidado de grupos/bandas adelantado.", now);
            }
            else
                BotOperations::ReportOutcome(request.id, false, "Accion no reconocida por party-here.", now);
        }

        // Podar los recuerdos de grupo de jugadores que no han vuelto a tiempo.
        if (cfg.persistMinutes)
            for (auto it = g_persisted.begin(); it != g_persisted.end();)
                it = (now > it->second.savedMs && now - it->second.savedMs > TimeMs::MinsToMs(cfg.persistMinutes))
                         ? g_persisted.erase(it) : std::next(it);
        else
            g_persisted.clear();

        // Presupuesto compartido con los demás módulos que llaman a ProcessOne
        // (ver BotGear.h): cuesta decenas de milisegundos reequipar a un bot.
        BotGear::ProcessOne();

        if (now < g_nextScan)
            return;
        g_nextScan = now + cfg.scanIntervalMs;

        // Exclusiones de M16 vencidas.
        for (auto it = g_excluded.begin(); it != g_excluded.end();)
        {
            for (auto bot = it->second.begin(); bot != it->second.end();)
                bot = now >= bot->second ? it->second.erase(bot) : std::next(bot);
            it = it->second.empty() ? g_excluded.erase(it) : std::next(it);
        }

        // Peticiones pendientes (M17). Se copian las claves: PumpDemand borra
        // la suya al completarse o cancelarse.
        std::vector<ObjectGuid> demanders;
        demanders.reserve(g_demands.size());
        for (auto const& pair : g_demands)
            demanders.push_back(pair.first);
        for (ObjectGuid const& guid : demanders)
            PumpDemand(guid, now);

        for (auto it = g_parties.begin(); it != g_parties.end();)
        {
            Player* human = ObjectAccessor::FindPlayer(it->first);
            Party& party = it->second;

            // Tres estados, no dos: "desconectado" (sin sesion) hay que
            // desmantelar la party; "cargando" (sesion viva, !IsInWorld: cambio
            // de mapa, piedra de hogar, vuelo entre continentes) es una ventana
            // de gracia -antes se destruia la party en plena pantalla de carga y
            // se perdia el relevo con queue-bots / .grupo fuera dejaba de
            // funcionar tras la carga (T5, variante).
            WorldSession* session = human ? human->GetSession() : nullptr;

            if (!session)   // desconectado del todo
            {
                DissolveParty(party);
                it = g_parties.erase(it);
                continue;
            }

            if (!human->IsInWorld())   // sesion viva pero cargando de mapa
            {
                if (!party.graceUntilMs)
                    party.graceUntilMs = now + TimeMs::SecsToMs(cfg.loadGraceSecs);
                else if (now >= party.graceUntilMs)
                {
                    LOG_INFO("module", "[party-here] El amo lleva demasiado sin volver al mundo: se disuelve la party.");
                    DissolveParty(party);
                    it = g_parties.erase(it);
                    continue;
                }
                ++it;
                continue;
            }

            party.graceUntilMs = 0;   // de vuelta en el mundo

            Tick(human, party, now);

            if (party.bots.empty() && party.autoQuests.empty())
                it = g_parties.erase(it);
            else
                ++it;
        }

        // Estadisticas para el panel (pestaña "Colas y grupos"): solo este
        // OnUpdate es dueño de g_parties.
        {
            BotOperations::GroupStats stats;
            stats.enabled = cfg.enabled;
            for (auto const& pair : g_parties)
            {
                Party const& party = pair.second;
                if (party.bots.empty())
                    continue;
                stats.botsInGroups += static_cast<uint32_t>(party.bots.size());
                Player* human = ObjectAccessor::FindPlayer(pair.first);
                Group* group = human ? human->GetGroup() : nullptr;
                if (group && group->isRaidGroup())
                    ++stats.raidsFormed;
                else
                    ++stats.groupsFormed;
            }
            stats.updatedAtMs = now;
            BotOperations::PublishGroupStats(std::move(stats));
        }
    }

private:
    void Handle(Request const& request, uint64 now)
    {
        if (request.kind == REQ_LOGOUT)
        {
            g_demands.erase(request.human);   // una petición no sobrevive a su sesión (M17)
            auto it = g_parties.find(request.human);
            if (it != g_parties.end())
            {
                // Antes de disolver, si había un grupo MANUAL (wantedSize > 0;
                // los automáticos de misión nunca lo fijan) y PersistMinutes lo
                // permite, se guarda su tamaño para recomponerlo al volver.
                // Antes sólo se guardaba con wantedSize > 5, así que un grupo
                // de cinco (".grupo"/".grupo mazmorra", el tamaño más
                // frecuente) nunca sobrevivía a un logout mientras que uno de
                // seis sí (M50: hallazgo de M29, sin justificación aparte de
                // "así estaba escrito"). ".grupo fuera <nombre>" y las bajas
                // individuales ya bajan wantedSize antes de llegar aquí (ver
                // Dismiss/DismissOne), así que lo que se guarda es siempre el
                // tamaño real que quedaba, y ".grupo fuera" (todos) borra el
                // recuerdo aparte (REQ_DISMISS).
                if (cfg.persistMinutes && it->second.wantedSize > 1)
                {
                    bool anyAuto = false;
                    for (Companion const& c : it->second.bots)
                        if (c.automatic) { anyAuto = true; break; }
                    if (!anyAuto && !it->second.bots.empty())
                        g_persisted[request.human] = { it->second.wantedSize, now };
                }
                DissolveParty(it->second);
                g_parties.erase(it);
            }
            return;
        }

        if (request.kind == REQ_RESTORE)
        {
            auto it = g_persisted.find(request.human);
            if (it == g_persisted.end())
                return;
            uint32 const wanted = it->second.wantedSize;
            uint64 const savedMs = it->second.savedMs;
            g_persisted.erase(it);
            if (!cfg.persistMinutes || now < savedMs || now - savedMs > TimeMs::MinsToMs(cfg.persistMinutes))
                return;
            // FindConnectedPlayer: puede estar aún en la pantalla de carga. El
            // recuerdo pasa a ser una petición pendiente (M17): antes se
            // borraba y, sin bots despiertos, la banda no volvía.
            Player* p = ObjectAccessor::FindConnectedPlayer(request.human);
            if (!p || !p->GetSession() || p->GetSession()->IsHeadless() || p->GetGroup())
                return;
            Tell(p, Acore::StringFormat(ModLocale::L(p, "Se recompone tu grupo de antes ({} miembros)."), wanted));
            StartDemand(p, wanted, DEMAND_RESTORE, false, {}, now);
            return;
        }

        Player* player = ObjectAccessor::FindPlayer(request.human);
        if (!IsHuman(player))
            return;

        switch (request.kind)
        {
            case REQ_GROUP:
            {
                uint32 size = request.value ? request.value + 1 : 5;
                if (size > 5)
                {
                    // Tope para ".grupo N": PartyHere.MaxGroupBots si está puesto,
                    // si no MaxRaidBots. Siempre 40 miembros como máximo, no 41.
                    uint32 const botCap = cfg.maxGroupBots ? cfg.maxGroupBots : cfg.maxRaidBots;
                    size = std::min<uint32>(size, std::min<uint32>(botCap + 1, 40));
                }
                StartDemand(player, size, DEMAND_COMMAND, false, {}, now);
                break;
            }
            case REQ_RAID:
            {
                uint32 size = request.value;
                if (!size)
                {
                    Difficulty const raid = player->GetRaidDifficulty();
                    size = (raid == RAID_DIFFICULTY_25MAN_NORMAL || raid == RAID_DIFFICULTY_25MAN_HEROIC) ? 25 : 10;
                }
                size = std::clamp<uint32>(size, 6, std::min<uint32>(cfg.maxRaidBots + 1, 40));   // tope real de una banda: 40
                StartDemand(player, size, DEMAND_COMMAND, false, {}, now);
                break;
            }
            case REQ_DISMISS:
            {
                // Despedir a todos cancela todo lo que traería bots otra vez
                // (M16): la petición pendiente, la reposición y el recuerdo
                // para después de un relogin.
                bool const hadDemand = g_demands.erase(request.human) > 0;
                g_persisted.erase(request.human);
                auto it = g_parties.find(request.human);
                if (it != g_parties.end())
                    it->second.wantedSize = 0;
                if (it == g_parties.end() || it->second.bots.empty())
                {
                    if (hadDemand)
                        Tell(player, ModLocale::L(player, "Peticion de companeros cancelada."));
                    else
                        TellError(player, ModLocale::L(player, "No tienes companeros de este modulo en el grupo."));
                    break;
                }
                Dismiss(player, it->second, false, "a peticion tuya");
                if (hadDemand)
                    Tell(player, ModLocale::L(player, "Peticion de companeros pendiente cancelada."));
                break;
            }
            case REQ_KEEP:
            {
                auto it = g_parties.find(request.human);
                if (it == g_parties.end() || it->second.bots.empty())
                {
                    TellError(player, ModLocale::L(player, "No tienes companeros de este modulo en el grupo."));
                    break;
                }
                KeepAsManual(player, it->second, request.text, now);
                break;
            }
            case REQ_DISMISS_ONE:
            case REQ_REPLACE_ONE:
            {
                auto it = g_parties.find(request.human);
                if (it == g_parties.end() || it->second.bots.empty())
                {
                    TellError(player, ModLocale::L(player, "No tienes companeros de este modulo en el grupo."));
                    break;
                }
                // FindConnectedPlayer: un compañero en pantalla de carga sigue
                // siendo tuyo y se le puede despedir (queda pendiente).
                auto b = std::find_if(it->second.bots.begin(), it->second.bots.end(), [&request](Companion const& c)
                {
                    Player* bot = ObjectAccessor::FindConnectedPlayer(c.bot);
                    return bot && NameEq(bot->GetName(), request.text);
                });
                if (b == it->second.bots.end())
                {
                    TellError(player, Acore::StringFormat(ModLocale::L(player, "No tienes ningun companero llamado '{}'."), request.text));
                    break;
                }
                DismissOne(player, it->second, b, request.kind == REQ_REPLACE_ONE, now);
                break;
            }
            case REQ_HOLD:
            case REQ_FOLLOW:
            {
                auto it = g_parties.find(request.human);
                if (it == g_parties.end() || it->second.bots.empty())
                {
                    TellError(player, ModLocale::L(player, "No tienes companeros de este modulo en el grupo."));
                    break;
                }
                bool const hold = request.kind == REQ_HOLD;
                uint32 n = 0;
                for (Companion& companion : it->second.bots)
                {
                    if (!request.text.empty())
                    {
                        Player* bot = ObjectAccessor::FindPlayer(companion.bot);
                        if (!bot || !NameEq(bot->GetName(), request.text))
                            continue;
                    }
                    SetBotHold(companion.bot, hold);
                    // held gobierna el summon (Tick), aparte de si playerbots
                    // ha podido cambiarle la estrategia ahora mismo (M20): un
                    // bot sin sesión resuelta también debe quedar marcado.
                    companion.held = hold;
                    if (!hold && !companion.dismissPending)
                    {
                        // Reanudar viaje: recalcular destino y vigencia. No
                        // arrastrar un summonMs/giveUpMs que vencieron mientras
                        // estuvo quieto (daría un DROP inmediato por "minuto
                        // agotado" en vez de intentarlo de verdad).
                        companion.summonMs = now + TimeMs::SecsToMs(cfg.summonRetrySecs);
                        companion.giveUpMs = now + TimeMs::SecsToMs(cfg.summonGiveUpSecs);
                    }
                    ++n;
                }
                if (!n && !request.text.empty())
                    TellError(player, Acore::StringFormat(ModLocale::L(player, "No tienes ningun companero llamado '{}'."), request.text));
                else
                    Tell(player, Acore::StringFormat(ModLocale::L(player, "{} companero(s) {}."), n,
                                                     hold ? ModLocale::L(player, "se quedan quietos") : ModLocale::L(player, "vuelven a seguirte")));
                break;
            }
            case REQ_STATUS:
            {
                ChatHandler handler(player->GetSession());
                auto it = g_parties.find(request.human);
                auto demand = g_demands.find(request.human);
                bool const hasBots = it != g_parties.end() && !it->second.bots.empty();
                if (!hasBots && demand == g_demands.end())
                {
                    handler.SendSysMessage(ModLocale::L(handler, "Sin companeros de este modulo. Usa .grupo, .grupo N o .grupo banda [10|25|40]."));
                    break;
                }
                if (demand != g_demands.end())
                {
                    Group* group = player->GetGroup();
                    handler.PSendSysMessage(ModLocale::L(handler, "Preparando companeros ({}): {}/{}; se deja de esperar en {} s."),
                        ModLocale::L(handler, DemandReasonText(demand->second.reason)), group ? group->GetMembersCount() : 1u, demand->second.size,
                        demand->second.expiresMs > now ? (demand->second.expiresMs - now) / 1000 : 0);
                }
                if (hasBots)
                    for (Companion const& companion : it->second.bots)
                    {
                        Player* bot = ObjectAccessor::FindConnectedPlayer(companion.bot);
                        if (!bot)
                            continue;
                        handler.PSendSysMessage(ModLocale::L(handler, "  {} (nivel {}, {}{}{}{})"), bot->GetName(), bot->GetLevel(),
                            ModLocale::L(handler, BotRole(bot) == lfg::PLAYER_ROLE_TANK ? "tanque" : (BotRole(bot) == lfg::PLAYER_ROLE_HEALER ? "sanador" : "dano")),
                            companion.automatic ? ModLocale::L(handler, ", por mision de grupo") : "",
                            companion.held ? ModLocale::L(handler, ", quieto") : "",
                            companion.dismissPending ? (companion.replaceOnLeave ? ModLocale::L(handler, ", se cambia al terminar el combate")
                                                                                  : ModLocale::L(handler, ", se va al terminar el combate")) : "");
                    }
                break;
            }
            case REQ_QUEST_ACCEPT:
            {
                if (!cfg.autoQuests)
                    break;
                Quest const* quest = sObjectMgr->GetQuestTemplate(request.value);
                if (!quest || quest->GetSuggestedPlayers() < std::max<uint32>(cfg.autoQuestMinSuggested, 1))
                    break;

                Map* map = player->GetMap();
                if (!map || map->IsDungeon() || map->IsBattlegroundOrArena())
                    break;

                // Sólo si el grupo actual no llega a lo sugerido.
                uint32 const suggested = std::min<uint32>(quest->GetSuggestedPlayers(), cfg.autoMaxBots + 1);
                uint32 const current = player->GetGroup() ? player->GetGroup()->GetMembersCount() : 1;
                if (current >= suggested)
                {
                    g_parties[request.human].autoQuests.insert(request.value);
                    break;
                }

                LOG_INFO("module", "[party-here] '{}' sugiere {} jugadores: se buscan {} companeros para {}.",
                         quest->GetTitle(), quest->GetSuggestedPlayers(), suggested - current, player->GetName());
                StartDemand(player, suggested, DEMAND_QUEST, true, { request.value }, now);
                break;
            }
            case REQ_QUEST_DONE:
            {
                // OnPlayerCompleteQuest salta al cumplir OBJETIVOS, no al
                // entregar en el PNJ: si la misión sigue en el diario (COMPLETE,
                // pendiente de recompensa) no se echa a los bots todavía — el
                // Tick los suelta cuando pase a REWARDED. El abandono
                // (QUEST_STATUS_NONE) sí es inmediato (§4.2 P3).
                QuestStatus const status = player->GetQuestStatus(request.value);
                if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
                    break;
                // Una petición automática que se quede sin misiones se cancela
                // en su siguiente intento (ServeDemand), con aviso.
                if (auto demand = g_demands.find(request.human); demand != g_demands.end() && demand->second.automatic)
                    demand->second.quests.erase(request.value);
                auto it = g_parties.find(request.human);
                if (it == g_parties.end())
                    break;
                it->second.autoQuests.erase(request.value);
                if (it->second.autoQuests.empty() && !it->second.lingerUntilMs)
                    it->second.lingerUntilMs = now + TimeMs::SecsToMs(cfg.lingerSecs);
                break;
            }
        }
    }

    void Tick(Player* human, Party& party, uint64 now)
    {
        Group* group = human->GetGroup();

        // Bots que ya no están en el grupo (los echaste, se fueron): se olvidan.
        // FindConnectedPlayer: uno en pantalla de carga sigue en el grupo; con
        // FindPlayer se le soltaba la reserva sin sacarlo de él.
        for (auto it = party.bots.begin(); it != party.bots.end();)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(it->bot);
            if (!bot || !group || bot->GetGroup() != group)
            {
                ReleaseSyncedQuests(it->bot, it->syncedQuests);
                DetachBot(it->bot, false);   // ya está fuera del grupo: solo IA + reserva
                it = party.bots.erase(it);
            }
            else
                ++it;
        }

        // Despedidas pendientes (M16): se van en cuanto dejan de estar ocupados.
        std::string leaving;
        bool replace = false;
        bool replaceAutomatic = false;
        for (auto it = party.bots.begin(); it != party.bots.end();)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(it->bot);
            if (!it->dismissPending || !bot || IsBusy(bot))
            {
                ++it;
                continue;
            }
            leaving += (leaving.empty() ? "" : ", ") + bot->GetName();
            if (it->replaceOnLeave)
            {
                replace = true;
                replaceAutomatic = it->automatic;
            }
            LOG_INFO("module", "[party-here] {} deja el grupo de {} (despedida pendiente).", bot->GetName(), human->GetName());
            ReleaseSyncedQuests(it->bot, it->syncedQuests);
            DetachBot(it->bot, true);
            it = party.bots.erase(it);
        }
        if (!leaving.empty())
            Tell(human, Acore::StringFormat(ModLocale::L(human, "Se despiden: {}."), leaving));
        if (replace)
            StartReplacement(human, party, replaceAutomatic, now);
        group = human->GetGroup();   // sacar al último bot deshace el grupo

        // Traerlos a tu lado.
        for (Companion& companion : party.bots)
        {
            // Retenido (M20): ni el primer summon ni un reintento aplazado lo
            // traen mientras siga quieto, aunque summonMs ya haya vencido.
            if (companion.dismissPending || companion.held)
                continue;

            // summonMs sólo se arma al unirse (§join) o al soltar ".grupo
            // quieto" (§sigue, M20): en cuanto TrySummon termina (llega o se
            // rinde) lo deja en 0 para siempre. Si el jugador viaja lejos
            // DESPUES (cambio de continente por GM, `.go`/`.tele`...) nada
            // volvía a mirar la distancia y el companero se quedaba donde
            // estaba sin aviso ni reintento (PLAN M28, hallazgo 25/09/2026:
            // el grupo sobrevivía pero la presencia del companero no). Un
            // companero resuelto que ahora está lejos de verdad (otro mapa o
            // fuera de SummonDistance) se rearma con un giveUpMs nuevo; uno
            // que sigue cerca no cuesta nada extra (el chequeo es barato).
            if (!companion.summonMs)
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(companion.bot);
                if (bot && bot->IsInWorld() && !bot->IsBeingTeleported()
                    && (bot->GetMapId() != human->GetMapId() || !bot->IsWithinDist(human, cfg.summonDistance, false)))
                {
                    companion.summonMs = now;
                    companion.giveUpMs = now + TimeMs::SecsToMs(cfg.summonGiveUpSecs);
                }
            }

            if (!companion.summonMs || now < companion.summonMs)
                continue;
            switch (TrySummon(human, companion, now))
            {
                case SUMMON_LATER: companion.summonMs = now + TimeMs::SecsToMs(cfg.summonRetrySecs); break;
                default:           companion.summonMs = 0;          break;
            }
        }

        // SP05: sincronizar tus misiones activas a tus compañeros (manuales o
        // automáticos), para que también puedan aprovechar sus propias copias
        // de botín de misión de mod-quest-loot-party (SP04). No pide ninguna
        // reserva nueva de BotClaims: el bot ya es tuyo mientras está en tu
        // Group. Se retira aquí mismo en cuanto tú abandones/entregues la
        // misión o el bot deje de tenerla por otra vía; al dejar de ser tu
        // compañero por cualquier otra causa, la retira ReleaseSyncedQuests
        // en su punto de salida correspondiente.
        if (cfg.syncQuestsToGroup)
            for (Companion& companion : party.bots)
            {
                if (companion.dismissPending)
                    continue;
                Player* bot = ObjectAccessor::FindConnectedPlayer(companion.bot);
                if (!bot || !bot->IsInWorld() || !group || bot->GetGroup() != group)
                    continue;

                for (auto synced = companion.syncedQuests.begin(); synced != companion.syncedQuests.end();)
                {
                    QuestStatus const humanStatus = human->GetQuestStatus(*synced);
                    QuestStatus const botStatus = bot->GetQuestStatus(*synced);
                    bool const stillOpen = (humanStatus == QUEST_STATUS_INCOMPLETE || humanStatus == QUEST_STATUS_COMPLETE)
                                         && (botStatus == QUEST_STATUS_INCOMPLETE || botStatus == QUEST_STATUS_COMPLETE);
                    if (stillOpen)
                    {
                        ++synced;
                        continue;
                    }
                    if (botStatus == QUEST_STATUS_INCOMPLETE || botStatus == QUEST_STATUS_COMPLETE)
                        QuestSyncAbandonBotQuest(bot, *synced);
                    synced = companion.syncedQuests.erase(synced);
                }

                for (auto const& [questId, statusData] : human->getQuestStatusMap())
                {
                    // COMPLETE también cuenta (objetivos ya hechos, sin entregar
                    // todavía): un `.quest add` sin objetivos reales -o unos ya
                    // cumplidos por casualidad- deja al jugador en COMPLETE de
                    // inmediato, no en INCOMPLETE, y aun así sigue siendo "su
                    // misión" mientras no la entregue.
                    bool const humanStillHasIt = statusData.Status == QUEST_STATUS_INCOMPLETE
                                              || statusData.Status == QUEST_STATUS_COMPLETE;
                    if (!humanStillHasIt || companion.syncedQuests.count(questId))
                        continue;
                    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                    if (!quest || !IsSyncableQuestType(quest) || !QuestSyncCanBotTake(bot, quest))
                        continue;

                    QuestSyncGiveQuest(bot, quest);
                    if (bot->GetQuestStatus(questId) == QUEST_STATUS_NONE)
                        continue;   // AddQuestAndCheckCompletion pudo rechazarla igual con la prevalidación en verde

                    companion.syncedQuests.insert(questId);
                    LOG_INFO("module", "[party-here] {} tambien lleva '{}' de {} (companero de grupo).",
                             bot->GetName(), quest->GetTitle(), human->GetName());
                    if (cfg.announce)
                        Tell(human, Acore::StringFormat(ModLocale::L(human, "{} tambien lleva tu mision \"{}\"."), bot->GetName(), quest->GetTitle()));
                }
            }

        // Los automáticos: si cambiaste de zona (fuera de instancias) o la
        // misión ya no está, empieza la cuenta atrás; al acabar, se van.
        bool hasAuto = false;
        bool leftZone = false;
        Map* map = human->GetMap();
        bool const inInstance = map && (map->IsDungeon() || map->IsBattlegroundOrArena());
        for (Companion const& companion : party.bots)
        {
            if (!companion.automatic || companion.dismissPending)
                continue;
            hasAuto = true;
            if (!inInstance && companion.zone && companion.zone != human->GetZoneId())
                leftZone = true;
        }

        if (hasAuto)
        {
            // Misiones automáticas que ya no están en el diario.
            for (auto it = party.autoQuests.begin(); it != party.autoQuests.end();)
            {
                QuestStatus const status = human->GetQuestStatus(*it);
                it = (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE) ? std::next(it) : party.autoQuests.erase(it);
            }

            // Cambiar de zona solo echa a los automáticos si su misión ya no
            // está activa; muchas misiones "(Grupo)" se aceptan en un sitio y se
            // hacen o entregan en otro (§4.2 P3). PartyHere.DismissAutoOnZoneChange
            // restaura el comportamiento antiguo (echar siempre al cambiar de zona).
            bool const dismissOnZone = leftZone && cfg.dismissAutoOnZoneChange;

            if ((party.autoQuests.empty() || dismissOnZone) && !inInstance)
            {
                if (!party.lingerUntilMs)
                    party.lingerUntilMs = now + TimeMs::SecsToMs(cfg.lingerSecs);
                else if (now >= party.lingerUntilMs)
                    Dismiss(human, party, true,
                            (dismissOnZone && !party.autoQuests.empty()) ? "has cambiado de zona" : "mision terminada");
            }
            else
                party.lingerUntilMs = 0;
        }

        // Reponer composición: un grupo manual (".grupo N"/".grupo banda") que
        // ha perdido bots (muertos y expulsados, echados a mano, se fueron) se
        // vuelve a llenar hasta su tamaño, fuera de instancias y sin cuenta
        // atrás en marcha. BuildGroup ya respeta la composición y el "current >=
        // size -> nada".
        //
        // Si el humano ya no tiene grupo (salió por el cliente, no con
        // ".grupo fuera"), NO es "perdió bots": es que se fue él. BuildGroup con
        // group==nullptr forma uno nuevo y te vuelve a meter como líder -> "me
        // saco del grupo y me vuelve a meter". wantedSize a 0 lo trata como una
        // salida deliberada, igual que REQ_DISMISS.
        //
        // Con una petición pendiente (M17) no se repone: ya la está
        // completando PumpDemand, y dos caminos llenando a la vez despertarían
        // el doble. Las bajas decididas por el jugador ya han bajado
        // wantedSize (M16), así que aquí sólo se reponen las involuntarias.
        if (!group)
            party.wantedSize = 0;
        else if (cfg.replenish && party.wantedSize && !hasAuto && !inInstance && !party.lingerUntilMs
            && now >= party.nextReplenishMs && !g_demands.count(human->GetGUID()))
        {
            uint32 const current = group->GetMembersCount();
            if (current < party.wantedSize)
            {
                party.nextReplenishMs = now + TimeMs::SecsToMs(cfg.replenishThrottleSecs);
                uint32 const added = BuildGroup(human, party.wantedSize, BuildOptions()).joined;
                if (added)
                    LOG_INFO("module", "[party-here] {} companero(s) repuestos en el grupo de {}.",
                             added, human->GetName());
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Hooks de misión (hilo del mapa: sólo apuntan)
// ─────────────────────────────────────────────────────────────────────────────
class mod_party_here_player : public PlayerScript
{
public:
    mod_party_here_player() : PlayerScript("mod_party_here_player",
        { PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT, PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_QUEST_ABANDON,
          PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerLogin(Player* player) override
    {
        // Al volver a entrar, recomponer el grupo manual que tenías (si es
        // pronto). El hilo del mundo comprueba PersistMinutes y que no traigas
        // ya un grupo.
        if (g_enabledHot.load(std::memory_order_relaxed) && player && player->GetSession()
            && !player->GetSession()->IsHeadless())
            Push({ player->GetGUID(), REQ_RESTORE, 0 });
    }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (g_enabledHot.load(std::memory_order_relaxed) && g_autoQuestsHot.load(std::memory_order_relaxed)
            && quest && IsHuman(player) && quest->GetSuggestedPlayers() > 0)
            Push({ player->GetGUID(), REQ_QUEST_ACCEPT, quest->GetQuestId() });
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (g_enabledHot.load(std::memory_order_relaxed) && quest && IsHuman(player))
            Push({ player->GetGUID(), REQ_QUEST_DONE, quest->GetQuestId() });
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        if (g_enabledHot.load(std::memory_order_relaxed) && IsHuman(player))
            Push({ player->GetGUID(), REQ_QUEST_DONE, questId });
    }

    void OnPlayerLogout(Player* player) override
    {
        // Cualquier personaje: la próxima sesión de un bot vuelve a esperar
        // BotMinWorldSeconds antes de traerlo (M08).
        if (player)
            BotWorldAge::Forget(player->GetGUID().GetCounter());

        // Solo humanos: playerbots/MakeRoom desconectan bots constantemente y
        // g_parties está indexado por humano, así que REQ_LOGOUT con GUID de bot
        // era ruido de cola (§4.2 P3). La baja de un companion la ve el Tick.
        if (player && player->GetSession() && !player->GetSession()->IsHeadless())
            Push({ player->GetGUID(), REQ_LOGOUT, 0 });
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  El comando .grupo (sólo apunta la petición)
// ─────────────────────────────────────────────────────────────────────────────
class mod_party_here_command : public CommandScript
{
public:
    mod_party_here_command() : CommandScript("mod_party_here_command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable grupoTable =
        {
            { "mazmorra", HandleDungeon, SEC_PLAYER, Console::No },
            { "banda",    HandleRaid,    SEC_PLAYER, Console::No },
            { "fuera",    HandleDismiss, SEC_PLAYER, Console::No },
            { "cambia",   HandleReplace, SEC_PLAYER, Console::No },
            { "conserva", HandleKeep,    SEC_PLAYER, Console::No },
            { "quieto",   HandleHold,    SEC_PLAYER, Console::No },
            { "sigue",    HandleFollow,  SEC_PLAYER, Console::No },
            { "estado",   HandleStatus,  SEC_PLAYER, Console::No },
            { "",         HandleGroup,   SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "grupo", grupoTable },
        };
        return commandTable;
    }

    static bool Ready(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!g_enabledHot.load(std::memory_order_relaxed))
        {
            handler->SendSysMessage(ModLocale::L(handler, "mod-party-here esta desactivado."));
            return false;
        }
        if (!IsHuman(player))
            return false;
        if (player->InBattleground() || player->InBattlegroundQueue() || player->InArena())
        {
            handler->SendSysMessage(ModLocale::L(handler, "No se forma grupo dentro de un campo de batalla, una arena o su cola."));
            return false;
        }
        if (sLFGMgr->GetState(player->GetGUID()) != lfg::LFG_STATE_NONE)
        {
            handler->SendSysMessage(ModLocale::L(handler, "Estas en el buscador: de esa cola se ocupa mod-queue-bots."));
            return false;
        }
        return true;
    }

    static bool HandleGroup(ChatHandler* handler, Optional<uint32> count)
    {
        if (!Ready(handler))
            return true;
        uint32 const n = count ? std::clamp<uint32>(*count, 1, g_maxRaidBotsHot.load(std::memory_order_relaxed)) : 0;
        Push({ handler->GetSession()->GetPlayer()->GetGUID(), REQ_GROUP, n });
        return true;
    }

    static bool HandleDungeon(ChatHandler* handler)
    {
        if (!Ready(handler))
            return true;
        Push({ handler->GetSession()->GetPlayer()->GetGUID(), REQ_GROUP, 0 });
        return true;
    }

    static bool HandleRaid(ChatHandler* handler, Optional<uint32> size)
    {
        if (!Ready(handler))
            return true;
        Push({ handler->GetSession()->GetPlayer()->GetGUID(), REQ_RAID, size ? *size : 0 });
        return true;
    }

    static bool HandleDismiss(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!g_enabledHot.load(std::memory_order_relaxed) || !IsHuman(player))
            return true;
        if (name && !name->empty())
            Push({ player->GetGUID(), REQ_DISMISS_ONE, 0, *name });
        else
            Push({ player->GetGUID(), REQ_DISMISS, 0 });
        return true;
    }

    // ".grupo cambia <nombre>": se va ese compañero y se busca otro, sin
    // bajar el tamaño del grupo (a diferencia de ".grupo fuera <nombre>").
    static bool HandleReplace(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!g_enabledHot.load(std::memory_order_relaxed) || !IsHuman(player))
            return true;
        if (!name || name->empty())
        {
            handler->SendSysMessage(ModLocale::L(handler, "Uso: .grupo cambia <nombre del companero>"));
            return true;
        }
        Push({ player->GetGUID(), REQ_REPLACE_ONE, 0, *name });
        return true;
    }

    // ".grupo conserva [<nombre>]" (M19): convierte a manuales los
    // compañeros que vinieron por una misión de grupo, para que no se vayan
    // al entregarla ni al cambiar de zona.
    static bool HandleKeep(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!g_enabledHot.load(std::memory_order_relaxed) || !IsHuman(player))
            return true;
        Push({ player->GetGUID(), REQ_KEEP, 0, name ? *name : std::string() });
        return true;
    }

    static bool HandleHold(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!g_enabledHot.load(std::memory_order_relaxed) || !IsHuman(player))
            return true;
        Push({ player->GetGUID(), REQ_HOLD, 0, name ? *name : std::string() });
        return true;
    }

    static bool HandleFollow(ChatHandler* handler, Optional<std::string> name)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!g_enabledHot.load(std::memory_order_relaxed) || !IsHuman(player))
            return true;
        Push({ player->GetGUID(), REQ_FOLLOW, 0, name ? *name : std::string() });
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!g_enabledHot.load(std::memory_order_relaxed) || !IsHuman(player))
            return true;
        Push({ player->GetGUID(), REQ_STATUS, 0 });
        return true;
    }
};

void AddSC_mod_party_here()
{
    ModLocale::Register(PartyHereLocale::kEntries);
    new mod_party_here_world();
    new mod_party_here_player();
    new mod_party_here_command();
}
