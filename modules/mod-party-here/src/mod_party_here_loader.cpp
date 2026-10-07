// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-party-here — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_party_hereScripts() a partir del nombre
 * de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

#include "Log.h"
#include "SharedAbi.h"


void AddSC_mod_party_here();

void Addmod_party_hereScripts()
{
    LOG_INFO("module", "[party-here] cabeceras shared/ ABI v{}", SharedAbi::kSharedAbiVersion);
    AddSC_mod_party_here();
}
