// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * BotWorldAge.h -- antiguedad compartida de una sesion bot en el mundo.
 *
 * AddPlayerBot termina el login en varias fases. PlayerLoading deja de estar
 * activo antes de que la IA y el mapa se hayan asentado del todo, por lo que
 * teletransportar al bot inmediatamente puede dejar una sesion muda. Se toma
 * como instante cero la primera observacion de cada objeto WorldSession; una
 * sesion nueva para el mismo GUID reinicia la espera: por el puntero de la
 * sesion y, como el nuevo WorldSession puede reutilizar la direccion del
 * anterior, por Forget() al desconectar (M08).
 */

#ifndef WOTLK_SP_BOT_WORLD_AGE_H
#define WOTLK_SP_BOT_WORLD_AGE_H

#include "TimeMs.h"

#include <cstdint>
#include <iterator>
#include <mutex>
#include <unordered_map>

namespace BotWorldAge
{
    // firstSeenMs mide la antigüedad de la sesión; lastSeenMs sólo sirve para
    // podar ausencias (M08). Antes un solo campo hacía las dos cosas y una
    // sesión conectada más de una hora volvía a "recién llegada".
    struct Seen
    {
        void const* session = nullptr;
        uint64_t firstSeenMs = 0;
        uint64_t lastSeenMs = 0;
    };

    inline std::mutex& Lock()
    {
        static std::mutex lock;
        return lock;
    }

    inline std::unordered_map<uint32_t, Seen>& Bots()
    {
        static std::unordered_map<uint32_t, Seen> bots;
        return bots;
    }

    // Un registro que nadie ha consultado en una hora es de un bot que ya no
    // está (o que ningún consumidor mira): se poda. La antigüedad de una
    // sesión que se sigue observando no caduca nunca.
    constexpr uint64_t kStaleMs = 3600u * 1000u;

    // La sesión de este GUID se ha cerrado: la siguiente empieza de cero
    // aunque el nuevo WorldSession caiga en la misma dirección de memoria.
    // La llaman los consumidores desde OnPlayerLogout; repetirla no hace daño.
    inline void Forget(uint32_t guidLow)
    {
        std::lock_guard<std::mutex> guard(Lock());
        Bots().erase(guidLow);
    }

    inline bool IsMature(uint32_t guidLow, void const* session, uint64_t now,
                         uint32_t minWorldSeconds)
    {
        if (!minWorldSeconds)
            return true;

        std::lock_guard<std::mutex> guard(Lock());
        auto& bots = Bots();

        // Poda perezosa de ausencias, nunca de sesiones que se siguen viendo.
        if (bots.size() > 4096)
            for (auto it = bots.begin(); it != bots.end();)
                it = (now >= it->second.lastSeenMs && now - it->second.lastSeenMs > kStaleMs)
                    ? bots.erase(it) : std::next(it);

        Seen& seen = bots[guidLow];
        // Sin Forget (un cierre que no pasó por OnPlayerLogout), una hora sin
        // observarlo tampoco prueba que sea la misma sesión: se reinicia.
        bool const absent = seen.lastSeenMs && now >= seen.lastSeenMs
            && now - seen.lastSeenMs > kStaleMs;
        if (seen.session != session || !seen.firstSeenMs || now < seen.firstSeenMs || absent)
        {
            seen = { session, now, now };
            return false;
        }

        seen.lastSeenMs = now;
        return now - seen.firstSeenMs >= TimeMs::SecsToMs(minWorldSeconds);
    }
}

#endif // WOTLK_SP_BOT_WORLD_AGE_H
