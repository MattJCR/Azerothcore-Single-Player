// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-arac-trainer-audit — punto de entrada.
 *
 * El núcleo genera la llamada a Addmod_arac_trainer_auditScripts() a partir
 * del nombre de la carpeta del módulo (los guiones pasan a ser guiones
 * bajos).
 */

void AddSC_mod_arac_trainer_audit();
void AddSC_mod_arac_trainer_trace();

void Addmod_arac_trainer_auditScripts()
{
    AddSC_mod_arac_trainer_audit();
    AddSC_mod_arac_trainer_trace();
}
