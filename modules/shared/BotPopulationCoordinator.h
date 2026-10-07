// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * BotPopulationCoordinator.h -- arbitro compartido de logins de playerbots.
 *
 * Un AddPlayerBot tarda varios segundos en reflejarse como jugador online. Sin
 * una reserva comun, queue-bots, world-bots, party-here y home-guild pueden
 * contar el mismo hueco y seleccionar el mismo GUID durante esa ventana.
 *
 * El coordinador conserva los GUID concedidos hasta verlos online (o hasta el
 * timeout), cuenta online + pendientes contra MaxRandomBots y limita tambien
 * la rafaga total, por faccion y por tramo solicitado. Todo es inline para que
 * las copias identicas que instala modules/shared compartan una sola instancia
 * por ODR, igual que BotClaims.
 *
 * CONFIGURACION. Cada modulo que reserve llama a LoadSettings() en su
 * OnAfterConfigLoad: asi el tope global sale siempre de AiPlayerbot.MaxRandomBots
 * aunque mod-world-bots (donde vive el bloque BotPopulation.* documentado del
 * .conf) este desactivado. Las claves BotPopulation.* son opcionales; si no
 * estan en ningun .conf se usan los valores por defecto.
 *
 * FOTO DE ONLINE. RefreshOnline() escanea ObjectAccessor y esta cacheada 1 s;
 * el hilo del mundo la mantiene fresca con RefreshOnlineFrom(snapshot) una vez
 * por pasada, de modo que Reserve() y GetDiagnostics() no vuelven a escanear.
 * El recuento usa WorldSession::IsHeadless() (incluye alt-bots de jugadores), igual
 * que los CountOnlineBots de cada modulo: sesgo conocido de MEJORAS §3.2.
 *
 * SIN AddPlayerBot fiable. PlayerbotMgr::AddPlayerBot devuelve void, asi que un
 * login que se cuelgue no se detecta; su reserva caduca por
 * BotPopulation.PendingTimeoutSeconds. Release() esta para los caminos en que
 * el modulo decide no seguir tras reservar.
 */

#ifndef WOTLK_SP_BOT_POPULATION_COORDINATOR_H
#define WOTLK_SP_BOT_POPULATION_COORDINATOR_H

#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "TimeMs.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace BotPopulationCoordinator
{
    enum class Faction : uint8_t
    {
        Alliance = 0,
        Horde = 1
    };

    enum class RejectReason : uint8_t
    {
        None,
        DuplicateGuid,
        AlreadyOnline,
        GlobalLimit,
        TotalBudget,
        FactionBudget,
        RangeBudget
    };

    struct Settings
    {
        uint32_t globalCap = 200;
        uint32_t pendingTimeoutSecs = 90;
        uint32_t maxPendingTotal = 40;
        uint32_t maxPendingPerFaction = 24;
        uint32_t maxPendingPerRange = 12;
    };

    struct Pending
    {
        uint32_t guidLow = 0;
        std::string module;
        Faction faction = Faction::Alliance;
        uint32_t minLevel = 1;
        uint32_t maxLevel = 80;
        uint64_t requestedAtMs = 0;
    };

    struct Rejection
    {
        std::string module;
        RejectReason reason = RejectReason::None;
        Faction faction = Faction::Alliance;
        uint32_t minLevel = 1;
        uint32_t maxLevel = 80;
        uint64_t atMs = 0;
    };

    struct Diagnostics
    {
        Settings settings;
        uint64_t dataAgeMs = 0;   // antiguedad de la foto de online que se muestra
        uint32_t onlineTotal = 0;
        std::array<uint32_t, 2> onlineByFaction { 0, 0 };
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> onlineByRange;
        std::vector<Pending> pending;
        std::map<std::string, uint32_t> pendingByModule;
        std::array<uint32_t, 2> pendingByFaction { 0, 0 };
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> pendingByRange;
        std::vector<Rejection> recentRejections;
    };

    struct Reservation
    {
        bool granted = false;
        RejectReason reason = RejectReason::None;
        explicit operator bool() const { return granted; }
    };

    struct Online
    {
        Faction faction = Faction::Alliance;
        uint32_t level = 1;
    };

    struct State
    {
        Settings settings;
        std::unordered_map<uint32_t, Online> online;
        std::unordered_map<uint32_t, Pending> pending;
        std::deque<Rejection> rejections;
        uint64_t nextRefreshMs = 0;
        uint64_t lastRefreshMs = 0;
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

    inline uint32_t FactionIndex(Faction faction)
    {
        return faction == Faction::Alliance ? 0u : 1u;
    }

    // Tope de nivel de la etapa de progresion activa (mod-world-bots), UINT32_MAX
    // si no hay ninguna restriccion (modulo/etapa apagados, o etapa WotLK). Los
    // modulos que despiertan bots de la reserva por su cuenta (queue-bots,
    // party-here, home-guild) lo consultan antes de elegir candidato: sin esto
    // podian traer un bot por encima del tope sin pasar por StageCap(), y
    // StagePass nunca los corrige porque llegan reclamados (no "libres").
    inline uint32_t& StageCapState()
    {
        static uint32_t cap = UINT32_MAX;
        return cap;
    }

    inline uint32_t StageCap()
    {
        std::lock_guard<std::mutex> guard(Lock());
        return StageCapState();
    }

    // La llama mod-world-bots cada vez que aplica o restaura una etapa.
    inline void SetStageCap(uint32_t cap)
    {
        std::lock_guard<std::mutex> guard(Lock());
        StageCapState() = cap ? cap : UINT32_MAX;
    }

    inline std::pair<uint32_t, uint32_t> LevelBucket(uint32_t level)
    {
        uint32_t const first = ((std::max<uint32_t>(level, 1) - 1) / 10) * 10 + 1;
        return { first, first + 9 };
    }

    inline void ExpirePending(State& state, uint64_t now)
    {
        uint64_t const ttl = TimeMs::SecsToMs(state.settings.pendingTimeoutSecs);
        for (auto it = state.pending.begin(); it != state.pending.end();)
            it = (ttl && now - it->second.requestedAtMs >= ttl) ? state.pending.erase(it) : std::next(it);
    }

    inline void Configure(Settings settings)
    {
        if (!settings.globalCap)
            settings.globalCap = 100000;   // "sin tope" != "rechazar todo"
        std::lock_guard<std::mutex> guard(Lock());
        Data().settings = settings;
    }

    // La llama cada modulo consumidor en su OnAfterConfigLoad. Varias llamadas
    // con los mismos valores son inofensivas (gana la ultima, todas escriben
    // lo mismo). El tope global es AiPlayerbot.MaxRandomBots siempre.
    inline void LoadSettings()
    {
        Settings s;
        s.globalCap            = sConfigMgr->GetOption<uint32_t>("AiPlayerbot.MaxRandomBots", 200);
        s.pendingTimeoutSecs   = sConfigMgr->GetOption<uint32_t>("BotPopulation.PendingTimeoutSeconds", 90);
        s.maxPendingTotal      = sConfigMgr->GetOption<uint32_t>("BotPopulation.MaxPendingTotal", 40);
        s.maxPendingPerFaction = sConfigMgr->GetOption<uint32_t>("BotPopulation.MaxPendingPerFaction", 24);
        s.maxPendingPerRange   = sConfigMgr->GetOption<uint32_t>("BotPopulation.MaxPendingPerRange", 12);
        Configure(s);
    }

    // El modulo reservo un GUID pero decide no conectarlo: suelta el hueco ya,
    // sin esperar al timeout.
    inline void Release(uint32_t guidLow)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Data().pending.erase(guidLow);
    }

    // ¿Tiene este GUID un login reservado y aún sin cumplir? Para que quien
    // recorre un roster no gaste un intento en él (M11).
    inline bool IsPending(uint32_t guidLow, uint64_t now = TimeMs::NowMs())
    {
        std::lock_guard<std::mutex> guard(Lock());
        State& state = Data();
        ExpirePending(state, now);
        return state.pending.count(guidLow) != 0;
    }

    // Reconcilia la foto de online con lo que acaba de recoger el llamador y
    // retira las reservas ya cumplidas o caducadas.
    inline void CommitOnline(std::unordered_map<uint32_t, Online>&& found, uint64_t now)
    {
        std::lock_guard<std::mutex> guard(Lock());
        State& state = Data();
        state.online = std::move(found);
        for (auto it = state.pending.begin(); it != state.pending.end();)
            it = state.online.count(it->first) ? state.pending.erase(it) : std::next(it);
        ExpirePending(state, now);
        state.nextRefreshMs = now + 1000;
        state.lastRefreshMs = now;
    }

    // El escaneo esta cacheado un segundo y se comparte entre todos los
    // llamadores. force se usa tras operaciones administrativas si hiciera falta.
    inline void RefreshOnline(uint64_t now = TimeMs::NowMs(), bool force = false)
    {
        {
            std::lock_guard<std::mutex> guard(Lock());
            if (!force && now < Data().nextRefreshMs)
                return;
            Data().nextRefreshMs = now + 1000;
        }

        std::unordered_map<uint32_t, Online> found;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* player = pair.second;
            WorldSession* session = player ? player->GetSession() : nullptr;
            if (!session || !session->IsHeadless() || !player->IsInWorld())
                continue;
            found[player->GetGUID().GetCounter()] = {
                player->GetTeamId() == TEAM_ALLIANCE ? Faction::Alliance : Faction::Horde,
                player->GetLevel()
            };
        }

        CommitOnline(std::move(found), now);
    }

    // El hilo del mundo ya recorre ObjectAccessor una vez por pasada: le pasa
    // aqui su lista de bots y el coordinador no vuelve a escanear en Reserve()
    // ni en el diagnostico durante esa pasada.
    inline void RefreshOnlineFrom(std::vector<Player*> const& bots, uint64_t now = TimeMs::NowMs())
    {
        std::unordered_map<uint32_t, Online> found;
        found.reserve(bots.size());
        for (Player* player : bots)
        {
            if (!player)
                continue;
            found[player->GetGUID().GetCounter()] = {
                player->GetTeamId() == TEAM_ALLIANCE ? Faction::Alliance : Faction::Horde,
                player->GetLevel()
            };
        }
        CommitOnline(std::move(found), now);
    }

    // Recuento de bots online compartido: usa la foto cacheada, asi que un
    // modulo puede sustituir su propio CountOnlineBots sin pagar otro escaneo.
    inline uint32_t OnlineCount(uint64_t now = TimeMs::NowMs())
    {
        RefreshOnline(now);
        std::lock_guard<std::mutex> guard(Lock());
        return static_cast<uint32_t>(Data().online.size());
    }

    inline void RememberReject(State& state, char const* module, RejectReason reason,
                               Faction faction, uint32_t minLevel, uint32_t maxLevel,
                               uint64_t now)
    {
        state.rejections.push_back({ module ? module : "unknown", reason, faction, minLevel, maxLevel, now });
        while (state.rejections.size() > 20)
            state.rejections.pop_front();
    }

    inline Reservation Reserve(uint32_t guidLow, char const* module, Faction faction,
                               uint32_t minLevel, uint32_t maxLevel,
                               uint64_t now = TimeMs::NowMs())
    {
        RefreshOnline(now);
        std::lock_guard<std::mutex> guard(Lock());
        State& state = Data();
        ExpirePending(state, now);

        auto reject = [&](RejectReason reason)
        {
            RememberReject(state, module, reason, faction, minLevel, maxLevel, now);
            return Reservation { false, reason };
        };

        if (state.pending.count(guidLow))
            return reject(RejectReason::DuplicateGuid);
        if (state.online.count(guidLow))
            return reject(RejectReason::AlreadyOnline);
        if (!state.settings.globalCap || state.online.size() + state.pending.size() >= state.settings.globalCap)
            return reject(RejectReason::GlobalLimit);
        if (state.settings.maxPendingTotal && state.pending.size() >= state.settings.maxPendingTotal)
            return reject(RejectReason::TotalBudget);

        uint32_t factionPending = 0;
        uint32_t rangePending = 0;
        for (auto const& [unused, pending] : state.pending)
        {
            (void)unused;
            if (pending.faction == faction)
                ++factionPending;
            if (pending.faction == faction && pending.minLevel == minLevel && pending.maxLevel == maxLevel)
                ++rangePending;
        }
        if (state.settings.maxPendingPerFaction && factionPending >= state.settings.maxPendingPerFaction)
            return reject(RejectReason::FactionBudget);
        if (state.settings.maxPendingPerRange && rangePending >= state.settings.maxPendingPerRange)
            return reject(RejectReason::RangeBudget);

        state.pending.emplace(guidLow, Pending {
            guidLow, module ? module : "unknown", faction, minLevel, maxLevel, now
        });
        return { true, RejectReason::None };
    }

    // Solo diagnostico. Normalmente el hilo del mundo ya refresco la foto esta
    // pasada (RefreshOnlineFrom) o hace un instante (Reserve); solo si nadie la
    // ha tocado en 3 s se paga aqui un escaneo. Corre en el hilo del mundo,
    // asi que es seguro; se evita en el caso normal.
    inline Diagnostics GetDiagnostics(uint64_t now = TimeMs::NowMs())
    {
        bool stale;
        {
            std::lock_guard<std::mutex> guard(Lock());
            uint64_t const last = Data().lastRefreshMs;
            stale = !last || (now >= last && now - last > 3000);
        }
        if (stale)
            RefreshOnline(now, true);

        std::lock_guard<std::mutex> guard(Lock());
        State& state = Data();
        Diagnostics out;
        out.settings = state.settings;
        out.dataAgeMs = state.lastRefreshMs && now >= state.lastRefreshMs ? now - state.lastRefreshMs : 0;
        out.onlineTotal = static_cast<uint32_t>(state.online.size());
        for (auto const& [unused, online] : state.online)
        {
            (void)unused;
            ++out.onlineByFaction[FactionIndex(online.faction)];
            ++out.onlineByRange[LevelBucket(online.level)];
        }
        for (auto const& [unused, pending] : state.pending)
        {
            (void)unused;
            out.pending.push_back(pending);
            ++out.pendingByModule[pending.module];
            ++out.pendingByFaction[FactionIndex(pending.faction)];
            ++out.pendingByRange[{ pending.minLevel, pending.maxLevel }];
        }
        out.recentRejections.assign(state.rejections.begin(), state.rejections.end());
        return out;
    }

    inline char const* ReasonName(RejectReason reason)
    {
        switch (reason)
        {
            case RejectReason::DuplicateGuid:  return "GUID ya pendiente";
            case RejectReason::AlreadyOnline: return "GUID ya online";
            case RejectReason::GlobalLimit:   return "limite global";
            case RejectReason::TotalBudget:   return "presupuesto pendiente global";
            case RejectReason::FactionBudget: return "presupuesto de faccion";
            case RejectReason::RangeBudget:   return "presupuesto de tramo";
            default:                          return "ninguno";
        }
    }
}

#endif // WOTLK_SP_BOT_POPULATION_COORDINATOR_H
