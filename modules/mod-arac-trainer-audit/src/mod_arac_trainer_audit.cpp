// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-arac-trainer-audit — verificar en el log lo que aprenden los
 * instructores genéricos de mod-arac, con los hooks nuevos del core.
 *
 * EL PROBLEMA
 * mod-arac abre cada clase a todas las razas y expone un instructor genérico
 * por clase (creature_template 26324-26332, ver patches/arac/04) para que
 * cualquier combinación raza/clase tenga dónde entrenar. El core sólo
 * comprueba la CLASE del instructor, nunca la raza (memoria del proyecto), y
 * el primer intento de completar el árbol de hechizos de ARAC fue
 * PlayerStart.CustomSpells (playercreateinfo_spell_custom aplicado entero al
 * crear personaje): se revirtió porque esa tabla mezcla sin nivel hechizos
 * iniciales con habilidades de nivel alto (docs: playerstart-customspells-
 * peligroso). El arreglo real, caso por caso, es dar el hechizo que falte
 * como fila normal de trainer_spell del instructor genérico (así se hizo
 * para Domar Bestia, patches/arac/05-tame-beast-trainer.sql).
 *
 * Se investigó generalizar ese parche por datos, comparando
 * playercreateinfo_spell_custom contra trainer_spell: de las ~200 filas de
 * ARAC sin fila de entrenador, la inmensa mayoría son pasivos, raciales,
 * idiomas y hechizos internos (DND) que NUNCA deben poder comprarse en un
 * entrenador. Automatizarlo habría añadido cientos de filas sin sentido
 * (ver CHANGELOG, 20/09/2026). Se descarta esa vía.
 *
 * LO QUE SÍ HACE ESTE MÓDULO
 * Usa dos hooks nuevos de PlayerScript (core 20260918, #27552):
 *
 *   - OnPlayerAfterTrainSpell: deja constancia en el log de cada compra en un
 *     entrenador (raza, clase, id de entrenador, hechizo). Es la forma más
 *     barata de comprobar en la VM, sin jugar cada combinación raza/clase a
 *     mano, que los entrenadores genéricos de ARAC siguen funcionando tras
 *     esta actualización del core (verificación pendiente).
 *   - OnPlayerCanLearnSpell: red de seguridad. Si algún día una fila de
 *     trainer_spell queda mal puesta (entrenador equivocado, copia manual sin
 *     revisar raza/clase), este hook veta el aprendizaje y avisa con WARN en
 *     vez de dejar pasar un hechizo que no encaja. El core ya filtra esto
 *     dentro de Trainer::GetSpellState antes de comprar, así que en el camino
 *     normal esto no debería disparar nunca; es defensa en profundidad, no
 *     una función nueva.
 *
 * No añade ningún hechizo, no toca trainer_spell ni SQL. Compila sin
 * mod-arac ni mod-playerbots (no depende de ningún símbolo ajeno).
 * También puede reenviar la lista tras comprar en el instructor 26324 como
 * mitigación del bloqueo Feral de una compra por apertura. Ver README.md:
 * la causa exacta y la eficacia en juego siguen pendientes de confirmación.
 */

#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

namespace
{
    struct Config
    {
        bool enabled          = true;
        bool logPurchases     = true;
        bool guardClassRaceFit = true;
        bool refreshDruidTrainerList = false;
    };

    Config cfg;
}

class mod_arac_trainer_audit_world : public WorldScript
{
public:
    mod_arac_trainer_audit_world() : WorldScript("mod_arac_trainer_audit_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        cfg.enabled           = sConfigMgr->GetOption<bool>("AracTrainerAudit.Enable", true);
        cfg.logPurchases      = sConfigMgr->GetOption<bool>("AracTrainerAudit.LogTrainerPurchases", true);
        cfg.guardClassRaceFit = sConfigMgr->GetOption<bool>("AracTrainerAudit.GuardClassRaceFit", true);
        cfg.refreshDruidTrainerList = sConfigMgr->GetOption<bool>("AracTrainerAudit.RefreshDruidTrainerList", false);
    }
};

class mod_arac_trainer_audit_player : public PlayerScript
{
public:
    mod_arac_trainer_audit_player() : PlayerScript("mod_arac_trainer_audit_player",
        { PLAYERHOOK_CAN_LEARN_SPELL, PLAYERHOOK_ON_AFTER_TRAIN_SPELL }) { }

    bool OnPlayerCanLearnSpell(Player* player, uint32 spellId) override
    {
        if (!cfg.enabled || !cfg.guardClassRaceFit || !player)
            return true;

        // Defensa en profundidad: el core ya descarta esto en
        // Trainer::GetSpellState antes de dejar comprar, así que este aviso
        // sólo debería verse si algo (una fila de trainer_spell mal puesta,
        // u otro camino de aprendizaje distinto del entrenador) se saltó ese
        // filtro.
        if (!player->IsSpellFitByClassAndRace(spellId))
        {
            LOG_WARN("module", "[arac-trainer-audit] {} (raza {}, clase {}) intentó aprender el hechizo {}, "
                     "que no encaja con su raza/clase: bloqueado.",
                     player->GetName(), player->getRace(), player->getClass(), spellId);
            return false;
        }

        return true;
    }

    void OnPlayerAfterTrainSpell(Player* player, Creature* trainer, uint32 spellId) override
    {
        if (!cfg.enabled || !player)
            return;

        if (cfg.logPurchases)
            LOG_INFO("module", "[arac-trainer-audit] {} (raza {}, clase {}) aprendió el hechizo {} del entrenador {}.",
                     player->GetName(), player->getRace(), player->getClass(), spellId,
                     trainer ? trainer->GetEntry() : 0);

        // Compatibilidad con el instructor genérico de druida de ARAC (patches/arac/04).
        // Caso observado: una compra Feral por apertura; cerrar/abrir recupera otra.
        // El cliente recalcula los estados tras aprender y BuyTrainerService no
        // envía nada si su fila interna ya no está disponible. La causa concreta
        // de la discrepancia no está confirmada: reenviar la lista es una mitigación.
        // Este hook corre DESPUÉS de SMSG_TRAINER_BUY_SUCCEEDED. Usar el recorrido
        // normal conserva clase, requisitos, descuento, idioma y filtro de era;
        // no aprende, no cobra y no vuelve a disparar OnPlayerAfterTrainSpell.
        if (cfg.refreshDruidTrainerList && trainer && trainer->GetEntry() == 26324)
            if (WorldSession* session = player->GetSession())
                session->SendTrainerList(trainer);
    }
};

void AddSC_mod_arac_trainer_audit()
{
    new mod_arac_trainer_audit_world();
    new mod_arac_trainer_audit_player();
}
