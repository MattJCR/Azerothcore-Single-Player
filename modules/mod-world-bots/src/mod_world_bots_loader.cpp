// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-world-bots — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_world_botsScripts() a partir del nombre
 * de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

#include "Log.h"
#include "SharedAbi.h"


void AddSC_mod_world_bots();
void AddSC_mod_world_bots_pvp();

void Addmod_world_botsScripts()
{
    LOG_INFO("module", "[world-bots] cabeceras shared/ ABI v{}", SharedAbi::kSharedAbiVersion);
    AddSC_mod_world_bots();
    AddSC_mod_world_bots_pvp();
}
