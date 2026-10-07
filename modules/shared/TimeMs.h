// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * TimeMs.h — reloj monótono de 64 bits para los módulos propios. Cabecera
 * compartida (copia byte a byte en cada módulo; la pone el instalador desde
 * modules/shared/).
 *
 * EL PROBLEMA
 * Los siete módulos que llevan plazos ("dentro de N segundos, haz X") tenían
 * cada uno su propio NowMs() truncando GameTime::GetGameTimeMS() (un int64_t
 * de milisegundos desde el arranque) a uint32_t, y comparaban esos plazos en
 * absoluto (`if (now >= due) ...`). Un uint32_t de milisegundos da la vuelta
 * a los 2^32 ms, unos 49,7 días de servidor sin reiniciar: los plazos que
 * quedan "en el futuro" tras la vuelta no vencen nunca, y los que ya habían
 * vencido antes de la vuelta se disparan de golpe.
 *
 * LA SOLUCIÓN
 * Una única función NowMs() sin truncar (el origen ya es de 64 bits) y un
 * ayudante SecsToMs()/MinsToMs() que promociona antes de multiplicar, para
 * que "segundos de config -> milisegundos" tampoco desborde con un valor de
 * configuración grande (relacionado con la fase 0.8, aritmética de config).
 *
 * Funciones inline puras sin estado: a diferencia de BotClaims.h/BotGear.h no
 * hay ODR que cuidar aquí (no hay estáticas locales), así que no importa que
 * el instalador la reparta a los ocho módulos aunque sólo la usen unos pocos.
 */

#ifndef WOTLK_SP_TIME_MS_H
#define WOTLK_SP_TIME_MS_H

#include "GameTime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace TimeMs
{
    // Milisegundos desde el arranque del worldserver, sin truncar.
    //
    // GameTime::GetGameTimeMS() se actualiza UNA vez por tick del mundo, así que
    // NowMs() no avanza dentro de un tick. Correcto para plazos "dentro de N
    // segundos" (su único uso), pero es la herramienta equivocada para perfilar
    // dentro del tick: para eso está SteadyNowMs().
    inline uint64_t NowMs()
    {
        static_assert(sizeof(GameTime::GetGameTimeMS().count()) == 8,
                      "GetGameTimeMS() debe ser de 64 bits: TimeMs existe para no truncarlo");
        return static_cast<uint64_t>(GameTime::GetGameTimeMS().count());
    }

    // Reloj monótono de verdad (steady_clock), que SÍ avanza dentro de un tick.
    // Úsalo solo para medir duraciones cortas (perfilado de un módulo, como en
    // la vigilancia de ticks lentos); NUNCA lo compares contra NowMs().
    inline uint64_t SteadyNowMs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Segundos/minutos de configuración -> milisegundos, promocionando a 64 bits
    // antes de multiplicar. La saturación es un no-op para los uint32 que pasan
    // los módulos hoy, pero protege si alguien pasa un uint64 grande (o un dato
    // sucio de una tabla SQL): se topa en vez de dar la vuelta.
    inline uint64_t SecsToMs(uint64_t secs)
    {
        constexpr uint64_t kMaxSecs = std::numeric_limits<uint64_t>::max() / 1000;
        return std::min(secs, kMaxSecs) * 1000;
    }

    inline uint64_t MinsToMs(uint64_t mins)
    {
        constexpr uint64_t kMaxMins = std::numeric_limits<uint64_t>::max() / 60000;
        return std::min(mins, kMaxMins) * 60000;
    }
}

#endif // WOTLK_SP_TIME_MS_H
