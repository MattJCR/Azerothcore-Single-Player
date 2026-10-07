// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * WorldBotsPolicy.h -- decisiones de población de zona de mod-world-bots,
 * separadas de la ejecución.
 *
 * Mismo patrón que modules/mod-queue-bots/src/QueueBotsPolicy.h: structs y
 * funciones libres `constexpr` sobre enteros/booleanos planos, sin Player*,
 * sin WorldDatabase, sin ningún include del core, probadas con
 * `static_assert` en tiempo de compilación. `mod_world_bots.cpp` sigue
 * leyendo Player* / AreaTableEntry* y recorriendo `snapshot.bots` (eso SÍ
 * necesita el core), pero la aritmética de "cuántos hacen falta, de qué
 * facción, con qué tramo de nivel" vive aquí.
 *
 * `ResolveZoneFactionSplit` recibe `desiredTheirs` ya calculado por el
 * llamador con `std::lround(target * oppositeShare)`: `std::lround` no es
 * `constexpr` en C++17, así que el redondeo se queda en la parte impura en
 * vez de forzar esta cabecera a dejar de serlo.
 *
 * No sustituye a shared/: esto es específico de mod-world-bots, no una
 * cabecera compartida entre módulos, así que no lleva versión de SharedAbi.
 */

#ifndef WOTLK_SP_WORLD_BOTS_POLICY_H
#define WOTLK_SP_WORLD_BOTS_POLICY_H

#include <algorithm>
#include <cstdint>

namespace WorldBotsPolicy
{
    // Tramo de nivel admitido en una zona (FillZone). En una capital vale
    // cualquier nivel: maxLevel ya viene topado por la etapa activa
    // (StageCap()) antes de llegar aquí. Fuera de capital,
    // [nivel-levelBelow, nivel+levelAbove], nunca por debajo de 1 ni por
    // encima de maxLevel.
    struct ZoneLevelRange
    {
        uint32_t minLevel;
        uint32_t maxLevel;
    };

    constexpr ZoneLevelRange ResolveZoneLevelRange(uint32_t level, uint32_t levelBelow, uint32_t levelAbove, uint32_t maxLevel, bool city)
    {
        if (city)
            return { 1u, maxLevel };
        return { level > levelBelow ? level - levelBelow : 1u, std::min(level + levelAbove, maxLevel) };
    }

    static_assert(ResolveZoneLevelRange(10, 5, 3, 80, false).minLevel == 5 && ResolveZoneLevelRange(10, 5, 3, 80, false).maxLevel == 13);
    // Nivel por debajo de levelBelow: el mínimo se queda en 1, no en negativo.
    static_assert(ResolveZoneLevelRange(3, 5, 3, 80, false).minLevel == 1 && ResolveZoneLevelRange(3, 5, 3, 80, false).maxLevel == 6);
    // Capital: cualquier nivel hasta el tope de etapa, da igual el nivel del jugador.
    static_assert(ResolveZoneLevelRange(10, 5, 3, 70, true).minLevel == 1 && ResolveZoneLevelRange(10, 5, 3, 70, true).maxLevel == 70);
    // El máximo nunca supera maxLevel (tope de etapa), aunque nivel+levelAbove lo superaría.
    static_assert(ResolveZoneLevelRange(78, 5, 5, 80, false).maxLevel == 80);

    // Cuántos hacen falta esta pasada: lo que falta para el objetivo, topado
    // por cuántos se teletransportan por pasada y por cuántos teletransportes
    // quedan. El llamador ya garantiza have < target (FillZone corta antes,
    // con "zona llena", si have >= target); el `target > have ? ... : 0`
    // sólo es una guarda defensiva, no cambia ningún caso real.
    constexpr uint32_t MissingZoneBots(uint32_t target, uint32_t have, uint32_t maxPerPass, uint32_t teleportsLeft)
    {
        return std::min({ target > have ? target - have : 0u, maxPerPass, teleportsLeft });
    }

    static_assert(MissingZoneBots(25, 20, 5, 10) == 5);
    static_assert(MissingZoneBots(25, 20, 3, 10) == 3);
    static_assert(MissingZoneBots(25, 20, 5, 2) == 2);
    static_assert(MissingZoneBots(25, 25, 5, 10) == 0);

    // Reparto por facción: en zona de una sola facción, todos van a esa
    // (aunque no sea la del jugador: en Elwynn hay humanos). En zona
    // contestada, una parte de la facción contraria para que se note que lo
    // es; en zona neutral sin marcar contestada no se pide nada — mismo
    // comportamiento que el `if/else if` original, que no tiene rama `else`.
    struct ZoneFactionSplit
    {
        uint32_t wantMine;
        uint32_t wantTheirs;
    };

    constexpr ZoneFactionSplit ResolveZoneFactionSplit(bool zoneTeamIsMine, bool zoneTeamIsTheirs, bool contested,
                                                         uint32_t missing, uint32_t desiredTheirs, uint32_t haveTheirs)
    {
        if (zoneTeamIsMine)
            return { missing, 0u };
        if (zoneTeamIsTheirs)
            return { 0u, missing };
        if (contested)
        {
            uint32_t const wantTheirs = desiredTheirs > haveTheirs ? std::min(missing, desiredTheirs - haveTheirs) : 0u;
            return { missing - wantTheirs, wantTheirs };
        }
        return { 0u, 0u };
    }

    static_assert(ResolveZoneFactionSplit(true, false, false, 10, 0, 0).wantMine == 10 && ResolveZoneFactionSplit(true, false, false, 10, 0, 0).wantTheirs == 0);
    static_assert(ResolveZoneFactionSplit(false, true, false, 10, 0, 0).wantMine == 0 && ResolveZoneFactionSplit(false, true, false, 10, 0, 0).wantTheirs == 10);
    // Neutral sin contestar: ninguna facción admitida por la zona pide nada aquí.
    static_assert(ResolveZoneFactionSplit(false, false, false, 10, 0, 0).wantMine == 0 && ResolveZoneFactionSplit(false, false, false, 10, 0, 0).wantTheirs == 0);
    // Contestada: desiredTheirs=7 (20*0.35 redondeado por el llamador), sin ninguno todavía.
    static_assert(ResolveZoneFactionSplit(false, false, true, 10, 7, 0).wantTheirs == 7 && ResolveZoneFactionSplit(false, false, true, 10, 7, 0).wantMine == 3);
    // Ya hay tantos o más de la contraria que el objetivo: no se pide ninguno más de ella.
    static_assert(ResolveZoneFactionSplit(false, false, true, 10, 7, 7).wantTheirs == 0 && ResolveZoneFactionSplit(false, false, true, 10, 7, 7).wantMine == 10);
    // desiredTheirs por encima de lo que falta esta pasada: se topa a missing.
    static_assert(ResolveZoneFactionSplit(false, false, true, 5, 20, 0).wantTheirs == 5 && ResolveZoneFactionSplit(false, false, true, 5, 20, 0).wantMine == 0);

    // Objetivo de bots de una zona con densidad adaptativa activa (ZoneTarget,
    // sin densidad es urand(lo,hi), que se queda en la parte impura porque es
    // aleatorio). Punto medio estable del rango más un extra por cada humano
    // de más en la zona (nunca por debajo de 1 humano), topado por
    // adaptiveMax o, si no está fijado, el doble del máximo del rango.
    constexpr uint32_t AdaptiveZoneTarget(uint32_t lo, uint32_t hi, uint32_t humansInZone, uint32_t perHuman, uint32_t adaptiveMax)
    {
        uint32_t const base = (lo + hi) / 2;
        uint32_t const target = base + (humansInZone > 1 ? (humansInZone - 1) * perHuman : 0u);
        uint32_t const cap = adaptiveMax ? adaptiveMax : hi * 2;
        return std::min(target, cap);
    }

    static_assert(AdaptiveZoneTarget(12, 25, 1, 4, 0) == 18);
    static_assert(AdaptiveZoneTarget(12, 25, 4, 4, 0) == 30);
    // Tope duro por debajo de lo que daría la fórmula: gana el tope.
    static_assert(AdaptiveZoneTarget(12, 25, 4, 4, 20) == 20);
    // adaptiveMax=0: el tope es 2x el máximo del rango, no "sin tope".
    static_assert(AdaptiveZoneTarget(12, 25, 100, 4, 0) == 50);

    // Población objetivo para un número dado de humanos conectados (nunca por
    // debajo de 1 humano: si esto se llama es porque hay al menos uno),
    // topada por el límite duro configurado.
    constexpr uint32_t PlayerScaleTarget(uint32_t humanCount, uint32_t perPlayer, uint32_t ceiling)
    {
        uint64_t const target = static_cast<uint64_t>(perPlayer) * (humanCount > 1u ? humanCount : 1u);
        return static_cast<uint32_t>(std::min<uint64_t>(target, ceiling));
    }

    static_assert(PlayerScaleTarget(1, 150, 600) == 150);
    static_assert(PlayerScaleTarget(5, 150, 600) == 600);
    static_assert(PlayerScaleTarget(0, 150, 600) == 150);
    static_assert(PlayerScaleTarget(3, 150, 600) == 450);

    // ── Guerra de mundo (mod_world_bots_pvp.cpp): ronda 5 de C4 (16/09/2026).
    // StartEvent/TickEvent/BeginEnd manejan Player*/PvpTeam/WorldDatabase de
    // verdad (eso sí necesita el core), pero tres decisiones son aritmética
    // plana sobre enteros: si hay bastantes bots para arrancar, cómo avanza
    // el marcador de control y quién va por delante. Esta última se repetía
    // igual en TickEvent (aviso de progreso) y en BeginEnd (ganador final);
    // vive aquí una sola vez.

    // ¿Hay bastantes bots libres para arrancar la escaramuza? Cada bando
    // necesita lo que StartEvent sorteó (wantAttackers/wantDefenders), pero
    // topado por minBotsToStart: no hace falta llenar el cupo entero para
    // que sea jugable, sólo el mínimo.
    constexpr bool EnoughBotsToStart(uint32_t gotAttackers, uint32_t wantAttackers,
                                      uint32_t gotDefenders, uint32_t wantDefenders, uint32_t minBotsToStart)
    {
        return gotAttackers >= std::min(wantAttackers, minBotsToStart)
            && gotDefenders >= std::min(wantDefenders, minBotsToStart);
    }

    static_assert(EnoughBotsToStart(5, 5, 5, 5, 3));
    // Menos de lo pedido pero por encima del mínimo jugable: basta.
    static_assert(EnoughBotsToStart(3, 5, 3, 5, 3));
    // Por debajo del mínimo jugable en un bando: no arranca.
    static_assert(!EnoughBotsToStart(2, 5, 5, 5, 3));
    static_assert(!EnoughBotsToStart(5, 5, 2, 5, 3));
    // wantAttackers ya por debajo de minBotsToStart (sorteo corto): manda el sorteo.
    static_assert(EnoughBotsToStart(2, 2, 5, 5, 3));

    // Marcador de control (TickEvent, cada ~15 s): el bando con más bots
    // vivos cerca del punto suma un tick; en empate no se mueve nada, igual
    // que el `if/else if` original sin rama `else`.
    struct CaptureScore
    {
        uint32_t attackers;
        uint32_t defenders;
    };

    constexpr CaptureScore AdvanceCaptureScore(uint32_t scoreAttackers, uint32_t scoreDefenders,
                                                uint32_t nearAttackers, uint32_t nearDefenders)
    {
        if (nearAttackers > nearDefenders)      ++scoreAttackers;
        else if (nearDefenders > nearAttackers) ++scoreDefenders;
        return { scoreAttackers, scoreDefenders };
    }

    static_assert(AdvanceCaptureScore(0, 0, 3, 1).attackers == 1 && AdvanceCaptureScore(0, 0, 3, 1).defenders == 0);
    static_assert(AdvanceCaptureScore(0, 0, 1, 3).attackers == 0 && AdvanceCaptureScore(0, 0, 1, 3).defenders == 1);
    // Empate en presencia: el marcador no se mueve.
    static_assert(AdvanceCaptureScore(2, 2, 1, 1).attackers == 2 && AdvanceCaptureScore(2, 2, 1, 1).defenders == 2);
    static_assert(AdvanceCaptureScore(2, 2, 0, 0).attackers == 2 && AdvanceCaptureScore(2, 2, 0, 0).defenders == 2);

    // ¿Alguno de los dos ha llegado al objetivo? (BeginEnd corta el evento).
    constexpr bool CaptureGoalReached(uint32_t scoreAttackers, uint32_t scoreDefenders, uint32_t goal)
    {
        return scoreAttackers >= goal || scoreDefenders >= goal;
    }

    static_assert(!CaptureGoalReached(3, 3, 5));
    static_assert(CaptureGoalReached(5, 3, 5));
    static_assert(CaptureGoalReached(3, 5, 5));

    // ¿Quién va por delante? Sólo se llama con marcador desempatado (los dos
    // sitios que lo usan comprueban `scoreAtt != scoreDef` antes); en empate
    // devuelve false por convención, sin que eso se use nunca en ese caso.
    constexpr bool AttackersLead(uint32_t scoreAttackers, uint32_t scoreDefenders)
    {
        return scoreAttackers > scoreDefenders;
    }

    static_assert(AttackersLead(5, 3));
    static_assert(!AttackersLead(3, 5));
}

#endif // WOTLK_SP_WORLD_BOTS_POLICY_H
