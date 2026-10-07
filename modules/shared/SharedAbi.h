// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * SharedAbi.h — versión ABI ÚNICA de todas las cabeceras de modules/shared/.
 *
 * EL PROBLEMA
 * BotClaims.h y BotOperations.h llevaban cada uno su propio
 * kSharedAbiVersion, pensados para avisar de lo mismo (un despliegue parcial,
 * p.ej. `install.sh --only 3/4/5` a medias, deja dos módulos con copias de
 * shared/ distintas) pero contados por separado: cambiar BotWake.h,
 * BotWorldAge.h, BotGear.h, BotPopulationCoordinator.h, TimeMs.h o
 * BotEligibility.h (que no declaraban ninguno de los dos) no subía ningún
 * contador, y BotClaims.h y BotOperations.h podían evolucionar cada uno a su
 * ritmo sin que el otro se enterase — dos números que no dicen lo mismo
 * mirando el mismo Server.log, y ninguno de los dos cubría en realidad TODA
 * la carpeta shared/.
 *
 * LA SOLUCIÓN
 * Una única constante para TODAS las cabeceras de shared/, no solo las dos
 * que llevaban su propio contador. Se sube al tocar CUALQUIERA de ellas
 * (BotClaims.h, BotEligibility.h, BotGear.h, BotOperations.h,
 * BotPopulationCoordinator.h, BotWake.h, BotWorldAge.h, TimeMs.h). Los seis
 * `*_loader.cpp` que la registran en Server.log al cargar leen
 * `SharedAbi::kSharedAbiVersion`: si un despliegue parcial deja un módulo con
 * la copia vieja, el número que reporta ese módulo difiere de los demás y la
 * deriva se ve ahí (regla 6 de modules/README.md).
 *
 * Misma regla ODR que el resto de shared/: copia byte a byte en cada módulo
 * (la pone el instalador desde modules/shared/, ver install_own_modules en
 * lib/utils.sh). Se edita solo modules/shared/SharedAbi.h.
 */

#ifndef WOTLK_SP_SHARED_ABI_H
#define WOTLK_SP_SHARED_ABI_H

#include <cstdint>

namespace SharedAbi
{
    inline constexpr uint32_t kSharedAbiVersion = 8;
}

#endif // WOTLK_SP_SHARED_ABI_H
