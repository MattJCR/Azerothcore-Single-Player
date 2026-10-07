// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * BotEligibility.h — criterio compartido de "¿este bot está libre AHORA
 * MISMO para que un módulo lo use?" y de "¿esta sesión es de verdad un
 * jugador humano?" (IsHuman).
 *
 * EL PROBLEMA
 * mod-queue-bots, mod-party-here y mod-world-bots (dos ficheros: el general y
 * el de eventos PvP) llevan cada uno su propia función IsFreeBot con el mismo
 * objetivo, pero mantenidas por separado durante meses han ido divergiendo:
 * mod-queue-bots no comprobaba WorldSession::PlayerLoading() ni
 * Player::IsBeingTeleported(), y no exigía que el bot fuese uno aleatorio de
 * verdad (RandomPlayerbotMgr::IsRandomBot). Con playerbots delante eso deja
 * colar en una cola o un grupo un bot a medio cargar (su IA todavía no está
 * lista), un bot a medio saltar de mapa (se le pierde el rastro a mitad de
 * teletransporte), o el alt/selfbot de un jugador que no es un bot aleatorio
 * y por tanto no es de nadie más que repartir.
 *
 * LA SOLUCIÓN
 * Esta cabecera fija el núcleo de comprobaciones que NINGÚN módulo debe
 * saltarse: en el mundo, sesión de bot ya cargada del todo, vivo, sin
 * combate, sin vuelo, sin teletransporte en curso, sin banda, sin cola de
 * campo de batalla/arena, sin grupo, sin LFG, y no está ahora mismo en una
 * mazmorra ni en un campo de batalla/arena. Cada módulo sigue añadiendo
 * encima lo que le es propio a propósito y no se centraliza aquí: el
 * criterio de propiedad de BotClaims (ByOther vs. cualquier reserva),
 * la antigüedad mínima en el mundo (BotWorldAge, sólo world-bots) o la
 * exclusión de los bots de la hermandad de casa (sólo el evento PvP de
 * world-bots). Tampoco decide "es un bot aleatorio": eso exige comprobar
 * RandomPlayerbotMgr, que sólo existe si mod-playerbots está compilado, y
 * cada módulo ya detecta esa dependencia opcional con su propia macro de
 * compilación (mirar Playerbots.h con __has_include); repetir esa detección
 * aquí no simplificaría nada.
 *
 * USO
 *   if (!BotEligibility::IsAvailable(bot)) return false;
 *   // ... aquí encima: BotClaims, antigüedad, hermandad, IsRandomBot ...
 *
 * Misma regla ODR que el resto de shared/: copia byte a byte en cada módulo
 * (la pone el instalador desde modules/shared/, ver install_own_modules en
 * lib/utils.sh). Sube SharedAbi::kSharedAbiVersion al tocar esta cabecera.
 */

#ifndef WOTLK_SP_BOT_ELIGIBILITY_H
#define WOTLK_SP_BOT_ELIGIBILITY_H

#include "Group.h"
#include "LFGMgr.h"
#include "Map.h"
#include "Player.h"
#include "WorldSession.h"

namespace BotEligibility
{
    // ¿Es una sesión de jugador de verdad (no un bot) y está en el mundo?
    // Copiada, byte a byte idéntica, en mod-home-guild, mod-party-here,
    // mod-quest-mates y los dos ficheros de mod-world-bots antes de esta
    // cabecera: una de las pocas duplicaciones
    // de shared/ que era tan trivial y tan estable que unificarla no tenía
    // ningún riesgo de comportamiento.
    inline bool IsHuman(Player* player)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        return session && !session->IsHeadless() && player->IsInWorld();
    }

    // ¿Vivo, cargado del todo y sin nada en curso que haga una grosería
    // sacarlo de donde está? NO comprueba reservas (BotClaims), antigüedad
    // en el mundo, hermandad de casa, ni si es un bot aleatorio: eso es
    // cosa de quien llama, justo después de esta comprobación.
    inline bool IsAvailable(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        WorldSession* session = bot->GetSession();
        if (!session || !session->IsHeadless() || session->PlayerLoading())
            return false;

        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->IsBeingTeleported())
            return false;

        if (bot->InBattleground() || bot->InBattlegroundQueue() || bot->GetGroup())
            return false;

        if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_NONE)
            return false;

        if (Map* map = bot->GetMap())
            if (map->IsDungeon() || map->IsBattlegroundOrArena())
                return false;

        return true;
    }
}

#endif // WOTLK_SP_BOT_ELIGIBILITY_H
