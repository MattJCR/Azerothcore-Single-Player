// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-bot-operations (ver ModLocale.h).
 * La clave es el literal español tal como está en mod_bot_operations.cpp.
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_BOT_OPERATIONS_LOCALE_H
#define WOTLK_SP_BOT_OPERATIONS_LOCALE_H

#include "ModLocale.h"

namespace BotOperationsLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "bot-operations: puente {} (BotOperations.Enable).", "bot-operations: bridge {} (BotOperations.Enable)." },
        { "activo", "active" },
        { "inactivo", "inactive" },
        { "colas: {} pendientes, {} resueltas ({} done, {} failed, {} expired).", "queues: {} pending, {} resolved ({} done, {} failed, {} expired)." },
        { "  pendiente #{} {} param={} edad={}s", "  pending #{} {} param={} age={}s" },
        { "  resuelta #{} {} [{}] edad={}s: {}", "  resolved #{} {} [{}] age={}s: {}" },
    };
}

#endif // WOTLK_SP_BOT_OPERATIONS_LOCALE_H
