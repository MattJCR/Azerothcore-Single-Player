// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-quest-mates (ver ModLocale.h).
 * La clave es el literal español tal como está en mod_quest_mates.cpp.
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_QUEST_MATES_LOCALE_H
#define WOTLK_SP_QUEST_MATES_LOCALE_H

#include "ModLocale.h"

namespace QuestMatesLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "{} también van a por \"{}\".", "{} are also going for \"{}\"." },
        { "Tus companeros van a entregar la mision.", "Your mates are going to hand in the quest." },
    };
}

#endif // WOTLK_SP_QUEST_MATES_LOCALE_H
