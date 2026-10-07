// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-party-here (ver ModLocale.h).
 * La clave es el literal español tal como está en mod_party_here.cpp; los {}
 * se conservan (la traducción puede reordenarlos con {0}, {1}...).
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_PARTY_HERE_LOCALE_H
#define WOTLK_SP_PARTY_HERE_LOCALE_H

#include "ModLocale.h"

namespace PartyHereLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "Ya estas en el grupo de otro: no se te forma ninguno.", "You are in someone else's group: none is formed for you." },
        { "Se unen a tu grupo: {} ({}/{}).", "Joining your group: {} ({}/{})." },
        { "{} companero(s) se despiden ({}).", "{} mate(s) are leaving ({})." },
        { "{} companero(s) estan en combate o de viaje: se iran en cuanto terminen.", "{} mate(s) are in combat or travelling: they will leave as soon as they finish." },
        { "No hay {} libre de nivel {}-{}: tu grupo {} sin el. Una mazmorra necesita tanque y sanador; "
          "prueba '.grupo cambia <nombre>' mas tarde o el buscador de mazmorras.",
          "There is no free {} of level {}-{}: your group {} without one. A dungeon needs a tank and a healer; "
          "try '.grupo cambia <name>' later or the dungeon finder." },
        { "Tu grupo esta completo ({}/{}).", "Your group is complete ({}/{})." },
        { "Se cancela la peticion de companeros ({}/{}): {}.", "The request for mates is cancelled ({}/{}): {}." },
        { "Preparando companeros ({}/{}): se estan despertando bots. Te aviso cuando esten (hasta {} s); '.grupo fuera' lo cancela.",
          "Preparing mates ({}/{}): bots are being woken up. I will tell you when they are here (up to {} s); '.grupo fuera' cancels it." },
        { "Se quedan como grupo manual: {} ({}/{}).", "They stay as a manual group: {} ({}/{})." },
        { "{} ya se va en cuanto termine.", "{} is already leaving as soon as they finish." },
        { "{} esta en combate o de viaje: se ira en cuanto termine{}.", "{} is in combat or travelling: they will leave as soon as they finish{}." },
        { "{} se despide{}.", "{} says goodbye{}." },
        { "Se recompone tu grupo de antes ({} miembros).", "Your previous group is being rebuilt ({} members)." },
        { "Peticion de companeros cancelada.", "Request for mates cancelled." },
        { "No tienes companeros de este modulo en el grupo.", "You have no mates from this module in the group." },
        { "Peticion de companeros pendiente cancelada.", "Pending request for mates cancelled." },
        { "No tienes ningun companero llamado '{}'.", "You have no mate called '{}'." },
        { "{} companero(s) {}.", "{} mate(s) {}." },
        { "Se despiden: {}.", "Leaving: {}." },
        { "{} tambien lleva tu mision \"{}\".", "{} is also carrying your quest \"{}\"." },
        { "Sin companeros de este modulo. Usa .grupo, .grupo N o .grupo banda [10|25|40].",
          "No mates from this module. Use .grupo, .grupo N or .grupo banda [10|25|40]." },
        { "Preparando companeros ({}): {}/{}; se deja de esperar en {} s.", "Preparing mates ({}): {}/{}; it stops waiting in {} s." },
        { "  {} (nivel {}, {}{}{}{})", "  {} (level {}, {}{}{}{})" },
        { "mod-party-here esta desactivado.", "mod-party-here is disabled." },
        { "No se forma grupo dentro de un campo de batalla, una arena o su cola.", "No group is formed inside a battleground, an arena or their queue." },
        { "Estas en el buscador: de esa cola se ocupa mod-queue-bots.", "You are in the finder: mod-queue-bots takes care of that queue." },
        { "Uso: .grupo cambia <nombre del companero>", "Usage: .grupo cambia <mate's name>" },
        { "No tienes companeros automaticos que conservar.", "You have no automatic mates to keep." },
        { "'{}' no es un companero automatico que conservar.", "'{}' is not an automatic mate to keep." },
        { " y vendra otro", " and another will come" },
        { ": se busca otro companero", ": another mate is being sought" },
        { "se quedan quietos", "stay put" },
        { "vuelven a seguirte", "follow you again" },
        { ", por mision de grupo", ", for a group quest" },
        { ", quieto", ", staying put" },
        { ", se cambia al terminar el combate", ", will be swapped when combat ends" },
        { ", se va al terminar el combate", ", will leave when combat ends" },
        { "se completa", "is completed" },
        { "se queda", "stays" },

        // ── Textos que se eligen en tiempo de ejecución (no hay L("literal") en el código) ──
        { "tanque", "tank" },
        { "sanador", "healer" },
        { "dano", "damage" },
        { "tanque ni sanador", "tank or healer" },
        { "mision de grupo", "group quest" },
        { "grupo de antes", "previous group" },
        { "cambio de companero", "mate swap" },
        { "orden .grupo", ".grupo command" },
        { "estas en el grupo de otro", "you are in someone else's group" },
        { "has dejado el grupo", "you have left the group" },
        { "estas en una cola o en un campo de batalla", "you are in a queue or a battleground" },
        { "la mision de grupo ya no esta en tu diario", "the group quest is no longer in your log" },
        { "no han llegado companeros en {} s", "no mates arrived within {} s" },
        { "a peticion tuya", "at your request" },
        { "has cambiado de zona", "you changed zone" },
        { "mision terminada", "quest finished" },
    };
}

#endif // WOTLK_SP_PARTY_HERE_LOCALE_H
