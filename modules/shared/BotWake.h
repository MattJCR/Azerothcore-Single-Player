// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * BotWake.h -- seleccion barata y compartida de bots dormidos.
 *
 * ORDER BY RAND() obliga a MariaDB a materializar y ordenar todos los
 * personajes que cumplen el filtro. Los modulos que necesitan despertar bots
 * lo hacian desde el hilo del mundo y podian sumar varios filesort en una sola
 * pasada. Este helper elige un GUID pivote y recorre la clave primaria desde
 * ahi, con una segunda consulta para envolver al principio de la tabla.
 */

#ifndef WOTLK_SP_BOT_WAKE_H
#define WOTLK_SP_BOT_WAKE_H

#include "DatabaseEnv.h"
#include "Random.h"
#include "TimeMs.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace BotWake
{
    inline uint32_t MaxCharacterGuid()
    {
        struct Cache
        {
            uint32_t value = 0;
            uint64_t expiresAt = 0;
        };

        static std::mutex lock;
        static Cache cache;
        std::lock_guard<std::mutex> guard(lock);

        uint64_t const now = TimeMs::NowMs();
        if (cache.value && now < cache.expiresAt)
            return cache.value;

        QueryResult result = CharacterDatabase.Query("SELECT MAX(guid) FROM characters");
        cache.value = result ? (*result)[0].Get<uint32>() : 0;
        cache.expiresAt = now + TimeMs::SecsToMs(60);
        return cache.value;
    }

    // classes: lista SQL de clases ("1,2,6") o nullptr para cualquiera. Sale
    // siempre de una constante del llamador, nunca de texto del jugador.
    inline void AppendRange(std::vector<uint32_t>& out, uint32_t limit,
                            uint32_t minLevel, uint32_t maxLevel,
                            char const* races, char const* classes,
                            uint32_t pivot, bool wrap)
    {
        if (out.size() >= limit)
            return;

        uint32_t const remaining = limit - static_cast<uint32_t>(out.size());
        std::string const classFilter = (classes && *classes)
            ? std::string("AND class IN (") + classes + ") " : std::string();
        QueryResult result = wrap
            ? CharacterDatabase.Query(
                "SELECT guid FROM characters WHERE online = 0 AND level BETWEEN {} AND {} "
                "AND race IN ({}) {}AND guid < {} ORDER BY guid LIMIT {}",
                minLevel, maxLevel, races, classFilter, pivot, remaining)
            : CharacterDatabase.Query(
                "SELECT guid FROM characters WHERE online = 0 AND level BETWEEN {} AND {} "
                "AND race IN ({}) {}AND guid >= {} ORDER BY guid LIMIT {}",
                minLevel, maxLevel, races, classFilter, pivot, remaining);

        if (!result)
            return;

        do
            out.push_back((*result)[0].Get<uint32>());
        while (out.size() < limit && result->NextRow());
    }

    // Devuelve mas candidatos que los pedidos porque la tabla characters
    // tambien contiene personajes humanos; el llamador confirma IsRandomBot.
    // El multiplicador es 6 (no 4) porque en tramos de nivel bajo se acumulan
    // los alts humanos aparcados: con una ventana estrecha se corria el riesgo
    // de devolver < wanted bots reales y rellenar de menos esa pasada.
    // classes (M26) limita a las clases capaces de un rol cuando un grupo
    // espera tanque o sanador: sin filtro, en un tramo con pocos bots se
    // despertaban sólo dps y la espera de rol no servía de nada.
    inline std::vector<uint32_t> SelectOfflineCandidates(uint32_t wanted,
                                                          uint32_t minLevel,
                                                          uint32_t maxLevel,
                                                          bool alliance,
                                                          char const* classes = nullptr)
    {
        if (!wanted || minLevel > maxLevel)
            return {};

        uint32_t const maxGuid = MaxCharacterGuid();
        if (!maxGuid)
            return {};

        uint32_t const candidateLimit = wanted > std::numeric_limits<uint32_t>::max() / 6
            ? std::numeric_limits<uint32_t>::max()
            : wanted * 6;
        uint32_t const pivot = urand(1, maxGuid);
        char const* races = alliance ? "1,3,4,7,11" : "2,5,6,8,10";

        std::vector<uint32_t> candidates;
        candidates.reserve(candidateLimit);
        AppendRange(candidates, candidateLimit, minLevel, maxLevel, races, classes, pivot, false);
        AppendRange(candidates, candidateLimit, minLevel, maxLevel, races, classes, pivot, true);
        return candidates;
    }
}

#endif // WOTLK_SP_BOT_WAKE_H
