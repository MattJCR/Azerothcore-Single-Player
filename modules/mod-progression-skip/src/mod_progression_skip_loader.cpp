// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-progression-skip — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_progression_skipScripts() a partir del
 * nombre de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

void AddSC_mod_progression_skip();

void Addmod_progression_skipScripts()
{
    AddSC_mod_progression_skip();
}
