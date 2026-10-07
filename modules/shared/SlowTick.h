// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * SlowTick.h — aviso común de "esta pasada ha tardado más de la cuenta".
 *
 * EL PROBLEMA
 * Varios módulos hacen trabajo periódico en el hilo del mundo (OnUpdate):
 * repartir bots, rellenar colas, publicar el snapshot del panel... Si una
 * pasada se alarga (una consulta lenta, un pico de candidatos, un `.reload`
 * de por medio), el síntoma en juego es el mismo para cualquiera de ellas:
 * el mundo se queda mudo un instante. Sin medir CADA módulo por separado, la
 * sospecha recae por defecto en el core (el 05/09/2026 la causa
 * de una pausa así eran 180 consultas síncronas de un módulo, no el core).
 *
 * LA SOLUCIÓN
 * `WarnIfSlow(modulo, paso, startMs)`: mismo umbral que usa el propio core
 * para su "Update time diff" (100 ms; si el core sigue avisando de ticks
 * lentos y aquí no sale nada, no son de estos módulos). Como mucho un aviso
 * por (módulo, paso) y minuto, con el peor caso visto en ese minuto: un paso
 * lento y frecuente se ve en Server.log sin inundarlo.
 *
 * Nace de la única copia que existía hasta ahora (mod-adaptive-ai, que
 * también pasa a usar ésta), y se extiende a mod-bot-operations y a los
 * módulos que reparten bots, para tener la "duración de pasadas" y los
 * "ticks lentos" sin duplicar el mecanismo módulo a módulo.
 *
 * USO
 *   uint32 const t0 = getMSTime();
 *   ... trabajo de la pasada ...
 *   SlowTick::WarnIfSlow("bot-operations", "ProcessActions", t0);
 *
 * Misma regla ODR que el resto de shared/: copia byte a byte en cada módulo
 * (la pone el instalador desde modules/shared/, ver install_own_modules en
 * lib/utils.sh). Sube SharedAbi::kSharedAbiVersion al tocar esta cabecera.
 */

#ifndef WOTLK_SP_SLOW_TICK_H
#define WOTLK_SP_SLOW_TICK_H

#include "Log.h"
#include "StringFormat.h"
#include "Timer.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>

namespace SlowTick
{
    inline void WarnIfSlow(char const* module, char const* step, uint32 startMs, uint32 thresholdMs = 100)
    {
        uint32 const ms = GetMSTimeDiffToNow(startMs);
        if (ms < thresholdMs)
            return;

        // Clave "modulo/paso": el mapa es una sola instancia para todo el
        // proceso (regla ODR de shared/), y así los pasos de módulos
        // distintos con el mismo nombre no se pisan el uno al otro.
        static std::map<std::string, std::pair<uint32, uint32>> s_slow;   // (ultimo aviso, peor desde entonces)
        std::string const key = std::string(module) + "/" + step;
        uint32 const now = getMSTime();
        auto& [lastMs, worst] = s_slow[key];
        worst = std::max(worst, ms);
        if (lastMs && now - lastMs < 60000)
            return;

        LOG_WARN("module", "[{}] Tick lento: {} ha tardado {} ms{}.", module, step, ms,
                 worst > ms ? Acore::StringFormat(" (peor del ultimo minuto: {} ms)", worst) : std::string());
        lastMs = now;
        worst = 0;
    }

    // Aviso de "esta cola trajo de golpe más peticiones de la cuenta" (26/09/2026): mismo patrón umbral + una vez por (módulo,cola)
    // y minuto que WarnIfSlow, pero sobre la PROFUNDIDAD de una cola en vez de
    // una duración. Las colas `g_pending`/`g_qbCmd`/etc. se vacían enteras en
    // cada `OnUpdate` mientras los hilos de mapa están parados (regla 5 de
    // modules/README.md), así que no pueden arrastrar nada de una pasada a la
    // siguiente: lo único que importa es si UNA pasada llega a acumular una
    // ráfaga grande. Verlo sin tener que contar a mano en un `.reload` o una
    // sesión de depuración.
    inline void WarnIfDeep(char const* module, char const* queue, size_t size, size_t thresholdSize = 20)
    {
        if (size < thresholdSize)
            return;

        static std::map<std::string, std::pair<uint32, size_t>> s_deep;   // (ultimo aviso, peor desde entonces)
        std::string const key = std::string(module) + "/" + queue;
        uint32 const now = getMSTime();
        auto& [lastMs, worst] = s_deep[key];
        worst = std::max(worst, size);
        if (lastMs && now - lastMs < 60000)
            return;

        LOG_WARN("module", "[{}] Cola profunda: {} traia {} peticiones de golpe{}.", module, queue, size,
                 worst > size ? Acore::StringFormat(" (peor del ultimo minuto: {})", worst) : std::string());
        lastMs = now;
        worst = 0;
    }
}

#endif // WOTLK_SP_SLOW_TICK_H
