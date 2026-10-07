// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-standby — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_standbyScripts() a partir del nombre de
 * la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

void AddSC_mod_standby();

void Addmod_standbyScripts()
{
    AddSC_mod_standby();
}
