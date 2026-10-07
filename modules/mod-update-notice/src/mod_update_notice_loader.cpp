// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-update-notice — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_update_noticeScripts() a partir del
 * nombre de la carpeta del módulo (los guiones pasan a ser guiones bajos).
 */

void AddSC_mod_update_notice();

void Addmod_update_noticeScripts()
{
    AddSC_mod_update_notice();
}
