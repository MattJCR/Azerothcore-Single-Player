// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * QueueBotsPolicy.h -- decisiones de selección/presupuesto de mod-queue-bots,
 * separadas de la ejecución.
 *
 * EL PROBLEMA
 * mod_queue_bots.cpp mezcla en el mismo sitio "cuánto/qué hace falta" (una
 * cuenta con enteros y booleanos) con "cómo conseguirlo" (recorrer el grupo,
 * llamar a PlayerbotAI, tocar WorldDatabase, invocar CollectBots/WakeBots).
 * Lo primero es una función pura, fácil de equivocarse (el reparto de
 * tanque/sanador/daño de una banda ya tuvo bugs reales) y trivial de probar
 * sin compilar el worldserver; lo segundo necesita el core entero.
 *
 * LA SOLUCIÓN
 * Esta cabecera sólo tiene structs y funciones libres `constexpr` sobre
 * enteros/booleanos planos — sin Player*, sin WorldDatabase, sin ningún
 * include del core. `mod_queue_bots.cpp` sigue calculando `haveTanks`,
 * `haveHealers`, etc. recorriendo el grupo (eso SÍ necesita el core), pero
 * la aritmética de "cuánto falta" vive aquí y se puede probar con
 * `static_assert` en tiempo de compilación, igual que ya hacía
 * `MissingArenaPlayers` (movida aquí sin cambios, era el mismo patrón).
 *
 * No sustituye a shared/: esto es específico de mod-queue-bots, no una
 * cabecera compartida entre módulos, así que no lleva versión de SharedAbi.
 */

#ifndef WOTLK_SP_QUEUE_BOTS_POLICY_H
#define WOTLK_SP_QUEUE_BOTS_POLICY_H

#include <algorithm>
#include <cstdint>

namespace QueueBotsPolicy
{
    struct ArenaNeeds
    {
        uint32_t mine;
        uint32_t theirs;
    };

    constexpr ArenaNeeds MissingArenaPlayers(uint32_t size, uint32_t haveMine, uint32_t haveTheirs)
    {
        return { size > haveMine ? size - haveMine : 0,
                 size > haveTheirs ? size - haveTheirs : 0 };
    }

    static_assert(MissingArenaPlayers(1, 1, 0).mine == 0 && MissingArenaPlayers(1, 1, 0).theirs == 1);
    static_assert(MissingArenaPlayers(2, 1, 0).mine == 1 && MissingArenaPlayers(2, 1, 0).theirs == 2);
    static_assert(MissingArenaPlayers(2, 2, 0).mine == 0 && MissingArenaPlayers(2, 2, 0).theirs == 2);
    static_assert(MissingArenaPlayers(3, 1, 0).mine == 2 && MissingArenaPlayers(3, 1, 0).theirs == 3);
    static_assert(MissingArenaPlayers(3, 2, 0).mine == 1 && MissingArenaPlayers(3, 2, 0).theirs == 3);
    static_assert(MissingArenaPlayers(3, 3, 0).mine == 0 && MissingArenaPlayers(3, 3, 0).theirs == 3);
    static_assert(MissingArenaPlayers(5, 1, 0).mine == 4 && MissingArenaPlayers(5, 1, 0).theirs == 5);
    static_assert(MissingArenaPlayers(5, 3, 0).mine == 2 && MissingArenaPlayers(5, 3, 0).theirs == 5);
    static_assert(MissingArenaPlayers(5, 5, 0).mine == 0 && MissingArenaPlayers(5, 5, 0).theirs == 5);
    static_assert(MissingArenaPlayers(5, 5, 5).mine == 0 && MissingArenaPlayers(5, 5, 5).theirs == 0);

    // Presupuesto de una banda: cuántos tanques/sanadores/daño hacen falta
    // esta pasada y cuántos huecos se llenan de una vez (de tres en tres,
    // para no tardar un minuto en formar una banda de diez). `shouldFill`
    // en false es el mismo "no hay nada que hacer todavía" que antes hacía
    // `return` pronto en FillRaid (banda ya al tope, o por encima de
    // MaxRaidBots) — el llamador conserva esa decisión de reintentar más
    // tarde, aquí sólo se dice SI hace falta intentarlo.
    struct RaidBudget
    {
        bool shouldFill = false;
        uint32_t size = 0;
        uint32_t room = 0;
        uint32_t neededTanks = 0;
        uint32_t neededHealers = 0;
        uint32_t neededDps = 0;
    };

    // configuredSize/configuredTanks/configuredHealers: 0 = "sin fijar en el
    // .conf", igual que cfg.raidSize/raidTanks/raidHealers. is25PlusDifficulty
    // sustituye a player->GetRaidDifficulty(): lo decide el llamador antes de
    // entrar aquí, porque Difficulty es un tipo del core.
    constexpr RaidBudget ComputeRaidBudget(uint32_t configuredSize, bool is25PlusDifficulty,
                                            uint32_t configuredTanks, uint32_t configuredHealers,
                                            uint32_t current, uint32_t maxRaidBots,
                                            uint32_t haveTanks, uint32_t haveHealers)
    {
        RaidBudget budget;
        budget.size = configuredSize ? configuredSize : (is25PlusDifficulty ? 25u : 10u);

        if (current >= budget.size || current > maxRaidBots)
            return budget;   // shouldFill se queda en false: nada que hacer todavía

        // Composición: una banda sin tanques ni sanadores no es una banda. Los
        // números son los que se jugaban de verdad — 2/3 en las de diez, 3/6 en
        // las de veinticinco — y para las de cuarenta los de vanilla, que pedían
        // muchísimo más sanador. Mejor pasarse que quedarse corto: un sanador
        // llevado por la IA rinde menos que uno humano.
        uint32_t const wantTanks = configuredTanks ? configuredTanks
                                                    : (budget.size <= 10 ? 2u : (budget.size <= 25 ? 3u : 5u));
        uint32_t const wantHealers = configuredHealers ? configuredHealers
                                                        : (budget.size <= 10 ? 3u : (budget.size <= 25 ? 6u : 12u));

        // Cuántos de daño CABEN: los que sobran tras reservar tanque y sanador.
        // Sin esta reserva el daño se come la banda antes de que aparezca un
        // tanque libre.
        uint32_t const wantDps = budget.size > wantTanks + wantHealers ? budget.size - wantTanks - wantHealers : 0;
        uint32_t const haveDps = current > haveTanks + haveHealers ? current - haveTanks - haveHealers : 0;

        budget.shouldFill = true;
        budget.room = std::min<uint32_t>(budget.size - current, 3);
        budget.neededTanks = wantTanks > haveTanks ? wantTanks - haveTanks : 0;
        budget.neededHealers = wantHealers > haveHealers ? wantHealers - haveHealers : 0;
        budget.neededDps = wantDps > haveDps ? wantDps - haveDps : 0;
        return budget;
    }

    // ── Pruebas en tiempo de compilación ───────────────────────────────────

    // Banda de 10 sin nada todavía: 2 tanques, 3 sanadores, 5 de daño, hueco
    // de 3 (limitado por el tope de "de tres en tres", no por el tamaño).
    static_assert(ComputeRaidBudget(10, false, 0, 0, 0, 40, 0, 0).shouldFill);
    static_assert(ComputeRaidBudget(10, false, 0, 0, 0, 40, 0, 0).neededTanks == 2);
    static_assert(ComputeRaidBudget(10, false, 0, 0, 0, 40, 0, 0).neededHealers == 3);
    static_assert(ComputeRaidBudget(10, false, 0, 0, 0, 40, 0, 0).neededDps == 5);
    static_assert(ComputeRaidBudget(10, false, 0, 0, 0, 40, 0, 0).room == 3);

    // size=0 (sin fijar en .conf): 10 si la dificultad no es de 25, 25 si lo es.
    static_assert(ComputeRaidBudget(0, false, 0, 0, 0, 40, 0, 0).size == 10);
    static_assert(ComputeRaidBudget(0, true, 0, 0, 0, 40, 0, 0).size == 25);

    // Banda de 25: 3 tanques, 6 sanadores.
    static_assert(ComputeRaidBudget(25, false, 0, 0, 0, 40, 0, 0).neededTanks == 3);
    static_assert(ComputeRaidBudget(25, false, 0, 0, 0, 40, 0, 0).neededHealers == 6);

    // Banda de 40 (vanilla): 5 tanques, 12 sanadores.
    static_assert(ComputeRaidBudget(40, false, 0, 0, 0, 40, 0, 0).neededTanks == 5);
    static_assert(ComputeRaidBudget(40, false, 0, 0, 0, 40, 0, 0).neededHealers == 12);

    // Ya completa (current >= size): no hace falta nada.
    static_assert(!ComputeRaidBudget(10, false, 0, 0, 10, 40, 2, 3).shouldFill);
    // Por encima del tope global de bots de banda: tampoco, aunque falten huecos.
    static_assert(!ComputeRaidBudget(10, false, 0, 0, 5, 4, 1, 1).shouldFill);

    // Ya hay tanque y sanador de sobra: sólo pide daño, y el sobrante de
    // tanque/sanador no "regala" hueco de daño (el diseño original tampoco
    // lo hacía: sólo se resta lo que falta, nunca se acredita lo que sobra).
    static_assert(ComputeRaidBudget(10, false, 0, 0, 5, 40, 4, 4).neededTanks == 0);
    static_assert(ComputeRaidBudget(10, false, 0, 0, 5, 40, 4, 4).neededHealers == 0);

    // Valores fijados en el .conf ganan a la tabla por tamaño.
    static_assert(ComputeRaidBudget(10, false, 4, 4, 0, 40, 0, 0).neededTanks == 4);
    static_assert(ComputeRaidBudget(10, false, 4, 4, 0, 40, 0, 0).neededHealers == 4);

    // El hueco nunca supera lo que falta para llenar la banda, aunque el
    // límite de "tres en tres" sea mayor.
    static_assert(ComputeRaidBudget(10, false, 0, 0, 9, 40, 2, 3).room == 1);

    // ── Mazmorra (FillDungeon): mismo patrón, ronda 2 de C4 (16/09/2026) ────
    //
    // El tramo de nivel de una mazmorra sale de LFGDungeonData; para las
    // aleatorias ese rango puede venir a cero, y entonces se usa
    // [nivel-3, nivel] del jugador (nunca por debajo de 1).
    struct DungeonLevelRange
    {
        uint32_t minLevel;
        uint32_t maxLevel;
    };

    constexpr DungeonLevelRange ResolveDungeonLevelRange(uint32_t dungeonMinLevel, uint32_t dungeonMaxLevel, uint32_t playerLevel)
    {
        if (!dungeonMinLevel || dungeonMaxLevel < dungeonMinLevel)
            return { playerLevel > 3 ? playerLevel - 3 : 1u, playerLevel };
        return { dungeonMinLevel, dungeonMaxLevel };
    }

    static_assert(ResolveDungeonLevelRange(10, 20, 80).minLevel == 10 && ResolveDungeonLevelRange(10, 20, 80).maxLevel == 20);
    static_assert(ResolveDungeonLevelRange(0, 0, 15).minLevel == 12 && ResolveDungeonLevelRange(0, 0, 15).maxLevel == 15);
    static_assert(ResolveDungeonLevelRange(0, 0, 2).minLevel == 1 && ResolveDungeonLevelRange(0, 0, 2).maxLevel == 2);
    // maxLevel < minLevel es tan "sin declarar" como 0: mismo fallback.
    static_assert(ResolveDungeonLevelRange(20, 10, 80).minLevel == 77 && ResolveDungeonLevelRange(20, 10, 80).maxLevel == 80);

    // Reparto de roles de la mazmorra.
    //
    // Antes se restaba un hueco por CADA rol marcado por el jugador: con
    // tanque+daño en un 1/1/3 quedaban sólo 3 bots por buscar (y 2 con los
    // tres roles), así que la cola nunca juntaba cinco participantes. Y un
    // tanque sobrante se contaba como daño sin serlo (ConsumeRole).
    //
    // Ahora cada participante ocupa UNA plaza de un rol que haya marcado. Los
    // fijos (el jugador y sus compañeros de grupo) no se pueden quitar; los
    // bots ya encolados sí (droppable): si tras un cambio de selección no
    // caben, sobran y el llamador los saca de la cola. Entre los repartos
    // válidos gana, por este orden: el que conserva más bots encolados, el
    // que deja menos plazas sin bots libres que las cubran (avail*: bots
    // libres por rol ahora mismo) y, a igualdad, el que da a los primeros
    // participantes el rol más escaso (tanque, luego sanador, luego daño).
    // Los roles son las máscaras de LFG (PLAYER_ROLE_TANK/HEALER/DAMAGE); el
    // .cpp comprueba con static_assert que coinciden.
    constexpr uint8_t ROLE_TANK   = 0x02;
    constexpr uint8_t ROLE_HEALER = 0x04;
    constexpr uint8_t ROLE_DAMAGE = 0x08;
    constexpr uint8_t ROLE_ANY    = ROLE_TANK | ROLE_HEALER | ROLE_DAMAGE;
    constexpr uint32_t MAX_DUNGEON_PARTICIPANTS = 16;

    struct DungeonParticipant
    {
        uint8_t roles = 0;        // máscara ROLE_*; 0 se trata como "cualquiera"
        bool droppable = false;   // bot ya encolado: puede sobrar
    };

    struct DungeonRolePlan
    {
        bool     feasible  = false;   // los fijos caben todos en la composición
        uint8_t  assigned[MAX_DUNGEON_PARTICIPANTS] = {};   // ROLE_* de cada uno; 0 = sobra
        uint32_t tanks     = 0;       // plazas que faltan por cubrir con bots
        uint32_t healers   = 0;
        uint32_t damage    = 0;
        uint32_t kept      = 0;       // bots encolados que se conservan
        uint32_t shortfall = 0;       // plazas sin bots libres suficientes
    };

    namespace detail
    {
        constexpr uint8_t RoleBit(uint32_t r)
        {
            return r == 0 ? ROLE_TANK : (r == 1 ? ROLE_HEALER : ROLE_DAMAGE);
        }

        struct RoleSearch
        {
            DungeonParticipant const* participants = nullptr;
            uint32_t count = 0;
            uint32_t quota[3] = {};
            uint32_t avail[3] = {};
            uint32_t used[3] = {};
            uint8_t  current[MAX_DUNGEON_PARTICIPANTS] = {};
            DungeonRolePlan best;
        };

        constexpr void Evaluate(RoleSearch& s, uint32_t kept)
        {
            uint32_t open[3] = {};
            uint32_t shortfall = 0;
            for (uint32_t r = 0; r < 3; ++r)
            {
                open[r] = s.quota[r] - s.used[r];
                shortfall += open[r] > s.avail[r] ? open[r] - s.avail[r] : 0;
            }

            // Se recorre en orden tanque, sanador, daño, sobra: el primero que
            // se encuentra ya es el preferido a igualdad, así que sólo se
            // sustituye por uno estrictamente mejor.
            bool const better = !s.best.feasible || kept > s.best.kept
                || (kept == s.best.kept && shortfall < s.best.shortfall);
            if (!better)
                return;

            s.best.feasible = true;
            s.best.kept = kept;
            s.best.shortfall = shortfall;
            s.best.tanks = open[0];
            s.best.healers = open[1];
            s.best.damage = open[2];
            for (uint32_t i = 0; i < MAX_DUNGEON_PARTICIPANTS; ++i)
                s.best.assigned[i] = i < s.count ? s.current[i] : 0;
        }

        constexpr void Search(RoleSearch& s, uint32_t index, uint32_t kept)
        {
            if (index == s.count)
            {
                Evaluate(s, kept);
                return;
            }

            DungeonParticipant const& p = s.participants[index];
            uint8_t const mask = (p.roles & ROLE_ANY) ? (p.roles & ROLE_ANY) : ROLE_ANY;
            for (uint32_t r = 0; r < 3; ++r)
            {
                if (!(mask & RoleBit(r)) || s.used[r] >= s.quota[r])
                    continue;
                ++s.used[r];
                s.current[index] = RoleBit(r);
                Search(s, index + 1, kept + (p.droppable ? 1u : 0u));
                --s.used[r];
            }

            if (p.droppable)
            {
                s.current[index] = 0;
                Search(s, index + 1, kept);
            }
        }
    }

    // 'count' se trunca a MAX_DUNGEON_PARTICIPANTS; los que pasen de ahí no
    // tienen plaza (el llamador sólo pone fijos delante de los encolados, y
    // un grupo de mazmorra tiene cinco).
    constexpr DungeonRolePlan PlanDungeonRoles(uint32_t quotaTanks, uint32_t quotaHealers, uint32_t quotaDamage,
                                               DungeonParticipant const* participants, uint32_t count,
                                               uint32_t availTanks, uint32_t availHealers, uint32_t availDamage)
    {
        detail::RoleSearch s;
        s.participants = participants;
        s.count = count < MAX_DUNGEON_PARTICIPANTS ? count : MAX_DUNGEON_PARTICIPANTS;
        s.quota[0] = quotaTanks;  s.quota[1] = quotaHealers;  s.quota[2] = quotaDamage;
        s.avail[0] = availTanks;  s.avail[1] = availHealers;  s.avail[2] = availDamage;
        detail::Search(s, 0, 0);
        return s.best;
    }

    namespace detail
    {
        // Atajo para las pruebas: jugador solo con 'roles', bots de sobra.
        constexpr DungeonRolePlan Solo(uint8_t roles, uint32_t aT = 9, uint32_t aH = 9, uint32_t aD = 9)
        {
            DungeonParticipant const p[1] = { { roles, false } };
            return PlanDungeonRoles(1, 1, 3, p, 1, aT, aH, aD);
        }

        constexpr uint32_t Total(DungeonRolePlan const& plan)
        {
            return plan.tanks + plan.healers + plan.damage;
        }
    }

    // Las siete combinaciones de roles del jugador solo: siempre cuatro bots
    // (antes, 3 con dos roles marcados y 2 con los tres).
    static_assert(detail::Total(detail::Solo(ROLE_TANK)) == 4 && detail::Solo(ROLE_TANK).tanks == 0);
    static_assert(detail::Total(detail::Solo(ROLE_HEALER)) == 4 && detail::Solo(ROLE_HEALER).healers == 0);
    static_assert(detail::Total(detail::Solo(ROLE_DAMAGE)) == 4 && detail::Solo(ROLE_DAMAGE).damage == 2);
    static_assert(detail::Total(detail::Solo(ROLE_TANK | ROLE_HEALER)) == 4);
    static_assert(detail::Total(detail::Solo(ROLE_TANK | ROLE_DAMAGE)) == 4);
    static_assert(detail::Total(detail::Solo(ROLE_HEALER | ROLE_DAMAGE)) == 4);
    static_assert(detail::Total(detail::Solo(ROLE_ANY)) == 4);
    // A igualdad, el rol escaso para el jugador: tanque+daño con bots de sobra -> tanque.
    static_assert(detail::Solo(ROLE_TANK | ROLE_DAMAGE).assigned[0] == ROLE_TANK);
    // Falta de tanques libres: tanque+daño hace de tanque; de daño libre: hace de daño.
    static_assert(detail::Solo(ROLE_TANK | ROLE_DAMAGE, 0, 9, 9).assigned[0] == ROLE_TANK);
    static_assert(detail::Solo(ROLE_TANK | ROLE_DAMAGE, 9, 9, 0).assigned[0] == ROLE_DAMAGE);
    // Falta temporal de sanador: con los tres roles, el jugador cura.
    static_assert(detail::Solo(ROLE_ANY, 9, 0, 9).assigned[0] == ROLE_HEALER);
    // Máscara vacía = cualquiera (el diseño anterior no restaba nada: pedía 5).
    static_assert(detail::Total(detail::Solo(0)) == 4);

    // Grupo parcial: jugador daño + compañero tanque -> faltan 1 sanador y 2 de daño.
    static_assert([] {
        DungeonParticipant const p[2] = { { ROLE_DAMAGE, false }, { ROLE_TANK, false } };
        DungeonRolePlan const plan = PlanDungeonRoles(1, 1, 3, p, 2, 9, 9, 9);
        return plan.feasible && plan.tanks == 0 && plan.healers == 1 && plan.damage == 2;
    }());
    // Dos fijos que sólo tanquean en un 1/1/3: no hay reparto (el núcleo
    // tampoco lo aceptaría); nada que buscar.
    static_assert([] {
        DungeonParticipant const p[2] = { { ROLE_TANK, false }, { ROLE_TANK, false } };
        return !PlanDungeonRoles(1, 1, 3, p, 2, 9, 9, 9).feasible;
    }());
    // Tanque+daño con un bot tanque ya en cola: el jugador pasa a daño y el bot se queda.
    static_assert([] {
        DungeonParticipant const p[2] = { { ROLE_TANK | ROLE_DAMAGE, false }, { ROLE_TANK, true } };
        DungeonRolePlan const plan = PlanDungeonRoles(1, 1, 3, p, 2, 9, 9, 9);
        return plan.kept == 1 && plan.assigned[0] == ROLE_DAMAGE && plan.assigned[1] == ROLE_TANK
            && plan.tanks == 0 && plan.healers == 1 && plan.damage == 2;
    }());
    // Cambio de selección a sólo tanque con un bot tanque encolado: el bot
    // sobra (no se reclasifica como daño) y faltan 1 sanador y 3 de daño.
    static_assert([] {
        DungeonParticipant const p[2] = { { ROLE_TANK, false }, { ROLE_TANK, true } };
        DungeonRolePlan const plan = PlanDungeonRoles(1, 1, 3, p, 2, 9, 9, 9);
        return plan.feasible && plan.kept == 0 && plan.assigned[1] == 0
            && plan.tanks == 0 && plan.healers == 1 && plan.damage == 3;
    }());
    // Grupo ya completo con cuatro bots encolados: nada que buscar, nadie sobra.
    static_assert([] {
        DungeonParticipant const p[5] = { { ROLE_ANY, false }, { ROLE_TANK, true }, { ROLE_HEALER, true },
                                          { ROLE_DAMAGE, true }, { ROLE_DAMAGE, true } };
        DungeonRolePlan const plan = PlanDungeonRoles(1, 1, 3, p, 5, 0, 0, 0);
        return plan.kept == 4 && plan.assigned[0] == ROLE_DAMAGE && detail::Total(plan) == 0;
    }());

    // ── Campo de batalla (FillBattleQueue, camino no-arena): ronda 4 de C4
    // (16/09/2026). El camino de arena ya usaba MissingArenaPlayers desde
    // antes de C4; sólo quedaba sin extraer la aritmética del campo de
    // batalla llano, que hasta ahora vivía como variables sueltas entre el
    // recorrido de `fill.bots` y las llamadas a CollectBots/WakeBots.
    struct BattlegroundNeeds
    {
        uint32_t perTeam;
        uint32_t wantMine;
        uint32_t needMine;
        uint32_t needTheirs;
        uint32_t totalNeeded;
    };

    // perTeam sale de MaxPlayersPerTeam si BgFillToFull está activo, o de
    // MinPlayersPerTeam si no (el diseño original prefiere completar rápido
    // con el mínimo). wantMine descuenta a los humanos ya encolados juntos
    // (queuedWithHuman); needMine/needTheirs descuentan además los bots que
    // ya están en la cola de pasadas anteriores. totalNeeded reproduce el
    // `fill.needed = wantMine + perTeam` original, calculado ANTES de saber
    // cuántos bots propios ya había (por eso no resta haveMine).
    constexpr BattlegroundNeeds ComputeBattlegroundNeeds(uint32_t maxPerTeam, uint32_t minPerTeam, bool fillToFull,
                                                          uint32_t queuedWithHuman, uint32_t haveMine, uint32_t haveTheirs)
    {
        BattlegroundNeeds needs{};
        needs.perTeam = fillToFull ? maxPerTeam : minPerTeam;
        needs.wantMine = needs.perTeam > queuedWithHuman ? needs.perTeam - queuedWithHuman : 0;
        needs.needMine = needs.wantMine > haveMine ? needs.wantMine - haveMine : 0;
        needs.needTheirs = needs.perTeam > haveTheirs ? needs.perTeam - haveTheirs : 0;
        needs.totalNeeded = needs.wantMine + needs.perTeam;
        return needs;
    }

    // BgFillToFull=false: perTeam es el mínimo, no el máximo.
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 1, 0, 0).perTeam == 5);
    static_assert(ComputeBattlegroundNeeds(10, 5, true, 1, 0, 0).perTeam == 10);

    // Humano solo (queuedWithHuman=1) en un campo de 5 por bando: hacen
    // falta 4 más de su bando y 5 del rival.
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 1, 0, 0).wantMine == 4);
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 1, 0, 0).needMine == 4);
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 1, 0, 0).needTheirs == 5);
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 1, 0, 0).totalNeeded == 9);

    // Grupo de 3 humanos: wantMine ya cuenta con ellos.
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 3, 0, 0).wantMine == 2);

    // Bots ya en cola de una pasada anterior: sólo se pide lo que falta.
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 1, 2, 3).needMine == 2);
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 1, 2, 3).needTheirs == 2);

    // Bando propio ya completo (queuedWithHuman >= perTeam): no hace falta
    // ningún bot propio, pero el rival sigue pidiendo el bando entero.
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 5, 0, 0).wantMine == 0);
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 5, 0, 0).needTheirs == 5);

    // Bando rival ya cubierto de sobra: no se pide de más.
    static_assert(ComputeBattlegroundNeeds(10, 5, false, 1, 0, 9).needTheirs == 0);
}

#endif // WOTLK_SP_QUEUE_BOTS_POLICY_H
