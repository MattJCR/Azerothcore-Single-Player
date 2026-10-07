// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-adaptive-ai — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_adaptive_aiScripts() a partir del nombre
 * de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

void AddSC_mod_adaptive_ai();
void AddSC_mod_adaptive_ai_arena();
void AddSC_mod_adaptive_ai_commands();

void Addmod_adaptive_aiScripts()
{
    AddSC_mod_adaptive_ai();
    AddSC_mod_adaptive_ai_arena();
    AddSC_mod_adaptive_ai_commands();
}
