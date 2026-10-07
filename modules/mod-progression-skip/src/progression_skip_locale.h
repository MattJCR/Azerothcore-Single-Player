// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes y del menú del Cronista de las Eras
 * (ver ModLocale.h). La clave es el literal español tal como está en
 * mod_progression_skip.cpp. tests/test_modlocale.py comprueba que no falte ni
 * sobre ninguna. El nombre del NPC y sus textos de diálogo no pasan por aquí:
 * van en el SQL (creature_template / npc_text con sus filas _locale).
 */

#ifndef WOTLK_SP_PROGRESSION_SKIP_LOCALE_H
#define WOTLK_SP_PROGRESSION_SKIP_LOCALE_H

#include "ModLocale.h"

namespace ProgressionSkipLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "Vuelve cuando estes con vida.", "Come back when you are alive." },
        { "No mientras estas en combate.", "Not while you are in combat." },
        { "No dentro de un campo de batalla ni de una arena.", "Not inside a battleground or an arena." },
        { "No dentro de una mazmorra ni de una banda. Sal al exterior.", "Not inside a dungeon or a raid. Step outside." },
        { "Sal del grupo antes de dar este paso: tus companeros podrian "
          "quedar en una etapa incompatible.",
          "Leave the group before taking this step: your mates could end up in an incompatible stage." },
        { "Cronista de las Eras: {}", "Chronicler of the Ages: {}" },
        { "La progresion por eras no esta activa en este servidor.", "Progression by eras is not active on this server." },
        { "Saltar Vanilla: dar por terminadas las fases de Vanilla y abrir Terrallende.",
          "Skip Vanilla: mark the Vanilla phases as finished and open Outland." },
        { "Saltar Terrallende: dar por terminadas las fases de Terrallende y abrir Rasganorte.",
          "Skip Outland: mark the Outland phases as finished and open Northrend." },
        { "Desactivar todas las etapas: desbloquear todo el contenido de Rasganorte.",
          "Disable all stages: unlock all the Northrend content." },
        { "Cronista de las Eras: el servidor limita la progresion a la etapa {}; no puedo adelantarte mas alla.",
          "Chronicler of the Ages: the server limits progression to stage {}; I cannot take you any further." },
        { "Si: saltar Vanilla. Es permanente.", "Yes: skip Vanilla. It is permanent." },
        { "Si: saltar Terrallende. Es permanente.", "Yes: skip Outland. It is permanent." },
        { "Si: desactivar todas las etapas. Es permanente.", "Yes: disable all stages. It is permanent." },
        { "No, dejalo estar.", "No, leave it." },
        { "Ese salto ya no corresponde a tu estado actual.", "That skip no longer matches your current state." },
        { "El servidor no permite avanzar mas alla de esa etapa.", "The server does not allow advancing beyond that stage." },
        { "Cronista de las Eras: hecho. Tu progresion queda en la etapa {}.", "Chronicler of the Ages: done. Your progression is now at stage {}." },
        { "Cronista de las Eras: el servidor no ha permitido el salto. Sigues en la etapa {}.",
          "Chronicler of the Ages: the server did not allow the skip. You remain at stage {}." },

        // ── Textos que se eligen en tiempo de ejecución (no hay L("literal") en el código) ──
        { "Esta decision es permanente para este personaje. No recibiras las "
          "recompensas ni los logros de las etapas omitidas. Deseas continuar?",
          "This decision is permanent for this character. You will not receive the rewards or achievements of the skipped stages. Do you want to continue?" },
    };
}

#endif // WOTLK_SP_PROGRESSION_SKIP_LOCALE_H
