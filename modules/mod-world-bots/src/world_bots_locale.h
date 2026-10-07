// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-world-bots (ver ModLocale.h):
 * los avisos a los jugadores (ayuda samaritana, guerra de mundo) y las salidas
 * de los comandos de GM (.wbots, .wpvp, .bots). La clave es el literal español
 * tal como está en mod_world_bots.cpp / mod_world_bots_pvp.cpp.
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_WORLD_BOTS_LOCALE_H
#define WOTLK_SP_WORLD_BOTS_LOCALE_H

#include "ModLocale.h"

namespace WorldBotsLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        // ── Avisos a los jugadores ──
        { "Un aventurero acude a ayudarte.", "An adventurer comes to help you." },
        { "Han llegado {} aventureros a la zona.", "{} adventurers have arrived in the zone." },
        { "|cffff8800[Duelos]|r Unos aventureros se retan a las puertas de {}.", "|cffff8800[Duels]|r Some adventurers challenge each other at the gates of {}." },
        { "|cffff8800[Guerra]|r {} marcha sobre {}. {} responde.", "|cffff8800[War]|r {} marches on {}. {} responds." },
        { "La Alianza", "The Alliance" },
        { "La Horda", "The Horde" },
        { "|cffff8800[Guerra]|r {} se impone en {} ({}-{}).", "|cffff8800[War]|r {} prevails in {} ({}-{})." },
        { "|cffff8800[Guerra]|r La escaramuza en {} se disuelve.", "|cffff8800[War]|r The skirmish in {} dissolves." },
        { "|cffff8800[Guerra]|r {} controla {} ({}-{}).", "|cffff8800[War]|r {} controls {} ({}-{})." },

        // ── .wbots ──
        { "[world-bots] No recibiras ayuda de bots samaritanos (hasta el reinicio).", "[world-bots] You will not receive help from samaritan bots (until the restart)." },
        { "[world-bots] Volveras a recibir ayuda de bots samaritanos cuando estes en apuros.", "[world-bots] You will receive help from samaritan bots again when you are in trouble." },
        { "[world-bots] El modo samaritano esta desactivado en el servidor (WorldBots.Samaritan = 0).", "[world-bots] Samaritan mode is disabled on the server (WorldBots.Samaritan = 0)." },
        { "[world-bots] No estas en el mundo ahora mismo.", "[world-bots] You are not in the world right now." },
        { "[world-bots] Relleno forzado de {}: objetivo {} bots.", "[world-bots] Forced fill of {}: target {} bots." },
        { "tu zona", "your zone" },
        { "[world-bots] {}.", "[world-bots] {}." },
        { "etapas: apagado (WorldBots.Stage.Enable)", "stages: off (WorldBots.Stage.Enable)" },
        { "etapa activa {} ({}): tope {}, mapas {}; desde el arranque {} re-aleatorizados, {} sacados de mapas bloqueados",
          "active stage {} ({}): cap {}, maps {}; since startup {} re-rolled, {} moved out of blocked maps" },
        { "sin jugadores", "no players" },
        { "todos", "all" },
        { "[world-bots] Desde el arranque: {} pasadas, {} rellenados de zona, "
          "{} bots reubicados, {} despertados, {} ayudas samaritanas.",
          "[world-bots] Since startup: {} passes, {} zone fills, {} bots relocated, {} woken, {} samaritan helps." },
        { "[world-bots] Samaritano: activo, {} ayudante(s) ahora mismo.", "[world-bots] Samaritan: active, {} helper(s) right now." },
        { "[world-bots] Ninguna zona con jugador en seguimiento.", "[world-bots] No zone with a tracked player." },
        { "  {} en {}: objetivo {} bots{}{}.", "  {} in {}: target {} bots{}{}." },
        { "zona ", "zone " },
        { ", capital", ", capital" },
        { ", repone ya", ", refills now" },
        { "[world-bots] El detalle por zona necesita un personaje en el juego.", "[world-bots] The per-zone detail needs a character in the game." },
        { "Este comando necesita un personaje en el juego.", "This command needs a character in the game." },
        { "mod-world-bots esta desactivado.", "mod-world-bots is disabled." },
        { "Uso: .wbots samaritano on|off", "Usage: .wbots samaritano on|off" },

        // ── .bots ──
        { "[bots] Global: {} online + {} pendientes / {} max; capacidad global ahora: {} (foto de hace {} s).",
          "[bots] Global: {} online + {} pending / {} max; global capacity now: {} (snapshot from {} s ago)." },
        { "[bots] Presupuesto pendiente: {}/{} total, Alianza {}/{}, Horda {}/{} (0 = sin limite intermedio).",
          "[bots] Pending budget: {}/{} total, Alliance {}/{}, Horde {}/{} (0 = no intermediate limit)." },
        { "[bots] Online por faccion y tramo:", "[bots] Online by faction and range:" },
        { "  Alianza: {}; Horda: {}.", "  Alliance: {}; Horde: {}." },
        { "  Tramos: ninguno.", "  Ranges: none." },
        { "  Nivel {}-{}: {} online.", "  Level {}-{}: {} online." },
        { "[bots] Pendientes por modulo:", "[bots] Pending by module:" },
        { "  Ninguno.", "  None." },
        { "  {}: {}.", "  {}: {}." },
        { "[bots] Pendientes por faccion/tramo (limite por tramo: {}):", "[bots] Pending by faction/range (limit per range: {}):" },
        { "  Nivel {}-{}: Alianza {}/{}, Horda {}/{} ({} total).", "  Level {}-{}: Alliance {}/{}, Horde {}/{} ({} total)." },
        { "[bots] Claims: {} totales; {} bots de hermandad preferentes.", "[bots] Claims: {} total; {} preferred guild bots." },
        { "  Claims por modulo: ninguno.", "  Claims per module: none." },
        { "  Claim {}: {}.", "  Claim {}: {}." },
        { "[bots] Reequipado: {} en cola ({} en recuperacion); hechos {}, sin cambios {}, "
          "cancelados {}, aplazados {}, caducados {}, fallos {}, recuperados {}, abandonados {}, sin sitio {}.",
          "[bots] Re-gearing: {} queued ({} recovering); done {}, unchanged {}, "
          "cancelled {}, postponed {}, expired {}, failures {}, recovered {}, abandoned {}, no room {}." },
        { "[bots] Ultimos rechazos (mas reciente primero):", "[bots] Latest rejections (most recent first):" },
        { "  hace {} s: {} / {} / {} / nivel {}-{}.", "  {} s ago: {} / {} / {} / level {}-{}." },

        // ── .wpvp ──
        { "Guerra de mundo: {} eventos activos, {} puntos calientes ({} activos).", "World war: {} active events, {} hotspots ({} active)." },
        { "  #{} {} ({}) {} vs {}: {} + {} bots, control {}-{}, {} min restantes{}", "  #{} {} ({}) {} vs {}: {} + {} bots, control {}-{}, {} min left{}" },
        { "duelos", "duels" },
        { "escaramuza", "skirmish" },
        { ", terminando", ", ending" },
        { "  #{} {} [{}] {}: {} vs {}, nivel {}-{}, zona {}, {}-{} min, peso {}{}", "  #{} {} [{}] {}: {} vs {}, level {}-{}, zone {}, {}-{} min, weight {}{}" },
        { ", enfriando", ", cooling down" },
        { "No hay ningun punto caliente llamado '{}'. Usa .wpvp lista.", "There is no hotspot called '{}'. Use .wpvp lista." },
        { "Evento #{} arrancado en {}.", "Event #{} started in {}." },
        { "No arranca en {}: {}.", "It does not start in {}: {}." },
        { "{} evento(s) terminando: los bots vuelven a casa.", "{} event(s) ending: the bots go back home." },
        { "Puntos calientes recargados: {}.", "Hotspots reloaded: {}." },
        { "La guerra de mundo de mod-world-bots esta desactivada (WorldBots.Pvp.Enable).", "The world war of mod-world-bots is disabled (WorldBots.Pvp.Enable)." },

        // ── Textos que se eligen en tiempo de ejecución (no hay L("literal") en el código) ──
        { "Alianza", "Alliance" },
        { "Horda", "Horde" },
        { "la Alianza", "the Alliance" },
        { "la Horda", "the Horde" },
        { "ambas", "both" },
        { "GUID ya pendiente", "GUID already pending" },
        { "GUID ya online", "GUID already online" },
        { "limite global", "global limit" },
        { "presupuesto pendiente global", "global pending budget" },
        { "presupuesto de faccion", "faction budget" },
        { "presupuesto de tramo", "range budget" },
        { "ninguno", "none" },
        { "no se pudo colocar a nadie sin que un jugador lo viera aparecer", "nobody could be placed without a player seeing them appear" },
    };
}

#endif // WOTLK_SP_WORLD_BOTS_LOCALE_H
