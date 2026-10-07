// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-quest-mates — compañeros de misión: cuando aceptas una misión, dos o
 * tres bots de tu zona la cogen también.
 *
 * EL PROBLEMA
 * mod-world-bots llena tu zona de bots de tu nivel, pero cada uno va a lo suyo:
 * caza donde le toca y hace SUS misiones. En un servidor de verdad, cuando
 * aceptas "mata diez jabalíes" hay otros tres matando los mismos jabalíes, y
 * eso es lo que hace que una zona parezca habitada y no decorada.
 *
 * LA SOLUCIÓN
 * Al aceptar una misión, se eligen dos o tres bots aleatorios libres de tu
 * zona, de tu facción y de tu tramo de nivel, que puedan cogerla (nivel, raza,
 * clase, requisitos previos y hueco en el diario, según el propio núcleo). Se
 * les mete la misión en el diario y se le dice a su estrategia de rol que se
 * ponga con ella: es lo mismo que hace el comando "rpg status do quest" de
 * playerbots, y a partir de ahí su IA va a los objetivos, mata, recoge y
 * entrega por su cuenta. Los mismos bots repiten contigo mientras sigan en la
 * zona, así que son "tus compañeros", no desconocidos cada vez.
 *
 * Quedan fuera las misiones de mazmorra, banda, escolta, JcJ, heroicas,
 * diarias y de evento: o no tienen sentido para un bot suelto, o estorbarían.
 *
 * DÓNDE SE HACE EL TRABAJO
 * OnPlayerQuestAccept corre en el hilo del mapa del jugador; tocar a otros
 * bots desde ahí no es seguro con varios hilos de mapas. El hook sólo apunta
 * la petición y OnUpdate del mundo la atiende (modules/README.md, regla 5).
 *
 * DEPENDENCIA CON PLAYERBOTS
 * Tres cosas, entre guardas: saber si un bot es aleatorio (IsRandomBot),
 * forzarle la estrategia "new rpg" (ChangeStrategy) y cambiarle el estado de
 * rol (rpgInfo.ChangeToDoQuest), miembros públicos de PlayerbotAI. El módulo
 * depende de que playerbots deje al bot en modo new-rpg: si no, la misión se
 * queda en el diario sin que el bot vaya a por ella. Sin playerbots, compila
 * y no hace nada.
 */

#include "BotClaims.h"
#include "BotEligibility.h"
#include "BotOperations.h"
#include "Chat.h"
#include "Config.h"
#include "Containers.h"
#include "DBCStores.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SlowTick.h"
#include "TimeMs.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__has_include)
#  if __has_include("Playerbots.h") && __has_include("PlayerbotAI.h")
#    include "PlayerbotAI.h"
#    include "Playerbots.h"
#    include "RandomPlayerbotMgr.h"
#    define QUEST_MATES_WITH_PLAYERBOTS 1
#  endif
#endif

namespace
{
    char const* const OWNER = "quest-mates";

    struct Config
    {
        bool   enabled       = true;
        uint32 minMates      = 2;
        uint32 maxMates      = 3;
        uint32 levelBelow    = 5;
        uint32 levelAbove    = 3;
        uint32 chance        = 100;   // % de misiones aceptadas que arrastran compañeros
        uint32 rememberMin   = 30;    // minutos que un compañero sigue siendo "tuyo"
        bool   announce      = false;
        bool   rpgStrategyPush = true; // forzar "+new rpg" al dar la misión
        bool   debug         = false;  // eleva a INFO los logs de diagnóstico
        uint32 maxMatesTracked = 6;    // tope de bots retenidos por jugador
        uint32 cleanupIntervalMs = 3000; // cadencia del barrido de limpieza
        uint32 stuckMinutes  = 0;      // liberar la reserva si una misión inyectada no pasa de INCOMPLETE en N min (0 = no)
        bool   abandonStuck  = false;  // ...y además quitarla del diario del bot
        bool   followAbandon = true;   // si el jugador abandona una misión, sus compañeros también la sueltan
        bool   preferHomeGuild = true; // los bots de tu hermandad van primero al elegir compañeros
        bool   jointTurnIn   = true;   // cuando el jugador entrega, empujar a los compañeros a entregar también
        uint32 transitionGraceSecs = 60; // gracia a un jugador o bot cargando de mapa antes de soltar el vínculo
    };

    Config cfg;

    // El hook OnPlayerQuestAccept corre en el HILO DE MAPA del jugador y lee
    // este espejo de cfg.enabled (T7). ".reload config" corre en el hilo del
    // mundo con los mapas parados, así que no compite con él (modules/README.md,
    // regla 5; M05, 24/09/2026); el espejo se conserva por prudencia.
    std::atomic<bool> g_enabledHot{true};

    // Latch "ya limpiado con el módulo apagado": sin él, la rama de OnUpdate
    // con !enabled hacía BotClaims::ReleaseAll(OWNER) en CADA tick, y ReleaseAll
    // escanea el mapa entero de claims bajo el candado global de 5 módulos (T8).
    std::atomic<bool> g_cleanedWhileOff{false};

#define QUEST_MATES_LOG(...) \
    do { if (cfg.debug) LOG_INFO("module", __VA_ARGS__); else LOG_DEBUG("module", __VA_ARGS__); } while (false)

    enum RequestKind : uint8
    {
        REQ_QUEST_ACCEPT,
        REQ_QUEST_ABANDON,
        REQ_LOGOUT,
        REQ_LOGIN
    };

    struct Request
    {
        ObjectGuid player;
        uint32     questId = 0;
        uint8      kind = REQ_QUEST_ACCEPT;
    };

    // Productores: hooks de jugador. Consumidor: mod_quest_mates_world::OnUpdate.
    // Ningun Player* cruza el limite entre ambos hilos.
    std::mutex         g_pendingLock;
    std::deque<Request> g_pending;

    // Procedencia de una misión inyectada en un bot: el instante en que se le
    // dio (el jugador y el módulo son implícitos: todo lo que hay en Mate.quests
    // lo puso ESTE módulo para ESTE jugador). Solo estas misiones se pueden
    // limpiar o abandonar en automático — nunca una que el bot tuviera por
    // otra causa (CS-3.4).
    struct QuestOrigin
    {
        uint64 givenMs = 0;
        bool   nudged  = false;   // ya se le empujó a entregar tras hacerlo el jugador (JointTurnIn)
    };

    // Los compañeros de cada jugador: los mismos bots repiten mientras sigan
    // en la zona, para que sean caras conocidas.
    //
    // Un Mate es un RECUERDO ("este bot ya fue compañero tuyo") y puede además
    // sostener el VÍNCULO ACTIVO que tiene la reserva de BotClaims (M01). Los
    // dos duran distinto: el recuerdo vive hasta untilMs; el vínculo, sólo
    // mientras el bot lleve misiones inyectadas. BotClaims sólo sabe que el bot
    // es de "quest-mates", no de qué jugador, así que la identidad del vínculo
    // vive aquí: un recuerdo pasivo (bond == 0) no puede liberar ni reutilizar
    // la reserva que otro jugador haya hecho después sobre el mismo bot.
    struct Mate
    {
        ObjectGuid          bot;
        uint64              untilMs = 0;
        uint64              bond = 0;          // generación del vínculo activo que sostiene (0 = recuerdo pasivo)
        uint64              graceUntilMs = 0;  // bot cargando de mapa (M18): 0 = no
        WorldSession const* session = nullptr; // sesión del bot al vincularlo (M18)
        std::map<uint32, QuestOrigin> quests;  // misiones inyectadas que mantienen la reserva
    };

    // Los compañeros de un jugador, atados a la sesión con la que los reclutó:
    // una sesión nueva con el mismo GUID no hereda trabajo de la anterior (M18).
    struct Roster
    {
        WorldSession const* session = nullptr;
        uint64              graceUntilMs = 0;  // jugador cargando de mapa (M18): 0 = no
        std::vector<Mate>   mates;
    };
    // Propietario exclusivo: mod_quest_mates_world::OnUpdate (hilo del mundo).
    std::unordered_map<ObjectGuid, Roster> g_mates;
    uint64 g_nextMateScan = 0;

    // Vínculos activos: bot -> (jugador, generación). Hay como mucho uno por
    // bot, y es el único que puede soltar su reserva de BotClaims (M01).
    // Propietario exclusivo: el hilo del mundo, como g_mates.
    struct Bond
    {
        ObjectGuid human;
        uint64     generation = 0;
    };
    std::unordered_map<ObjectGuid, Bond> g_bonds;
    uint64 g_bondGeneration = 0;

    // ¿Este Mate de 'human' sostiene el vínculo activo vigente de su bot?
    bool HoldsBond(ObjectGuid human, Mate const& mate)
    {
        if (!mate.bond)
            return false;
        auto it = g_bonds.find(mate.bot);
        return it != g_bonds.end() && it->second.human == human && it->second.generation == mate.bond;
    }

    // ¿El bot está vinculado ahora mismo a 'human'?
    bool BondedTo(ObjectGuid human, ObjectGuid bot)
    {
        auto it = g_bonds.find(bot);
        return it != g_bonds.end() && it->second.human == human;
    }

    // Suelta el vínculo y la reserva, pero sólo si este Mate es su dueño
    // vigente: un recuerdo viejo nunca libera la reserva de otro jugador.
    void DropBond(ObjectGuid human, Mate& mate)
    {
        if (HoldsBond(human, mate))
        {
            g_bonds.erase(mate.bot);
            BotClaims::Release(mate.bot.GetCounter(), OWNER);
        }
        mate.bond = 0;
    }

    void DropRoster(ObjectGuid human, Roster& roster)
    {
        for (Mate& mate : roster.mates)
            DropBond(human, mate);
    }

    using BotEligibility::IsHuman;

    bool IsRandomBot(Player* bot)
    {
#ifdef QUEST_MATES_WITH_PLAYERBOTS
        return sRandomPlayerbotMgr.IsRandomBot(bot);
#else
        (void)bot;
        return false;
#endif
    }

    // 'human' no vacío: se admite la reserva propia sólo si es el vínculo
    // activo con ESE jugador (reutilizar a un compañero suyo). Una reserva de
    // quest-mates para otro jugador cuenta como ocupada (M01).
    // 'freshClaim': quien llama acaba de reservarlo con una Lease nueva (aún
    // sin vínculo) y revalida; esa reserva es la suya y no cuenta.
    bool IsFreeBot(Player* bot, ObjectGuid human = ObjectGuid::Empty, bool freshClaim = false)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        WorldSession* session = bot->GetSession();
        if (!session || !session->IsHeadless() || session->PlayerLoading())
            return false;

        if (!bot->IsAlive() || bot->IsInFlight() || bot->IsBeingTeleported())
            return false;

        if (bot->InBattleground() || bot->InBattlegroundQueue() || bot->GetGroup())
            return false;

        if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
            return false;

        if (Map* map = bot->GetMap())
            if (map->IsDungeon() || map->IsBattlegroundOrArena())
                return false;

        // Reservado por otro módulo, o por este para otro jugador: se le deja en paz.
        uint32 const low = bot->GetGUID().GetCounter();
        if (BotClaims::IsClaimed(low)
            && (BotClaims::IsClaimedByOther(low, OWNER)
                || !(freshClaim || (!human.IsEmpty() && BondedTo(human, bot->GetGUID())))))
            return false;

        return IsRandomBot(bot);
    }

    // Misiones que no tienen sentido para un bot suelto, o que estorbarían.
    // Solo se filtra QUEST_TYPE_ESCORT (84); muchas escoltas jugables de WotLK
    // son tipo 0 con la mecánica por script y un bot al que se le da una de
    // esas se atasca (B-A9). El escape es QuestMates.ExcludeQuestIds.
    bool IsSuitableQuest(Quest const* quest)
    {
        switch (quest->GetType())
        {
            case QUEST_TYPE_PVP:
            case QUEST_TYPE_RAID:
            case QUEST_TYPE_DUNGEON:
            case QUEST_TYPE_WORLD_EVENT:
            case QUEST_TYPE_LEGENDARY:
            case QUEST_TYPE_ESCORT:
            case QUEST_TYPE_HEROIC:
            case QUEST_TYPE_RAID_10:
            case QUEST_TYPE_RAID_25:
                return false;
            default:
                break;
        }

        if (quest->IsDailyOrWeekly() || quest->IsRepeatable() || quest->IsSeasonal() || quest->IsAutoComplete())
            return false;

        return true;
    }

    // ¿Puede este bot coger la misión ahora mismo? Lo decide el núcleo con los
    // mismos criterios que a un jugador: nivel, raza, clase, requisitos previos,
    // hueco en el diario. Y que no la tenga ya ni la haya entregado.
    bool CanBotTake(Player* bot, Quest const* quest)
    {
        uint32 const questId = quest->GetQuestId();
        if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE || bot->GetQuestRewardStatus(questId))
            return false;

        return bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false);
    }

    // Quita una misión del diario de un bot (la misma secuencia que
    // WorldSession::HandleQuestLogRemoveQuest). Solo se llama sobre misiones
    // que inyectó este módulo (CS-3.4).
    void AbandonBotQuest(Player* bot, uint32 questId)
    {
        uint16 const slot = bot->FindQuestSlot(questId);
        bot->TakeQuestSourceItem(questId, false);
        bot->AbandonQuest(questId);       // destruye los objetos de misión recibidos
        bot->RemoveActiveQuest(questId);
        if (slot < MAX_QUEST_LOG_SIZE)
            bot->SetQuestSlot(slot, 0);
    }

    void GiveQuest(Player* bot, Quest const* quest)
    {
        bot->AddQuestAndCheckCompletion(quest, nullptr);

#ifdef QUEST_MATES_WITH_PLAYERBOTS
        // Que su estrategia de rol se ponga con ella ya, en vez de con lo que
        // estuviera haciendo. Es lo que hace "rpg status do quest <id>".
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
        {
            // rpgInfo es del motor "new rpg". Si el bot no lo tiene activo
            // (viajando/grind), ChangeToDoQuest no basta y la mision se queda
            // INCOMPLETE reteniendo la reserva 30 min (B-A2). Se fuerza la
            // estrategia (idempotente si ya esta) antes de fijar el estado.
            if (cfg.rpgStrategyPush)
                botAI->ChangeStrategy("+new rpg", BOT_STATE_NON_COMBAT);
            botAI->rpgInfo.ChangeToDoQuest(quest->GetQuestId(), quest);
            QUEST_MATES_LOG("[quest-mates] {} coge '{}' (rpg forzado: {}).",
                      bot->GetName(), quest->GetTitle(), cfg.rpgStrategyPush);
        }
#endif
    }
}

class mod_quest_mates_world : public WorldScript
{
public:
    mod_quest_mates_world() : WorldScript("mod_quest_mates_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        cfg.enabled     = sConfigMgr->GetOption<bool>("QuestMates.Enable", true);
        cfg.minMates    = sConfigMgr->GetOption<uint32>("QuestMates.MinMates", 2);
        cfg.maxMates    = sConfigMgr->GetOption<uint32>("QuestMates.MaxMates", 3);
        cfg.levelBelow  = sConfigMgr->GetOption<uint32>("QuestMates.LevelBelow", 5);
        cfg.levelAbove  = sConfigMgr->GetOption<uint32>("QuestMates.LevelAbove", 3);
        cfg.chance      = sConfigMgr->GetOption<uint32>("QuestMates.Chance", 100);
        cfg.rememberMin = sConfigMgr->GetOption<uint32>("QuestMates.RememberMinutes", 30);
        cfg.announce    = sConfigMgr->GetOption<bool>("QuestMates.Announce", false);
        cfg.rpgStrategyPush = sConfigMgr->GetOption<bool>("QuestMates.RpgStrategyPush", true);
        cfg.debug       = sConfigMgr->GetOption<bool>("QuestMates.Debug", false);
        cfg.maxMatesTracked = std::max<uint32>(sConfigMgr->GetOption<uint32>("QuestMates.MaxMatesTracked", 6), 1);
        cfg.cleanupIntervalMs = std::max<uint32>(sConfigMgr->GetOption<uint32>("QuestMates.CleanupIntervalMs", 3000), 500);
        cfg.stuckMinutes = sConfigMgr->GetOption<uint32>("QuestMates.StuckMinutes", 0);
        cfg.abandonStuck = sConfigMgr->GetOption<bool>("QuestMates.AbandonStuck", false);
        cfg.followAbandon = sConfigMgr->GetOption<bool>("QuestMates.FollowAbandon", true);
        cfg.preferHomeGuild = sConfigMgr->GetOption<bool>("QuestMates.PreferHomeGuild", true);
        cfg.jointTurnIn = sConfigMgr->GetOption<bool>("QuestMates.JointTurnIn", true);
        cfg.transitionGraceSecs = sConfigMgr->GetOption<uint32>("QuestMates.TransitionGraceSeconds", 60);

        if (cfg.maxMates < cfg.minMates)
            cfg.maxMates = cfg.minMates;
        cfg.chance = std::min<uint32>(cfg.chance, 100);

#ifndef QUEST_MATES_WITH_PLAYERBOTS
        if (cfg.enabled)
        {
            LOG_INFO("module", "[quest-mates] Compilado sin mod-playerbots: no hay bots, el modulo no hace nada.");
            cfg.enabled = false;
        }
#endif
        g_enabledHot.store(cfg.enabled, std::memory_order_relaxed);   // espejo para el hook de hilo de mapa (T7)
        g_cleanedWhileOff.store(false, std::memory_order_relaxed);    // rearmar el latch de T8 en cada recarga
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        uint32 const t0 = getMSTime();
        DoUpdate();
        SlowTick::WarnIfSlow("quest-mates", "OnUpdate", t0);
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
                for (auto& [human, roster] : g_mates)
                    DropRoster(human, roster);
                g_mates.clear();
                g_bonds.clear();
                BotClaims::ReleaseAll(OWNER);

                // Última estadística con enabled=false (M07).
                BotOperations::QuestMateStats stats;
                stats.updatedAtMs = TimeMs::NowMs();
                BotOperations::PublishQuestMateStats(std::move(stats));
            }
            return;
        }

        std::deque<Request> requests;
        {
            std::lock_guard<std::mutex> lock(g_pendingLock);
            requests.swap(g_pending);
        }
        SlowTick::WarnIfDeep("quest-mates", "g_pending", requests.size());

        for (Request const& request : requests)
            Handle(request);

        uint64_t const now = TimeMs::NowMs();
        if (now >= g_nextMateScan)
        {
            g_nextMateScan = now + cfg.cleanupIntervalMs;
            CleanupMates(now);
        }

        // Estadisticas para el panel (pestaña "Colas y grupos"): solo lectura,
        // este modulo no tiene ninguna pasada periodica que "adelantar" (es
        // reactivo a aceptar/abandonar misión, ver cabecera del fichero).
        {
            BotOperations::QuestMateStats stats;
            stats.enabled = cfg.enabled;
            for (auto const& pair : g_mates)
            {
                if (pair.second.mates.empty())
                    continue;
                ++stats.humansWithMates;
                stats.activePairings += static_cast<uint32_t>(pair.second.mates.size());
            }
            stats.updatedAtMs = now;
            BotOperations::PublishQuestMateStats(std::move(stats));
        }
    }

private:
    void Handle(Request const& request)
    {
        // Logout, y también login: una sesión nueva con el mismo GUID empieza
        // de cero aunque el logout anterior no llegara a verse (M18).
        if (request.kind == REQ_LOGOUT || request.kind == REQ_LOGIN)
        {
            ReleaseMates(request.player);
            return;
        }

        if (request.kind == REQ_QUEST_ABANDON)
        {
            HandleAbandon(request.player, request.questId);
            return;
        }

        Player* player = ObjectAccessor::FindPlayer(request.player);
        if (!IsHuman(player))
            return;

        Quest const* quest = sObjectMgr->GetQuestTemplate(request.questId);
        if (!quest || !IsSuitableQuest(quest))
            return;

        Map* map = player->GetMap();
        if (!map || map->IsDungeon() || map->IsBattlegroundOrArena())
            return;

        if (cfg.chance < 100 && urand(1, 100) > cfg.chance)
            return;

        uint64_t const now = TimeMs::NowMs();
        uint32 const zoneId = player->GetZoneId();
        uint32 const level = player->GetLevel();
        uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        uint32 const minLevel = level > cfg.levelBelow ? level - cfg.levelBelow : 1;
        uint32 const topLevel = std::min(level + cfg.levelAbove, maxLevel);
        TeamId const team = player->GetTeamId();

        uint32 const wanted = urand(cfg.minMates, cfg.maxMates);
        std::vector<Player*> chosen;
        ObjectGuid const humanGuid = player->GetGUID();

        // Compañeros de otra sesión del mismo personaje: no se heredan (M18).
        if (auto stale = g_mates.find(humanGuid); stale != g_mates.end() && stale->second.session != player->GetSession())
            ReleaseMates(humanGuid);

        // Primero los compañeros de siempre, si siguen en la zona y libres. Se
        // consulta con find (no operator[]): así una misión que no recluta a
        // nadie no deja una entrada vacía en g_mates (B-A8).
        if (auto matesIt = g_mates.find(humanGuid); matesIt != g_mates.end())
        {
            std::vector<Mate>& mates = matesIt->second.mates;
            for (auto mate = mates.begin(); mate != mates.end();)
            {
                if (now > mate->untilMs && mate->quests.empty())
                {
                    DropBond(humanGuid, *mate);
                    mate = mates.erase(mate);
                }
                else
                    ++mate;
            }

            for (Mate& mate : mates)
            {
                if (chosen.size() >= wanted)
                    break;

                Player* bot = ObjectAccessor::FindPlayer(mate.bot);
                if (!IsFreeBot(bot, humanGuid) || bot->GetMapId() != player->GetMapId() || bot->GetZoneId() != zoneId)
                    continue;

                // La banda de nivel también a los reutilizados: un mate que ha
                // subido fuera del tramo dejaba de valer para caras nuevas pero
                // seguía recibiendo misiones (B-A4).
                if (bot->GetLevel() < minLevel || bot->GetLevel() > topLevel)
                    continue;

                if (!CanBotTake(bot, quest))
                    continue;

                chosen.push_back(bot);
                mate.untilMs = now + TimeMs::MinsToMs(cfg.rememberMin);
            }

            if (mates.empty())
                g_mates.erase(matesIt);
        }

        // Y luego caras nuevas de la zona. Con varios humanos en la misma zona no
        // hay reparto: el primero que acepta una misión coge los bots libres y
        // el segundo se queda sin (B-A5). Irrelevante con un solo jugador; un
        // tope por jugador (QuestMates.MaxMatesTracked) lo acotaría.
        //
        // El recorrido de ObjectAccessor::GetPlayers() sin candado es seguro solo
        // porque va en el hilo del mundo (patrón habitual de AC, B-A7).
        if (chosen.size() < wanted)
        {
            std::vector<Player*> candidates;
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* bot = pair.second;
                if (!IsFreeBot(bot))
                    continue;

                if (bot->GetTeamId() != team || bot->GetMapId() != player->GetMapId() || bot->GetZoneId() != zoneId)
                    continue;

                if (bot->GetLevel() < minLevel || bot->GetLevel() > topLevel)
                    continue;

                if (std::find(chosen.begin(), chosen.end(), bot) != chosen.end())
                    continue;

                if (!CanBotTake(bot, quest))
                    continue;

                candidates.push_back(bot);
            }

            Acore::Containers::RandomShuffle(candidates);

            // Los bots de tu hermandad (mod-home-guild) primero: los mismos
            // compañeros semana tras semana, como en las colas y en .grupo.
            if (cfg.preferHomeGuild)
                std::stable_partition(candidates.begin(), candidates.end(), [](Player* bot)
                {
                    return BotClaims::IsHomeGuildBot(bot->GetGUID().GetCounter());
                });

            for (Player* bot : candidates)
            {
                if (chosen.size() >= wanted)
                    break;

                chosen.push_back(bot);
            }
        }

        if (chosen.empty())
        {
            QUEST_MATES_LOG("[quest-mates] Nadie en la zona {} de nivel {}-{} puede coger '{}' con {}.",
                      zoneId, minLevel, topLevel, quest->GetTitle(), player->GetName());
            return;
        }

        std::string names;
        for (Player* bot : chosen)
        {
            // Una reserva previa de quest-mates sólo vale si es el vínculo
            // activo con ESTE jugador; la que acaba de hacer esta Lease, sí.
            BotClaims::Lease claim(bot->GetGUID().GetCounter(), OWNER);
            if (!claim || !IsFreeBot(bot, humanGuid, claim.IsNew()) || bot->GetMapId() != player->GetMapId() ||
                bot->GetZoneId() != zoneId || !CanBotTake(bot, quest))
                continue;

            // Tope de bots retenidos por jugador: un bot que ya es mate suyo
            // puede coger otra misión; uno nuevo no, si ya está al tope. Se
            // comprueba ANTES de GiveQuest para no dar la misión y luego soltar.
            if (auto ex = g_mates.find(humanGuid); ex != g_mates.end())
            {
                bool const already = std::any_of(ex->second.mates.begin(), ex->second.mates.end(),
                    [bot](Mate const& m) { return m.bot == bot->GetGUID(); });
                if (!already && ex->second.mates.size() >= cfg.maxMatesTracked)
                    continue;
            }

            GiveQuest(bot, quest);

            // AddQuestAndCheckCompletion puede rechazar la operacion aunque la
            // prevalidacion pasara. Sin mision no se conserva la reserva.
            if (bot->GetQuestStatus(request.questId) == QUEST_STATUS_NONE)
                continue;

            // Solo ahora, con un bot que sí se queda, se toca g_mates (B-A8).
            Roster& roster = g_mates[humanGuid];
            roster.session = player->GetSession();
            std::vector<Mate>& mates = roster.mates;
            auto mate = std::find_if(mates.begin(), mates.end(), [bot](Mate const& value)
            {
                return value.bot == bot->GetGUID();
            });
            if (mate == mates.end())
            {
                Mate fresh;
                fresh.bot = bot->GetGUID();
                mates.push_back(fresh);
                mate = std::prev(mates.end());
            }
            mate->untilMs = now + TimeMs::MinsToMs(cfg.rememberMin);
            mate->quests[request.questId] = { now };   // procedencia: dada ahora por este módulo (CS-3.4)

            // Vínculo activo: uno nuevo si la reserva es nueva; si ya era de
            // este jugador (AlreadyOwned + BondedTo), se adopta el vigente.
            if (claim.IsNew() || !BondedTo(humanGuid, bot->GetGUID()))
                g_bonds[bot->GetGUID()] = { humanGuid, ++g_bondGeneration };
            mate->bond = g_bonds[bot->GetGUID()].generation;
            mate->session = bot->GetSession();
            mate->graceUntilMs = 0;
            claim.Keep();
            names += (names.empty() ? "" : ", ") + bot->GetName();
        }

        if (names.empty())
        {
            QUEST_MATES_LOG("[quest-mates] Los candidatos dejaron de estar libres antes de coger '{}' con {}.",
                      quest->GetTitle(), player->GetName());
            return;
        }

        LOG_INFO("module", "[quest-mates] {} cogen '{}' con {} (zona {}, nivel {}-{}).",
                 names, quest->GetTitle(), player->GetName(), zoneId, minLevel, topLevel);

        if (cfg.announce)
            ChatHandler(player->GetSession()).PSendSysMessage("{} también van a por \"{}\".", names, quest->GetTitle());
    }

    void ReleaseMates(ObjectGuid humanGuid)
    {
        auto it = g_mates.find(humanGuid);
        if (it == g_mates.end())
            return;
        DropRoster(humanGuid, it->second);
        g_mates.erase(it);
    }

    // El jugador ha abandonado una misión: sus compañeros la sueltan también.
    // Sólo se toca la copia que inyectó ESTE módulo (mate.quests), nunca una
    // que el bot tuviera por otra causa (CS-3.4).
    void HandleAbandon(ObjectGuid humanGuid, uint32 questId)
    {
        if (!cfg.followAbandon || !questId)
            return;
        auto it = g_mates.find(humanGuid);
        if (it == g_mates.end())
            return;

        uint32 dropped = 0;
        for (auto mate = it->second.mates.begin(); mate != it->second.mates.end();)
        {
            auto q = mate->quests.find(questId);
            if (q == mate->quests.end())
            {
                ++mate;
                continue;
            }

            // Sólo si este Mate sostiene aún el vínculo: un recuerdo pasivo no
            // toca el diario de un bot que ahora acompaña a otro jugador (M01).
            Player* bot = HoldsBond(humanGuid, *mate) ? ObjectAccessor::FindPlayer(mate->bot) : nullptr;
            if (bot)
            {
                QuestStatus const status = bot->GetQuestStatus(questId);
                if ((status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
                    && !bot->GetQuestRewardStatus(questId))
                    AbandonBotQuest(bot, questId);
            }
            mate->quests.erase(q);
            ++dropped;

            // Sin misiones que lo aten, se suelta la reserva ya (sólo si el
            // vínculo sigue siendo suyo); el barrido de CleanupMates quita el
            // recuerdo de g_mates cuando venza su untilMs.
            if (mate->quests.empty())
                DropBond(humanGuid, *mate);
            ++mate;
        }

        if (dropped)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            LOG_INFO("module", "[quest-mates] {} companero(s) sueltan '{}' porque el jugador la abandono.",
                     dropped, quest ? quest->GetTitle() : "?");
        }
    }

    // Tres estados, no dos, para el jugador y para cada bot (M18). Sin sesión,
    // con otra sesión o saliendo: se suelta. Sesión viva fuera del mundo
    // (pantalla de carga de un portal o de un cambio de continente): se espera
    // hasta TransitionGraceSeconds sin tocar nada. ObjectAccessor::FindPlayer
    // devuelve nulo en esa ventana; FindConnectedPlayer no, porque el
    // personaje sigue en el HashMapHolder hasta LogoutPlayer. Mismo criterio
    // que la gracia del amo en party-here.
    enum class Presence : uint8 { Gone, Loading, Here };

    Presence PresenceOf(Player* player, WorldSession const* expected, bool wantBot)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        if (!session || session->IsHeadless() != wantBot || session->PlayerLogout()
            || (expected && session != expected))
            return Presence::Gone;
        return player->IsInWorld() && !session->PlayerLoading() ? Presence::Here : Presence::Loading;
    }

    // true = sigue dentro de la gracia; false = se acabó y hay que soltar.
    bool InGrace(uint64& graceUntilMs, uint64 now)
    {
        if (!graceUntilMs)
            graceUntilMs = now + TimeMs::SecsToMs(cfg.transitionGraceSecs);
        return now < graceUntilMs;
    }

    void CleanupMates(uint64 now)
    {
        for (auto human = g_mates.begin(); human != g_mates.end();)
        {
            Roster& roster = human->second;
            Player* const master = ObjectAccessor::FindConnectedPlayer(human->first);
            Presence const masterPresence = PresenceOf(master, roster.session, false);
            if (masterPresence == Presence::Loading && InGrace(roster.graceUntilMs, now))
            {
                ++human;   // cargando de mapa: ni se suelta ni se toca a nadie
                continue;
            }
            if (masterPresence != Presence::Here)
            {
                if (masterPresence == Presence::Loading)
                    LOG_INFO("module", "[quest-mates] El jugador lleva mas de {} s fuera del mundo: se sueltan sus companeros.",
                             cfg.transitionGraceSecs);
                DropRoster(human->first, roster);
                human = g_mates.erase(human);
                continue;
            }
            roster.graceUntilMs = 0;   // de vuelta en el mundo

            for (auto mate = roster.mates.begin(); mate != roster.mates.end();)
            {
                // Un recuerdo pasivo no mira al bot: su vínculo, si lo tuvo, ya
                // se soltó y el bot puede ser ahora de otro jugador (M01).
                if (!HoldsBond(human->first, *mate))
                {
                    mate->bond = 0;
                    mate->quests.clear();
                    if (now > mate->untilMs)
                        mate = roster.mates.erase(mate);
                    else
                        ++mate;
                    continue;
                }

                Player* bot = ObjectAccessor::FindConnectedPlayer(mate->bot);
                Presence const botPresence = PresenceOf(bot, mate->session, true);
                if (botPresence == Presence::Loading && InGrace(mate->graceUntilMs, now))
                {
                    ++mate;
                    continue;
                }
                if (botPresence != Presence::Here)
                {
                    DropBond(human->first, *mate);
                    mate = roster.mates.erase(mate);
                    continue;
                }
                mate->graceUntilMs = 0;

                uint64 const stuckMs = cfg.stuckMinutes ? TimeMs::MinsToMs(cfg.stuckMinutes) : 0;
                for (auto q = mate->quests.begin(); q != mate->quests.end();)
                {
                    uint32 const questId = q->first;

                    // Se conserva la reserva mientras el bot lleve la mision sin
                    // entregar: INCOMPLETE (objetivos a medias) y COMPLETE
                    // (objetivos hechos, va hacia el NPC de entrega). Antes se
                    // soltaba en COMPLETE y otro modulo podia teletransportarlo
                    // a mitad de camino (B-A3).
                    QuestStatus const status = bot->GetQuestStatus(questId);
                    bool keep = (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
                                && !bot->GetQuestRewardStatus(questId);

                    // Misión atascada: lleva más de StuckMinutes sin pasar de
                    // INCOMPLETE. Solo se toca porque la inyectó este módulo
                    // (CS-3.4): se suelta la reserva y, si AbandonStuck, se le
                    // quita del diario para que no ocupe hueco eternamente.
                    if (keep && stuckMs && status == QUEST_STATUS_INCOMPLETE
                        && now > q->second.givenMs && now - q->second.givenMs > stuckMs)
                    {
                        keep = false;
                        if (cfg.abandonStuck)
                            AbandonBotQuest(bot, questId);
                        Quest const* q2 = sObjectMgr->GetQuestTemplate(questId);
                        LOG_INFO("module", "[quest-mates] {} lleva {} min con '{}' ({}) sin avanzar: se suelta{}.",
                                 bot->GetName(), cfg.stuckMinutes, q2 ? q2->GetTitle() : "?", questId,
                                 cfg.abandonStuck ? " y se abandona" : "");
                    }

                    // Entrega conjunta: el jugador ya entregó la misión y el bot
                    // la tiene COMPLETE (objetivos hechos, esperando). Se le
                    // reenfoca la estrategia de rol a "hacer misión" para que
                    // vaya al NPC de entrega, una sola vez, y se avisa.
                    if (cfg.jointTurnIn && keep && !q->second.nudged && master
                        && status == QUEST_STATUS_COMPLETE && master->GetQuestRewardStatus(questId))
                    {
                        q->second.nudged = true;
#ifdef QUEST_MATES_WITH_PLAYERBOTS
                        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                            if (Quest const* qq = sObjectMgr->GetQuestTemplate(questId))
                            {
                                if (cfg.rpgStrategyPush)
                                    botAI->ChangeStrategy("+new rpg", BOT_STATE_NON_COMBAT);
                                botAI->rpgInfo.ChangeToDoQuest(questId, qq);
                            }
#endif
                        if (cfg.announce && master->GetSession())
                            ChatHandler(master->GetSession()).SendSysMessage("Tus companeros van a entregar la mision.");
                    }

                    q = keep ? std::next(q) : mate->quests.erase(q);
                }

                if (now > mate->untilMs)
                    mate->quests.clear();

                if (mate->quests.empty())
                    DropBond(human->first, *mate);

                if (now > mate->untilMs && mate->quests.empty())
                    mate = roster.mates.erase(mate);
                else
                    ++mate;
            }

            human = roster.mates.empty() ? g_mates.erase(human) : std::next(human);
        }
    }
};

class mod_quest_mates_player : public PlayerScript
{
public:
    mod_quest_mates_player() : PlayerScript("mod_quest_mates_player",
        { PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT, PLAYERHOOK_ON_QUEST_ABANDON, PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (!g_enabledHot.load(std::memory_order_relaxed) || !quest || !IsHuman(player))
            return;   // hilo de mapa: espejo atómico, no el POD `cfg` (T7)

        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending.push_back({ player->GetGUID(), quest->GetQuestId(), REQ_QUEST_ACCEPT });
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        if (!g_enabledHot.load(std::memory_order_relaxed) || !IsHuman(player))
            return;
        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending.push_back({ player->GetGUID(), questId, REQ_QUEST_ABANDON });
    }

    // Una sesión nueva no hereda compañeros de la anterior (M18). Sin IsHuman:
    // en OnPlayerLogin el personaje puede no estar todavía en el mundo.
    void OnPlayerLogin(Player* player) override
    {
        if (!player || !player->GetSession() || player->GetSession()->IsHeadless())
            return;
        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending.push_back({ player->GetGUID(), 0, REQ_LOGIN });
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending.push_back({ player->GetGUID(), 0, REQ_LOGOUT });
    }
};

void AddSC_mod_quest_mates()
{
    new mod_quest_mates_world();
    new mod_quest_mates_player();
}
