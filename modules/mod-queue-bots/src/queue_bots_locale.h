// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-queue-bots (ver ModLocale.h).
 * La clave es el literal español tal como está en mod_queue_bots.cpp.
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_QUEUE_BOTS_LOCALE_H
#define WOTLK_SP_QUEUE_BOTS_LOCALE_H

#include "ModLocale.h"

namespace QueueBotsLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "Se han apuntado {} contrincantes a tu cola ({}/{}).", "{} opponents have joined your queue ({}/{})." },
        { "Los roles elegidos en tu grupo no caben en una mazmorra: cambialos para que se busquen companeros.",
          "The roles chosen in your group do not fit in a dungeon: change them so mates are sought." },
        { "Se han apuntado {} companeros a tu mazmorra ({}/{}).", "{} mates have joined your dungeon ({}/{})." },
        { "Formando banda de {} con los bots disponibles.", "Forming a raid of {} with the available bots." },
        { "mod-queue-bots esta desactivado.", "mod-queue-bots is disabled." },
        { "Se revisara tu cola en la proxima pasada de rellenado.", "Your queue will be checked on the next filling pass." },
        { "No hay bots de este modulo en tu cola.", "There are no bots from this module in your queue." },
        { "Se han sacado {} bots de tu cola (tu sigues en ella).", "{} bots have been removed from your queue (you are still in it)." },
        { "Sin bots de este modulo en cola. Ponte en una cola (mazmorra, banda, "
          "campo de batalla, arena) y se rellenara sola.",
          "No bots from this module in the queue. Join a queue (dungeon, raid, battleground, arena) and it will fill by itself." },
        { "Cola de {}: {}/{} bots ({} tanque, {} sanador, {} dano).", "Queue of {}: {}/{} bots ({} tank, {} healer, {} damage)." },
        { "Faltan {} por entrar. '.queuebots traer' fuerza otra pasada; "
          "'.queuebots salir' saca los bots.",
          "{} still to join. '.queuebots traer' forces another pass; '.queuebots salir' removes the bots." },
        { "Cola completa. '.queuebots salir' saca los bots.", "Queue complete. '.queuebots salir' removes the bots." },
        { "cola", "queue" },
        { "arena 1c1", "1v1 arena" },
        { "campo de batalla", "battleground" },
        { "arena", "arena" },
        { "mazmorra", "dungeon" },
        { "banda", "raid" },
    };
}

#endif // WOTLK_SP_QUEUE_BOTS_LOCALE_H
