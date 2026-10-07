// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-standby (ver ModLocale.h).
 * La clave es el literal español tal como está en mod_standby.cpp. El aviso de
 * cuenta atrás lo manda el core (ShutdownServ) y ya sale en el idioma del cliente.
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_STANDBY_LOCALE_H
#define WOTLK_SP_STANDBY_LOCALE_H

#include "ModLocale.h"

namespace StandbyLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "Modo en espera: {}", "Standby mode: {}" },
        { "activado", "enabled" },
        { "DESACTIVADO", "DISABLED" },
        { "  Activacion de socket heredada: {}{}", "  Inherited socket activation: {}{}" },
        { "si", "yes" },
        { "no", "no" },
        { " -> el modulo NO apagara el servidor", " -> the module will NOT shut the server down" },
        { "  Ventana de inactividad: {} min | aviso: {} s | margen tras arranque: {} min",
          "  Idle window: {} min | warning: {} s | margin after startup: {} min" },
        { "  Sesiones humanas (ultimo recuento): {}", "  Human sessions (last count): {}" },
        { "  Apagado EN CURSO: quedan {} s ({}{})", "  Shutdown IN PROGRESS: {} s left ({}{})" },
        { "modo en espera, ", "standby mode, " },
        { "  No se dormira: hay jugadores conectados.", "  It will not go to sleep: there are players online." },
        { "  Estado indeterminado (aun no se ha hecho la primera comprobacion).", "  Undetermined state (the first check has not been done yet)." },
        { "  Se dormira en ~{} min si no entra nadie.", "  It will go to sleep in ~{} min if nobody comes in." },
        { "  Suspendido por '.standby mantener': {} min restantes.", "  Suspended by '.standby mantener': {} min left." },
        { "El apagado por inactividad en curso pasa a ser manual: quedan {} s.", "The idle shutdown in progress becomes manual: {} s left." },
        { "Ya hay un apagado manual del modo en espera en curso.", "A manual standby shutdown is already in progress." },
        { "Ya hay un apagado en curso que no es del modo en espera; no se toca.", "A shutdown that is not from standby mode is already in progress; it is left alone." },
        { "Modo en espera: apagando el worldserver en {} s.", "Standby mode: shutting the worldserver down in {} s." },
        { "Uso: .standby mantener <minutos> (1-1440).", "Usage: .standby mantener <minutes> (1-1440)." },
        { "Apagado del modo en espera cancelado.", "Standby mode shutdown cancelled." },
        { "Hay un apagado en curso que no es del modo en espera; no se cancela.", "A shutdown that is not from standby mode is in progress; it is not cancelled." },
        { "Modo en espera suspendido {} min. '.standby reanudar' lo vuelve a activar.", "Standby mode suspended for {} min. '.standby reanudar' enables it again." },
        { "Modo en espera reanudado.", "Standby mode resumed." },

        // ── Textos que se eligen en tiempo de ejecución (no hay L("literal") en el código) ──
        { "inactividad", "inactivity" },
        { "manual", "manual" },
        { "ajeno", "external" },
    };
}

#endif // WOTLK_SP_STANDBY_LOCALE_H
