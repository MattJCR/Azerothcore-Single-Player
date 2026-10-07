// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-home-guild (ver ModLocale.h).
 * La clave es el literal español tal como está en mod_home_guild.cpp.
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_HOME_GUILD_LOCALE_H
#define WOTLK_SP_HOME_GUILD_LOCALE_H

#include "ModLocale.h"

namespace HomeGuildLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "Se renueva tu hermandad: reclutar y nivelar en la proxima pasada.", "Your guild is being renewed: recruiting and levelling on the next pass." },
        { "No tienes hermandad. mod-home-guild no crea ninguna automáticamente: "
          "funda una en el juego si quieres una hermandad de casa.",
          "You have no guild. mod-home-guild does not create one automatically: found one in the game if you want a home guild." },
        { "Tu hermandad '{}': {} companeros, {} conectados ahora. Escribe .grupo para salir con ellos.",
          "Your guild '{}': {} mates, {} online now. Type .grupo to go out with them." },
        { "No tienes hermandad. Funda una en el juego primero.", "You have no guild. Found one in the game first." },
        { "Solo el lider fundador de la hermandad puede usar esto.", "Only the founding leader of the guild can use this." },
        { "Tu hermandad no es una hermandad de casa. Usa .hermandad activar si eres su fundador"
          "{}.",
          "Your guild is not a home guild. Use .hermandad activar if you are its founder{}." },
        { "Hermandad de casa '{}': {} companeros bot, {} conectados ({} tanques, {} sanadores conocidos).",
          "Home guild '{}': {} bot mates, {} online ({} tanks, {} known healers)." },
        { "  {} (nivel {}, {}){}", "  {} (level {}, {}){}" },
        { "Tu hermandad no es una hermandad de casa.", "Your guild is not a home guild." },
        { "Falta el nombre del companero.", "The mate's name is missing." },
        { "No hay ningun companero '{}' en tu hermandad.", "There is no mate '{}' in your guild." },
        { "Companero {}.", "Mate {}." },
        { "Tu hermandad ya es una hermandad de casa.", "Your guild is already a home guild." },
        { "Hermandad de casa activada: se poblara con companeros de tu nivel.", "Home guild enabled: it will be populated with mates of your level." },
        { "Hermandad de casa desactivada: {} companeros bot expulsados. La hermandad sigue existiendo.",
          "Home guild disabled: {} bot mates kicked out. The guild still exists." },
        { "mod-home-guild esta desactivado.", "mod-home-guild is disabled." },
        { " (o funda una nueva)", " (or found a new one)" },
        { " [fijado]", " [pinned]" },

        // ── Textos que se eligen en tiempo de ejecución (no hay L("literal") en el código) ──
        { "tanque", "tank" },
        { "sanador", "healer" },
        { "dano", "damage" },
        { "fijado: se conecta primero y no se le echa", "pinned: it logs in first and is not kicked out" },
        { "sin marca", "unmarked" },
        { "excluido: se saca de la hermandad y no se le vuelve a reclutar", "excluded: it is removed from the guild and not recruited again" },
    };
}

#endif // WOTLK_SP_HOME_GUILD_LOCALE_H
