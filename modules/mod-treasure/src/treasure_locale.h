// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * Traducción al inglés de los mensajes de mod-treasure (ver ModLocale.h).
 * La clave es el literal español tal como está en mod_treasure.cpp. Son salidas
 * de los comandos de GM (.tesoro); los nombres de los cofres van en el SQL.
 * tests/test_modlocale.py comprueba que no falte ni sobre ninguna.
 */

#ifndef WOTLK_SP_TREASURE_LOCALE_H
#define WOTLK_SP_TREASURE_LOCALE_H

#include "ModLocale.h"

namespace TreasureLocale
{
    inline constexpr ModLocale::Entry kEntries[] =
    {
        { "El modulo debe estar activo y el personaje humano conectado.", "The module must be active and the human character online." },
        { "Calidad o zona sin cofre configurado (calidades: 1 basico, 2 raro, 3 epico).", "Quality or zone without a configured chest (qualities: 1 basic, 2 rare, 3 epic)." },
        { "Ya hay doce cofres de prueba para este personaje; retiralos primero.", "There are already twelve test chests for this character; remove them first." },
        { "Cofre de prueba {} (calidad {}, botin zona {}) frente a {}. Se retira en {} s o con .tesoro prueba retirar {}.",
          "Test chest {} (quality {}, zone loot {}) in front of {}. It is removed in {} s or with .tesoro prueba retirar {}." },
        { "No hay suelo accesible y libre frente al personaje; muévelo a un espacio abierto.", "There is no accessible, free ground in front of the character; move them to an open space." },
        { "Retirados {} cofres de prueba de {}.", "Removed {} test chests of {}." },
        { "Uso: .tesoro prueba equipo <personaje> <tramo 1 intermedio / 2 final> <variante 1 / 2>.",
          "Usage: .tesoro prueba equipo <character> <tier 1 intermediate / 2 final> <variant 1 / 2>." },
        { "Cofre de equipo {} (tramo {}, variante {}) frente a {}. Se retira en {} s o con .tesoro prueba retirar {}.",
          "Gear chest {} (tier {}, variant {}) in front of {}. It is removed in {} s or with .tesoro prueba retirar {}." },
        { "No hay suelo accesible y libre frente al personaje; muevelo a un espacio abierto.", "There is no accessible, free ground in front of the character; move them to an open space." },
        { "Esta zona no tiene tesoros configurados.", "This zone has no treasures configured." },
        { "Tesoros zona {}: {}. Puntos aprobados {}/{}. Cuotas basico/raros/epicos: {}/{}/{}.",
          "Zone {} treasures: {}. Approved points {}/{}. Quotas basic/rare/epic: {}/{}/{}." },
        { "activos", "active" },
        { "inactivos", "inactive" },
        { "Ultimo veto a bot: interaccion GUID {}, botin GUID {}.", "Last veto on a bot: interaction GUID {}, loot GUID {}." },
        { "  calidad {} plaza {}: punto {} anterior {} vence {} repone {} abierto {} pos {} {} {}",
          "  quality {} slot {}: point {} previous {} expires {} respawns {} opened {} pos {} {} {}" },
        { "Esta zona no tiene candidatos.", "This zone has no candidates." },
        { "Tesoros: {} puntos examinados; {} aprobados desde su posicion.", "Treasures: {} points examined; {} approved from their position." },
        { "Faltan puntos accesibles: {} aprobados, {} separados, {} necesarios.", "Accessible points are missing: {} approved, {} apart, {} needed." },
        { "Tesoros activados en la zona {}.", "Treasures enabled in zone {}." },
        { "Tesoros desactivados en la zona {}.", "Treasures disabled in zone {}." },
    };
}

#endif // WOTLK_SP_TREASURE_LOCALE_H
