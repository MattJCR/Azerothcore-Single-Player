// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-world-bots — guerra de mundo: escaramuzas entre facciones y duelos.
 *
 * EL PROBLEMA
 * world-bots pone un 35 % de la facción contraria en las zonas contestadas,
 * pero nadie va a la guerra: cada bot caza a lo suyo. En un servidor de verdad
 * la Horda asalta Costasur, la Alianza responde desde Molino Tarren, y a las
 * puertas de las capitales la gente se reta a duelos.
 *
 * DE DÓNDE VIENE
 * Es un fork propio de mod-playerbot-world-pvp (TopHatMan, commit ed7962cc,
 * 08/2026): de él se conservan los puntos calientes (tabla y 23 filas con sus
 * coordenadas), el ciclo del evento (reunión → avance → fin → vuelta a casa),
 * las estrategias de playerbots que activa ("+pvp,+boost,+dps debuff,
 * -passive,-stay") y los duelos con el hechizo 7266. Se descartó lo que lo
 * hacía peligroso aquí (CHANGELOG.md anexo A5, ronda 2):
 *
 *   - su comprobación de seguridad sólo miraba la cuenta: aquí un bot vale si
 *     está libre según el mismo criterio que el resto de world-bots (ni en
 *     grupo, ni en instancia, ni en cola, ni en combate, ni reservado por
 *     otro módulo, ni de tu hermandad);
 *   - no restauraba las estrategias al terminar: aquí se hace ResetStrategies
 *     y el bot vuelve a donde estaba;
 *   - la "respuesta reactiva" (40 bots enemigos de nivel 60 contra cualquier
 *     banda de 20 con marca JcJ) no existe: en un reino JcJ saltaría con cada
 *     banda del buscador camino de Núcleo de Magma.
 *
 * Y una regla nueva, pensada para un jugador: por defecto sólo estallan
 * escaramuzas en zonas donde hay un humano. Una guerra que nadie ve es CPU.
 *
 * CÓMO FUNCIONA
 * Cada minuto, con una probabilidad, se elige un punto caliente (por peso,
 * respetando su enfriamiento) de una zona con jugador. Los atacantes se
 * teletransportan al punto de reunión y los defensores al objetivo (nunca a
 * menos de 160 yardas de un humano: el punto se aleja de él), con la marca
 * JcJ y las estrategias de JcJ. Pasados unos segundos los atacantes avanzan
 * andando hasta el objetivo; la estrategia "pvp" de playerbots hace el resto
 * cuando ve enemigos a menos de 100 yardas. Al acabar la duración, cada bot
 * recupera sus estrategias y vuelve a donde estaba. En los puntos de duelo
 * (misma facción a las puertas de la capital) se forman parejas y uno reta
 * al otro; la estrategia "duel" acepta.
 *
 * DÓNDE SE HACE EL TRABAJO
 * Todo en WorldScript::OnUpdate. Los comandos GM sólo apuntan la petición.
 *
 * DEPENDENCIA CON PLAYERBOTS (entre guardas)
 * IsRandomBot, Reset y HasPlayerNearby (el teletransporte, como en el resto
 * de world-bots), ChangeStrategy y ResetStrategies (públicas: son lo que hace
 * el comando "co +pvp" y lo que hace queue-bots al entrar en una arena).
 */

#include "BotClaims.h"
#include "BotEligibility.h"
#include "BotGear.h"
#include "BotOperations.h"
#include "BotWorldAge.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Optional.h"
#include "Pet.h"
#include "Player.h"
#include "Position.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SlowTick.h"
#include "SpellDefines.h"
#include "StringFormat.h"
#include "TimeMs.h"
#include "Timer.h"
#include "World.h"
#include "WorldBotsPolicy.h"
#include "WorldSession.h"
#include "LFGMgr.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <list>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__has_include)
#  if __has_include("Playerbots.h") && __has_include("PlayerbotAI.h")
#    include "PlayerbotAI.h"
#    include "Playerbots.h"
#    include "RandomPlayerbotMgr.h"
#    define WORLD_BOTS_PVP_WITH_PLAYERBOTS 1
#  endif
#endif

using namespace Acore::ChatCommands;

namespace
{
    char const* const OWNER = "world-bots";

    struct PvpConfig
    {
        bool        enabled            = true;
        uint32      tickSecs           = 60;
        float       chancePerTick      = 25.0f;
        uint32      maxActive          = 1;
        uint32      startupDelaySecs   = 120;
        uint32      minLevel           = 20;
        uint32      maxPerSide         = 8;
        uint32      moveDelaySecs      = 15;
        uint32      jitter             = 8;
        bool        duels              = true;
        uint32      duelPairs          = 4;
        uint32      duelDelaySecs      = 8;
        uint32      duelSpell          = 7266;
        bool        onlyWithPlayer     = true;
        bool        announce           = true;
        std::string combatStrategies   = "+pvp,+boost,+dps debuff,-passive,-stay";
        std::string nonCombatStrategies = "+pvp,+duel,+boost,-passive,-stay";
        uint32 botMinWorldSecs = 60;
        float  nearbyRadius    = 150.0f; // HasPlayerNearby al teletransportar
        float  keepAwayDist    = 160.0f; // no colocar un punto a menos de esto de un humano
        float  repositionDist  = 200.0f; // ... se reproyecta a esta distancia
        uint32 combatGraceSecs = 60;     // gracia en combate al terminar antes de forzar la vuelta
        float  duelRange       = 35.0f;  // distancia máxima para lanzar el hechizo de duelo
        uint32 duelMaxTries    = 20;     // intentos de acercar al retado
        uint32 minBotsToStart  = 2;      // mínimo por bando para que un evento arranque

        // Reactivación automática de los puntos calientes de 50-60 (SearingGorge,
        // BlackrockMountain, LightHopeChapel: enabled=0 en el SQL) cuando hay
        // población de ese nivel en ambas facciones.
        bool autoEnableHighHotspots = true;

        // Objetivo de captura: los bots que aguantan cerca del punto suman
        // control; al llegar al umbral, ese bando "captura" y el evento termina.
        bool  captureObjective = true;
        float captureRadius    = 40.0f;
        uint32 captureGoal     = 10;     // ticks de control (uno cada ~15 s) para ganar

        // Equipo de los bots de la guerra a la fase del humano de la zona
        // (mismas claves que QueueBots.Gear*, prefijo WorldBots.Pvp).
        BotGear::Settings gear;
    };

    PvpConfig pcfg;

    enum PvpTeam : uint8
    {
        PVP_ALLIANCE = 0,
        PVP_HORDE    = 1,
        PVP_ANY      = 2
    };

    struct Hotspot
    {
        uint32      id = 0;
        std::string name;
        std::string label;          // el nombre que se anuncia
        bool        enabled = true;
        PvpTeam     attackers = PVP_HORDE;
        PvpTeam     defenders = PVP_ALLIANCE;
        uint8       minLevel = 20;
        uint8       maxLevel = 60;
        uint32      mapId = 0;
        Position    rally;
        Position    target;
        uint32      attackersMin = 2, attackersMax = 5;
        uint32      defendersMin = 2, defendersMax = 5;
        uint32      durationMin = 5, durationMax = 12;   // minutos
        uint32      weight = 100;
        uint32      cooldownSecs = 1800;
        uint32      zoneId = 0;                          // calculada al cargar
        uint64      lastStartMs = 0;
        bool        autoManaged = false;                 // enabled lo decide la población (50-60)
    };

    struct Member
    {
        ObjectGuid    guid;
        bool          attacker = false;
        WorldLocation origin;
        uint64        moveAtMs = 0;       // 0 = no avanza (defensores, duelos)
        bool          moved = false;
        uint64        returnUntilMs = 0;  // > 0: se está intentando devolver
    };

    struct Duel
    {
        ObjectGuid challenger;
        ObjectGuid target;
        uint64     atMs = 0;
        uint8      tries = 0;
    };

    struct PvpEvent
    {
        uint32              id = 0;
        std::string         spotName;
        std::string         label;
        uint32              zoneId = 0;
        uint32              mapId = 0;
        PvpTeam             attackers = PVP_HORDE;
        PvpTeam             defenders = PVP_ALLIANCE;
        bool                duelMode = false;
        uint64              startMs = 0;
        uint64              endMs = 0;
        bool                ending = false;
        std::vector<Member> members;
        std::vector<Duel>   duels;

        // Objetivo de captura (P3c): ticks de control acumulados por bando y los
        // relojes del tick de captura y del anuncio de progreso.
        uint32              scoreAtt = 0;
        uint32              scoreDef = 0;
        uint64              nextCaptureMs = 0;
        uint64              nextProgressMs = 0;
    };

    std::vector<Hotspot> g_hotspots;
    // std::list y no std::vector: TickEvent y el bucle de duelos mantienen
    // referencias vivas a PvpEvent& mientras se recorre; un push_back mal
    // colocado en un vector las invalidaría (era una "Nota" de §3.2).
    std::list<PvpEvent>    g_events;
    uint32 g_nextEventId = 1;
    uint64 g_nextTickMs = 0;
    uint64 g_startedMs = 0;
    bool   g_pvpEnabled = false;   // ultimo estado conocido, para el flanco off->on en reload

    // Peticiones de los comandos GM (se apuntan; las atiende OnUpdate).
    enum RequestKind : uint8 { REQ_STATUS, REQ_LIST, REQ_START, REQ_STOP, REQ_RELOAD };
    struct Request
    {
        ObjectGuid    gm;
        uint8         kind = REQ_STATUS;
        std::string   name;
        Optional<uint32> id; // REQ_STOP: vacio = todos los eventos, valor = uno concreto (M13)
    };
    std::mutex          g_pendingLock;
    std::deque<Request> g_pending;

    using BotEligibility::IsHuman;

    bool IsRandomBot(Player* bot)
    {
#ifdef WORLD_BOTS_PVP_WITH_PLAYERBOTS
        return sRandomPlayerbotMgr.IsRandomBot(bot);
#else
        (void)bot;
        return false;
#endif
    }

    // El mismo criterio base que el resto de world-bots (BotEligibility.h), y
    // además: ni reservado por nadie (tampoco por nosotros: los que ya están
    // en un evento), ni de tu hermandad (esos están para ti, no para la guerra).
    bool IsFreeBot(Player* bot, bool allowOwnClaim = false)
    {
        if (!BotEligibility::IsAvailable(bot))
            return false;

        if (!BotWorldAge::IsMature(bot->GetGUID().GetCounter(), bot->GetSession(),
                                   TimeMs::NowMs(), pcfg.botMinWorldSecs))
            return false;

        uint32 const low = bot->GetGUID().GetCounter();
        if ((allowOwnClaim ? BotClaims::IsClaimedByOther(low, OWNER) : BotClaims::IsClaimed(low)) ||
            BotClaims::IsHomeGuildBot(low))
            return false;

        return IsRandomBot(bot);
    }

    char const* TeamLabel(PvpTeam team)
    {
        return team == PVP_ALLIANCE ? "la Alianza" : (team == PVP_HORDE ? "la Horda" : "ambas");
    }

    // parseo estricto del id de evento que llega como texto desde el
    // panel (BotOperations::ActionRequest::param). Cadena vacia = "todos"
    // (outId queda vacio); cualquier otra entrada debe ser exactamente un
    // uint32 -sin signo, sin desbordar, sin basura al final- o se rechaza en
    // vez de degradar a 0 (que "parar todos" con un id fuera de rango).
    bool ParseEventIdParam(std::string const& param, Optional<uint32>& outId)
    {
        outId.reset();
        if (param.empty())
            return true;

        if (param.find_first_not_of("0123456789") != std::string::npos)
            return false;

        errno = 0;
        char* end = nullptr;
        unsigned long long value = std::strtoull(param.c_str(), &end, 10);
        if (errno == ERANGE || end != param.c_str() + param.size())
            return false;
        if (value > std::numeric_limits<uint32>::max())
            return false;

        outId = static_cast<uint32>(value);
        return true;
    }

    bool TeamMatches(Player* bot, PvpTeam team)
    {
        if (team == PVP_ANY)
            return true;
        return (bot->GetTeamId() == TEAM_ALLIANCE) == (team == PVP_ALLIANCE);
    }

    PvpTeam Opposite(PvpTeam team)
    {
        return team == PVP_ALLIANCE ? PVP_HORDE : PVP_ALLIANCE;
    }

    float Jitter()
    {
        return pcfg.jitter ? static_cast<float>(irand(-static_cast<int32>(pcfg.jitter), static_cast<int32>(pcfg.jitter))) : 0.0f;
    }

    // Humanos por zona: las escaramuzas se hacen donde hay alguien que las vea.
    std::vector<Player*> Humans()
    {
        std::vector<Player*> out;
        for (auto const& pair : ObjectAccessor::GetPlayers())
            if (IsHuman(pair.second))
                out.push_back(pair.second);
        return out;
    }

    // Un punto nunca a menos de 160 yardas de un humano: se aleja de él hasta
    // las 200, en la dirección contraria. Que no vea aparecer a nadie.
    void KeepAwayFromHumans(uint32 mapId, float& x, float& y, std::vector<Player*> const& humans)
    {
        for (Player* human : humans)
        {
            if (human->GetMapId() != mapId)
                continue;
            float const dx = x - human->GetPositionX();
            float const dy = y - human->GetPositionY();
            float const dist = std::sqrt(dx * dx + dy * dy);
            if (dist >= pcfg.keepAwayDist)
                continue;
            float const angle = dist > 1.0f ? std::atan2(dy, dx) : frand(0.0f, 2.0f * static_cast<float>(M_PI));
            x = human->GetPositionX() + std::cos(angle) * pcfg.repositionDist;
            y = human->GetPositionY() + std::sin(angle) * pcfg.repositionDist;
        }
    }

    // ─── Teletransporte (la secuencia de RandomTeleport de playerbots) ──────
    bool TeleportBotTo(Player* bot, uint32 mapId, float x, float y, float z, float o)
    {
#ifdef WORLD_BOTS_PVP_WITH_PLAYERBOTS
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        if (botAI->HasPlayerNearby(pcfg.nearbyRadius))
            return false;                   // alguien lo está viendo

        Map* map = sMapMgr->FindMap(mapId, 0);
        if (!map)
            return false;

        float const ground = map->GetHeight(bot->GetPhaseMask(), x, y, z + 50.0f, true, 100.0f);
        if (ground > INVALID_HEIGHT)
            z = ground + 0.05f;

        bot->GetMotionMaster()->Clear();
        botAI->Reset(true);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        if (!bot->TeleportTo(mapId, x, y, z, o))
            return false;
        bot->SendMovementFlagUpdate();
        if (Pet* pet = bot->GetPet())
            pet->NearTeleportTo(x, y, z, o);
        return true;
#else
        (void)bot; (void)mapId; (void)x; (void)y; (void)z; (void)o;
        return false;
#endif
    }

    void SetWarStrategies(Player* bot, bool war)
    {
#ifdef WORLD_BOTS_PVP_WITH_PLAYERBOTS
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;
        if (war)
        {
            botAI->ChangeStrategy(pcfg.combatStrategies, BOT_STATE_COMBAT);
            botAI->ChangeStrategy(pcfg.nonCombatStrategies, BOT_STATE_NON_COMBAT);
        }
        else
        {
            // Lo que el original no hacía: devolverle sus estrategias de siempre.
            botAI->ResetStrategies();
            botAI->rpgInfo.ChangeToIdle();
        }
#else
        (void)bot; (void)war;
#endif
    }

    // Declarada más abajo; LoadHotspots la necesita para cerrar eventos cuyo
    // punto caliente ha desaparecido de la tabla al recargar.
    void BeginEnd(PvpEvent& ev, uint64_t now, char const* why);

    // ─── Puntos calientes ───────────────────────────────────────────────────
    void LoadHotspots()
    {
        // Recargar (".wpvp recargar") con eventos en marcha no debe perder su
        // enfriamiento ni dejarlos huérfanos: se guarda por nombre antes de
        // vaciar g_hotspots y se restaura a los que sigan existiendo.
        std::unordered_map<std::string, uint64_t> oldCooldowns;
        for (Hotspot const& h : g_hotspots)
            if (h.lastStartMs)
                oldCooldowns.emplace(h.name, h.lastStartMs);

        g_hotspots.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT id, name, label, enabled, attacker_team, defender_team, min_level, max_level, map_id, "
            "rally_x, rally_y, rally_z, rally_o, target_x, target_y, target_z, target_o, "
            "attackers_min, attackers_max, defenders_min, defenders_max, duration_min, duration_max, weight, cooldown_seconds "
            "FROM world_bots_pvp_hotspot ORDER BY id");
        if (!result)
            LOG_INFO("module", "[world-bots] Sin tabla world_bots_pvp_hotspot (o vacia): sin guerra de mundo. La crea la fase 5.");
        else
        do
        {
            Field* f = result->Fetch();
            Hotspot h;
            h.id           = f[0].Get<uint32>();
            h.name         = f[1].Get<std::string>();
            h.label        = f[2].Get<std::string>();
            h.enabled      = f[3].Get<uint8>() != 0;
            h.attackers    = static_cast<PvpTeam>(std::min<uint8>(f[4].Get<uint8>(), 2));
            h.defenders    = static_cast<PvpTeam>(std::min<uint8>(f[5].Get<uint8>(), 2));
            h.minLevel     = f[6].Get<uint8>();
            h.maxLevel     = f[7].Get<uint8>();
            h.mapId        = f[8].Get<uint32>();
            h.rally        = Position(f[9].Get<float>(), f[10].Get<float>(), f[11].Get<float>(), f[12].Get<float>());
            h.target       = Position(f[13].Get<float>(), f[14].Get<float>(), f[15].Get<float>(), f[16].Get<float>());
            h.attackersMin = f[17].Get<uint32>();
            h.attackersMax = f[18].Get<uint32>();
            h.defendersMin = f[19].Get<uint32>();
            h.defendersMax = f[20].Get<uint32>();
            h.durationMin  = f[21].Get<uint32>();
            h.durationMax  = f[22].Get<uint32>();
            h.weight       = f[23].Get<uint32>();
            h.cooldownSecs = f[24].Get<uint32>();
            if (h.label.empty())
                h.label = h.name;

            // Un punto de 50-60 que llega apagado del SQL lo gobierna la
            // población: RefreshHotspotAvailability lo enciende cuando hay bots
            // de ese nivel en ambas facciones y lo apaga si dejan de existir.
            h.autoManaged = !h.enabled && h.minLevel >= 45;

            // Una fila con min > max llega cruda a urand() (StartEvent), y
            // urand tiene ASSERT(max >= min) activo incluso en Release: una
            // fila sucia en la tabla tumbaría el worldserver entero.
            auto fixOrder = [&](uint32& lo, uint32& hi, char const* field)
            {
                if (lo > hi)
                {
                    LOG_ERROR("module", "[world-bots] Punto caliente '{}' ({}): {} invertido ({} > {}), corregido al cargar.",
                              h.name, h.id, field, lo, hi);
                    std::swap(lo, hi);
                }
            };
            fixOrder(h.attackersMin, h.attackersMax, "attackers_min/max");
            fixOrder(h.defendersMin, h.defendersMax, "defenders_min/max");
            fixOrder(h.durationMin, h.durationMax, "duration_min/max");
            if (h.minLevel > h.maxLevel)
            {
                LOG_ERROR("module", "[world-bots] Punto caliente '{}' ({}): min_level/max_level invertido ({} > {}), corregido al cargar.",
                          h.name, h.id, h.minLevel, h.maxLevel);
                std::swap(h.minLevel, h.maxLevel);
            }

            if (Map* map = sMapMgr->CreateBaseMap(h.mapId))
                h.zoneId = map->GetZoneId(PHASEMASK_NORMAL, h.target.GetPositionX(), h.target.GetPositionY(), h.target.GetPositionZ());

            auto oldCooldown = oldCooldowns.find(h.name);
            if (oldCooldown != oldCooldowns.end())
                h.lastStartMs = oldCooldown->second;

            g_hotspots.push_back(h);
        } while (result->NextRow());

        // Enfriamientos que sobreviven a un reinicio del worldserver (los de
        // memoria solo sobreviven a `.wpvp recargar`, arriba). No pisan un
        // enfriamiento en memoria más reciente.
        if (QueryResult state = WorldDatabase.Query("SELECT name, last_start_unix FROM world_bots_pvp_state"))
        {
            uint64 const nowUnix = static_cast<uint64>(GameTime::GetGameTime().count());
            uint64 const nowMs = TimeMs::NowMs();
            do
            {
                Field* sf = state->Fetch();
                std::string const name = sf[0].Get<std::string>();
                uint64 const startUnix = sf[1].Get<uint64>();
                for (Hotspot& h : g_hotspots)
                    if (h.name == name && !h.lastStartMs && startUnix && nowUnix >= startUnix)
                    {
                        uint64 const ageSecs = nowUnix - startUnix;
                        if (ageSecs < h.cooldownSecs)
                        {
                            uint64 const back = TimeMs::SecsToMs(static_cast<uint32>(ageSecs));
                            // Con uptime < antigüedad del evento la resta envuelve;
                            // el `now - lastStartMs` de PickHotspot recupera el
                            // delta correcto por aritmética modular. Solo se evita
                            // el 0 exacto, que se interpretaría como "sin arrancar".
                            h.lastStartMs = (nowMs - back) ? (nowMs - back) : 1;
                        }
                    }
            } while (state->NextRow());
        }

        uint32 enabled = 0;
        for (Hotspot const& h : g_hotspots)
            if (h.enabled)
                ++enabled;
        LOG_INFO("module", "[world-bots] Guerra de mundo: {} puntos calientes cargados, {} activos.", g_hotspots.size(), enabled);

        // Un evento cuyo punto caliente ha desaparecido de la tabla (borrado o
        // renombrado) queda huérfano: PickHotspot ya no lo empareja con nada y
        // su hueco se reutilizaría de inmediato. Se cierra como cualquier otro.
        uint64_t const now = TimeMs::NowMs();
        for (PvpEvent& ev : g_events)
        {
            if (ev.ending)
                continue;
            bool const stillExists = std::any_of(g_hotspots.begin(), g_hotspots.end(),
                [&ev](Hotspot const& h) { return h.name == ev.spotName; });
            if (!stillExists)
                BeginEnd(ev, now, "el punto caliente ya no existe (recargado)");
        }
    }

    bool IsDuelSpot(Hotspot const& h)
    {
        return pcfg.duels && h.attackers != PVP_ANY && h.attackers == h.defenders;
    }

    // Bots libres de un bando y un tramo, barajados, sin los ya cogidos.
    std::vector<Player*> Collect(uint32 wanted, PvpTeam team, uint8 minLevel, uint8 maxLevel, std::set<ObjectGuid> const& taken)
    {
        std::vector<Player*> out;
        if (!wanted)
            return out;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* bot = pair.second;
            if (!IsFreeBot(bot) || !TeamMatches(bot, team))
                continue;
            if (bot->GetLevel() < minLevel || bot->GetLevel() > maxLevel || taken.count(bot->GetGUID()))
                continue;
            out.push_back(bot);
            if (out.size() >= wanted * 4 + 8)
                break;
        }
        Acore::Containers::RandomShuffle(out);
        if (out.size() > wanted)
            out.resize(wanted);
        return out;
    }

    void Announce(uint32 zoneId, uint32 mapId, std::string const& text)
    {
        if (!pcfg.announce)
            return;
        for (Player* human : Humans())
            if (human->GetMapId() == mapId && human->GetZoneId() == zoneId && human->GetSession())
                ChatHandler(human->GetSession()).SendSysMessage(text);
    }

    // ─── Empezar un evento ──────────────────────────────────────────────────
    bool StartEvent(Hotspot& spot, uint64_t now, std::string* why)
    {
        PvpTeam attackers = spot.attackers;
        PvpTeam defenders = spot.defenders;
        if (attackers == PVP_ANY)
            attackers = urand(0, 1) ? PVP_ALLIANCE : PVP_HORDE;
        if (defenders == PVP_ANY)
            defenders = Opposite(attackers);
        bool const duelMode = IsDuelSpot(spot);

        uint8 const minLevel = std::max<uint8>(spot.minLevel, static_cast<uint8>(pcfg.minLevel));
        uint8 const maxLevel = std::max<uint8>(spot.maxLevel, minLevel);

        uint32 const needAtt = urand(std::min(spot.attackersMin, pcfg.maxPerSide), std::min(spot.attackersMax, pcfg.maxPerSide));
        uint32 const needDef = urand(std::min(spot.defendersMin, pcfg.maxPerSide), std::min(spot.defendersMax, pcfg.maxPerSide));

        std::set<ObjectGuid> taken;
        std::vector<Player*> att = Collect(needAtt, attackers, minLevel, maxLevel, taken);
        for (Player* bot : att)
            taken.insert(bot->GetGUID());
        std::vector<Player*> def = Collect(needDef, defenders, minLevel, maxLevel, taken);

        if (!WorldBotsPolicy::EnoughBotsToStart(static_cast<uint32_t>(att.size()), needAtt,
                                                 static_cast<uint32_t>(def.size()), needDef, pcfg.minBotsToStart))
        {
            if (why)
                *why = Acore::StringFormat("faltan bots libres de nivel {}-{}: {} atacantes de {} y {} defensores de {}",
                                           minLevel, maxLevel, att.size(), needAtt, def.size(), needDef);
            return false;
        }

        std::vector<Player*> humans = Humans();

        // Un humano de la zona como referencia para topar el equipo de los bots
        // a su fase de progresión (no hay un "dueño" en una escaramuza).
        Player* refHuman = nullptr;
        for (Player* h : humans)
            if (h->GetMapId() == spot.mapId && h->GetZoneId() == spot.zoneId)
            {
                refHuman = h;
                break;
            }

        PvpEvent ev;
        ev.id = g_nextEventId++;
        ev.spotName = spot.name;
        ev.label = spot.label;
        ev.zoneId = spot.zoneId;
        ev.mapId = spot.mapId;
        ev.attackers = attackers;
        ev.defenders = defenders;
        ev.duelMode = duelMode;
        ev.startMs = now;
        ev.endMs = now + TimeMs::MinsToMs(urand(spot.durationMin, std::max(spot.durationMin, spot.durationMax)));
        // El control no se mide hasta que los atacantes han tenido tiempo de
        // llegar; el primer anuncio de progreso, un poco más tarde aún.
        ev.nextCaptureMs  = now + TimeMs::SecsToMs(pcfg.moveDelaySecs + 20);
        ev.nextProgressMs = now + TimeMs::SecsToMs(pcfg.moveDelaySecs + 60);

        auto place = [&](Player* bot, bool attacker)
        {
            BotClaims::Lease claim(bot->GetGUID().GetCounter(), OWNER);
            if (!claim || !IsFreeBot(bot, true))
            {
                LOG_DEBUG("module", "[world-bots] {} dejo de estar libre antes de reservarlo para {}.",
                          bot->GetName(), spot.name);
                return;
            }

            Position const& base = attacker ? spot.rally : spot.target;
            float x = base.GetPositionX() + Jitter();
            float y = base.GetPositionY() + Jitter();
            KeepAwayFromHumans(spot.mapId, x, y, humans);

            Member m;
            m.guid = bot->GetGUID();
            m.attacker = attacker;
            m.origin = WorldLocation(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation());

            if (!TeleportBotTo(bot, spot.mapId, x, y, base.GetPositionZ(), base.GetOrientation()))
            {
                LOG_DEBUG("module", "[world-bots] {} no se ha podido llevar a {} (alguien lo ve, o sin suelo).", bot->GetName(), spot.name);
                return;
            }

            if (!duelMode)
                bot->SetPvP(true);
            SetWarStrategies(bot, true);

            // Equipo a la fase del humano de la zona, no de Naxx40. El tope de
            // fase manda; con GearMode 0 no toca nada.
            //
            // Al consumirse (M02): la guerra sigue en marcha (no terminando) y
            // el bot sigue en ella; el objetivo se recalcula con el humano de
            // referencia si sigue en el mundo, y si no se conserva el de ahora.
            if (refHuman)
            {
                uint32 const target = BotGear::TargetItemLevel(refHuman, pcfg.gear);
                BotGear::Schedule(bot->GetGUID(), target, pcfg.gear, OWNER, BotGear::PRIORITY_EVENT,
                    [eventId = ev.id, ref = refHuman->GetGUID(), target](Player* member, uint32& ilvl)
                    {
                        auto evIt = std::find_if(g_events.begin(), g_events.end(),
                            [eventId](PvpEvent const& e) { return e.id == eventId; });
                        if (evIt == g_events.end() || evIt->ending
                            || std::none_of(evIt->members.begin(), evIt->members.end(),
                                   [member](Member const& m) { return m.guid == member->GetGUID(); }))
                            return BotGear::Decision::Cancel;
                        Player* human = ObjectAccessor::FindPlayer(ref);
                        ilvl = human ? BotGear::TargetItemLevel(human, pcfg.gear)
                                     : (pcfg.gear.mode != BotGear::MODE_OFF ? target : 0);
                        return ilvl ? BotGear::Decision::Apply : BotGear::Decision::Cancel;
                    });
            }

            if (attacker && !duelMode)
                m.moveAtMs = now + TimeMs::SecsToMs(pcfg.moveDelaySecs) + urand(0, 3000);
            ev.members.push_back(m);
            claim.Keep();
        };

        for (Player* bot : att)
            place(bot, true);
        for (Player* bot : def)
            place(bot, false);

        uint32 gotAtt = 0, gotDef = 0;
        for (Member const& m : ev.members)
            (m.attacker ? gotAtt : gotDef) += 1;

        if (gotAtt < 1 || gotDef < 1)
        {
            for (Member const& m : ev.members)
            {
                if (Player* bot = ObjectAccessor::FindPlayer(m.guid))
                {
                    SetWarStrategies(bot, false);
                    bot->SetPvP(false);
                    // No dejarlos plantados en el punto de reunión con sus
                    // estrategias normales: se les devuelve a donde estaban (P11).
                    TeleportBotTo(bot, m.origin.GetMapId(), m.origin.GetPositionX(), m.origin.GetPositionY(),
                                  m.origin.GetPositionZ(), m.origin.GetOrientation());
                }
                BotClaims::Release(m.guid.GetCounter(), OWNER);
            }
            if (why)
                *why = "no se pudo colocar a nadie sin que un jugador lo viera aparecer";
            return false;
        }

        // Duelos: parejas, uno reta al otro pasados unos segundos.
        if (duelMode)
        {
            std::vector<ObjectGuid> a, d;
            for (Member const& m : ev.members)
                (m.attacker ? a : d).push_back(m.guid);
            uint32 const pairs = std::min<uint32>(std::min(a.size(), d.size()), pcfg.duelPairs);
            for (uint32 i = 0; i < pairs; ++i)
                ev.duels.push_back({ a[i], d[i], now + TimeMs::SecsToMs(pcfg.moveDelaySecs + pcfg.duelDelaySecs + i * 4), 0 });
        }

        spot.lastStartMs = now;
        // Persistir el arranque: sin esto, tras un `systemctl restart` todos los
        // puntos calientes arrancan sin enfriar (§3.2 BAJO). Escritura asíncrona;
        // el nombre se escapa por si una fila editada a mano trae comillas.
        {
            std::string safeName = spot.name;
            WorldDatabase.EscapeString(safeName);
            WorldDatabase.Execute("INSERT INTO world_bots_pvp_state (name, last_start_unix) VALUES ('{}', {}) "
                                  "ON DUPLICATE KEY UPDATE last_start_unix = VALUES(last_start_unix)",
                                  safeName, static_cast<uint64>(GameTime::GetGameTime().count()));
        }
        g_events.push_back(ev);

        LOG_INFO("module", "[world-bots] Guerra #{}: {} en {} ({}): {} de {} contra {} de {}, {} min.",
                 ev.id, duelMode ? "duelos" : "escaramuza", spot.name, spot.zoneId,
                 gotAtt, TeamLabel(attackers), gotDef, TeamLabel(defenders), (ev.endMs - now) / 60000);

        if (duelMode)
            Announce(spot.zoneId, spot.mapId, Acore::StringFormat("|cffff8800[Duelos]|r Unos aventureros se retan a las puertas de {}.", spot.label));
        else
            Announce(spot.zoneId, spot.mapId, Acore::StringFormat("|cffff8800[Guerra]|r {} marcha sobre {}. {} responde.",
                                                                 attackers == PVP_ALLIANCE ? "La Alianza" : "La Horda", spot.label,
                                                                 defenders == PVP_ALLIANCE ? "La Alianza" : "La Horda"));
        return true;
    }

    // ─── Terminar: estrategias de siempre y vuelta a casa ───────────────────
    void BeginEnd(PvpEvent& ev, uint64_t now, char const* why)
    {
        if (ev.ending)
            return;
        ev.ending = true;
        ev.duels.clear();
        for (Member& m : ev.members)
            m.returnUntilMs = now + TimeMs::SecsToMs(pcfg.combatGraceSecs);
        LOG_INFO("module", "[world-bots] Guerra #{} en {} termina ({}) — control {}-{}.",
                 ev.id, ev.spotName, why, ev.scoreAtt, ev.scoreDef);
        if (!ev.duelMode)
        {
            if (pcfg.captureObjective && ev.scoreAtt != ev.scoreDef && (ev.scoreAtt || ev.scoreDef))
            {
                PvpTeam const winner = WorldBotsPolicy::AttackersLead(ev.scoreAtt, ev.scoreDef) ? ev.attackers : ev.defenders;
                Announce(ev.zoneId, ev.mapId, Acore::StringFormat(
                    "|cffff8800[Guerra]|r {} se impone en {} ({}-{}).",
                    winner == PVP_ALLIANCE ? "La Alianza" : "La Horda", ev.label, ev.scoreAtt, ev.scoreDef));
            }
            else
                Announce(ev.zoneId, ev.mapId, Acore::StringFormat("|cffff8800[Guerra]|r La escaramuza en {} se disuelve.", ev.label));
        }
    }

    // Devuelve true cuando el bot ya está de vuelta (o perdido) y se puede olvidar.
    bool ReturnMember(Member& m, uint64_t now)
    {
        Player* bot = ObjectAccessor::FindPlayer(m.guid);
        if (!bot || !bot->IsInWorld())
        {
            BotClaims::Release(m.guid.GetCounter(), OWNER);
            return true;
        }

        if (bot->IsBeingTeleported())
            return false;

        if (bot->IsInCombat() && now < m.returnUntilMs)
            return false;                   // que acabe la pelea; un minuto como mucho

        if (bot->IsInCombat())
            bot->CombatStop(true);

        if (bot->isDead())
        {
            bot->ResurrectPlayer(1.0f, false);
            bot->SpawnCorpseBones();
        }

        SetWarStrategies(bot, false);
        bot->SetPvP(false);          // no vuelve a casa con la marca JcJ puesta (§3.2 BAJO)
        if (!TeleportBotTo(bot, m.origin.GetMapId(), m.origin.GetPositionX(), m.origin.GetPositionY(),
                           m.origin.GetPositionZ(), m.origin.GetOrientation()))
        {
            // Alguien lo ve: se queda donde está, con sus estrategias de siempre.
            LOG_DEBUG("module", "[world-bots] {} se queda tras la guerra (alguien lo ve).", bot->GetName());
        }
        BotClaims::Release(m.guid.GetCounter(), OWNER);
        return true;
    }

    void TickEvent(PvpEvent& ev, uint64_t now)
    {
        if (!ev.ending && now >= ev.endMs)
            BeginEnd(ev, now, "duracion cumplida");

        if (ev.ending)
        {
            for (auto it = ev.members.begin(); it != ev.members.end();)
                it = ReturnMember(*it, now) ? ev.members.erase(it) : std::next(it);
            return;
        }

        // Avance de los atacantes hacia el objetivo.
        for (Member& m : ev.members)
        {
            if (!m.attacker || m.moved || !m.moveAtMs || now < m.moveAtMs)
                continue;
            Player* bot = ObjectAccessor::FindPlayer(m.guid);
            if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
                continue;
            m.moved = true;
            if (bot->IsInCombat())
                continue;                   // ya está en faena
            Hotspot const* spot = nullptr;
            for (Hotspot const& h : g_hotspots)
                if (h.name == ev.spotName)
                    spot = &h;
            if (!spot)
                continue;
            bot->GetMotionMaster()->MovePoint(0, spot->target.GetPositionX() + Jitter(), spot->target.GetPositionY() + Jitter(), spot->target.GetPositionZ());
            LOG_DEBUG("module", "[world-bots] {} avanza hacia {}.", bot->GetName(), spot->label);
        }

        // ─── Objetivo de captura ───────────────────────────────────────────
        // Cada ~15 s, el bando con más bots vivos dentro de captureRadius del
        // punto suma un tick de control. Al llegar a captureGoal ese bando
        // "captura" y el evento termina antes de tiempo.
        if (pcfg.captureObjective && !ev.duelMode && now >= ev.nextCaptureMs)
        {
            ev.nextCaptureMs = now + 15000;

            Hotspot const* spot = nullptr;
            for (Hotspot const& h : g_hotspots)
                if (h.name == ev.spotName) { spot = &h; break; }

            if (spot)
            {
                float const r2 = pcfg.captureRadius * pcfg.captureRadius;
                uint32 nearAtt = 0, nearDef = 0;
                for (Member const& m : ev.members)
                {
                    Player* bot = ObjectAccessor::FindPlayer(m.guid);
                    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetMapId() != ev.mapId)
                        continue;
                    if (bot->GetExactDist2dSq(spot->target.GetPositionX(), spot->target.GetPositionY()) <= r2)
                        (m.attacker ? nearAtt : nearDef) += 1;
                }

                WorldBotsPolicy::CaptureScore const score =
                    WorldBotsPolicy::AdvanceCaptureScore(ev.scoreAtt, ev.scoreDef, nearAtt, nearDef);
                ev.scoreAtt = score.attackers;
                ev.scoreDef = score.defenders;

                if (now >= ev.nextProgressMs && ev.scoreAtt != ev.scoreDef)
                {
                    ev.nextProgressMs = now + 60000;
                    PvpTeam const lead = WorldBotsPolicy::AttackersLead(ev.scoreAtt, ev.scoreDef) ? ev.attackers : ev.defenders;
                    Announce(ev.zoneId, ev.mapId, Acore::StringFormat(
                        "|cffff8800[Guerra]|r {} controla {} ({}-{}).",
                        lead == PVP_ALLIANCE ? "La Alianza" : "La Horda", ev.label, ev.scoreAtt, ev.scoreDef));
                }

                if (WorldBotsPolicy::CaptureGoalReached(ev.scoreAtt, ev.scoreDef, pcfg.captureGoal))
                {
                    BeginEnd(ev, now, "objetivo capturado");
                    return;
                }
            }
        }

        // Los que se hayan ido (desconectados, o alguien los echó): se olvidan.
        for (auto it = ev.members.begin(); it != ev.members.end();)
        {
            Player* bot = ObjectAccessor::FindPlayer(it->guid);
            if (!bot || !bot->IsInWorld())
            {
                BotClaims::Release(it->guid.GetCounter(), OWNER);
                it = ev.members.erase(it);
            }
            else
                ++it;
        }

        // Duelos.
        for (auto it = ev.duels.begin(); it != ev.duels.end();)
        {
            if (now < it->atMs)
            {
                ++it;
                continue;
            }
            Player* a = ObjectAccessor::FindPlayer(it->challenger);
            Player* b = ObjectAccessor::FindPlayer(it->target);
            if (!a || !b || !a->IsInWorld() || !b->IsInWorld() || a->IsBeingTeleported() || b->IsBeingTeleported() || it->tries >= pcfg.duelMaxTries)
            {
                if (a && b && it->tries < pcfg.duelMaxTries)
                {
                    ++it->tries;
                    it->atMs = now + 2000;
                    ++it;
                }
                else
                    it = ev.duels.erase(it);
                continue;
            }

            if (a->GetMapId() != b->GetMapId() || a->GetDistance(b) > pcfg.duelRange)
            {
                // Acercarlos: el hechizo de duelo pide distancia corta. Se ajusta
                // la z al suelo para no dejar al retado bajo tierra o cayendo
                // (§3.2 BAJO). No se comprueba HasPlayerNearby a propósito: los
                // duelos son a las puertas de la capital, donde siempre hay
                // jugadores, y esa comprobación los cancelaría todos.
                float dx = a->GetPositionX() + 3.0f;
                float dy = a->GetPositionY() + 3.0f;
                float dz = a->GetPositionZ();
                if (Map* map = a->GetMap())
                {
                    float const ground = map->GetHeight(a->GetPhaseMask(), dx, dy, dz + 2.0f, true, 20.0f);
                    if (ground > INVALID_HEIGHT)
                        dz = ground + 0.05f;
                }
                b->NearTeleportTo(dx, dy, dz, a->GetOrientation());
                ++it->tries;
                it->atMs = now + 3000;
                ++it;
                continue;
            }

            if (a->IsInCombat())
                a->CombatStop(true);
            if (b->IsInCombat())
                b->CombatStop(true);

            a->CastSpell(b, pcfg.duelSpell, true);
            a->SetFacingToObject(b);
            b->SetFacingToObject(a);
            LOG_INFO("module", "[world-bots] Duelo #{}: {} reta a {} en {}.", ev.id, a->GetName(), b->GetName(), ev.label);
            it = ev.duels.erase(it);
        }
    }

    // ─── Reactivación automática de los puntos calientes de 50-60 ───────────
    // Los tres puntos altos (SearingGorge, BlackrockMountain, LightHopeChapel)
    // vienen apagados del SQL. Se encienden cuando hay al menos minBotsToStart
    // bots libres de su tramo en las DOS facciones (StartEvent elige atacante y
    // defensor entre ellas), y se apagan si esa población desaparece.
    void RefreshHotspotAvailability()
    {
        if (!pcfg.autoEnableHighHotspots)
            return;
        std::set<ObjectGuid> const none;
        for (Hotspot& h : g_hotspots)
        {
            if (!h.autoManaged)
                continue;
            uint32 const ally  = static_cast<uint32>(
                Collect(pcfg.minBotsToStart, PVP_ALLIANCE, h.minLevel, h.maxLevel, none).size());
            uint32 const horde = static_cast<uint32>(
                Collect(pcfg.minBotsToStart, PVP_HORDE, h.minLevel, h.maxLevel, none).size());
            bool const ready = ally >= pcfg.minBotsToStart && horde >= pcfg.minBotsToStart;
            if (ready != h.enabled)
            {
                h.enabled = ready;
                LOG_INFO("module", "[world-bots] Punto caliente {} ({}-{}) {} por poblacion.",
                         h.name, h.minLevel, h.maxLevel, ready ? "activado" : "desactivado");
            }
        }
    }

    // ─── Elegir un punto caliente ───────────────────────────────────────────
    Hotspot* PickHotspot(uint64_t now)
    {
        std::vector<Player*> humans = Humans();
        std::set<uint32> humanZones;
        for (Player* human : humans)
        {
            Map* map = human->GetMap();
            if (map && !map->IsDungeon() && !map->IsBattlegroundOrArena())
                humanZones.insert(human->GetZoneId());
        }

        std::vector<Hotspot*> candidates;
        uint32 total = 0;
        for (Hotspot& h : g_hotspots)
        {
            if (!h.enabled || !h.weight)
                continue;
            if (h.lastStartMs && now - h.lastStartMs < TimeMs::SecsToMs(h.cooldownSecs))
                continue;
            if (pcfg.onlyWithPlayer && !humanZones.count(h.zoneId))
                continue;
            bool busy = false;
            for (PvpEvent const& ev : g_events)
                if (ev.spotName == h.name)
                    busy = true;
            if (busy)
                continue;
            candidates.push_back(&h);
            total += h.weight;
        }
        if (candidates.empty())
            return nullptr;

        uint32 roll = urand(1, total);
        uint32 cursor = 0;
        for (Hotspot* h : candidates)
        {
            cursor += h->weight;
            if (roll <= cursor)
                return h;
        }
        return candidates.back();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Configuración, carga y trabajo
// ─────────────────────────────────────────────────────────────────────────────
class mod_world_bots_pvp_world : public WorldScript
{
public:
    mod_world_bots_pvp_world() : WorldScript("mod_world_bots_pvp_world",
                                             { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        bool const parent = sConfigMgr->GetOption<bool>("WorldBots.Enable", true);
        pcfg.enabled             = parent && sConfigMgr->GetOption<bool>("WorldBots.Pvp.Enable", true);
        pcfg.tickSecs            = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.TickSeconds", 60);
        pcfg.chancePerTick       = std::clamp(sConfigMgr->GetOption<float>("WorldBots.Pvp.EventChancePerTick", 25.0f), 0.0f, 100.0f);
        pcfg.maxActive           = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.MaxActiveEvents", 1);
        pcfg.startupDelaySecs    = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.StartupDelaySeconds", 120);
        pcfg.minLevel            = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.MinLevel", 20);
        pcfg.maxPerSide          = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("WorldBots.Pvp.MaxBotsPerSide", 8));
        pcfg.moveDelaySecs       = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.MoveDelaySeconds", 15);
        pcfg.jitter              = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.PositionJitter", 8);
        pcfg.duels               = sConfigMgr->GetOption<bool>("WorldBots.Pvp.Duels", true);
        pcfg.duelPairs           = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.DuelPairs", 4);
        pcfg.duelDelaySecs       = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.DuelDelaySeconds", 8);
        pcfg.duelSpell           = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.DuelSpellId", 7266);
        pcfg.onlyWithPlayer      = sConfigMgr->GetOption<bool>("WorldBots.Pvp.OnlyWithPlayerInZone", true);
        pcfg.announce            = sConfigMgr->GetOption<bool>("WorldBots.Pvp.Announce", true);
        pcfg.combatStrategies    = sConfigMgr->GetOption<std::string>("WorldBots.Pvp.CombatStrategies", "+pvp,+boost,+dps debuff,-passive,-stay");
        pcfg.nonCombatStrategies = sConfigMgr->GetOption<std::string>("WorldBots.Pvp.NonCombatStrategies", "+pvp,+duel,+boost,-passive,-stay");
        pcfg.botMinWorldSecs      = sConfigMgr->GetOption<uint32>("WorldBots.BotMinWorldSeconds", 60);
        pcfg.nearbyRadius         = std::max(sConfigMgr->GetOption<float>("WorldBots.Pvp.NearbyPlayerRadius",
                                             sConfigMgr->GetOption<float>("WorldBots.NearbyPlayerRadius", 150.0f)), 50.0f);
        pcfg.keepAwayDist         = std::max(sConfigMgr->GetOption<float>("WorldBots.Pvp.KeepAwayDistance", 160.0f), 0.0f);
        pcfg.repositionDist       = std::max(sConfigMgr->GetOption<float>("WorldBots.Pvp.RepositionDistance", 200.0f), 1.0f);
        pcfg.combatGraceSecs      = sConfigMgr->GetOption<uint32>("WorldBots.Pvp.CombatGraceSeconds", 60);
        pcfg.duelRange            = std::max(sConfigMgr->GetOption<float>("WorldBots.Pvp.DuelRange", 35.0f), 5.0f);
        pcfg.duelMaxTries         = std::max<uint32>(sConfigMgr->GetOption<uint32>("WorldBots.Pvp.DuelMaxTries", 20), 1);
        pcfg.minBotsToStart       = std::max<uint32>(sConfigMgr->GetOption<uint32>("WorldBots.Pvp.MinBotsToStart", 2), 1);
        pcfg.autoEnableHighHotspots = sConfigMgr->GetOption<bool>("WorldBots.Pvp.AutoEnableHighHotspots", true);
        pcfg.captureObjective     = sConfigMgr->GetOption<bool>("WorldBots.Pvp.CaptureObjective", true);
        pcfg.captureRadius        = std::max(sConfigMgr->GetOption<float>("WorldBots.Pvp.CaptureRadius", 40.0f), 5.0f);
        pcfg.captureGoal          = std::max<uint32>(sConfigMgr->GetOption<uint32>("WorldBots.Pvp.CaptureGoal", 10), 1);
        pcfg.gear                 = BotGear::LoadSettings("WorldBots.Pvp");

#ifndef WORLD_BOTS_PVP_WITH_PLAYERBOTS
        pcfg.enabled = false;
#endif

        // Activacion en caliente: los hotspots solo se cargaban en OnStartup, asi
        // que encender el submodulo con ".reload config" lo dejaba sin datos y
        // sin arrancar nunca. Al detectar el flanco off->on se hace lo mismo que
        // en OnStartup.
        if (reload && pcfg.enabled && !g_pvpEnabled && g_events.empty())
        {
            LoadHotspots();
            g_startedMs = TimeMs::NowMs();
            g_nextTickMs = g_startedMs + TimeMs::SecsToMs(pcfg.startupDelaySecs);
            LOG_INFO("module", "[world-bots-pvp] Activado en caliente: {} hotspots cargados.", g_hotspots.size());
        }
        g_pvpEnabled = pcfg.enabled;
    }

    void OnStartup() override
    {
        if (!pcfg.enabled)
            return;
        LoadHotspots();
        g_startedMs = TimeMs::NowMs();
        g_nextTickMs = g_startedMs + TimeMs::SecsToMs(pcfg.startupDelaySecs);
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        uint32 const t0 = getMSTime();
        DoUpdate();
        SlowTick::WarnIfSlow("world-bots-pvp", "OnUpdate", t0);
    }

private:
    // ya se publicó la estadística con enabled=false de este apagado.
    bool _offPublished = false;

    void DoUpdate()
    {
        uint64_t const now = TimeMs::NowMs();

        if (!pcfg.enabled)
        {
            // buzón cerrado (con eventos terminando, "parar" ya lo hace
            // el propio apagado) y una última estadística con enabled=false.
            BotOperations::SetTargetOffline(BotOperations::Target::WorldBotsPvp, now,
                                            "La guerra de mundo esta desactivada.");
            if (g_events.empty())
            {
                if (!_offPublished)
                {
                    _offPublished = true;
                    BotOperations::WorldPvpStats stats;
                    stats.hotspotCount = static_cast<uint32_t>(g_hotspots.size());
                    stats.updatedAtMs = now;
                    BotOperations::PublishWorldPvpStats(std::move(stats));
                }
                return;
            }
        }
        else
            _offPublished = false;

        if (!pcfg.enabled)
        {
            for (PvpEvent& ev : g_events)
            {
                BeginEnd(ev, now, "modulo desactivado");
                TickEvent(ev, now);
            }
            g_events.remove_if([](PvpEvent const& ev) { return ev.ending && ev.members.empty(); });
            if (g_events.empty())
                BotClaims::ReleaseAll(OWNER);
            return;
        }

        std::deque<Request> requests;
        {
            std::lock_guard<std::mutex> lock(g_pendingLock);
            requests.swap(g_pending);
        }
        for (Request const& request : requests)
            Handle(request, now);

        // Acciones del panel (mod-bot-operations, via BotOperations.h):
        // "para un evento atascado" se traduce a la misma peticion interna
        // que ya atiende ".wpvp parar" (REQ_STOP, gm vacio: Reply() no hace
        // nada si no encuentra jugador). Se resuelve en el acto, no se
        // reencola en g_pending, para no perder un tick.
        for (BotOperations::ActionRequest const& request : BotOperations::TakeRequests(BotOperations::Target::WorldBotsPvp, now))
        {
            if (request.type == BotOperations::ActionType::StopWorldPvp)
            {
                Optional<uint32> eventId;
                if (!ParseEventIdParam(request.param, eventId))
                {
                    BotOperations::ReportOutcome(request.id, false,
                        "Identificador de evento invalido.", now);
                }
                else
                {
                    uint32 const before = static_cast<uint32>(g_events.size());
                    Handle({ ObjectGuid::Empty, REQ_STOP, "", eventId }, now);
                    BotOperations::ReportOutcome(request.id, true,
                        Acore::StringFormat("{} evento(s) activos antes de parar.", before), now);
                }
            }
            else
                BotOperations::ReportOutcome(request.id, false, "Accion no reconocida por world-bots-pvp.", now);
        }

        // Equipo de los bots de la guerra a la fase del humano (presupuesto
        // global compartido con queue-bots / party-here).
        BotGear::ProcessOne();

        for (PvpEvent& ev : g_events)
            TickEvent(ev, now);
        g_events.remove_if([](PvpEvent const& ev) { return ev.ending && ev.members.empty(); });

        // Estadisticas para el panel (pestaña "Mundo y etapa" → PvP): solo
        // este OnUpdate es dueño de g_events/g_hotspots.
        {
            BotOperations::WorldPvpStats stats;
            stats.enabled = pcfg.enabled;
            for (PvpEvent const& ev : g_events)
            {
                BotOperations::PvpEventInfo info;
                info.id = ev.id;
                info.label = ev.label;
                info.zoneId = ev.zoneId;
                info.mapId = ev.mapId;
                info.attackers = 0;
                info.defenders = 0;
                for (Member const& member : ev.members)
                    member.attacker ? ++info.attackers : ++info.defenders;
                info.ending = ev.ending;
                info.startedAtMs = ev.startMs;
                stats.activeEvents.push_back(std::move(info));
            }
            stats.hotspots.reserve(g_hotspots.size());
            for (Hotspot const& h : g_hotspots)
            {
                BotOperations::HotspotInfo info;
                info.id = h.id;
                info.name = h.name;
                info.label = h.label;
                info.enabled = h.enabled;
                info.attackerTeam = static_cast<uint8_t>(h.attackers);
                info.defenderTeam = static_cast<uint8_t>(h.defenders);
                info.minLevel = h.minLevel;
                info.maxLevel = h.maxLevel;
                info.zoneId = h.zoneId;
                // Misma comprobación que PickHotspot/el aviso de ".wpvp estado"
                // más abajo (resta, no comparación de marcas absolutas: ver el
                // comentario de BotOperations::HotspotInfo::cooldownRemainingMs).
                uint64_t const cooldownMs = TimeMs::SecsToMs(h.cooldownSecs);
                bool const onCooldown = h.lastStartMs && (now - h.lastStartMs) < cooldownMs;
                info.cooldownRemainingMs = onCooldown ? (cooldownMs - (now - h.lastStartMs)) : 0;
                stats.hotspots.push_back(std::move(info));
            }
            stats.hotspotCount = static_cast<uint32_t>(g_hotspots.size());
            stats.updatedAtMs = now;
            BotOperations::PublishWorldPvpStats(std::move(stats));
        }

        if (now < g_nextTickMs)
            return;
        g_nextTickMs = now + TimeMs::SecsToMs(pcfg.tickSecs);

        RefreshHotspotAvailability();

        if (g_hotspots.empty() || g_events.size() >= pcfg.maxActive)
            return;
        if (!roll_chance_f(pcfg.chancePerTick))
            return;

        Hotspot* spot = PickHotspot(now);
        if (!spot)
            return;

        std::string why;
        if (!StartEvent(*spot, now, &why))
            LOG_INFO("module", "[world-bots] Guerra en {} no arranca: {}.", spot->name, why);
    }

private:
    void Reply(ObjectGuid gm, std::string const& text)
    {
        Player* player = ObjectAccessor::FindPlayer(gm);
        if (player && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(text);
    }

    void Handle(Request const& r, uint64_t now)
    {
        switch (r.kind)
        {
            case REQ_STATUS:
            {
                Reply(r.gm, Acore::StringFormat("Guerra de mundo: {} eventos activos, {} puntos calientes ({} activos).",
                                                g_events.size(), g_hotspots.size(),
                                                std::count_if(g_hotspots.begin(), g_hotspots.end(), [](Hotspot const& h) { return h.enabled; })));
                for (PvpEvent const& ev : g_events)
                {
                    uint32 att = 0, def = 0;
                    for (Member const& m : ev.members)
                        (m.attacker ? att : def) += 1;
                    Reply(r.gm, Acore::StringFormat("  #{} {} ({}) {} vs {}: {} + {} bots, control {}-{}, {} min restantes{}",
                                                    ev.id, ev.spotName, ev.duelMode ? "duelos" : "escaramuza",
                                                    TeamLabel(ev.attackers), TeamLabel(ev.defenders), att, def,
                                                    ev.scoreAtt, ev.scoreDef,
                                                    ev.endMs > now ? (ev.endMs - now) / 60000 : 0, ev.ending ? ", terminando" : ""));
                }
                break;
            }
            case REQ_LIST:
            {
                for (Hotspot const& h : g_hotspots)
                    Reply(r.gm, Acore::StringFormat("  #{} {} [{}] {}: {} vs {}, nivel {}-{}, zona {}, {}-{} min, peso {}{}",
                                                    h.id, h.name, h.label,
                                                    h.autoManaged ? (h.enabled ? "auto" : "auto-espera") : (h.enabled ? "on" : "off"),
                                                    TeamLabel(h.attackers), TeamLabel(h.defenders),
                                                    h.minLevel, h.maxLevel, h.zoneId, h.durationMin, h.durationMax, h.weight,
                                                    (h.lastStartMs && now - h.lastStartMs < TimeMs::SecsToMs(h.cooldownSecs)) ? ", enfriando" : ""));
                break;
            }
            case REQ_START:
            {
                Hotspot* spot = nullptr;
                for (Hotspot& h : g_hotspots)
                    if (h.name == r.name)
                        spot = &h;
                if (!spot)
                {
                    Reply(r.gm, Acore::StringFormat("No hay ningun punto caliente llamado '{}'. Usa .wpvp lista.", r.name));
                    break;
                }
                std::string why;
                if (StartEvent(*spot, now, &why))
                    Reply(r.gm, Acore::StringFormat("Evento #{} arrancado en {}.", g_events.back().id, spot->label));
                else
                    Reply(r.gm, Acore::StringFormat("No arranca en {}: {}.", spot->label, why));
                break;
            }
            case REQ_STOP:
            {
                // r.id vacio (nullopt) es la unica forma explicita de
                // "todos"; un id concreto -aunque sea 0 o no exista- nunca
                // toca el resto de eventos.
                uint32 stopped = 0;
                bool const all = !r.id.has_value();
                for (PvpEvent& ev : g_events)
                    if (all || ev.id == *r.id)
                    {
                        BeginEnd(ev, now, "parado por el GM");
                        ++stopped;
                    }
                Reply(r.gm, Acore::StringFormat("{} evento(s) terminando: los bots vuelven a casa.", stopped));
                break;
            }
            case REQ_RELOAD:
            {
                LoadHotspots();
                Reply(r.gm, Acore::StringFormat("Puntos calientes recargados: {}.", g_hotspots.size()));
                break;
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  .wpvp (GM): estado, lista, iniciar <nombre>, parar [id], recargar
// ─────────────────────────────────────────────────────────────────────────────
class mod_world_bots_pvp_command : public CommandScript
{
public:
    mod_world_bots_pvp_command() : CommandScript("mod_world_bots_pvp_command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wpvpTable =
        {
            { "estado",   HandleStatus, SEC_GAMEMASTER, Console::No },
            { "lista",    HandleList,   SEC_GAMEMASTER, Console::No },
            { "iniciar",  HandleStart,  SEC_GAMEMASTER, Console::No },
            { "parar",    HandleStop,   SEC_GAMEMASTER, Console::No },
            { "recargar", HandleReload, SEC_GAMEMASTER, Console::No },
            { "",         HandleStatus, SEC_GAMEMASTER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "wpvp", wpvpTable },
        };
        return commandTable;
    }

    static ObjectGuid Gm(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return ObjectGuid::Empty;
        if (!pcfg.enabled)
        {
            handler->SendSysMessage("La guerra de mundo de mod-world-bots esta desactivada (WorldBots.Pvp.Enable).");
            return ObjectGuid::Empty;
        }
        return player->GetGUID();
    }

    static void Push(Request const& r)
    {
        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending.push_back(r);
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        if (ObjectGuid gm = Gm(handler))
            Push({ gm, REQ_STATUS, "", {} });
        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        if (ObjectGuid gm = Gm(handler))
            Push({ gm, REQ_LIST, "", {} });
        return true;
    }

    static bool HandleStart(ChatHandler* handler, std::string_view name)
    {
        if (ObjectGuid gm = Gm(handler))
            Push({ gm, REQ_START, std::string(name), {} });
        return true;
    }

    static bool HandleStop(ChatHandler* handler, Optional<uint32> id)
    {
        // sin argumento = todos los eventos; con argumento (incluido 0)
        // se busca ese id concreto, sin colapsar ambos casos en un mismo 0.
        if (ObjectGuid gm = Gm(handler))
            Push({ gm, REQ_STOP, "", id });
        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        if (ObjectGuid gm = Gm(handler))
            Push({ gm, REQ_RELOAD, "", {} });
        return true;
    }
};

void AddSC_mod_world_bots_pvp()
{
    new mod_world_bots_pvp_world();
    new mod_world_bots_pvp_command();
}
