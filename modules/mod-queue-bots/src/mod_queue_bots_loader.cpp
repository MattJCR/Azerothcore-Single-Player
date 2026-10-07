// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-queue-bots — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_queue_botsScripts() a partir del nombre
 * de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

#include "Log.h"
#include "SharedAbi.h"


void AddSC_mod_queue_bots();

void Addmod_queue_botsScripts()
{
    LOG_INFO("module", "[queue-bots] cabeceras shared/ ABI v{}", SharedAbi::kSharedAbiVersion);
    AddSC_mod_queue_bots();
}
