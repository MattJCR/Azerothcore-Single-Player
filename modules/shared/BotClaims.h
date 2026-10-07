// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * BotClaims.h — registro compartido de "este bot está ocupado por un módulo".
 *
 * EL PROBLEMA
 * Hay varios módulos moviendo bots a la vez: mod-queue-bots (colas y bandas),
 * mod-world-bots (poblar zonas), mod-quest-mates (compañeros de misión),
 * mod-party-here (grupos sin cola) y mod-home-guild (tu hermandad). Casi todo
 * lo que hace "ocupado" a un bot se ve en el núcleo (grupo, cola, mazmorra,
 * combate), pero hay huecos: un bot elegido para un grupo que todavía no está
 * dentro, un bot al que se está trayendo, o un bot que un módulo de terceros
 * ha reservado para un evento. Sin un registro común, dos módulos pueden
 * elegir el mismo bot en el mismo tick.
 *
 * LA SOLUCIÓN
 * Un mapa guid -> módulo propietario, en funciones inline con estáticas
 * locales: el enlazador las funde en UNA sola instancia para todo el
 * worldserver, sin que ningún módulo dependa de otro al enlazar. Cada módulo
 * lleva su copia de este fichero (la pone el instalador desde modules/shared/)
 * y las copias tienen que ser byte a byte idénticas (regla ODR).
 *
 * Además guarda el conjunto de bots de la hermandad del jugador
 * (mod-home-guild), para que los demás módulos nunca los manden a dormir y
 * los prefieran al formar grupos.
 *
 * USO
 *   BotClaims::Claim(guidLow, "queue-bots")   // false si ya es de otro
 *   BotClaims::Release(guidLow, "queue-bots") // sólo suelta lo propio
 *   BotClaims::IsClaimed(guidLow)
 *   BotClaims::IsHomeGuildBot(guidLow)
 *
 * Toda reserva es exclusiva y no se cede implicitamente: solo el propietario
 * que figura en el mapa puede liberarla. La hermandad de casa es una
 * preferencia de seleccion, no una reserva, y por tanto no bloquea propietarios.
 *
 * Compatible con el BotActivityRegistry.h de mod-playerbot-world-pvp en
 * espíritu, no en símbolos: si algún día se instala aquel módulo, hay que
 * hacer que consulte este registro (o al revés) con un parche de tres líneas.
 */

#ifndef WOTLK_SP_BOT_CLAIMS_H
#define WOTLK_SP_BOT_CLAIMS_H

#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace BotClaims
{
    struct Diagnostics
    {
        size_t totalClaims = 0;
        size_t homeGuildBots = 0;
        std::map<std::string, uint32_t> claimsByOwner;
    };

    enum class ClaimResult : uint8_t
    {
        Acquired,
        AlreadyOwned,
        Busy
    };

    // shared_mutex: los lectores (IsClaimed / IsClaimedByOther / IsHomeGuildBot)
    // en los caminos IsFreeBot de los 4 hilos de mapa superan de largo a los
    // escritores (Lease, Release, SetHomeGuildBots).
    inline std::shared_mutex& Lock()
    {
        static std::shared_mutex lock;
        return lock;
    }

    inline std::unordered_map<uint32_t, std::string>& Claims()
    {
        static std::unordered_map<uint32_t, std::string> claims;
        return claims;
    }

    inline std::unordered_set<uint32_t>& HomeGuild()
    {
        static std::unordered_set<uint32_t> bots;
        return bots;
    }

    // Intenta reservar de forma atomica y distingue una reserva nueva de una
    // que ya pertenecia al mismo propietario. Esa distincion permite que el
    // guard de abajo no libere por error una relacion anterior.
    inline ClaimResult TryClaim(uint32_t guidLow, char const* owner)
    {
        std::lock_guard<std::shared_mutex> guard(Lock());
        auto [it, inserted] = Claims().try_emplace(guidLow, owner);
        if (inserted)
            return ClaimResult::Acquired;
        return it->second == owner ? ClaimResult::AlreadyOwned : ClaimResult::Busy;
    }

    // API historica: true si ya era suyo o estaba libre.
    inline bool Claim(uint32_t guidLow, char const* owner)
    {
        return TryClaim(guidLow, owner) != ClaimResult::Busy;
    }

    // Suelta el bot, pero sólo si es de 'owner': nadie pisa la reserva de otro.
    inline void Release(uint32_t guidLow, char const* owner)
    {
        std::lock_guard<std::shared_mutex> guard(Lock());
        auto it = Claims().find(guidLow);
        if (it != Claims().end() && it->second == owner)
            Claims().erase(it);
    }

    // Suelta todo lo que tenga 'owner' (al apagar o al reiniciar un módulo).
    inline void ReleaseAll(char const* owner)
    {
        std::lock_guard<std::shared_mutex> guard(Lock());
        for (auto it = Claims().begin(); it != Claims().end();)
            it = (it->second == owner) ? Claims().erase(it) : std::next(it);
    }

    inline bool IsClaimed(uint32_t guidLow)
    {
        std::shared_lock<std::shared_mutex> guard(Lock());
        return Claims().count(guidLow) != 0;
    }

    // ¿Está reservado por OTRO módulo? (Lo propio no cuenta como ocupado.)
    inline bool IsClaimedByOther(uint32_t guidLow, char const* owner)
    {
        std::shared_lock<std::shared_mutex> guard(Lock());
        auto it = Claims().find(guidLow);
        return it != Claims().end() && it->second != owner;
    }

    inline std::string Owner(uint32_t guidLow)
    {
        std::shared_lock<std::shared_mutex> guard(Lock());
        auto it = Claims().find(guidLow);
        return it != Claims().end() ? it->second : std::string();
    }

    // Reserva temporal con liberacion automatica en todos los caminos de
    // error. Keep() transfiere la reserva nueva al estado persistente del
    // modulo. Si la reserva ya era del mismo propietario, el destructor no la
    // toca: pertenecia a una operacion anterior. ReleaseNow() es simetrico al
    // destructor: solo suelta una reserva NUEVA (Acquired), nunca una que ya
    // pertenecia a otra operacion viva del mismo modulo (AlreadyOwned).
    class Lease
    {
    public:
        Lease(uint32_t guidLow, char const* owner)
            : _guidLow(guidLow), _owner(owner ? owner : ""), _result(TryClaim(guidLow, _owner.c_str())) { }

        Lease(Lease const&) = delete;
        Lease& operator=(Lease const&) = delete;
        Lease& operator=(Lease&&) = delete;

        Lease(Lease&& other) noexcept
            : _guidLow(other._guidLow), _owner(std::move(other._owner)), _result(other._result), _keep(other._keep)
        {
            other._result = ClaimResult::Busy;
            other._keep = true;
        }

        ~Lease()
        {
            if (_result == ClaimResult::Acquired && !_keep)
                Release(_guidLow, _owner.c_str());
        }

        explicit operator bool() const { return _result != ClaimResult::Busy; }
        bool IsNew() const { return _result == ClaimResult::Acquired; }
        void Keep() { _keep = true; }

        void ReleaseNow()
        {
            // Igual que el destructor: solo se suelta lo que reservo este Lease.
            // Con AlreadyOwned la reserva es de una operacion anterior que sigue
            // viva; soltarla aqui la mataria por error.
            if (_result == ClaimResult::Acquired)
                Release(_guidLow, _owner.c_str());
            _result = ClaimResult::Busy;
            _keep = true;
        }

    private:
        uint32_t    _guidLow = 0;
        std::string _owner;
        ClaimResult _result = ClaimResult::Busy;
        bool        _keep = false;
    };

    // ─── Bots de la hermandad del jugador (mod-home-guild) ──────────────────
    inline void SetHomeGuildBots(std::unordered_set<uint32_t> const& bots)
    {
        std::lock_guard<std::shared_mutex> guard(Lock());
        HomeGuild() = bots;
    }

    inline void AddHomeGuildBot(uint32_t guidLow)
    {
        std::lock_guard<std::shared_mutex> guard(Lock());
        HomeGuild().insert(guidLow);
    }

    inline void RemoveHomeGuildBot(uint32_t guidLow)
    {
        std::lock_guard<std::shared_mutex> guard(Lock());
        HomeGuild().erase(guidLow);
    }

    inline bool IsHomeGuildBot(uint32_t guidLow)
    {
        std::shared_lock<std::shared_mutex> guard(Lock());
        return HomeGuild().count(guidLow) != 0;
    }

    inline size_t HomeGuildSize()
    {
        std::shared_lock<std::shared_mutex> guard(Lock());
        return HomeGuild().size();
    }

    // Foto coherente para diagnósticos: claims y hermandad se leen bajo el
    // mismo candado, sin exponer referencias a las colecciones compartidas.
    inline Diagnostics GetDiagnostics()
    {
        std::shared_lock<std::shared_mutex> guard(Lock());
        Diagnostics out;
        out.totalClaims = Claims().size();
        out.homeGuildBots = HomeGuild().size();
        for (auto const& [unused, owner] : Claims())
        {
            (void)unused;
            ++out.claimsByOwner[owner];
        }
        return out;
    }
}

#endif // WOTLK_SP_BOT_CLAIMS_H
