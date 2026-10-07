// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-quest-mates — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_quest_matesScripts() a partir del nombre
 * de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

#include "Log.h"
#include "SharedAbi.h"


void AddSC_mod_quest_mates();

void Addmod_quest_matesScripts()
{
    LOG_INFO("module", "[quest-mates] cabeceras shared/ ABI v{}", SharedAbi::kSharedAbiVersion);
    AddSC_mod_quest_mates();
}
