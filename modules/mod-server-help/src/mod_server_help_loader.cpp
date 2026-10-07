// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-server-help — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_server_helpScripts() a partir del nombre
 * de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

void AddSC_mod_server_help();

void Addmod_server_helpScripts()
{
    AddSC_mod_server_help();
}
