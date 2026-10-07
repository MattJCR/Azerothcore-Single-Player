// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * BotOperations.h -- buzon compartido entre los modulos de bots y mod-bot-operations.
 *
 * EL PROBLEMA. El panel web quiere mostrar y operar el ecosistema de bots
 * (colas, mundo/etapa, grupos, hermandades, companeros de mision) sin leer
 * memoria privada de mod-queue-bots/mod-world-bots/mod-party-here/
 * mod-quest-mates/mod-home-guild (regla 1 de modules/README.md: incluir
 * cabeceras de otro modulo lo convierte en un parche encubierto) y sin que
 * mod-bot-operations interprete texto de SOAP ni ejecute comandos arbitrarios
 * (ver REFERENCES.md, operaciones de bots en el panel web).
 *
 * LA SOLUCION. Un buzon de dos direcciones, con el mismo patron ODR-seguro
 * que BotClaims.h (funciones inline con estaticas locales: el enlazador funde
 * las copias identicas de cada modulo en una sola instancia por proceso):
 *
 *   - ARRIBA (estadisticas). Cada modulo productor llena SU struct desde el
 *     OnUpdate que ya posee su estado (regla 5 del README: solo el dueno del
 *     estado lo toca sin carrera) y llama al Publish*() correspondiente.
 *     mod-bot-operations lee el conjunto con GetSnapshot() cada 10 s y lo
 *     vuelca a bot_operations_snapshot (acore_world). Nunca al reves: ningun
 *     modulo productor lee el snapshot de otro.
 *
 *   - ABAJO (acciones). mod-bot-operations procesa bot_operations_action
 *     (acore_world, las filas que deja el panel) y encola cada peticion con
 *     EnqueueRequest() contra el Target que le corresponde. El modulo destino
 *     la recoge en su propio OnUpdate con TakeRequests() -swap+drain, igual
 *     que ya hacen internamente mod_party_here_world::OnUpdate o
 *     mod_queue_bots_world::RunCommands()-, la ejecuta con SU logica interna
 *     y publica el resultado con ReportOutcome(). mod-bot-operations lo
 *     recoge con TakeOutcomes() y actualiza la fila.
 *
 * Por que Target separa WorldBotsStage de WorldBotsPvp: aunque las dos viven
 * en el modulo mod-world-bots, son dos .cpp con su propio WorldScript y su
 * propio estado en anonymous namespace (mod_world_bots.cpp / g_stage... y
 * mod_world_bots_pvp.cpp / g_events...). Un solo Target compartido entre
 * ambos haria que el OnUpdate que llame primero a TakeRequests() se quede con
 * las peticiones del otro. Cada .cpp drena solo su propio Target.
 *
 * Igual que BotClaims: cada modulo lleva su COPIA de este fichero en src/
 * (la pone install_own_modules() desde modules/shared/), y las copias tienen
 * que ser byte a byte identicas. Se edita solo modules/shared/BotOperations.h.
 * La version ABI que el *_loader.cpp de mod-bot-operations escribe en
 * Server.log al cargar es la UNICA y compartida por toda shared/:
 * SharedAbi::kSharedAbiVersion (ver SharedAbi.h; regla 6 del README).
 */

#ifndef WOTLK_SP_BOT_OPERATIONS_H
#define WOTLK_SP_BOT_OPERATIONS_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace BotOperations
{
    // Modulo (en realidad: WorldScript concreto) al que va dirigida una
    // accion. Cada uno drena solo su propio buzon en su OnUpdate.
    //
    // mod-quest-mates no tiene Target: es puramente reactivo (aceptar/
    // abandonar una mision), sin ninguna pasada periodica que "adelantar" -
    // solo se pide forzar pasada para poblacion, colas y cuidado
    // de grupos/hermandades. Solo publica QuestMateStats (solo lectura).
    enum class Target : uint8_t
    {
        WorldBotsStage = 0,  // mod_world_bots.cpp :: mod_world_bots_world::OnUpdate
        WorldBotsPvp,        // mod_world_bots_pvp.cpp :: mod_world_bots_pvp_world::OnUpdate
        QueueBots,           // mod_queue_bots.cpp :: mod_queue_bots_world::OnUpdate
        PartyHere,           // mod_party_here.cpp :: mod_party_here_world::OnUpdate
        HomeGuild,           // mod_home_guild.cpp :: mod_home_guild_world::OnUpdate
        Count
    };

    // Accion seleccionable desde el panel. Ninguna teletransporta, desconecta, sube de nivel, reequipa
    // ni fuerza logins masivos: solo adelanta una pasada normal (no salta los
    // limites/presupuestos que el modulo ya aplica) o para un evento PvP.
    enum class ActionType : uint8_t
    {
        None = 0,
        WorldBotsPass,   // -> WorldBotsStage: adelanta poblacion/etapa
        StopWorldPvp,    // -> WorldBotsPvp: para un evento atascado (param = id, "" = todos)
        QueueBotsPass,   // -> QueueBots: adelanta el relleno de colas
        PartyHerePass,   // -> PartyHere: adelanta el cuidado de grupos/bandas
        HomeGuildPass,   // -> HomeGuild: adelanta el cuidado de la hermandad
    };

    inline Target TargetFor(ActionType type)
    {
        switch (type)
        {
            case ActionType::WorldBotsPass: return Target::WorldBotsStage;
            case ActionType::StopWorldPvp:  return Target::WorldBotsPvp;
            case ActionType::QueueBotsPass: return Target::QueueBots;
            case ActionType::PartyHerePass: return Target::PartyHere;
            case ActionType::HomeGuildPass: return Target::HomeGuild;
            default:                        return Target::WorldBotsStage;
        }
    }

    struct ActionRequest
    {
        uint64_t    id = 0;   // bot_operations_action.id: casa el resultado con la fila
        ActionType  type = ActionType::None;
        std::string param;    // opcional (p.ej. id de evento PvP como texto)
        uint64_t    requestedAtMs = 0;
        // instante (TimeMs::NowMs) a partir del cual la fila ya está
        // caducada en SQL. TakeRequests no entrega una solicitud vencida: la
        // devuelve como "expired" sin ejecutarla. 0 = sin caducidad.
        uint64_t    expiresAtMs = 0;
    };

    struct ActionOutcome
    {
        uint64_t    id = 0;
        bool        ok = false;
        std::string message;
        uint64_t    completedAtMs = 0;
        bool        expired = false;   // no se ejecutó porque venció en el buzón
    };

    // ---- M07: límites del buzón y disponibilidad del destino ----
    //
    // Antes el buzón no tenía tope ni caducidad, y un destino apagado (retorna
    // antes de TakeRequests) o no compilado dejaba los mensajes acumulándose:
    // al reactivarlo ejecutaba acciones que SQL ya daba por caducadas. Ahora:
    //   - cada destino lleva un latido: TakeRequests lo renueva; un destino
    //     que se apaga llama a SetTargetOffline (vacía su buzón con resultado
    //     "fallida") y deja de aceptar; uno que nunca drenó (no compilado) o
    //     que lleva kTargetStaleMs sin drenar no acepta nada nuevo;
    //   - EnqueueRequest rechaza con motivo explícito (lleno / no disponible)
    //     en vez de encolar sin límite;
    //   - CancelRequest retira del buzón una solicitud que el puente da por
    //     caducada, para que no se ejecute después.
    constexpr std::size_t kMaxInbox      = 32;
    constexpr uint64_t    kTargetStaleMs = 30000;

    enum class EnqueueResult : uint8_t
    {
        Queued = 0,
        Full,          // el buzón del destino ya tiene kMaxInbox solicitudes
        Unavailable    // destino apagado, no compilado o sin drenar hace rato
    };

    struct TargetState
    {
        bool     online = false;     // drenó alguna vez y no se ha declarado apagado
        uint64_t lastDrainMs = 0;    // último TakeRequests
    };

    // ---- Estadisticas publicadas por cada modulo productor ----
    // "enabled" refleja el interruptor propio del modulo (Config.Enable):
    // el panel lo usa para no mostrar una pestana como "vacia" cuando en
    // realidad el modulo esta apagado.

    // Huecos totales por tipo de cola (no por rol): Fill no guarda el rol con
    // el que se metió cada bot, así que "roles faltantes" tal cual se pidió
    // se queda en hueco total por ahora — desglosar tanque/
    // sanador/daño exigiría instrumentar FillDungeon/FillRaid por dentro
    // (los objetivos de rol son locales a esa llamada, no se guardan). Mejor
    // un total honesto que un desglose inventado.
    struct QueueStats
    {
        bool     enabled = false;
        uint32_t waitingHumans = 0;          // humanos con algun hueco pendiente de rellenar
        uint32_t botsFilled = 0;             // bots metidos por el modulo, todas las colas
        uint32_t arena1v1Missing = 0;
        uint32_t battlegroundMissing = 0;
        uint32_t arenaMissing = 0;
        uint32_t dungeonMissing = 0;
        uint32_t raidMissing = 0;
        uint64_t updatedAtMs = 0;
    };

    struct WorldStageStats
    {
        bool                  enabled = false;
        uint8_t               stage = 2;   // 0 Vanilla, 1 TBC, 2 WotLK
        std::string           stageName;
        std::string           decidedBy;   // nombre del jugador que fija la etapa, vacio si nadie la fuerza
        uint32_t              levelCap = 0;
        std::vector<uint32_t> maps;
        uint32_t              zonesPopulated = 0;   // zonas con bots repartidos esta pasada
        uint32_t              relocatedTotal = 0;   // bots recolocados por la etapa desde el arranque
        uint32_t              rerolledTotal = 0;    // bots rehechos (nivel/equipo) por la etapa desde el arranque
        uint64_t              updatedAtMs = 0;
    };

    struct PvpEventInfo
    {
        uint32_t    id = 0;
        std::string label;
        uint32_t    zoneId = 0;
        uint32_t    mapId = 0;
        uint32_t    attackers = 0;
        uint32_t    defenders = 0;
        bool        ending = false;
        uint64_t    startedAtMs = 0;
    };

    // Un punto de la guerra de mundo, activo ahora mismo o no: el panel
    // pedía ver también los que no tienen escaramuza en curso, no sólo el
    // recuento — antes sólo se
    // publicaba hotspotCount. attackerTeam/defenderTeam: 0 Alianza, 1 Horda,
    // 2 cualquiera (mismo valor en los dos = punto de duelos si no es 2,2).
    struct HotspotInfo
    {
        uint32_t    id = 0;
        std::string name;
        std::string label;
        bool        enabled = false;
        uint8_t     attackerTeam = 2;
        uint8_t     defenderTeam = 2;
        uint8_t     minLevel = 0;
        uint8_t     maxLevel = 0;
        uint32_t    zoneId = 0;
        // Milisegundos que faltan para que acabe el enfriamiento; 0 = listo
        // ahora mismo. Deliberadamente una DURACION, no una marca de tiempo:
        // el reloj de origen (TimeMs::NowMs(), ver TimeMs.h) es "ms desde que
        // arranco el worldserver", y el enfriamiento que sobrevive a un
        // reinicio (world_bots_pvp_state) se reconstruye con una resta que
        // puede desbordar/envolver a proposito (ver el comentario de
        // LoadHotspots() en mod_world_bots_pvp.cpp) — comparar dos marcas de
        // tiempo absolutas directamente con ">" rompe con ese envoltorio; una
        // resta sí es segura bajo aritmetica modular, así que el calculo se
        // hace en el productor (que conoce el truco) y aquí sólo se publica
        // el resultado ya a salvo.
        uint64_t    cooldownRemainingMs = 0;
    };

    struct WorldPvpStats
    {
        bool                       enabled = false;
        std::vector<PvpEventInfo>  activeEvents;
        std::vector<HotspotInfo>   hotspots;
        uint32_t                   hotspotCount = 0;
        uint64_t                   updatedAtMs = 0;
    };

    struct GroupStats
    {
        bool     enabled = false;
        uint32_t groupsFormed = 0;
        uint32_t raidsFormed = 0;
        uint32_t botsInGroups = 0;
        uint64_t updatedAtMs = 0;
    };

    struct QuestMateStats
    {
        bool     enabled = false;
        uint32_t humansWithMates = 0;
        uint32_t activePairings = 0;
        uint64_t updatedAtMs = 0;
    };

    struct GuildStats
    {
        bool     enabled = false;
        uint32_t homeGuilds = 0;
        uint32_t botsManaged = 0;
        uint64_t updatedAtMs = 0;
    };

    struct Snapshot
    {
        QueueStats      queues;
        WorldStageStats worldStage;
        WorldPvpStats   worldPvp;
        GroupStats      groups;
        QuestMateStats  questMates;
        GuildStats      guilds;
    };

    struct State
    {
        Snapshot snapshot;
        std::deque<ActionRequest> inbox[static_cast<size_t>(Target::Count)];
        TargetState targets[static_cast<size_t>(Target::Count)];
        std::vector<ActionOutcome> outcomes;
    };

    inline std::mutex& Lock()
    {
        static std::mutex lock;
        return lock;
    }

    inline State& Data()
    {
        static State state;
        return state;
    }

    inline Snapshot GetSnapshot()
    {
        std::lock_guard<std::mutex> guard(Lock());
        return Data().snapshot;
    }

    inline void PublishQueueStats(QueueStats stats)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().snapshot.queues = std::move(stats);
    }

    inline void PublishWorldStageStats(WorldStageStats stats)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().snapshot.worldStage = std::move(stats);
    }

    inline void PublishWorldPvpStats(WorldPvpStats stats)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().snapshot.worldPvp = std::move(stats);
    }

    inline void PublishGroupStats(GroupStats stats)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().snapshot.groups = std::move(stats);
    }

    inline void PublishQuestMateStats(QuestMateStats stats)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().snapshot.questMates = std::move(stats);
    }

    inline void PublishGuildStats(GuildStats stats)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().snapshot.guilds = std::move(stats);
    }

    // La llama solo mod-bot-operations, al procesar una fila pendiente de
    // bot_operations_action. No ejecuta nada: el modulo destino decide en su
    // propio OnUpdate si puede atenderla (y con que limites propios).
    inline EnqueueResult EnqueueRequest(ActionRequest request, uint64_t now)
    {
        Target const target = TargetFor(request.type);
        std::lock_guard<std::mutex> guard(Lock());
        TargetState const& state = Data().targets[static_cast<size_t>(target)];
        if (!state.online || !state.lastDrainMs || now - state.lastDrainMs > kTargetStaleMs)
            return EnqueueResult::Unavailable;
        std::deque<ActionRequest>& inbox = Data().inbox[static_cast<size_t>(target)];
        if (inbox.size() >= kMaxInbox)
            return EnqueueResult::Full;
        inbox.push_back(std::move(request));
        return EnqueueResult::Queued;
    }

    // Retira una solicitud que sigue en algún buzón (el puente la ha dado por
    // caducada). true = estaba y ya no se ejecutará; false = no estaba (ya la
    // recogió su destino, o nunca se encoló).
    inline bool CancelRequest(uint64_t id)
    {
        std::lock_guard<std::mutex> guard(Lock());
        for (std::deque<ActionRequest>& inbox : Data().inbox)
            for (auto it = inbox.begin(); it != inbox.end(); ++it)
                if (it->id == id)
                {
                    inbox.erase(it);
                    return true;
                }
        return false;
    }

    // El puente se apaga: nada de lo encolado debe ejecutarse sin él, ni sus
    // resultados quedarse sin dueño. Las filas siguen 'pending' en SQL, que es
    // la fuente de verdad: al volver a encenderse las relee y aplica su edad.
    inline void ClearAll()
    {
        std::lock_guard<std::mutex> guard(Lock());
        for (std::deque<ActionRequest>& inbox : Data().inbox)
            inbox.clear();
        Data().outcomes.clear();
    }

    // La llama el modulo destino en su OnUpdate para recoger lo suyo.
    // Swap+drain: mismo patron que ya usan mod_party_here_world::OnUpdate o
    // mod_queue_bots_world::RunCommands() con sus colas internas. Renueva el
    // latido del destino y no entrega lo ya vencido (M07): eso vuelve al
    // puente como "expired" sin ejecutarse.
    inline std::deque<ActionRequest> TakeRequests(Target target, uint64_t now)
    {
        std::lock_guard<std::mutex> guard(Lock());
        TargetState& state = Data().targets[static_cast<size_t>(target)];
        state.online = true;
        state.lastDrainMs = now;
        std::deque<ActionRequest> taken;
        taken.swap(Data().inbox[static_cast<size_t>(target)]);
        for (auto it = taken.begin(); it != taken.end();)
        {
            if (it->expiresAtMs && now >= it->expiresAtMs)
            {
                Data().outcomes.push_back({ it->id, false, "Caducada en el buzon: no se ejecuto.", now, true });
                it = taken.erase(it);
            }
            else
                ++it;
        }
        return taken;
    }

    // El destino se apaga (o no tiene con qué trabajar): lo que tenga en el
    // buzón vuelve como fallido con 'reason' y deja de aceptar solicitudes
    // hasta que vuelva a drenar. Idempotente: se puede llamar en cada tick.
    inline void SetTargetOffline(Target target, uint64_t now, char const* reason)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().targets[static_cast<size_t>(target)].online = false;
        std::deque<ActionRequest>& inbox = Data().inbox[static_cast<size_t>(target)];
        for (ActionRequest const& request : inbox)
            Data().outcomes.push_back({ request.id, false, reason, now, false });
        inbox.clear();
    }

    inline TargetState GetTargetState(Target target)
    {
        std::lock_guard<std::mutex> guard(Lock());
        return Data().targets[static_cast<size_t>(target)];
    }

    inline std::size_t InboxSize(Target target)
    {
        std::lock_guard<std::mutex> guard(Lock());
        return Data().inbox[static_cast<size_t>(target)].size();
    }

    inline void ReportOutcome(uint64_t id, bool ok, std::string message, uint64_t now)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().outcomes.push_back({ id, ok, std::move(message), now, false });
    }

    // La llama solo mod-bot-operations, cada pasada, para volcar los
    // resultados pendientes a bot_operations_action.
    inline std::vector<ActionOutcome> TakeOutcomes()
    {
        std::lock_guard<std::mutex> guard(Lock());
        std::vector<ActionOutcome> taken;
        taken.swap(Data().outcomes);
        return taken;
    }

    inline char const* ActionName(ActionType type)
    {
        switch (type)
        {
            case ActionType::WorldBotsPass: return "world_bots_pass";
            case ActionType::StopWorldPvp:  return "stop_world_pvp";
            case ActionType::QueueBotsPass: return "queue_bots_pass";
            case ActionType::PartyHerePass: return "party_here_pass";
            case ActionType::HomeGuildPass: return "home_guild_pass";
            default:                        return "none";
        }
    }

    inline ActionType ActionFromName(std::string const& name)
    {
        if (name == "world_bots_pass") return ActionType::WorldBotsPass;
        if (name == "stop_world_pvp")  return ActionType::StopWorldPvp;
        if (name == "queue_bots_pass") return ActionType::QueueBotsPass;
        if (name == "party_here_pass") return ActionType::PartyHerePass;
        if (name == "home_guild_pass") return ActionType::HomeGuildPass;
        return ActionType::None;
    }
}

#endif // WOTLK_SP_BOT_OPERATIONS_H
