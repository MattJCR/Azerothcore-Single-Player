// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-home-guild — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_home_guildScripts() a partir del nombre
 * de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

#include "Log.h"
#include "SharedAbi.h"


void AddSC_mod_home_guild();

void Addmod_home_guildScripts()
{
    LOG_INFO("module", "[home-guild] cabeceras shared/ ABI v{}", SharedAbi::kSharedAbiVersion);
    AddSC_mod_home_guild();
}
