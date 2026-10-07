// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-bot-operations — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_bot_operationsScripts() a partir del
 * nombre de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

#include "Log.h"
#include "SharedAbi.h"

void AddSC_mod_bot_operations();

void Addmod_bot_operationsScripts()
{
    LOG_INFO("module", "[bot-operations] cabeceras shared/ ABI v{}", SharedAbi::kSharedAbiVersion);
    AddSC_mod_bot_operations();
}
