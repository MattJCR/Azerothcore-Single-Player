// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-bot-operations — fuente de datos y cola de acciones para el panel web.
 *
 * EL PROBLEMA
 * El panel quiere una vista "Operaciones de bots" (colas, mundo/etapa, grupos,
 * hermandades, compañeros de misión, reservas) y unas pocas acciones seguras
 * (adelantar una pasada normal, parar un evento PvP atascado). Pero el panel
 * es un proceso Node aparte, sin acceso a la memoria del worldserver, y no
 * debe interpretar texto de SOAP ni ejecutar comandos arbitrarios (ver
 * REFERENCES.md, operaciones de bots en el panel web).
 *
 * LA SOLUCIÓN
 * Este módulo es el único puente entre `modules/shared/BotOperations.h` (en
 * memoria, dentro del worldserver) y MySQL (`acore_world`), que es lo único
 * que el panel lee y escribe:
 *
 *   - Cada `BotOperations.SnapshotIntervalSeconds` (10 s por defecto), junta
 *     lo que publican mod-queue-bots/mod-world-bots/mod-party-here/
 *     mod-quest-mates/mod-home-guild en BotOperations.h, más
 *     BotPopulationCoordinator::GetDiagnostics() y BotClaims::GetDiagnostics()
 *     (reservas de login y ocupación de bots, ya centralizadas ahí), y lo
 *     escribe como JSON en `bot_operations_snapshot` (una sola fila, id=1).
 *
 *   - Cada `BotOperations.ActionPollIntervalSeconds` (2 s por defecto), lee
 *     las filas `status='pending'` de `bot_operations_action` (las deja el
 *     panel), las traduce a `BotOperations::ActionRequest` y las encola con
 *     `BotOperations::EnqueueRequest()` para que el módulo destino las
 *     atienda en su propio `OnUpdate` (regla 5 de modules/README.md: solo el
 *     dueño del estado lo toca). El resultado que publican con
 *     `ReportOutcome()` se recoge con `TakeOutcomes()` y se vuelca a la fila.
 *     Una fila `pending` sin resolver más allá de `ActionMaxAgeSeconds` se
 *     marca `expired`: si el módulo destino está apagado o el proceso se
 *     reinició con la petición a medias, no se queda pendiente para siempre.
 *     Desde M07 (24/09/2026) también se retira del buzón (no se ejecuta al
 *     reactivarse el destino), un destino apagado, no cargado o colgado la
 *     rechaza al momento, el buzón tiene tope y una fila ya resuelta no se
 *     vuelve a encolar aunque una lectura atrasada la traiga como 'pending'
 *     (antes cada acción se ejecutaba dos veces por eso). Cada sección de la
 *     instantánea lleva `ageMs` y `stale`.
 *
 *   - `refresh_snapshot` es la única acción que no via ningún módulo destino:
 *     la resuelve este mismo módulo, adelantando su propio reloj de
 *     instantánea para publicar en el mismo tick.
 *
 * Este módulo NUNCA teletransporta, desconecta, sube de nivel, reequipa ni
 * fuerza logins masivos de bots — ninguna acción que expone lo hace; sólo
 * adelanta relojes que los módulos destino ya respetan con sus propios
 * límites y presupuestos.
 */

#include "AsyncCallbackProcessor.h"
#include "BotClaims.h"
#include "BotOperations.h"
#include "BotPopulationCoordinator.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ModLocale.h"
#include "bot_operations_locale.h"
#include "ScriptMgr.h"
#include "SlowTick.h"
#include "StringFormat.h"
#include "TimeMs.h"
#include "Timer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    struct Config
    {
        bool   enabled               = false;
        uint32 snapshotIntervalSecs  = 10;
        uint32 actionPollIntervalSecs = 2;
        uint32 actionQueueMax        = 20;
        uint32 actionMaxAgeSeconds   = 120;
    };
    Config cfg;

    // se incrementa en cada recarga de configuración. Una
    // respuesta de AsyncQuery que estaba en vuelo cuando llegó un `.reload`
    // (o un apagado/encendido en caliente, que también pasa por
    // OnAfterConfigLoad) queda etiquetada con la generación anterior y se
    // descarta al consumirse, en vez de encolar trabajo con una
    // configuración que ya no es la vigente.
    uint32 g_configGeneration = 0;

    uint64_t g_nextSnapshotMs = 0;
    uint64_t g_nextActionPollMs = 0;

    // Peticiones ya encoladas en BotOperations.h, a la espera de un resultado
    // (id -> instante en que se encolaron, sólo para el registro; la caducidad
    // real se calcula sobre requested_at de la fila, no sobre esto, así que
    // sobrevive a un reinicio del worldserver a medio contestar).
    std::unordered_map<uint64_t, uint64_t> g_inFlight;

    // filas ya resueltas por este proceso (id -> instante). Una lectura
    // de SQL lanzada antes de que llegara el UPDATE del resultado todavía las
    // trae como 'pending'; sin esto, al haber salido de g_inFlight, se
    // volverían a encolar y el destino las ejecutaría dos veces. Se olvidan
    // pasado kResolvedMemoryMs (mucho más que cualquier lectura en vuelo).
    std::unordered_map<uint64_t, uint64_t> g_resolved;
    constexpr uint64_t kResolvedMemoryMs = 10 * 60 * 1000;

    // La consulta de bot_operations_action era SÍNCRONA (WorldDatabase.Query):
    // bloqueaba el hilo del mundo hasta que MySQL contestaba, en cada pasada
    // de ActionPollIntervalSeconds (2 s por defecto). AsyncQuery() manda el
    // SELECT al hilo de BD y este procesador recoge el resultado cuando está
    // listo, en un OnUpdate posterior — el hilo del mundo nunca espera a
    // MySQL. Igual que World::_queryProcessor (World.cpp), pero a nivel de
    // módulo: propiedad exclusiva de mod_bot_operations_world::OnUpdate.
    QueryCallbackProcessor g_actionQuery;

    // como mucho una lectura de bot_operations_action en
    // vuelo. Antes, ProcessActions() lanzaba un AsyncQuery nuevo en CADA
    // pasada (cada ActionPollIntervalSeconds) sin comprobar si la anterior
    // seguía sin contestar — con MySQL lento eso podía acumular varias
    // consultas idénticas en vez de esperar a que la primera terminase.
    bool g_actionQueryPending = false;

    // ---- JSON minimo, de mano: sin libreria, solo escapado de cadenas. Solo
    // sirve para construir objetos planos con AddRaw para anidar arrays/objetos
    // ya construidos; no hace falta parsear nada, solo el panel (JS) lo lee.

    std::string JsonEscape(std::string const& value)
    {
        std::string out;
        out.reserve(value.size() + 8);
        for (unsigned char c : value)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                        out += static_cast<char>(c);
            }
        }
        return out;
    }

    class JsonBuilder
    {
    public:
        JsonBuilder& Add(char const* key, bool value)     { return AddRaw(key, value ? "true" : "false"); }
        JsonBuilder& Add(char const* key, uint8_t value)   { return AddRaw(key, std::to_string(value)); }
        JsonBuilder& Add(char const* key, uint32_t value)  { return AddRaw(key, std::to_string(value)); }
        JsonBuilder& Add(char const* key, uint64_t value)  { return AddRaw(key, std::to_string(value)); }

        JsonBuilder& Add(char const* key, std::string const& value)
        {
            Sep();
            buf += '"'; buf += key; buf += "\":\""; buf += JsonEscape(value); buf += '"';
            return *this;
        }

        JsonBuilder& AddRaw(char const* key, std::string const& rawJson)
        {
            Sep();
            buf += '"'; buf += key; buf += "\":"; buf += rawJson;
            return *this;
        }

        std::string Build() const { return "{" + buf + "}"; }

    private:
        void Sep() { if (!buf.empty()) buf += ','; }
        std::string buf;
    };

    std::string JsonArray(std::vector<uint32_t> const& values)
    {
        std::string out = "[";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i) out += ",";
            out += std::to_string(values[i]);
        }
        out += "]";
        return out;
    }

    // ---- Traducción struct -> JSON, una por cada área publicada en BotOperations.h ----

    std::string ToJson(BotOperations::QueueStats const& s)
    {
        return JsonBuilder()
            .Add("enabled", s.enabled)
            .Add("waitingHumans", s.waitingHumans)
            .Add("botsFilled", s.botsFilled)
            .Add("arena1v1Missing", s.arena1v1Missing)
            .Add("battlegroundMissing", s.battlegroundMissing)
            .Add("arenaMissing", s.arenaMissing)
            .Add("dungeonMissing", s.dungeonMissing)
            .Add("raidMissing", s.raidMissing)
            .Add("updatedAtMs", s.updatedAtMs)
            .Build();
    }

    std::string ToJson(BotOperations::WorldStageStats const& s)
    {
        return JsonBuilder()
            .Add("enabled", s.enabled)
            .Add("stage", static_cast<uint32_t>(s.stage))
            .Add("stageName", s.stageName)
            .Add("decidedBy", s.decidedBy)
            .Add("levelCap", s.levelCap)
            .AddRaw("maps", JsonArray(s.maps))
            .Add("zonesPopulated", s.zonesPopulated)
            .Add("relocatedTotal", s.relocatedTotal)
            .Add("rerolledTotal", s.rerolledTotal)
            .Add("updatedAtMs", s.updatedAtMs)
            .Build();
    }

    std::string ToJson(BotOperations::PvpEventInfo const& e)
    {
        return JsonBuilder()
            .Add("id", e.id)
            .Add("label", e.label)
            .Add("zoneId", e.zoneId)
            .Add("mapId", e.mapId)
            .Add("attackers", e.attackers)
            .Add("defenders", e.defenders)
            .Add("ending", e.ending)
            .Add("startedAtMs", e.startedAtMs)
            .Build();
    }

    std::string ToJson(BotOperations::HotspotInfo const& h)
    {
        return JsonBuilder()
            .Add("id", h.id)
            .Add("name", h.name)
            .Add("label", h.label)
            .Add("enabled", h.enabled)
            .Add("attackerTeam", h.attackerTeam)
            .Add("defenderTeam", h.defenderTeam)
            .Add("minLevel", h.minLevel)
            .Add("maxLevel", h.maxLevel)
            .Add("zoneId", h.zoneId)
            .Add("cooldownRemainingMs", h.cooldownRemainingMs)
            .Build();
    }

    std::string ToJson(BotOperations::WorldPvpStats const& s)
    {
        std::string events = "[";
        for (size_t i = 0; i < s.activeEvents.size(); ++i)
        {
            if (i) events += ",";
            events += ToJson(s.activeEvents[i]);
        }
        events += "]";

        std::string hotspots = "[";
        for (size_t i = 0; i < s.hotspots.size(); ++i)
        {
            if (i) hotspots += ",";
            hotspots += ToJson(s.hotspots[i]);
        }
        hotspots += "]";

        return JsonBuilder()
            .Add("enabled", s.enabled)
            .AddRaw("activeEvents", events)
            .AddRaw("hotspots", hotspots)
            .Add("hotspotCount", s.hotspotCount)
            .Add("updatedAtMs", s.updatedAtMs)
            .Build();
    }

    std::string ToJson(BotOperations::GroupStats const& s)
    {
        return JsonBuilder()
            .Add("enabled", s.enabled)
            .Add("groupsFormed", s.groupsFormed)
            .Add("raidsFormed", s.raidsFormed)
            .Add("botsInGroups", s.botsInGroups)
            .Add("updatedAtMs", s.updatedAtMs)
            .Build();
    }

    std::string ToJson(BotOperations::QuestMateStats const& s)
    {
        return JsonBuilder()
            .Add("enabled", s.enabled)
            .Add("humansWithMates", s.humansWithMates)
            .Add("activePairings", s.activePairings)
            .Add("updatedAtMs", s.updatedAtMs)
            .Build();
    }

    std::string ToJson(BotOperations::GuildStats const& s)
    {
        return JsonBuilder()
            .Add("enabled", s.enabled)
            .Add("homeGuilds", s.homeGuilds)
            .Add("botsManaged", s.botsManaged)
            .Add("updatedAtMs", s.updatedAtMs)
            .Build();
    }

    // Reservas de login (BotPopulationCoordinator) + ocupación de bots
    // (BotClaims): las dos cabeceras que ya centralizaban esto antes de que
    // existiera este módulo, sin duplicar su lógica.
    std::string ToJson(BotPopulationCoordinator::Diagnostics const& d, BotClaims::Diagnostics const& claims)
    {
        std::string onlineByRange = "[";
        bool firstOnline = true;
        for (auto const& [range, count] : d.onlineByRange)
        {
            if (!firstOnline) onlineByRange += ",";
            firstOnline = false;
            onlineByRange += JsonBuilder().Add("minLevel", range.first).Add("maxLevel", range.second).Add("count", count).Build();
        }
        onlineByRange += "]";

        std::string pendingByModule = "[";
        bool firstModule = true;
        for (auto const& [module, count] : d.pendingByModule)
        {
            if (!firstModule) pendingByModule += ",";
            firstModule = false;
            pendingByModule += JsonBuilder().Add("module", module).Add("count", count).Build();
        }
        pendingByModule += "]";

        std::string pendingByRange = "[";
        bool firstPending = true;
        for (auto const& [range, count] : d.pendingByRange)
        {
            if (!firstPending) pendingByRange += ",";
            firstPending = false;
            pendingByRange += JsonBuilder().Add("minLevel", range.first).Add("maxLevel", range.second).Add("count", count).Build();
        }
        pendingByRange += "]";

        std::string claimsByOwner = "[";
        bool firstClaim = true;
        for (auto const& [owner, count] : claims.claimsByOwner)
        {
            if (!firstClaim) claimsByOwner += ",";
            firstClaim = false;
            claimsByOwner += JsonBuilder().Add("module", owner).Add("count", count).Build();
        }
        claimsByOwner += "]";

        return JsonBuilder()
            .Add("dataAgeMs", d.dataAgeMs)
            .Add("globalCap", d.settings.globalCap)
            .Add("onlineTotal", d.onlineTotal)
            .Add("onlineAlliance", d.onlineByFaction[0])
            .Add("onlineHorde", d.onlineByFaction[1])
            .AddRaw("onlineByRange", onlineByRange)
            .Add("pendingTotal", static_cast<uint32_t>(d.pending.size()))
            .Add("pendingAlliance", d.pendingByFaction[0])
            .Add("pendingHorde", d.pendingByFaction[1])
            .AddRaw("pendingByModule", pendingByModule)
            .AddRaw("pendingByRange", pendingByRange)
            .Add("totalClaims", static_cast<uint32_t>(claims.totalClaims))
            .Add("homeGuildBots", static_cast<uint32_t>(claims.homeGuildBots))
            .AddRaw("claimsByOwner", claimsByOwner)
            .Build();
    }

    // cada sección lleva su edad. El puente renueva updated_at de la fila
    // entera en cada pasada, así que sin esto una sección que su productor
    // dejó de publicar (módulo apagado sin última estadística, o parado)
    // parecía tan reciente como las demás. `stale` = sin publicar nunca o hace
    // más de tres instantáneas (30 s como mínimo).
    std::string WithAge(std::string json, uint64_t updatedAtMs, uint64_t now)
    {
        uint64_t const staleMs = std::max<uint64_t>(TimeMs::SecsToMs(cfg.snapshotIntervalSecs) * 3, 30000);
        bool const never = updatedAtMs == 0;
        uint64_t const ageMs = never || now < updatedAtMs ? 0 : now - updatedAtMs;
        std::string extra = never ? std::string("\"ageMs\":null") : "\"ageMs\":" + std::to_string(ageMs);
        extra += never || ageMs > staleMs ? ",\"stale\":true" : ",\"stale\":false";
        json.insert(json.size() - 1, (json.size() > 2 ? "," : "") + extra);
        return json;
    }

    void PublishSnapshot(uint64_t now)
    {
        uint32 const t0 = getMSTime();

        BotOperations::Snapshot const snapshot = BotOperations::GetSnapshot();
        BotPopulationCoordinator::Diagnostics const population = BotPopulationCoordinator::GetDiagnostics(now);
        BotClaims::Diagnostics const claims = BotClaims::GetDiagnostics();

        std::string reservations = ToJson(population, claims);
        std::string worldStage   = WithAge(ToJson(snapshot.worldStage), snapshot.worldStage.updatedAtMs, now);
        std::string worldPvp     = WithAge(ToJson(snapshot.worldPvp), snapshot.worldPvp.updatedAtMs, now);
        std::string queues       = WithAge(ToJson(snapshot.queues), snapshot.queues.updatedAtMs, now);
        std::string groups       = WithAge(ToJson(snapshot.groups), snapshot.groups.updatedAtMs, now);
        std::string questMates   = WithAge(ToJson(snapshot.questMates), snapshot.questMates.updatedAtMs, now);
        std::string guilds       = WithAge(ToJson(snapshot.guilds), snapshot.guilds.updatedAtMs, now);

        // El JSON ya va escapado por dentro (JsonEscape); esto es aparte, para
        // que el propio texto quepa dentro de comillas SQL sin romper la
        // sentencia (mismo patron que world_bots_pvp_state con EscapeString).
        WorldDatabase.EscapeString(reservations);
        WorldDatabase.EscapeString(worldStage);
        WorldDatabase.EscapeString(worldPvp);
        WorldDatabase.EscapeString(queues);
        WorldDatabase.EscapeString(groups);
        WorldDatabase.EscapeString(questMates);
        WorldDatabase.EscapeString(guilds);

        WorldDatabase.Execute(
            "INSERT INTO bot_operations_snapshot "
            "(id, reservations, world_stage, world_pvp, queues, `groups`, quest_mates, guilds, updated_at) "
            "VALUES (1, '{}', '{}', '{}', '{}', '{}', '{}', '{}', NOW()) "
            "ON DUPLICATE KEY UPDATE "
            "reservations = VALUES(reservations), world_stage = VALUES(world_stage), "
            "world_pvp = VALUES(world_pvp), queues = VALUES(queues), `groups` = VALUES(`groups`), "
            "quest_mates = VALUES(quest_mates), guilds = VALUES(guilds), updated_at = VALUES(updated_at)",
            reservations, worldStage, worldPvp, queues, groups, questMates, guilds);

        SlowTick::WarnIfSlow("bot-operations", "PublishSnapshot", t0);
    }

    void CompleteAction(uint64_t id, char const* status, std::string message)
    {
        WorldDatabase.EscapeString(message);
        WorldDatabase.Execute(
            "UPDATE bot_operations_action SET status = '{}', result = '{}', completed_at = NOW() "
            "WHERE id = {} AND status = 'pending'",
            status, message, id);
        g_inFlight.erase(id);
        g_resolved[id] = TimeMs::NowMs();
    }

    // Vuelca a SQL los resultados que han dejado los destinos. Se llama antes
    // de interpretar una lectura de filas pendientes (M07): así una acción que
    // su destino ya ejecutó queda resuelta como 'done'/'failed' y no se marca
    // 'expired' ni se vuelve a encolar por venir en esa lectura.
    void DrainOutcomes()
    {
        for (BotOperations::ActionOutcome const& outcome : BotOperations::TakeOutcomes())
            CompleteAction(outcome.id, outcome.expired ? "expired" : (outcome.ok ? "done" : "failed"), outcome.message);
    }

    char const* TargetLabel(BotOperations::Target target)
    {
        switch (target)
        {
            case BotOperations::Target::WorldBotsStage: return "world-bots";
            case BotOperations::Target::WorldBotsPvp:   return "world-bots (guerra de mundo)";
            case BotOperations::Target::QueueBots:      return "queue-bots";
            case BotOperations::Target::PartyHere:      return "party-here";
            case BotOperations::Target::HomeGuild:      return "home-guild";
            default:                                    return "desconocido";
        }
    }

    // Una fila de bot_operations_action pendiente, ya leida de la respuesta
    // asincrona: el resto de la logica es identica a como era con la
    // consulta sincrona, solo cambia CUANDO se ejecuta (en un OnUpdate
    // posterior, no en el que la pidio).
    void HandlePendingActionRow(uint64_t now, uint64_t nowUnix, Field* f)
    {
        uint64_t const id              = f[0].Get<uint64>();
        std::string const action       = f[1].Get<std::string>();
        std::string const param        = f[2].Get<std::string>();
        uint64_t const requestedAtUnix = f[3].Get<uint64>();

        // Ya resuelta por este proceso: la lectura se lanzó antes de que
        // llegara su UPDATE (M07).
        if (g_resolved.count(id))
            return;

        uint64_t const ageSecs = nowUnix > requestedAtUnix ? nowUnix - requestedAtUnix : 0;
        if (cfg.actionMaxAgeSeconds && ageSecs > cfg.actionMaxAgeSeconds)
        {
            // Retirarla también del buzón (M07): antes sólo cambiaba la fila y
            // el destino, al reactivarse, ejecutaba igualmente la acción.
            BotOperations::CancelRequest(id);
            CompleteAction(id, "expired", "Caducada: nadie la atendio a tiempo.");
            return;
        }

        if (action == "refresh_snapshot")
        {
            CompleteAction(id, "done", "Instantanea actualizada.");
            g_nextSnapshotMs = now;   // se publica en el proximo OnUpdate
            return;
        }

        BotOperations::ActionType const type = BotOperations::ActionFromName(action);
        if (type == BotOperations::ActionType::None)
        {
            CompleteAction(id, "failed", "Accion no reconocida.");
            return;
        }

        if (g_inFlight.count(id))
        {
            // Ya encolada, esperando resultado. Si su destino dejó de drenar
            // (colgado o apagado sin avisar), se retira del buzón y se da por
            // fallida (M07): si no, estas filas ocupaban la ventana de
            // ActionQueueMax de la lectura y bloqueaban las de los demás
            // destinos hasta caducar.
            BotOperations::Target const inFlightTarget = BotOperations::TargetFor(type);
            BotOperations::TargetState const state = BotOperations::GetTargetState(inFlightTarget);
            if ((!state.online || now - state.lastDrainMs > BotOperations::kTargetStaleMs)
                && BotOperations::CancelRequest(id))
                CompleteAction(id, "failed", Acore::StringFormat(
                    "{} dejo de atender solicitudes: retirada sin ejecutar.", TargetLabel(inFlightTarget)));
            return;
        }

        BotOperations::ActionRequest request;
        request.id = id;
        request.type = type;
        request.param = param;
        request.requestedAtMs = now;
        // Vence en el buzón a la misma edad que la fila en SQL.
        if (cfg.actionMaxAgeSeconds)
            request.expiresAtMs = now + TimeMs::SecsToMs(cfg.actionMaxAgeSeconds - ageSecs);

        BotOperations::Target const target = BotOperations::TargetFor(type);
        switch (BotOperations::EnqueueRequest(std::move(request), now))
        {
            case BotOperations::EnqueueResult::Queued:
                g_inFlight[id] = now;
                break;
            case BotOperations::EnqueueResult::Full:
                CompleteAction(id, "failed", Acore::StringFormat(
                    "Rechazada: el buzon de {} esta lleno ({} solicitudes sin atender).",
                    TargetLabel(target), BotOperations::kMaxInbox));
                break;
            case BotOperations::EnqueueResult::Unavailable:
                CompleteAction(id, "failed", Acore::StringFormat(
                    "Destino no disponible: {} esta desactivado o no esta cargado.", TargetLabel(target)));
                break;
        }
    }

    void ProcessActions()
    {
        uint32 const t0 = getMSTime();

        // no lanzar una lectura nueva mientras la anterior
        // sigue sin contestar. Las filas que ya estén pendientes en SQL
        // simplemente se recogen en la siguiente pasada libre.
        if (!g_actionQueryPending)
        {
            // Antes: WorldDatabase.Query(...) SINCRONA, bloqueaba el hilo del
            // mundo hasta que MySQL contestaba.
            // AsyncQuery() no tiene la sobrecarga con formato "{}" de Query()/
            // Execute(): se compone el SQL a mano, con un entero de config,
            // sin texto de usuario que escapar.
            uint32 const generationAtIssue = g_configGeneration;
            g_actionQueryPending = true;
            g_actionQuery.AddCallback(WorldDatabase.AsyncQuery(
                "SELECT id, action, param, UNIX_TIMESTAMP(requested_at) FROM bot_operations_action "
                "WHERE status = 'pending' ORDER BY id ASC LIMIT " + std::to_string(cfg.actionQueueMax))
                .WithCallback([generationAtIssue](QueryResult result)
            {
                uint32 const tCallback0 = getMSTime();
                g_actionQueryPending = false;

                // El módulo pudo desactivarse o recargarse mientras esta
                // respuesta viajaba: una respuesta tardía no
                // debe encolar trabajo en otros módulos con una
                // configuración que ya no es la vigente. Las filas siguen
                // 'pending' en SQL: la próxima pasada, ya con la generación
                // actual, las recoge normalmente.
                if (result && cfg.enabled && generationAtIssue == g_configGeneration)
                {
                    // Primero lo que ya contestaron los destinos (M07).
                    DrainOutcomes();

                    // Edad calculada AHORA, al consumir la respuesta, no en
                    // el instante en que se lanzó la consulta: una respuesta
                    // que tarda varios intervalos no debe subestimar cuánto
                    // lleva esperando la fila.
                    uint64_t const nowConsumed = TimeMs::NowMs();
                    uint64_t const nowUnixConsumed = static_cast<uint64_t>(GameTime::GetGameTime().count());
                    do
                    {
                        HandlePendingActionRow(nowConsumed, nowUnixConsumed, result->Fetch());
                    } while (result->NextRow());
                }

                // Cubre el trabajo real del callback (antes sólo se medía
                // encolar el AsyncQuery, no procesar sus filas: ese trabajo
                // corre en un OnUpdate posterior, fuera de ProcessActions()).
                SlowTick::WarnIfSlow("bot-operations", "ProcessActions:callback", tCallback0);
            }));
        }

        DrainOutcomes();

        uint64_t const now = TimeMs::NowMs();
        for (auto it = g_resolved.begin(); it != g_resolved.end();)
            it = now - it->second > kResolvedMemoryMs ? g_resolved.erase(it) : std::next(it);

        SlowTick::WarnIfSlow("bot-operations", "ProcessActions", t0);
    }

    class mod_bot_operations_world : public WorldScript
    {
    public:
        mod_bot_operations_world() : WorldScript("mod_bot_operations_world",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            cfg.enabled                = sConfigMgr->GetOption<bool>("BotOperations.Enable", false);
            cfg.snapshotIntervalSecs   = std::max<uint32>(sConfigMgr->GetOption<uint32>("BotOperations.SnapshotIntervalSeconds", 10), 1);
            cfg.actionPollIntervalSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("BotOperations.ActionPollIntervalSeconds", 2), 1);
            cfg.actionQueueMax         = std::max<uint32>(sConfigMgr->GetOption<uint32>("BotOperations.ActionQueueMax", 20), 1);
            cfg.actionMaxAgeSeconds    = sConfigMgr->GetOption<uint32>("BotOperations.ActionMaxAgeSeconds", 120);
            ++g_configGeneration;

            // Puente apagado (M07): nada de lo encolado se ejecuta sin él. Las
            // filas siguen 'pending' en SQL y, si vuelve a encenderse, se
            // releen con su edad real.
            if (!cfg.enabled)
            {
                BotOperations::ClearAll();
                g_inFlight.clear();
            }
        }

        void OnUpdate(uint32 /*diff*/) override
        {
            // Recoge el resultado de cualquier AsyncQuery ya lista, aunque el
            // modulo se acabe de apagar en caliente: una respuesta que llega
            // tarde no debe quedarse sin drenar (mismo patron que
            // World::ProcessQueryCallbacks, llamado siempre, no solo cuando
            // toca la siguiente pasada).
            g_actionQuery.ProcessReadyCallbacks();

            if (!cfg.enabled)
                return;

            uint64_t const now = TimeMs::NowMs();

            if (now >= g_nextActionPollMs)
            {
                g_nextActionPollMs = now + TimeMs::SecsToMs(cfg.actionPollIntervalSecs);
                ProcessActions();
            }

            if (now >= g_nextSnapshotMs)
            {
                g_nextSnapshotMs = now + TimeMs::SecsToMs(cfg.snapshotIntervalSecs);
                PublishSnapshot(now);
            }
        }
    };

    // ─────────────────────────────────────────────────────────────────────
    //  .botops estado (GM, solo lectura)
    //
    // EL PROBLEMA. `bot_operations_action` es el buzon entre el panel y este
    // modulo (ver cabecera del fichero): antes de esto, comprobar su estado
    // desde el cliente sintetico (tools/cliente-sintetico) exigia SQL directo
    // contra acore_world, algo que M13 (CHANGELOG.md, 25/09/2026) sorteo
    // insertando filas a mano en vez de dejar un caso repetible en el
    // catalogo. Un comando
    // GM lo deja al alcance de la cuenta de pruebas del cliente sintetico
    // (mismo patron que ".wpvp estado"/".queuebots estado"), sin credenciales
    // de acore_world sueltas en el equipo que ejecuta las pruebas.
    //
    // LA SOLUCION. Un comando GM que hace por consola lo que antes solo se
    // veia con un SELECT: cuantas filas pendientes hay (con su tipo, param y
    // antiguedad) y el resultado de las ultimas resueltas. Es una consulta
    // SINCRONA (a diferencia de ProcessActions(), que usa AsyncQuery): se
    // ejecuta solo bajo demanda de un GM, no cada ActionPollIntervalSeconds,
    // asi que no compite con esa cola ni bloquea nada periodico. No toca
    // BotOperations.h ni el estado de otro modulo: es un espejo de solo
    // lectura de la misma tabla que ya lee ProcessActions().
    class mod_bot_operations_command : public CommandScript
    {
    public:
        mod_bot_operations_command() : CommandScript("mod_bot_operations_command") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable botopsTable =
            {
                { "estado", HandleStatus, SEC_GAMEMASTER, Console::No },
                { "",       HandleStatus, SEC_GAMEMASTER, Console::No },
            };
            static ChatCommandTable commandTable =
            {
                { "botops", botopsTable },
            };
            return commandTable;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
            handler->PSendSysMessage(ModLocale::L(handler, "bot-operations: puente {} (BotOperations.Enable)."),
                cfg.enabled ? ModLocale::L(handler, "activo") : ModLocale::L(handler, "inactivo"));

            uint32 pending = 0, done = 0, failed = 0, expired = 0;
            if (QueryResult totals = WorldDatabase.Query(
                    "SELECT status, COUNT(*) FROM bot_operations_action GROUP BY status"))
            {
                do
                {
                    Field* f = totals->Fetch();
                    std::string const status = f[0].Get<std::string>();
                    uint32 const count = f[1].Get<uint32>();
                    if (status == "pending")      pending = count;
                    else if (status == "done")    done = count;
                    else if (status == "failed")  failed = count;
                    else if (status == "expired") expired = count;
                } while (totals->NextRow());
            }
            handler->PSendSysMessage(ModLocale::L(handler, "colas: {} pendientes, {} resueltas ({} done, {} failed, {} expired)."),
                pending, done + failed + expired, done, failed, expired);

            // Hasta 10 pendientes, de la mas antigua a la mas nueva (mismo
            // orden que ProcessActions() lee para encolar).
            if (pending)
            {
                if (QueryResult rows = WorldDatabase.Query(
                        "SELECT id, action, param, TIMESTAMPDIFF(SECOND, requested_at, NOW()) "
                        "FROM bot_operations_action WHERE status = 'pending' ORDER BY id ASC LIMIT 10"))
                {
                    do
                    {
                        Field* f = rows->Fetch();
                        std::string const param = f[2].Get<std::string>();
                        handler->PSendSysMessage(ModLocale::L(handler, "  pendiente #{} {} param={} edad={}s"),
                            f[0].Get<uint64>(), f[1].Get<std::string>(),
                            param.empty() ? std::string("-") : param, f[3].Get<uint32>());
                    } while (rows->NextRow());
                }
            }

            // Hasta 5 ultimas resueltas (cualquier estado final), de la mas
            // reciente a la mas antigua.
            if (QueryResult rows = WorldDatabase.Query(
                    "SELECT id, action, status, result, TIMESTAMPDIFF(SECOND, completed_at, NOW()) "
                    "FROM bot_operations_action WHERE status IN ('done', 'failed', 'expired') "
                    "ORDER BY completed_at DESC LIMIT 5"))
            {
                do
                {
                    Field* f = rows->Fetch();
                    handler->PSendSysMessage(ModLocale::L(handler, "  resuelta #{} {} [{}] edad={}s: {}"),
                        f[0].Get<uint64>(), f[1].Get<std::string>(), f[2].Get<std::string>(),
                        f[4].Get<uint32>(), f[3].Get<std::string>());
                } while (rows->NextRow());
            }

            return true;
        }
    };
}

void AddSC_mod_bot_operations()
{
    ModLocale::Register(BotOperationsLocale::kEntries);
    new mod_bot_operations_world();
    new mod_bot_operations_command();
}
