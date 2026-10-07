// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-progression-skip — "Cronista de las Eras": salto de progresión individual.
 *
 * EL PROBLEMA
 * mod-individual-progression obliga a cada personaje a recorrer Vanilla -> TBC ->
 * WotLK jugándose el contenido de cada tramo. Un jugador que quiere empezar en
 * TBC, o directamente en WotLK, o que rehace un alt, no tiene forma limpia de
 * adelantar: sólo un GM con ".ip set", que además puede RETROCEDER el estado y
 * borra las misiones ocultas por dentro (ForceUpdateProgressionState).
 *
 * LA SOLUCIÓN
 * Un NPC neutral, opcional, que ofrece exactamente tres saltos IRREVERSIBLES:
 *
 *     Saltar Vanilla        -> etapa 8  (PROGRESSION_PRE_TBC)      si estado < 8
 *     Saltar Terrallende    -> etapa 13 (PROGRESSION_TBC_TIER_5)   si 8 <= estado <= 12
 *     Desactivar las etapas -> etapa 18 (PROGRESSION_WOTLK_TIER_5) si estado < 18
 *
 * No entrega nada: recompensas, logros, reputación y attunements NO se tocan.
 * Sólo desbloquea fases de contenido, reutilizando la API pública del módulo:
 *   - GetPlayerProgressionFromQuests(): leer el estado real (misiones 66000+n).
 *   - UpdateProgressionState(): avanzar. YA es monotónica (rechaza <= actual) y
 *     YA respeta IndividualProgression.ProgressionLimit. Es la segunda barrera;
 *     la primera es la validación de aquí.
 *   - CheckAdjustments() + checkIPPhasing(): refrescar NPCs, objetos y fases al
 *     instante, sin reloguear.
 * NO se usa ForceUpdateProgressionState: ésa borra las misiones ocultas y sirve
 * para administración/sincronización, no para un salto hacia delante.
 *
 * LA DEPENDENCIA CON mod-individual-progression
 * Va entre guardas __has_include (modules/README.md regla 1): si el módulo no
 * está compilado, este NPC sigue enlazando y sólo dice que la función no está
 * disponible; si el upstream cambia la firma de la API, falla la COMPILACIÓN con
 * un error claro, que es mejor que dejar de funcionar en silencio. Es el mismo
 * mecanismo que mod-queue-bots usa con PlayerbotAI.h.
 *
 * DÓNDE SE HACE EL TRABAJO
 * Todo en los hooks de gossip del CreatureScript, en el hilo del mapa del
 * jugador, tocando sólo a ese jugador. No aplica la regla 5 de
 * modules/README.md (WorldScript::OnUpdate) y no hay ningún cerrojo propio.
 *
 * SQL: data/sql/db-world/01-npc-cronista-de-las-eras.sql (plantilla 600200,
 * spawns 8000110-8000113 en Gadgetzan, Bahía del Botín, Shattrath y Dalaran).
 * Config: conf/mod_progression_skip.conf.dist (ProgressionSkip.Enable arranca en 0).
 */

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

#include <string>

#if defined(__has_include)
#  if __has_include("IndividualProgression.h")
#    include "IndividualProgression.h"
#    define PROGRESSION_SKIP_WITH_IP 1
#  endif
#endif

namespace
{
    // Destinos de cada salto (valores del enum ProgressionState del módulo).
    constexpr uint8 SKIP_VANILLA = 8;   // PROGRESSION_PRE_TBC
    constexpr uint8 SKIP_TBC     = 13;  // PROGRESSION_TBC_TIER_5
    constexpr uint8 SKIP_ALL     = 18;  // PROGRESSION_WOTLK_TIER_5
    constexpr uint8 SKIP_MAX     = 18;  // PROGRESSION_WOTLK_TIER_5 (tope absoluto)

    // npc_text del SQL.
    constexpr uint32 TEXT_HELLO    = 600200;
    constexpr uint32 TEXT_CONFIRM  = 600201;
    constexpr uint32 TEXT_NO_STAGE = 600202;

    // Familias de acción: +destino para no solaparse (8/13/18).
    constexpr uint32 ACTION_ASK = GOSSIP_ACTION_INFO_DEF + 100;  // pedir confirmación
    constexpr uint32 ACTION_DO  = GOSSIP_ACTION_INFO_DEF + 200;  // ejecutar
    constexpr uint32 ACTION_BACK = GOSSIP_ACTION_INFO_DEF + 1;   // volver al menú

    constexpr char CONFIRM_POPUP[] =
        "Esta decision es permanente para este personaje. No recibiras las "
        "recompensas ni los logros de las etapas omitidas. Deseas continuar?";

    bool ModuleEnabled()
    {
        return sConfigMgr->GetOption<bool>("ProgressionSkip.Enable", false);
    }

    // Devuelve "" si el personaje puede usar el NPC ahora; si no, el motivo.
    std::string InteractionBlockedReason(Player* player)
    {
        if (!player->IsAlive())
            return "Vuelve cuando estes con vida.";
        if (player->IsInCombat())
            return "No mientras estas en combate.";
        if (player->InBattleground() || player->InArena())
            return "No dentro de un campo de batalla ni de una arena.";

#ifdef PROGRESSION_SKIP_WITH_IP
        if (sIndividualProgression->isPlayerInDungeonOrRaid(player))
            return "No dentro de una mazmorra ni de una banda. Sal al exterior.";
#endif

        if (sConfigMgr->GetOption<bool>("ProgressionSkip.RequireNoGroup", true) && player->GetGroup())
            return "Sal del grupo antes de dar este paso: tus companeros podrian "
                   "quedar en una etapa incompatible.";

        return "";
    }

    void Reject(Player* player, std::string const& reason)
    {
        ChatHandler(player->GetSession()).PSendSysMessage("Cronista de las Eras: {}", reason);
        CloseGossipMenuFor(player);
    }
}

class npc_progression_skip : public CreatureScript
{
public:
    npc_progression_skip() : CreatureScript("npc_progression_skip") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);

        if (!ModuleEnabled())
        {
            SendGossipMenuFor(player, TEXT_NO_STAGE, creature->GetGUID());
            return true;
        }

#ifndef PROGRESSION_SKIP_WITH_IP
        Reject(player, "La progresion por eras no esta activa en este servidor.");
        return true;
#else
        if (!sIndividualProgression->enabled)
        {
            Reject(player, "La progresion por eras no esta activa en este servidor.");
            return true;
        }

        if (std::string reason = InteractionBlockedReason(player); !reason.empty())
        {
            Reject(player, reason);
            return true;
        }

        uint8 const current = sIndividualProgression->GetPlayerProgressionFromQuests(player);
        int const limit = sIndividualProgression->progressionLimit;

        bool anyOffer = false;
        bool cappedSomething = false;

        auto offer = [&](uint8 target, char const* label)
        {
            if (current >= target)
                return;
            if (limit && target > limit)
            {
                cappedSomething = true;
                return;
            }
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, GOSSIP_SENDER_MAIN, ACTION_ASK + target);
            anyOffer = true;
        };

        offer(SKIP_VANILLA, "Saltar Vanilla: dar por terminadas las fases de Vanilla y abrir Terrallende.");
        if (current >= SKIP_VANILLA && current <= 12)
            offer(SKIP_TBC, "Saltar Terrallende: dar por terminadas las fases de Terrallende y abrir Rasganorte.");
        offer(SKIP_ALL, "Desactivar todas las etapas: desbloquear todo el contenido de Rasganorte.");

        if (cappedSomething)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Cronista de las Eras: el servidor limita la progresion a la etapa {}; no puedo adelantarte mas alla.",
                sIndividualProgression->progressionLimit);

        if (!anyOffer)
        {
            SendGossipMenuFor(player, TEXT_NO_STAGE, creature->GetGUID());
            return true;
        }

        SendGossipMenuFor(player, TEXT_HELLO, creature->GetGUID());
        return true;
#endif
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        ClearGossipMenuFor(player);

        if (sender != GOSSIP_SENDER_MAIN)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (action == ACTION_BACK)
            return OnGossipHello(player, creature);

#ifndef PROGRESSION_SKIP_WITH_IP
        Reject(player, "La progresion por eras no esta activa en este servidor.");
        return true;
#else
        if (!ModuleEnabled() || !sIndividualProgression->enabled)
        {
            Reject(player, "La progresion por eras no esta activa en este servidor.");
            return true;
        }

        // Segundo diálogo: confirmación.
        if (action > ACTION_ASK && action <= ACTION_ASK + SKIP_MAX)
        {
            uint8 const target = static_cast<uint8>(action - ACTION_ASK);
            if (std::string reason = InteractionBlockedReason(player); !reason.empty())
            {
                Reject(player, reason);
                return true;
            }

            char const* label =
                target == SKIP_VANILLA ? "Si: saltar Vanilla. Es permanente." :
                target == SKIP_TBC     ? "Si: saltar Terrallende. Es permanente." :
                                         "Si: desactivar todas las etapas. Es permanente.";

            AddGossipItemFor(player, GOSSIP_ICON_CHAT, label, GOSSIP_SENDER_MAIN, ACTION_DO + target,
                             CONFIRM_POPUP, 0, false);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No, dejalo estar.", GOSSIP_SENDER_MAIN, ACTION_BACK);
            SendGossipMenuFor(player, TEXT_CONFIRM, creature->GetGUID());
            return true;
        }

        // Confirmado: revalidar TODO desde cero y aplicar.
        if (action > ACTION_DO && action <= ACTION_DO + SKIP_MAX)
        {
            uint8 const target = static_cast<uint8>(action - ACTION_DO);
            Apply(player, target);
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
#endif
    }

private:
#ifdef PROGRESSION_SKIP_WITH_IP
    static void Apply(Player* player, uint8 target)
    {
        if (std::string reason = InteractionBlockedReason(player); !reason.empty())
        {
            Reject(player, reason);
            return;
        }

        uint32 const current = sIndividualProgression->GetPlayerProgressionFromQuests(player);

        // No se fía del menú: sólo hacia delante, y nunca por encima del tope real.
        if (target <= current || target > SKIP_MAX)
        {
            Reject(player, "Ese salto ya no corresponde a tu estado actual.");
            return;
        }

        int const limit = sIndividualProgression->progressionLimit;
        if (limit && target > limit)
        {
            Reject(player, "El servidor no permite avanzar mas alla de esa etapa.");
            return;
        }

        sIndividualProgression->UpdateProgressionState(player, static_cast<ProgressionState>(target));
        sIndividualProgression->CheckAdjustments(player);
        sIndividualProgression->checkIPPhasing(player, player->GetAreaId());
        player->SaveToDB(false, false);

        uint32 const now = sIndividualProgression->GetPlayerProgressionFromQuests(player);

        // Rastro: es una acción irreversible.
        LOG_INFO("module.progression_skip",
            "[progression-skip] {} (GUID {}, cuenta {}) : etapa {} -> {} (destino pedido {})",
            player->GetName(), player->GetGUID().GetCounter(), player->GetSession()->GetAccountId(),
            current, now, uint32(target));

        if (now > current)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Cronista de las Eras: hecho. Tu progresion queda en la etapa {}.", now);
        else
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Cronista de las Eras: el servidor no ha permitido el salto. Sigues en la etapa {}.", now);

        CloseGossipMenuFor(player);
    }
#endif
};

void AddSC_mod_progression_skip()
{
    new npc_progression_skip();
}
