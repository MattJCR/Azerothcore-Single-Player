// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * BotGear.h — topar el equipo de un bot al del jugador y a su fase de
 * progresión. Cabecera compartida (copia byte a byte en cada módulo propio;
 * la pone el instalador desde modules/shared/).
 *
 * EL PROBLEMA
 * individual-progression te tiene en la fase de Núcleo de Magma, y los bots de
 * nivel 60 que entran en tu banda van con épicos de AQ40 y Naxxramas: el
 * sorteo de equipo de playerbots sólo mira el nivel y la calidad
 * (RandomGearQualityLimit = 4), no la fase. Llevas una banda mejor equipada
 * que el contenido, y el contenido deja de ser contenido.
 *
 * LA SOLUCIÓN
 * Cuando un bot entra en tu grupo (mod-queue-bots, mod-party-here) se le
 * fija un nivel de objeto objetivo: la media de tu equipo más un margen,
 * nunca por encima del tope de tu fase de progresión. Si el bot se pasa (o se
 * queda muy corto) se le reequipa con la fábrica de playerbots a ese nivel.
 * De uno en uno por tick: reequipar a un bot cuesta decenas de milisegundos y
 * una banda de 39 haría un tirón.
 *
 * LA FASE DEL JUGADOR
 * individual-progression la guarda como misiones ocultas recompensadas,
 * entradas 66000 + fase (IndividualProgression.cpp, GetPlayerProgressionFromQuests).
 * Se lee con API del núcleo (GetQuestStatus) sin incluir cabeceras del módulo.
 * Los topes por fase son los que documenta el propio playerbots.conf.dist en
 * AutoGearScoreLimit: MC 78, BWL 83, AQ40 88, Naxx40 92, Kara 125, SSC/TK 141,
 * Hyjal/BT 156, Sunwell 164, Naxx10/25 224, Ulduar 245, ToC 258, ICC 290.
 *
 * DEPENDENCIA CON PLAYERBOTS (entre guardas)
 * PlayerbotFactory::DestroyEquippedGear y PlayerbotFactory::AutoGear, ambas
 * estáticas y públicas: es lo mismo que hace su comando "autogear reset N".
 * Sin playerbots, compila y no hace nada.
 */

#ifndef WOTLK_SP_BOT_GEAR_H
#define WOTLK_SP_BOT_GEAR_H

#include "Config.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "QuestDef.h"
#include "TimeMs.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>

#if defined(__has_include)
#  if __has_include("PlayerbotFactory.h") && __has_include("Playerbots.h")
#    include "PlayerbotFactory.h"
#    include "Playerbots.h"
#    define BOT_GEAR_WITH_PLAYERBOTS 1
#  endif
#endif

namespace BotGear
{
    // Fases de individual-progression (ProgressionState) y su tope de nivel
    // de objeto. Copiado y documentado: es lo único que se comparte con él.
    constexpr uint32_t IP_QUEST_BASE = 66000;
    constexpr uint8_t  IP_LAST_STATE = 18;

    inline uint32_t PhaseCap(uint8_t state)
    {
        switch (state)
        {
            case 0:  return 78;   // nada completado: MC, Onyxia, ZG
            case 1:
            case 2:
            case 3:  return 83;   // BWL
            case 4:
            case 5:  return 88;   // AQ40
            case 6:
            case 7:  return 92;   // Naxx40
            case 8:  return 125;  // Kara, Gruul, Magtheridon
            case 9:  return 141;  // SSC, TK
            case 10:
            case 11: return 156;  // Hyjal, BT
            case 12: return 164;  // Sunwell
            case 13: return 224;  // Naxx 10/25, EoE, OS
            case 14: return 245;  // Ulduar
            case 15: return 258;  // ToC
            case 16: return 290;  // ICC
            case 17:              // Ruby Sanctum
            case 18: return 290;  // WOTLK_TIER_5: endgame, todo hecho; 290 (ICC)
                                  // es el techo real de ilvl en 3.3.5
            default: return 0;    // estado fuera de rango: sin tope (no deberia
                                  // pasar; IP_LAST_STATE = 18)
        }
    }

    // La fase del jugador: la mayor misión oculta de IP que tenga recompensada.
    // Sin individual-progression, esto da SIEMPRE 0: es indistinguible de un
    // jugador que de verdad está en la fase inicial (M22). Comprobar
    // Available() antes de fiarse de este valor.
    inline uint8_t PlayerPhase(Player* human)
    {
        uint8_t phase = 0;
        for (uint8_t i = 1; i <= IP_LAST_STATE; ++i)
            if (human->GetQuestStatus(IP_QUEST_BASE + i) == QUEST_STATUS_REWARDED)
                phase = i;
        return phase;
    }

    // Si individual-progression está instalado Y activo en este perfil (M22).
    // "IndividualProgression.Enable" sólo existe en la config fusionada cuando
    // el módulo está compilado y su .conf.dist se cargó; si no está, GetOption
    // devuelve el 'false' que se le pasa aquí, no el 1 por defecto del propio
    // módulo. Se lee en vivo (sin caché): un cambio de perfil se nota en el
    // siguiente OnUpdate/revalidador sin más plumbing (M02).
    inline bool Available()
    {
        return sConfigMgr->GetOption<bool>("IndividualProgression.Enable", false);
    }

    // Media simple de nivel de objeto de lo que lleva puesto (sin camisa ni
    // tabardo). NO es el CalculateGearScore de playerbots (que pondera ranuras
    // y estadísticas): aquí solo se compara contra un objetivo de ilvl, así que
    // basta la media.
    inline uint32_t WornItemLevel(Player* player)
    {
        uint32_t sum = 0;
        uint32_t count = 0;
        for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
                continue;
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                sum += item->GetTemplate()->ItemLevel;
                ++count;
            }
        }
        return count ? sum / count : 0;
    }

    enum Mode : uint8_t
    {
        MODE_OFF    = 0,   // no tocar el equipo de los bots
        MODE_PLAYER = 1,   // media del jugador + margen, topada por la fase
        MODE_PHASE  = 2    // sólo el tope de la fase
    };

    struct Settings
    {
        Mode     mode      = MODE_PLAYER;
        uint32_t margin    = 6;    // por encima de la media del jugador
        uint32_t tolerance = 8;    // se reequipa sólo si se sale de +-tolerancia
        uint32_t minIlvl   = 0;    // 0 = sin suelo
    };

    // Lee las claves comunes ("<Prefijo>.GearMode", ".GearMargin",
    // ".GearTolerance", ".GearMinItemLevel") del .conf del módulo que llama.
    inline Settings LoadSettings(std::string const& prefix)
    {
        Settings s;
        s.mode      = static_cast<Mode>(std::min<uint32_t>(sConfigMgr->GetOption<uint32_t>(prefix + ".GearMode", 1), 2));
        s.margin    = sConfigMgr->GetOption<uint32_t>(prefix + ".GearMargin", 6);
        s.tolerance = sConfigMgr->GetOption<uint32_t>(prefix + ".GearTolerance", 8);
        s.minIlvl   = sConfigMgr->GetOption<uint32_t>(prefix + ".GearMinItemLevel", 0);
        return s;
    }

    // Nivel de objeto objetivo para los bots que acompañan a 'human'. 0 = sin tope.
    //
    // El tope de fase manda siempre que exista (se aplica el último y
    // reemplaza cualquier valor por encima, venga de donde venga). El suelo
    // (GearMinItemLevel) sólo se nota cuando no hay tope de fase que lo tape:
    // antes se aplicaba ANTES del tope, así que un GearMinItemLevel alto colaba
    // equipo muy por encima del contenido de tu fase; y en MODE_PHASE y con
    // worn == 0 se saltaba entero, así que sin fase detectada (cap == 0) esas
    // dos ramas devolvían 0 en vez de respetar el suelo.
    //
    // Sin individual-progression en el perfil (M22), cap == 0 (sin tope): sus
    // 66000+i nunca se recompensan, así que PlayerPhase() siempre daría 0 y
    // topearía a 78 (MC) para siempre, degradando el equipo de un perfil que
    // no usa IP. MODE_PHASE sin IP no tiene fase que aplicar: sólo queda el
    // suelo, si lo hay.
    inline uint32_t TargetItemLevel(Player* human, Settings const& s)
    {
        if (s.mode == MODE_OFF || !human)
            return 0;

        uint32_t const cap = Available() ? PhaseCap(PlayerPhase(human)) : 0;

        uint32_t target;
        if (s.mode == MODE_PHASE)
            target = cap;
        else
        {
            uint32_t const worn = WornItemLevel(human);
            target = worn ? worn + s.margin : cap;
        }

        if (s.minIlvl && target < s.minIlvl)
            target = s.minIlvl;
        if (cap && target > cap)
            target = cap;
        return target;
    }

    // ─── Contexto de un trabajo (M02) ───────────────────────────────────────
    // Un trabajo puede esperar (combate, muerte, cola) y, mientras, la relación
    // que lo pidió puede terminar: el bot sale del grupo, cambia de amo, acaba
    // la guerra. Por eso cada trabajo lleva un revalidador que se consulta AL
    // CONSUMIRLO, no al encolarlo: dice si la relación sigue viva y recalcula
    // el objetivo con el estado de ese momento. Se llama sólo desde
    // ProcessOne, en WorldScript::OnUpdate (hilo del mundo, mapas parados), así
    // que puede leer el estado propio del módulo que lo encoló.
    enum class Decision : uint8_t
    {
        Apply,    // relación viva: reequipar a 'ilvl'
        Defer,    // relación viva pero no ahora (el amo está cargando de mapa)
        Cancel    // relación terminada, o el objetivo ya no aplica
    };

    using Resolver = std::function<Decision(Player* bot, uint32_t& ilvl)>;

    // Política de sustitución entre contextos: un trabajo pendiente del mismo
    // bot se reemplaza por uno nuevo de prioridad igual o mayor (el último que
    // pide manda) y NO por uno de prioridad menor. Así el repaso periódico de
    // la hermandad no pisa el objetivo de un grupo o de un evento, y una
    // petición nueva y válida nunca se pierde por deduplicación.
    enum Priority : uint8_t
    {
        PRIORITY_BACKGROUND = 0,   // mantenimiento sin relación activa (home-guild)
        PRIORITY_GROUP      = 1,   // grupo o banda con un jugador (queue-bots, party-here)
        PRIORITY_EVENT      = 2    // evento PvP (world-bots): el bot está reservado para él
    };

    // Revalidador estándar para "el bot va en el grupo de 'human'": se cancela
    // si el jugador se ha ido o el bot ya no comparte grupo con él; se aplaza
    // si el jugador está en una pantalla de carga; si no, recalcula el
    // objetivo con 's' (un puntero a la configuración viva del módulo, para
    // que un ".reload config" se note también en lo ya encolado).
    inline Resolver InGroupOf(ObjectGuid human, Settings const* s)
    {
        return [human, s](Player* bot, uint32_t& ilvl) -> Decision
        {
            Player* owner = ObjectAccessor::FindConnectedPlayer(human);
            WorldSession* session = owner ? owner->GetSession() : nullptr;
            if (!session || session->PlayerLogout())
                return Decision::Cancel;
            if (!bot->GetGroup() || bot->GetGroup() != owner->GetGroup())
                return Decision::Cancel;
            if (!owner->IsInWorld())
                return Decision::Defer;
            ilvl = TargetItemLevel(owner, *s);
            return ilvl ? Decision::Apply : Decision::Cancel;
        };
    }

    // ─── Cola de reequipados, con presupuesto global por tick ───────────────
    // La política (tolerancia) viaja con el trabajo, fijada por quien lo
    // encoló: si viajara en Settings del CONSUMIDOR, un trabajo de party-here
    // podría evaluarse con la tolerancia de queue-bots, o al revés.
    //
    // cada trabajo acaba en uno de estos estados, contados en Stats:
    //   - completado  : la fábrica terminó;
    //   - sin cambios : ya iba dentro de tolerancia;
    //   - cancelado   : su relación terminó (revalidador de M02) o su módulo
    //                   lo retiró al apagarse (CancelOwnedBy);
    //   - aplazado    : bot ocupado / amo cargando; vuelve a la cola con un
    //                   margen (kDeferRetryMs) y una edad máxima (kMaxDeferMs);
    //   - caducado    : superó esa edad sin poder aplicarse (bot siempre ocupado);
    //   - fallido     : la fábrica lanzó una excepción. Si fue DESPUÉS de vaciar
    //                   el equipo, el trabajo pasa a "recuperación": se reintenta
    //                   hasta kMaxAttempts veces con espera creciente, no lo
    //                   cancela el fin de la relación (el bot no puede quedarse
    //                   desnudo) y sobrevive a CancelOwnedBy y a la sustitución
    //                   por otro trabajo del mismo bot. Agotados los intentos
    //                   se abandona con un LOG_ERROR y queda contado.
    struct Job
    {
        ObjectGuid  bot;
        uint32_t    ilvl      = 0;      // objetivo al encolar (sólo para el registro si hay revalidador)
        uint32_t    tolerance = 8;
        std::string tag;
        uint8_t     priority  = PRIORITY_BACKGROUND;
        Resolver    resolve;            // vacío = objetivo fijo, sin contexto
        uint64_t    queuedMs    = 0;    // cuándo se pidió (edad máxima de aplazados)
        uint64_t    notBeforeMs = 0;    // no se vuelve a mirar antes de esto
        uint8_t     attempts    = 0;    // intentos fallidos de la fábrica
        bool        recovery    = false; // se vació su equipo y la fábrica falló
    };

    // Contadores desde el arranque (M12), para `.bots estado`.
    struct Stats
    {
        uint64_t completed = 0;
        uint64_t skipped   = 0;   // ya dentro de tolerancia: no se tocó
        uint64_t cancelled = 0;
        uint64_t deferred  = 0;   // veces que un trabajo volvió a la cola por ocupado
        uint64_t expired   = 0;
        uint64_t failed    = 0;   // excepciones de la fábrica
        uint64_t recovered = 0;   // recuperaciones terminadas con éxito
        uint64_t abandoned = 0;   // recuperaciones o reintentos agotados
        uint64_t dropped   = 0;   // cola llena
        std::size_t queued     = 0;   // ahora mismo
        std::size_t recovering = 0;   // ahora mismo, en recuperación
    };

    inline std::mutex& Lock()
    {
        static std::mutex lock;
        return lock;
    }

    inline std::deque<Job>& Queue()
    {
        static std::deque<Job> queue;
        return queue;
    }

    inline Stats& StatsLocked()
    {
        static Stats stats;
        return stats;
    }

    inline Stats GetStats()
    {
        std::lock_guard<std::mutex> guard(Lock());
        Stats stats = StatsLocked();
        stats.queued = Queue().size();
        stats.recovering = static_cast<std::size_t>(std::count_if(Queue().begin(), Queue().end(),
            [](Job const& job) { return job.recovery; }));
        return stats;
    }

    // Última vez que ProcessOne INTENTÓ reequipar a alguien, sea cual sea el
    // módulo que llamó. Una estática local inline: una sola instancia en todo
    // el binario, igual que Lock()/Queue(), así que "uno cada kGearIntervalMs"
    // es una propiedad real del sistema y no del número de consumidores que
    // llamen a ProcessOne en el mismo tick del mundo. Desde M12 se apunta
    // ANTES de llamar a la fábrica: un intento fallido también gasta el
    // presupuesto, y otro consumidor del mismo tick no lanza otro detrás.
    inline uint64_t& LastProcessedMs()
    {
        static uint64_t lastMs = 0;
        return lastMs;
    }

    constexpr uint64_t kGearIntervalMs = 200;

    // Con muchos grupos formándose a la vez la cola puede crecer a cientos y,
    // a un trabajo cada kGearIntervalMs, la latencia se dispara. Se topa: un
    // bot que no se reequipa no está roto, solo no topado.
    constexpr std::size_t kMaxQueue = 256;

    constexpr uint64_t kDeferRetryMs     = 1000;             // margen de un aplazado
    constexpr uint64_t kMaxDeferMs       = 5 * 60 * 1000;    // edad máxima de un aplazado
    constexpr uint64_t kMaxRecoveryAgeMs = 30 * 60 * 1000;   // una recuperación espera más
    constexpr uint8_t  kMaxAttempts      = 3;                // intentos de la fábrica
    constexpr uint64_t kRetryBackoffMs   = 30000;            // espera tras un fallo (x intentos)

    // Encola con la política de sustitución de arriba. Con el candado cogido.
    // 'requeue': vuelve un trabajo aplazado, que es MÁS VIEJO que cualquiera
    // que se haya pedido mientras para el mismo bot; ante igual prioridad
    // gana el pendiente, que es el más reciente. La marca de recuperación
    // (M12) nunca se pierde: pasa al trabajo que se queda.
    inline void EnqueueLocked(Job&& job, bool requeue = false)
    {
        for (Job& queued : Queue())
        {
            if (queued.bot != job.bot)
                continue;
            if (job.priority < queued.priority || (requeue && job.priority == queued.priority))
            {
                queued.recovery = queued.recovery || job.recovery;
                LOG_DEBUG("module", "[{}] reequipado no encolado: ya hay uno pendiente de [{}] con mas prioridad.",
                          job.tag, queued.tag);
                return;
            }
            job.recovery = job.recovery || queued.recovery;
            queued = std::move(job);   // mismo sitio en la cola, contexto nuevo
            return;
        }
        if (Queue().size() >= kMaxQueue)
        {
            ++StatsLocked().dropped;
            LOG_DEBUG("module", "[{}] cola de reequipado llena ({}): no se encola a mas bots.", job.tag, kMaxQueue);
            return;
        }
        Queue().push_back(std::move(job));
    }

    inline void Schedule(ObjectGuid bot, uint32_t ilvl, Settings const& s, char const* tag,
                         Priority priority, Resolver resolve)
    {
        if (!ilvl)
            return;
        Job job;
        job.bot       = bot;
        job.ilvl      = ilvl;
        job.tolerance = s.tolerance;
        job.tag       = tag;
        job.priority  = priority;
        job.resolve   = std::move(resolve);
        job.queuedMs  = TimeMs::NowMs();
        std::lock_guard<std::mutex> guard(Lock());
        EnqueueLocked(std::move(job));
    }

    // un módulo que se apaga retira lo que pidió él. Las recuperaciones
    // se quedan: un bot con el equipo vaciado no puede esperar a que el
    // módulo vuelva. Devuelve cuántos trabajos se retiraron.
    inline std::size_t CancelOwnedBy(std::string const& tag)
    {
        std::lock_guard<std::mutex> guard(Lock());
        std::size_t removed = 0;
        for (auto it = Queue().begin(); it != Queue().end();)
        {
            if (it->tag == tag && !it->recovery)
            {
                it = Queue().erase(it);
                ++removed;
            }
            else
                ++it;
        }
        StatsLocked().cancelled += removed;
        return removed;
    }

    // Vuelve a la cola un trabajo que no se pudo aplicar ahora (M12): con
    // margen para no reintentarlo en el mismo tick y edad máxima para que un
    // bot siempre ocupado no lo tenga dando vueltas para siempre.
    inline void Requeue(Job&& job, uint64_t now, uint64_t retryMs, char const* why)
    {
        std::lock_guard<std::mutex> guard(Lock());
        uint64_t const maxAge = job.recovery ? kMaxRecoveryAgeMs : kMaxDeferMs;
        if (now - job.queuedMs > maxAge)
        {
            if (job.recovery)
            {
                ++StatsLocked().abandoned;
                LOG_ERROR("module", "[{}] recuperacion de equipo abandonada tras {} min ({}): el bot {} puede seguir sin equipo.",
                          job.tag, maxAge / 60000, why, job.bot.GetCounter());
            }
            else
            {
                ++StatsLocked().expired;
                LOG_INFO("module", "[{}] reequipado caducado tras {} min aplazado ({}).", job.tag, maxAge / 60000, why);
            }
            return;
        }
        job.notBeforeMs = now + retryMs;
        EnqueueLocked(std::move(job), true);
    }

    // Atiende UN trabajo, como mucho una vez cada 'intervalMs' en todo el
    // proceso (kGearIntervalMs por defecto; se puede frenar bajo carga). Se
    // llama SOLO desde WorldScript::OnUpdate (ningún mapa se está actualizando):
    // por eso la comprobación de intervalo y el pop pueden ir en una sola
    // sección crítica sin TOCTOU. Devuelve true si se ha reequipado a alguien.
    inline bool ProcessOne(uint64_t intervalMs = kGearIntervalMs)
    {
        Job job;
        uint64_t const now = TimeMs::NowMs();
        {
            std::lock_guard<std::mutex> guard(Lock());
            if (now - LastProcessedMs() < intervalMs)
                return false;
            // El primero que ya se pueda mirar (los aplazados llevan margen).
            auto it = std::find_if(Queue().begin(), Queue().end(),
                                   [now](Job const& queued) { return queued.notBeforeMs <= now; });
            if (it == Queue().end())
                return false;
            job = std::move(*it);
            Queue().erase(it);
        }

#ifdef BOT_GEAR_WITH_PLAYERBOTS
        Player* bot = ObjectAccessor::FindPlayer(job.bot);
        if (!bot || !bot->IsInWorld() || !bot->GetSession() || !bot->GetSession()->IsHeadless())
        {
            // Un bot en recuperación que está cargando de mapa vuelve a la
            // cola; uno desconectado ya no se puede reequipar desde aquí.
            if (job.recovery && ObjectAccessor::FindConnectedPlayer(job.bot))
                Requeue(std::move(job), now, kDeferRetryMs, "cargando");
            else
            {
                std::lock_guard<std::mutex> guard(Lock());
                if (job.recovery)
                {
                    ++StatsLocked().abandoned;
                    LOG_ERROR("module", "[{}] el bot {} se ha desconectado con la recuperacion de equipo pendiente.",
                              job.tag, job.bot.GetCounter());
                }
                else
                    ++StatsLocked().cancelled;
            }
            return false;
        }

        // Primero el contexto: una relación terminada cancela aunque el bot
        // esté en combate; una viva recalcula el objetivo con el estado de
        // ahora (M02). Una recuperación (M12) no se cancela: se repone al
        // último objetivo que tenía.
        if (job.resolve)
        {
            uint32_t ilvl = 0;
            Decision const decision = job.resolve(bot, ilvl);
            if (decision == Decision::Cancel && !job.recovery)
            {
                LOG_DEBUG("module", "[{}] reequipado de {} cancelado: la relacion que lo pidio ya no sigue.",
                          job.tag, bot->GetName());
                std::lock_guard<std::mutex> guard(Lock());
                ++StatsLocked().cancelled;
                return false;
            }
            if (decision == Decision::Apply)
                job.ilvl = ilvl;
            else if (decision == Decision::Defer)
            {
                {
                    std::lock_guard<std::mutex> guard(Lock());
                    ++StatsLocked().deferred;
                }
                Requeue(std::move(job), now, kDeferRetryMs, "amo cargando");   // al final de la cola
                return false;
            }
        }

        if (bot->IsInCombat() || bot->IsBeingTeleported() || !bot->IsAlive())
        {
            // En combate: al final de la cola, salvo que mientras tanto se haya
            // pedido otro de igual o más prioridad para este bot (ese manda).
            {
                std::lock_guard<std::mutex> guard(Lock());
                ++StatsLocked().deferred;
            }
            Requeue(std::move(job), now, kDeferRetryMs, "bot ocupado");
            return false;
        }

        uint32_t const worn = WornItemLevel(bot);
        bool const tooHigh = worn > job.ilvl + job.tolerance;
        bool const tooLow  = worn + job.tolerance < job.ilvl;
        if (!tooHigh && !tooLow && !job.recovery)
        {
            LOG_DEBUG("module", "[{}] {} ya va a nivel de objeto {} (objetivo {}): no se toca.",
                      job.tag, bot->GetName(), worn, job.ilvl);
            std::lock_guard<std::mutex> guard(Lock());
            ++StatsLocked().skipped;
            return false;
        }

        uint32_t const quality = sConfigMgr->GetOption<uint32_t>("AiPlayerbot.AutoGearQualityLimit", 4);
        bool const twoRounds = sConfigMgr->GetOption<bool>("AiPlayerbot.TwoRoundsGearInit", true);

        // el presupuesto se gasta al INTENTAR, no al terminar bien.
        {
            std::lock_guard<std::mutex> guard(Lock());
            LastProcessedMs() = now;
        }

        // Lo mismo que "autogear reset <ilvl>": se vacía y se reequipa a ese
        // nivel. Con 'incremental' sólo se mejoraría, y aquí hay que bajar.
        // try/catch: una excepción de la fábrica no debe subir a World::Update.
        bool destroyed = false;
        std::string error;
        try
        {
            PlayerbotFactory::DestroyEquippedGear(bot);
            destroyed = true;
            PlayerbotFactory::AutoGear(bot, quality, job.ilvl, /*incremental*/ false, twoRounds);
        }
        catch (std::exception const& e)
        {
            error = e.what();
            if (error.empty())
                error = "sin mensaje";
        }
        catch (...)
        {
            error = "excepcion desconocida";
        }

        if (!error.empty())
        {
            ++job.attempts;
            job.recovery = job.recovery || destroyed;
            {
                std::lock_guard<std::mutex> guard(Lock());
                ++StatsLocked().failed;
            }
            if (job.attempts >= kMaxAttempts)
            {
                std::lock_guard<std::mutex> guard(Lock());
                ++StatsLocked().abandoned;
                LOG_ERROR("module", "[{}] reequipado de {} abandonado tras {} fallos ({}){}.", job.tag, bot->GetName(),
                          job.attempts, error, job.recovery ? ": puede haberse quedado sin equipo" : "");
                return false;
            }
            LOG_ERROR("module", "[{}] excepcion reequipando a {} (intento {}/{}): {}. {} en {} s.", job.tag, bot->GetName(),
                      job.attempts, kMaxAttempts, error, job.recovery ? "Se recupera su equipo" : "Se reintenta",
                      kRetryBackoffMs * job.attempts / 1000);
            Requeue(std::move(job), now, kRetryBackoffMs * job.attempts, "fallo de la fabrica");
            return false;
        }

        {
            std::lock_guard<std::mutex> guard(Lock());
            ++StatsLocked().completed;
            if (job.recovery)
                ++StatsLocked().recovered;
        }

        LOG_INFO("module", "[{}] {} reequipado de nivel de objeto {} a ~{} (fase y equipo del jugador){}.",
                 job.tag, bot->GetName(), worn, WornItemLevel(bot), job.recovery ? " — equipo recuperado" : "");
        return true;
#else
        return false;
#endif
    }
}

#endif // WOTLK_SP_BOT_GEAR_H
