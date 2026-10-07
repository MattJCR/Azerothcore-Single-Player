// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-adaptive-ai — arena de entrenamiento: planificador, creación de arenas
 * y campos de batalla instanciados, ciclo de vida de cada partida, series
 * manuales y calibración.
 *
 * CÓMO SE CREA UNA ARENA SIN COLA (fase 0, verificado en el core el 02/09/2026)
 * Es la misma secuencia que hace BattlegroundQueue al invitar y el jugador al
 * aceptar, sin la cola:
 *   1. sBattlegroundMgr->CreateNewBattleground(tipo, bracket, tamaño, no puntuada)
 *   2. bg->StartBattleground()           lo registra en el gestor
 *   3. bg->IncreaseInvitedCount(equipo)  para que no se borre antes de llegar
 *   4. bot->SetEntryPoint(); bot->SetBattlegroundId(instancia, tipo, ..., equipo)
 *   5. sBattlegroundMgr->SendToBattleground(bot, instancia, tipo)  teletransporta
 *   6. al llegar, el core llama a Battleground::AddPlayer (hook
 *      OnBattlegroundAddPlayer): se compensa el invitado
 *   7. OnBattlegroundSetup: se acorta la preparación (Arena.PrepTime del
 *      worldserver.conf no afecta a estas arenas)
 *   8. Arranque: en los campos de batalla llega OnBattlegroundStart; en las
 *      arenas no (Battleground::_ProcessJoin), así que se mira el estado en
 *      OnBattlegroundUpdate. Arrancan los cerebros de todos los bots.
 *   9. OnBattlegroundEnd: se cierra la partida (recompensa final, Elo, tabla)
 *  10. OnBattlegroundUpdate / planificador: unos segundos después,
 *      LeaveBattleground de cada bot (vuelven a donde estaban); tiempos de
 *      espera y máximos van en el planificador porque el hook de update no
 *      se llama con la arena vacía
 *  11. OnBattlegroundDestroy: se sueltan las reservas (BotClaims) y los grupos
 *
 * EQUIPOS (fase 3-5, 03/09/2026): la misma secuencia con N bots por lado. Los
 * de cada lado van en un grupo del core (banda si son más de cinco) para que
 * playerbots cure y asista a los suyos; el grupo se disuelve al acabar. En
 * las arenas los lados son Alianza/Horda de la arena (oro/verde), da igual la
 * facción del bot. En los campos de batalla (fase 6) el lado ES la facción:
 * se eligen N bots de la Alianza y N de la Horda, y se baja el mínimo de
 * jugadores del campo a N para que el core no lo cierre por falta de gente.
 * Los objetivos (banderas, bases, torres) los juegan las estrategias de
 * campo de batalla de playerbots; el módulo decide el combate y cobra los
 * objetivos leyendo la puntuación del campo cada pocos segundos.
 *
 * Los bots se eligen entre los aleatorios libres (sin grupo, sin cola, sin
 * combate, sin reserva de otro módulo, sin jugador cerca, más de 60 s
 * conectados) de la clase y, si se pide, la especialización. Si
 * AdaptiveAI.Arena.NivelObjeto > 0 se les iguala el equipo con la fábrica de
 * playerbots (lo mismo que "autogear").
 *
 * MODOS DE PARTIDA
 *   entrenar    todos deciden con la candidata y aprenden (autojuego)
 *   contraste   un lado decide con la candidata y aprende; el otro es
 *               playerbots de serie (línea base): la medida de si mejora
 *   mixto       alterna los dos
 *   referencia  los dos lados de serie: la línea base viva con la que se
 *               compara el contraste (AdaptiveAI.Arena.Referencia % de las
 *               partidas automáticas)
 *   calibracion un lado con la candidata, otro con la validada, nadie
 *               aprende: si la candidata gana con margen, pasa a validada
 */

#include "AdaptiveAI.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundScore.h"
#include "AsyncCallbackProcessor.h"
#include "Config.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryCallback.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SpellDefines.h"
#include "StringFormat.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <set>
#include <sstream>
#include <unordered_set>

#ifdef ADAPTIVE_WITH_PLAYERBOTS
#  include "AiFactory.h"
#  include "BotClaims.h"
#  include "PlayerbotAI.h"
#  include "PlayerbotFactory.h"
#  include "Playerbots.h"
#  include "RandomPlayerbotMgr.h"
#endif

// Los contadores de objetivo de BattlegroundScore (banderas, bases, torres)
// son virtuales y protegidos; no hay lectura publica. La instanciacion
// explicita de una plantilla puede nombrar miembros protegidos (C++ lo
// permite a proposito), y con eso se saca un puntero a cada uno sin tocar el
// core. Misma tecnica que mod-dungeon-clear con las listas de playerbots.
namespace
{
    template <typename Tag, typename Tag::type M>
    struct ScoreRob { friend typename Tag::type get(Tag) { return M; } };
    struct Attr1Tag { typedef uint32 (BattlegroundScore::*type)() const; friend type get(Attr1Tag); };
    struct Attr2Tag { typedef uint32 (BattlegroundScore::*type)() const; friend type get(Attr2Tag); };
    struct Attr3Tag { typedef uint32 (BattlegroundScore::*type)() const; friend type get(Attr3Tag); };
    struct Attr4Tag { typedef uint32 (BattlegroundScore::*type)() const; friend type get(Attr4Tag); };
    struct Attr5Tag { typedef uint32 (BattlegroundScore::*type)() const; friend type get(Attr5Tag); };
    template struct ScoreRob<Attr1Tag, &BattlegroundScore::GetAttr1>;
    template struct ScoreRob<Attr2Tag, &BattlegroundScore::GetAttr2>;
    template struct ScoreRob<Attr3Tag, &BattlegroundScore::GetAttr3>;
    template struct ScoreRob<Attr4Tag, &BattlegroundScore::GetAttr4>;
    template struct ScoreRob<Attr5Tag, &BattlegroundScore::GetAttr5>;

    uint32 ObjectiveScore(BattlegroundScore const* sc)
    {
        return (sc->*get(Attr1Tag{}))() + (sc->*get(Attr2Tag{}))() + (sc->*get(Attr3Tag{}))() + (sc->*get(Attr4Tag{}))() + (sc->*get(Attr5Tag{}))();
    }
}

namespace AdaptiveAI
{
    std::string Match::TypeName() const
    {
        if (isBg)
            return std::string("bg:") + BgName(uint8(bgTypeId));
        return std::to_string(size) + "c" + std::to_string(size);
    }

    namespace
    {
        constexpr char const* CLAIM_TAG = "adaptive-ai";

        struct Series
        {
            uint32 id = 0;
            CompSpec comp;
            std::string mode;        // entrenar | contraste | mixto | referencia | calibracion
            MatchSource source = SOURCE_MANUAL;
            uint32 remaining = 0;
            uint32 simultaneous = 1;
            uint32 launched = 0, finished = 0, aborted = 0;
            uint32 wins[2] = {0, 0};   // por lado: 0 = candidata/adaptativa, 1 = el otro
            uint32 draws = 0;
        };

        struct Calibration
        {
            bool   running = false;
            uint32 total = 0, launched = 0, finished = 0;
            uint32 candidateWins = 0, validatedWins = 0, draws = 0;
            uint32 startedMs = 0;
            time_t startedAt = 0;   // hora del sistema
            time_t endsAt = 0;      // cuándo concluye con lo que haya (0 = al completar Combates)
            uint32 seriesId = 0;
            std::map<uint8, std::pair<uint32, uint32>> byClass;   // clase del lado candidata (1c1) -> (gana, pierde)
            std::map<uint8, std::pair<uint32, uint32>> byClassValidated;   // clase del lado validada (1c1) -> (gana, pierde): la misma clase, con la versión anterior
            std::map<std::string, std::pair<uint32, uint32>> byType; // tipo de partida -> (gana, pierde)
        };

        // Recursivo: LeaveBattleground dispara OnBattlegroundRemovePlayerAtLeave
        // en el mismo hilo, y ese hook vuelve a entrar aquí.
        std::recursive_mutex s_lock;                       // partidas, series, calibración
        std::map<uint32, Match> s_matches;                 // por instancia
        std::vector<Series> s_series;
        Calibration s_calib;
        uint32 s_nextMatchId = 1;
        uint32 s_nextSeriesId = 1;
        uint32 s_lastLaunchMs = 0;
        size_t s_seriesCursor = 0;   // por donde sigue el reparto de turnos entre series (ver el bucle de lanzamiento)
        uint32 s_tickAccum = 0;
        uint32 s_calibTimerMs = 0;
        time_t s_lastExamWindow = 0;   // inicio de la última ventana de examen atendida (reloj): una por ventana
        time_t s_lastExamSkipLog = 0;  // ventana cuya espera por decisiones nuevas ya se avisó (no la marca como atendida)
        uint32 s_bgTimerMs = 0;
        uint32 s_updatesAtLastCalib = 0;
        uint32 s_autoPairIndex = 0;
        uint32 s_autoTeamIndex = 0;   // rueda propia de las composiciones de equipo (2c2, 3c3, 5c5)
        uint32 s_bgTypeIndex = 0;

        // Aprobado por clase y rondas (adaptive_clase). Una clase aprobada deja
        // sus arenas a las suspensas; cuando aprueban todas, ronda nueva.
        std::set<uint8> s_approved;
        uint32 s_round = 1;
        struct ClassWL { uint32 cw = 0, cl = 0, vw = 0, vl = 0; };   // con la candidata gana/pierde, con la validada gana/pierde
        // Acumulado por clase desde su último aprobado (o desde la vuelta a lo
        // anterior). Se guarda en adaptive_clase: una calibración de 200
        // combates deja 15-20 partidas por clase y lado, así que hacen falta
        // dos o tres para decidir, y antes el acumulado vivía solo en memoria:
        // cada reinicio lo borraba y con siete despliegues en una tarde no se
        // decidía nunca (03/09/2026).
        std::map<uint8, std::deque<ClassWL>> s_calibAcc;
        std::map<uint8, uint32> s_classCycles;                    // clase -> calibraciones sin aprobar en esta ronda

        // Suma de la ventana: solo los ultimos Calibrar.VentanaCiclos examenes.
        // Sumar sin limite congelaba la nota (el guerrero arrastraba 574
        // partidas de tres examenes y una mejora reciente no movia la media).
        ClassWL WindowSum(uint8 cls)
        {
            ClassWL out;
            auto it = s_calibAcc.find(cls);
            if (it == s_calibAcc.end())
                return out;
            for (ClassWL const& w : it->second)
            {
                out.cw += w.cw; out.cl += w.cl; out.vw += w.vw; out.vl += w.vl;
            }
            return out;
        }

        // La ventana de una clase, en texto, para adaptive_clase.historial
        // ("cw:cl:vw:vl,cw:cl:vw:vl", el mas reciente el ultimo): asi sobrevive
        // a los reinicios, que antes borraban el acumulado entero.
        std::string WindowText(uint8 cls)
        {
            std::string out;
            auto it = s_calibAcc.find(cls);
            if (it == s_calibAcc.end())
                return out;
            for (ClassWL const& w : it->second)
                out += Acore::StringFormat("{}{}:{}:{}:{}", out.empty() ? "" : ",", w.cw, w.cl, w.vw, w.vl);
            return out;
        }

        void WindowPush(uint8 cls, ClassWL const& exam)
        {
            std::deque<ClassWL>& q = s_calibAcc[cls];
            q.push_back(exam);
            while (q.size() > std::max<size_t>(1, cfg.calibrateWindow))
                q.pop_front();
        }
        std::vector<uint32> s_generations;     // versiones que fueron validadas: rivales de la candidata
        uint32 s_generationsRefreshMs = 0;

        // La escalera. Cerrojo propio y corto: la consultan los hilos de mapa
        // en cada combate real, y s_lock lo tiene cogido el hilo del mundo
        // mientras teletransporta bots.
        std::shared_mutex s_ladderLock;
        std::map<uint8, std::vector<LadderRung>> s_ladder;
        // Lo que habia medido antes de este arranque (adaptive_escalera): mientras
        // un peldano no tenga partidas nuevas suficientes, se hereda de aqui. Es
        // lo que hace que la escalera sobreviva a un reinicio y viaje en la
        // exportacion; si no, una instalacion limpia recibe las generaciones pero
        // no sabe cual es mejor y juega de serie hasta volver a medirlas.
        std::map<uint8, std::vector<LadderRung>> s_ladderSaved;
        uint32 s_ladderTimerMs = 8 * 60000;    // la primera medida, a los dos minutos del arranque
        uint32 s_ladderPick = 0;               // rueda de peldaños a medir

        void SaveApproval(uint8 cls, bool approved, uint32 version)
        {
            ClassWL a = WindowSum(cls);
            PlayerbotsDatabase.Execute(
                "REPLACE INTO adaptive_clase (clase, aprobada, ronda, version, fecha, cand_gana, cand_pierde, val_gana, val_pierde, ciclos, historial) "
                "VALUES ({}, {}, {}, {}, '{}', {}, {}, {}, {}, {}, '{}')",
                cls, approved ? 1 : 0, s_round, version, Now(), a.cw, a.cl, a.vw, a.vl, s_classCycles[cls], WindowText(cls));
        }

        void ResetAccumulated(uint8 cls)
        {
            s_calibAcc.erase(cls);
            s_classCycles.erase(cls);
        }

        void RefreshGenerations()
        {
            s_generations = Generations();
            s_generationsRefreshMs = getMSTime();
        }

        // Clases que entrenan de verdad (con catálogo y habilitadas)
        std::vector<uint8> TrainingClasses()
        {
            std::vector<uint8> out;
            for (uint8 cls = 1; cls <= 11; ++cls)
                if (cls != 10 && ClassEnabled(cls) && ActionsFor(cls).size() > 1)
                    out.push_back(cls);
            return out;
        }
        uint32 s_alternate = 0;
        uint32 s_calibArm = 0;        // rueda de los tres brazos del examen (Calibrar.MismoRival)
        // Parejas sin senal del examen, congeladas por ronda (05/09/2026): si se
        // recalculan en cada examen, la mezcla de rivales de una clase cambia
        // entre los dos examenes de la ventana y mueve la nota sin que nada haya
        // cambiado en la clase (esta noche fueron 21, 22, 23 y 16 parejas).
        std::set<std::pair<uint8, uint8>> s_saturated;
        uint32 s_saturatedRound = 0;
        bool   s_saturatedValid = false;
        bool   s_autoEnabled = true;
        std::set<std::string> s_reached;          // emparejamientos 1c1 con el objetivo alcanzado
        std::string s_objectivesText;
        // Las consultas gordas a la base (los objetivos) van al hilo de la base
        // con AsyncQuery y el resultado se recoge en el tick (05/09/2026). Antes
        // eran 180 consultas sincronas cada 10 min, dos por pareja y orientacion,
        // cada una recorriendo adaptive_match entera: el tick tardaba 1,7-3,2 s
        // y crecia con la tabla.
        QueryCallbackProcessor s_dbQueue;
        struct PairWL { uint32 n = 0, wins = 0; };
        std::map<std::pair<uint8, uint8>, PairWL> s_objContrast;   // (yo, rival) -> candidata contra serie, ultimas N
        std::map<std::pair<uint8, uint8>, PairWL> s_objRef;        // (menor, mayor) -> serie contra serie, gana la menor
        uint32 s_objPending = 0;                                   // consultas del objetivo en vuelo
        uint32 s_objectivesTimerMs = 9 * 60000;   // la primera comprobación, al minuto del arranque
        uint32 s_finishedToday = 0;
        uint32 s_abortedToday = 0;

        std::shared_mutex s_trainingLock;
        std::unordered_set<ObjectGuid> s_trainingBots;

        void TrackBot(ObjectGuid guid, bool add)
        {
            std::unique_lock<std::shared_mutex> guard(s_trainingLock);
            if (add) s_trainingBots.insert(guid);
            else     s_trainingBots.erase(guid);
        }

        BattlegroundTypeId ArenaTypeForMap(uint32 mapId)
        {
            switch (mapId)
            {
                case 559: return BATTLEGROUND_NA;
                case 562: return BATTLEGROUND_BE;
                case 572: return BATTLEGROUND_RL;
                case 617: return BATTLEGROUND_DS;
                case 618: return BATTLEGROUND_RV;
                default:  return BATTLEGROUND_TYPE_NONE;
            }
        }

        uint32 PickArenaMap()
        {
            if (cfg.arenaMap && ArenaTypeForMap(cfg.arenaMap) != BATTLEGROUND_TYPE_NONE)
                return cfg.arenaMap;
            static uint32 const maps[] = { 559, 562, 572, 617, 618 };
            return maps[urand(0, 4)];
        }

        int SideOf(Match const& m, ObjectGuid guid)
        {
            for (int s = 0; s < 2; ++s)
                for (Fighter const& f : m.side[s])
                    if (f.guid == guid)
                        return s;
            return -1;
        }

        Fighter* FighterOf(Match& m, ObjectGuid guid)
        {
            for (int s = 0; s < 2; ++s)
                for (Fighter& f : m.side[s])
                    if (f.guid == guid)
                        return &f;
            return nullptr;
        }

        bool AllArrived(Match const& m)
        {
            for (int s = 0; s < 2; ++s)
                for (Fighter const& f : m.side[s])
                    if (!f.arrived)
                        return false;
            return true;
        }

        uint32 ArrivedCount(Match const& m)
        {
            uint32 n = 0;
            for (int s = 0; s < 2; ++s)
                for (Fighter const& f : m.side[s])
                    if (f.arrived)
                        ++n;
            return n;
        }

        std::string SideNames(Match const& m, int s)
        {
            std::string out;
            for (Fighter const& f : m.side[s])
                out += (out.empty() ? "" : "+") + f.name;
            return out;
        }

        Match* FindMatch(Battleground* bg)
        {
            if (!bg)
                return nullptr;
            auto it = s_matches.find(bg->GetInstanceID());
            return it == s_matches.end() ? nullptr : &it->second;
        }

        char const* WinnerName(int8 winner)
        {
            switch (winner)
            {
                case 0:  return "a";
                case 1:  return "b";
                case -1: return "e";
                default: return "x";
            }
        }

#ifdef ADAPTIVE_WITH_PLAYERBOTS
        uint32 WornItemLevel(Player* player)
        {
            uint32 sum = 0, count = 0;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
                    continue;
                if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    sum += item->GetTemplate()->ItemLevel;
                    ++count;
                }
            }
            return count ? sum / count : 0;
        }

        // Algún jugador de verdad conectado (el entrenamiento baja de marcha)
        bool HumanOnline()
        {
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* p = pair.second;
                if (p && p->IsInWorld() && !IsBot(p))
                    return true;
            }
            return false;
        }

        uint32 RunningArenasOfSize(uint8 size)
        {
            uint32 n = 0;
            for (auto const& [id, m] : s_matches)
                if (!m.isBg && m.size == size)
                    ++n;
            return n;
        }

        std::string WithPlayerText()
        {
            std::string out;
            for (auto const& [size, cap] : cfg.arenaWithPlayer)
                if (cap)
                    out += Acore::StringFormat("{}{} {}c{}", out.empty() ? "" : ", ", cap, size, size);
            return out.empty() ? "ninguna arena" : out;
        }

        // Con un jugador conectado, tope por tamaño (AdaptiveAI.Arena.ConJugador)
        bool WithPlayerAllows(uint8 size, bool humanOnline)
        {
            if (!humanOnline || cfg.arenaWithPlayer.empty())
                return true;
            auto it = cfg.arenaWithPlayer.find(size);
            uint32 cap = it == cfg.arenaWithPlayer.end() ? 0 : it->second;
            return RunningArenasOfSize(size) < cap;
        }

        bool HumanInPvpQueue()
        {
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* p = pair.second;
                if (!p || !p->IsInWorld() || IsBot(p))
                    continue;
                if (p->InBattlegroundQueue() || p->InBattleground())
                    return true;
            }
            return false;
        }

        // Un bot libre. cls 0 = cualquier clase habilitada con catálogo;
        // faction -1 = da igual, si no TeamId exacto (campos de batalla).
        Player* FindFreeBot(ClassSpec const& want, uint32 minLevel, uint32 maxLevel, int faction, std::set<ObjectGuid> const& exclude)
        {
            std::vector<Player*> candidates;
            for (auto const& pair : ObjectAccessor::GetPlayers())
            {
                Player* p = pair.second;
                if (!p || !p->IsInWorld() || !p->GetSession() || p->GetSession()->isLogingOut())
                    continue;
                if (p->GetLevel() < minLevel || p->GetLevel() > maxLevel)
                    continue;
                if (want.cls ? p->getClass() != want.cls : (!ClassEnabled(p->getClass()) || ActionsFor(p->getClass()).size() <= 1))
                    continue;
                if (faction >= 0 && int(p->GetTeamId()) != faction)
                    continue;
                if (!sRandomPlayerbotMgr.IsRandomBot(p))
                    continue;
                if (!p->IsAlive() || p->IsInCombat() || p->GetGroup() || p->InBattleground() || p->InBattlegroundQueue() ||
                    p->IsBeingTeleported() || p->IsInFlight() || p->duel)
                    continue;
                // Recién conectado: la secuencia de login de playerbots lo va a
                // teletransportar a su zona y pisaría el viaje a la arena.
                if (GameTime::GetGameTime().count() - p->m_logintime < 60)
                    continue;
                if (exclude.count(p->GetGUID()))
                    continue;
                uint32 low = p->GetGUID().GetCounter();
                if (BotClaims::IsClaimed(low) || BotClaims::IsHomeGuildBot(low))
                    continue;
                if (want.spec >= 0 && AiFactory::GetPlayerSpecTab(p) != uint8(want.spec))
                    continue;
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
                if (!botAI || botAI->HasPlayerNearby(150.0f))
                    continue;
                candidates.push_back(p);
            }
            if (candidates.empty())
                return nullptr;
            return candidates[urand(0, uint32(candidates.size() - 1))];
        }

        void EqualizeGear(Player* bot, bool force = false)
        {
            if (!cfg.arenaGearScore)
                return;
            uint32 worn = WornItemLevel(bot);
            if (!force && worn + cfg.arenaGearTolerance >= cfg.arenaGearScore && worn <= cfg.arenaGearScore + cfg.arenaGearTolerance)
                return;
            uint32 quality = sConfigMgr->GetOption<uint32>("AiPlayerbot.AutoGearQualityLimit", 4);
            bool twoRounds = sConfigMgr->GetOption<bool>("AiPlayerbot.TwoRoundsGearInit", true);
            PlayerbotFactory::DestroyEquippedGear(bot);
            PlayerbotFactory::AutoGear(bot, quality, cfg.arenaGearScore, false, twoRounds);
            LOG_DEBUG("module", "[adaptive-ai] {} reequipado de nivel de objeto {} a ~{}.", bot->GetName(), worn, WornItemLevel(bot));
        }

        // Asigna modo y aprendizaje a cada lado según la regla de la serie.
        // Los bots de clases desactivadas (AdaptiveAI.Clases) juegan de serie.
        void AssignModes(Match& m, std::string const& rule)
        {
            std::string mode = rule;
            if (mode == "mixto")
                mode = (s_alternate & 1) ? "contraste" : "entrenar";
            int adaptiveSide = (s_alternate >> 1) & 1;
            ++s_alternate;
            // 1c1 con una clase aprobada y otra no: la que aprende es la suspensa
            // (la aprobada ya tiene su sitio en la validada; sus partidas son
            // para rescatar a las que van atrás).
            //
            // En el examen NO: el aprobado por clase compara lo que la clase
            // gana con la candidata contra lo que gana con la validada en las
            // mismas parejas. Si la suspensa lleva siempre la candidata contra
            // las aprobadas, como candidata juega el 60 % de sus partidas
            // contra paladín, sacerdote, CdM y mago (las fuertes, con su
            // modelo aprendido) y como validada solo contra suspensas: las dos
            // cifras no miden lo mismo, todas salían "peor" y el 04/09/2026 a
            // las 02:00 el tope de ciclos borró cinco clases por eso. En el
            // examen los lados alternan como siempre; la aprobada, cuando le
            // toca la candidata, juega con sus filas de la candidata (copia de
            // las aprobadas más lo poco que aprende en equipos), y la suspensa
            // se mide con la validada también contra las aprobadas.
            //
            // En la escalera TAMPOCO (05/09/2026): ahí se mide un peldaño (la
            // validada o una generación) contra la serie, y la clase aprobada es
            // justo la que hay que medir. Con esta regla el guerrero aprobado a
            // las 12:00 jugó 0 partidas de escalera con su modelo, no tenía
            // peldaño y en el mundo el módulo no se metía.
            //
            // Y desde el 05/09/2026 (N5) tampoco en el entrenamiento, salvo que
            // Arena.AprobadasEntrenan este a 0: con la regla, las aprobadas
            // dejaban de aprender hasta cerrar la ronda (0 lados con la candidata
            // desde las 14:05) y hacian de SERIE para las suspensas, que luego se
            // examinaban contra su modelo.
            bool oneApproved = false;
            int approvedSide = -1;
            if (m.size == 1 && !m.side[0].empty() && !m.side[1].empty())
            {
                bool a0 = s_approved.count(m.side[0].front().cls) > 0, a1 = s_approved.count(m.side[1].front().cls) > 0;
                oneApproved = a0 != a1;
                approvedSide = a0 ? 0 : (a1 ? 1 : -1);
            }
            if (!cfg.approvedKeepLearning && mode != "calibracion" && mode != "escalera" && mode != "sonda" && oneApproved)
                adaptiveSide = 1 - approvedSide;
            m.rule = mode;

            BrainMode sideMode[2] = { BRAIN_NONE, BRAIN_NONE };
            bool learn[2] = { false, false };
            uint32 version = 0;
            if (mode == "entrenar")
            {
                sideMode[0] = sideMode[1] = BRAIN_CANDIDATE;
                learn[0] = learn[1] = cfg.learn;
            }
            else if (mode == "contraste")
            {
                sideMode[adaptiveSide] = BRAIN_CANDIDATE;
                learn[adaptiveSide] = cfg.learn;
                // La aprobada, cuando le toca el lado no adaptativo, lleva su
                // validada una parte de las veces (Arena.AprobadaConModelo): asi
                // las suspensas entrenan tambien contra lo que se examina. Esas
                // partidas no entran en el contraste ni en la escalera (ningun
                // lado es "ninguna"): el informe las ve como candidata contra
                // validada fuera del examen.
                if (cfg.approvedKeepLearning && oneApproved && approvedSide == 1 - adaptiveSide &&
                    cfg.approvedWithModelPct && urand(1, 100) <= cfg.approvedWithModelPct)
                    sideMode[approvedSide] = BRAIN_VALIDATED;
            }
            else if (mode == "sonda")
            {
                // La candidata SIN explorar contra la serie: el peldano de la
                // candidata en la escalera (N2+N6). Se graba como "sonda" para
                // que ni el contraste (que mide la candidata explorando) ni la
                // escalera la confundan con las partidas de entrenamiento.
                sideMode[adaptiveSide] = BRAIN_CANDIDATE;
                learn[adaptiveSide] = false;
            }
            else if (mode == "calibracion")
            {
                // Mismo rival (05/09/2026): tres brazos en rueda. En los dos
                // primeros un lado lleva la candidata y el otro la validada
                // (alternando el lado), y de ahi sale la nota "con la candidata"
                // de la clase que la lleva. En el tercero los DOS llevan la
                // validada, y de ahi sale la nota "con la validada" de las dos
                // clases. Asi las dos notas de una clase se miden contra el
                // mismo rival (el rival con la validada); antes la segunda
                // salia de partidas donde el rival llevaba la candidata, y el
                // aprobado media "mi mejora mas la de mis rivales".
                if (cfg.calibrateSameRival)
                {
                    uint32 arm = s_calibArm++ % 3;
                    if (arm == 2)
                        sideMode[0] = sideMode[1] = BRAIN_VALIDATED;
                    else
                    {
                        sideMode[arm] = BRAIN_CANDIDATE;
                        sideMode[1 - arm] = BRAIN_VALIDATED;
                    }
                    m.calibArm = int8(arm);
                }
                else
                {
                    sideMode[adaptiveSide] = BRAIN_CANDIDATE;
                    sideMode[1 - adaptiveSide] = BRAIN_VALIDATED;
                    m.calibArm = int8(adaptiveSide);
                }
            }
            else if (mode == "escalera")
            {
                // Un peldaño concreto contra playerbots de serie: la medida que
                // coloca cada generación en su sitio. No se aprende: medir y
                // aprender a la vez cambia lo que se está midiendo.
                std::vector<uint32> rungs = s_generations;
                std::shared_ptr<QTable> v = Validated();
                if (v && std::find(rungs.begin(), rungs.end(), v->Version()) == rungs.end())
                    rungs.push_back(v->Version());
                sideMode[adaptiveSide] = BRAIN_VALIDATED;
                if (!rungs.empty())
                {
                    uint32 pick = rungs[s_ladderPick++ % rungs.size()];
                    if (!v || pick != v->Version())
                    {
                        if (GenerationTable(pick))
                        {
                            sideMode[adaptiveSide] = BRAIN_VERSION;
                            version = pick;
                        }
                    }
                }
            }
            else if (mode == "generacion")
            {
                // La candidata aprende contra una generación anterior al azar;
                // si no hay ninguna cargable, contra playerbots de serie.
                sideMode[adaptiveSide] = BRAIN_CANDIDATE;
                learn[adaptiveSide] = cfg.learn;
                if (!s_generations.empty())
                {
                    version = s_generations[urand(0, uint32(s_generations.size() - 1))];
                    if (GenerationTable(version))
                        sideMode[1 - adaptiveSide] = BRAIN_VERSION;
                    else
                        version = 0;
                }
                if (!version)
                    m.rule = "contraste";
            }
            // referencia: todo de serie
            for (int s = 0; s < 2; ++s)
            {
                m.sideMode[s] = sideMode[s];
                for (Fighter& f : m.side[s])
                {
                    bool enabled = ClassEnabled(f.cls);
                    f.mode = enabled ? sideMode[s] : BRAIN_NONE;
                    f.learn = enabled && learn[s];
                    // La versión con la que decide este lado (0 = serie): sin
                    // ella no se puede saber después qué peldaño jugó, que es
                    // lo que coloca cada generación en la escalera.
                    f.version = 0;
                    if (f.mode == BRAIN_VERSION)        f.version = version;
                    else if (f.mode == BRAIN_CANDIDATE) { if (auto t = Candidate()) f.version = t->Version(); }
                    else if (f.mode == BRAIN_VALIDATED) { if (auto t = Validated()) f.version = t->Version(); }
                    // N1 (05/09/2026): una tabla SIN filas de esta clase no es un
                    // modelo para ella. Hasta hoy el bot decidia igual, y sin
                    // aprender no hay bonus ni ruido: elegia por su personalidad
                    // (huir siempre, la primera defensiva siempre...), una
                    // politica fija que gana el 42 % donde la serie gana el 50 %.
                    // Ese lado juega de serie y se graba como serie (version 0):
                    // asi la nota "con la validada" del examen y los peldanos de
                    // la escalera vuelven a medir lo que dicen. El papel del lado
                    // (m.sideMode, m.calibArm) no cambia: el examen sigue
                    // contando esa partida en el brazo que le toca.
                    if ((f.mode == BRAIN_VALIDATED || f.mode == BRAIN_VERSION) && !f.learn)
                    {
                        std::shared_ptr<QTable> t = TableOfVersion(f.version);
                        if (!t || !t->HasClass(f.cls))
                        {
                            f.mode = BRAIN_NONE;
                            f.version = 0;
                        }
                    }
                }
            }
        }

        // Lo que de verdad juega un bot, para adaptive_match: "sonda" es la
        // candidata sin explorar (el peldano de la candidata en la escalera).
        char const* RecordedMode(Fighter const& f)
        {
            if (f.mode == BRAIN_CANDIDATE && !f.learn)
                return "sonda";
            return ModeName(f.mode);
        }

        void ReleaseFighter(Fighter& f)
        {
            if (f.claimed)
            {
                BotClaims::Release(f.guid.GetCounter(), CLAIM_TAG);
                f.claimed = false;
            }
            TrackBot(f.guid, false);
        }

        void DisbandGroups(Match& m)
        {
            for (int s = 0; s < 2; ++s)
            {
                if (!m.group[s])
                    continue;
                if (Group* g = sGroupMgr->GetGroupByGUID(m.group[s].GetCounter()))
                    g->Disband(true);
                m.group[s] = ObjectGuid::Empty;
            }
        }

        void MakeGroup(Match& m, int s, std::vector<Player*> const& members)
        {
            if (members.size() < 2)
                return;
            Group* group = new Group();
            if (!group->Create(members[0]))
            {
                delete group;
                LOG_WARN("module", "[adaptive-ai] Partida #{}: no se pudo crear el grupo del lado {}.", m.id, s);
                return;
            }
            sGroupMgr->AddGroup(group);
            if (members.size() > 5)
                group->ConvertToRaid();
            for (size_t i = 1; i < members.size(); ++i)
                if (!group->AddMember(members[i]))
                    LOG_WARN("module", "[adaptive-ai] Partida #{}: {} no entra en el grupo.", m.id, members[i]->GetName());
            m.group[s] = group->GetGUID();
        }

        // Llena los dos lados de una partida. Devuelve false (y suelta todo) si
        // falta algún bot o alguna reserva.
        bool PickFighters(Match& m, std::vector<ClassSpec> const& a, std::vector<ClassSpec> const& b,
                          uint32 minLevel, uint32 maxLevel, bool byFaction, std::vector<Player*> (&players)[2], std::string& error)
        {
            std::set<ObjectGuid> taken;
            for (int s = 0; s < 2; ++s)
            {
                std::vector<ClassSpec> const& specs = s == 0 ? a : b;
                int faction = byFaction ? (s == 0 ? int(TEAM_ALLIANCE) : int(TEAM_HORDE)) : -1;
                for (ClassSpec const& want : specs)
                {
                    Player* p = FindFreeBot(want, minLevel, maxLevel, faction, taken);
                    if (!p)
                    {
                        error = Acore::StringFormat("no hay ningun bot libre de {} nivel {}-{}{}", want.Name(), minLevel, maxLevel,
                                                    byFaction ? (s == 0 ? " de la Alianza" : " de la Horda") : "");
                        break;
                    }
                    taken.insert(p->GetGUID());
                    players[s].push_back(p);
                }
                if (players[s].size() != specs.size())
                    break;
            }
            if (players[0].size() != a.size() || players[1].size() != b.size())
                return false;

            for (int s = 0; s < 2; ++s)
                for (Player* p : players[s])
                    if (!BotClaims::Claim(p->GetGUID().GetCounter(), CLAIM_TAG))
                    {
                        error = "un bot elegido acaba de ser reservado por otro modulo";
                        for (int t = 0; t < 2; ++t)
                            for (Player* q : players[t])
                                BotClaims::Release(q->GetGUID().GetCounter(), CLAIM_TAG);
                        return false;
                    }

            for (int s = 0; s < 2; ++s)
                for (Player* p : players[s])
                {
                    Fighter f;
                    f.guid = p->GetGUID();
                    f.name = p->GetName();
                    f.cls = p->getClass();
                    f.spec = AiFactory::GetPlayerSpecTab(p);
                    f.claimed = true;
                    f.ratingBefore = GetProfile(p).rating;
                    m.side[s].push_back(f);
                }
            return true;
        }

        void ReleaseAll(Match& m)
        {
            for (int s = 0; s < 2; ++s)
                for (Fighter& f : m.side[s])
                    ReleaseFighter(f);
        }

        // Manda a todos a la instancia (pasos 3-5 de la cabecera).
        void SendFighters(Match& m, Battleground* bg, BattlegroundTypeId bgType, std::vector<Player*> (&players)[2])
        {
            for (int s = 0; s < 2; ++s)
            {
                if (m.size > 1)
                    MakeGroup(m, s, players[s]);
                for (size_t i = 0; i < players[s].size(); ++i)
                {
                    Player* p = players[s][i];
                    Fighter& f = m.side[s][i];
                    // Spec PvP (doble spec) y equipo: en arena igualado al nivel de
                    // objeto de la arena; en campo, el de su rating (loadout)
                    if (cfg.loadoutEnable)
                        ApplyPurpose(p, true, m.isBg ? PvpIlvlFor(GetProfile(p).rating) : cfg.arenaGearScore, false);
                    else
                    {
                        bool respec = EnsureSpec(p);
                        if (!m.isBg)
                            EqualizeGear(p, respec);
                    }
                    // La spec se anoto en PickFighters, ANTES de este cambio a la
                    // de PvP: la base decia que los picaros peleaban de asesinato
                    // cuando pelean de sutileza, y cualquier medida por spec salia
                    // mal. Se relee ya cambiada.
                    f.spec = AiFactory::GetPlayerSpecTab(p);
                    TeamId team = m.team[s];
                    bg->IncreaseInvitedCount(team);
                    f.invited = true;

                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(p))
                        botAI->Reset(true);
                    p->GetMotionMaster()->Clear();
                    p->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
                    p->SetEntryPoint();
                    // "invitado" = true: al llegar al mapa, HandleMoveWorldportAck solo
                    // añade a la instancia a quien tiene esa marca (los GM no la llevan).
                    p->SetBattlegroundId(m.instanceId, bgType, PLAYER_MAX_BATTLEGROUND_QUEUES, true, false, team);
                    if (!sBattlegroundMgr->SendToBattleground(p, m.instanceId, bgType))
                    {
                        LOG_WARN("module", "[adaptive-ai] No se pudo teletransportar a {} a la instancia {}.", p->GetName(), m.instanceId);
                        p->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE, PLAYER_MAX_BATTLEGROUND_QUEUES, false, false, TEAM_NEUTRAL);
                        bg->DecreaseInvitedCount(team);
                        f.invited = false;
                    }
                    TrackBot(f.guid, true);
                }
            }
        }

        // Llamar con s_lock cogido. Crea la arena y manda a los bots.
        bool LaunchArena(CompSpec const& comp, MatchSource source, uint32 seriesId, std::string const& rule, std::string& error)
        {
            Match m;
            std::vector<Player*> players[2];
            if (!PickFighters(m, comp.a, comp.b, cfg.arenaLevel, 80, false, players, error))
                return false;

            uint32 mapId = PickArenaMap();
            BattlegroundTypeId bgType = ArenaTypeForMap(mapId);
            PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(mapId, cfg.arenaLevel);
            uint8 arenaType = comp.Size() == 1 ? ARENA_TYPE_2v2 : (comp.Size() == 2 ? ARENA_TYPE_2v2 : (comp.Size() == 3 ? ARENA_TYPE_3v3 : ARENA_TYPE_5v5));
            Battleground* bg = bracket ? sBattlegroundMgr->CreateNewBattleground(bgType, bracket, arenaType, false) : nullptr;
            if (!bg)
            {
                ReleaseAll(m);
                error = Acore::StringFormat("no se pudo crear la arena del mapa {}", mapId);
                return false;
            }
            bg->StartBattleground();

            m.id = s_nextMatchId++;
            m.instanceId = bg->GetInstanceID();
            m.bgTypeId = bg->GetBgTypeID();
            m.mapId = bg->GetMapId();
            m.isBg = false;
            m.size = comp.Size();
            m.source = source;
            m.seriesId = seriesId;
            m.createdMs = getMSTime();
            AssignModes(m, rule);
            SendFighters(m, bg, bgType, players);

            s_matches[m.instanceId] = m;
            s_lastLaunchMs = getMSTime();
            Trace(Acore::StringFormat("Partida #{} ({}, {}): {} ({}) contra {} ({}) en el mapa {}, arena {}.",
                     m.id, SourceName(source), m.TypeName(), SideNames(m, 0), ModeName(m.sideMode[0]),
                     SideNames(m, 1), ModeName(m.sideMode[1]), m.mapId, m.instanceId));
            return true;
        }

        // Llamar con s_lock cogido. Crea el campo de batalla y manda a los bots.
        bool LaunchBattleground(uint8 bgTypeRaw, uint32 perTeam, MatchSource source, std::string const& rule, std::string& error)
        {
            BattlegroundTypeId bgType = BattlegroundTypeId(bgTypeRaw);
            Battleground* tmpl = sBattlegroundMgr->GetBattlegroundTemplate(bgType);
            if (!tmpl)
            {
                error = "ese campo de batalla no existe en el servidor";
                return false;
            }
            if (!perTeam)
                perTeam = cfg.bgPlayersPerTeam ? cfg.bgPlayersPerTeam : tmpl->GetMinPlayersPerTeam();
            perTeam = std::clamp<uint32>(perTeam, 1, std::max<uint32>(1, tmpl->GetMaxPlayersPerTeam()));
            PvPDifficultyEntry const* bracket = GetBattlegroundBracketByLevel(tmpl->GetMapId(), cfg.bgLevel);
            if (!bracket)
            {
                error = Acore::StringFormat("no hay bracket de nivel {} para ese campo", cfg.bgLevel);
                return false;
            }

            Match m;
            std::vector<ClassSpec> any(perTeam);   // cualquier clase habilitada
            std::vector<Player*> players[2];
            if (!PickFighters(m, any, any, bracket->minLevel, bracket->maxLevel, true, players, error))
                return false;

            Battleground* bg = sBattlegroundMgr->CreateNewBattleground(bgType, bracket, ARENA_TYPE_NONE, false);
            if (!bg)
            {
                ReleaseAll(m);
                error = "no se pudo crear el campo de batalla";
                return false;
            }
            // Que el core no lo cierre por falta de jugadores (Battleground.PrematureFinishTimer)
            bg->SetMinPlayersPerTeam(perTeam);
            bg->SetMaxPlayersPerTeam(std::max<uint32>(perTeam, bg->GetMaxPlayersPerTeam()));
            bg->StartBattleground();

            m.id = s_nextMatchId++;
            m.instanceId = bg->GetInstanceID();
            m.bgTypeId = bg->GetBgTypeID();
            m.mapId = bg->GetMapId();
            m.isBg = true;
            m.size = uint8(perTeam);
            m.source = source;
            m.seriesId = 0;
            m.createdMs = getMSTime();
            m.team[0] = TEAM_ALLIANCE;
            m.team[1] = TEAM_HORDE;
            AssignModes(m, rule);
            SendFighters(m, bg, bgType, players);

            s_matches[m.instanceId] = m;
            s_lastLaunchMs = getMSTime();
            Trace(Acore::StringFormat("Partida #{} ({}, {} {}c{}): Alianza {} ({}) contra Horda {} ({}), instancia {}.",
                     m.id, SourceName(source), m.TypeName(), perTeam, perTeam, SideNames(m, 0), ModeName(m.sideMode[0]),
                     SideNames(m, 1), ModeName(m.sideMode[1]), m.instanceId));
            return true;
        }

        Series* FindSeries(uint32 id)
        {
            for (Series& s : s_series)
                if (s.id == id)
                    return &s;
            return nullptr;
        }

        void RecordMatch(Match const& m)
        {
            if (!cfg.logMatches || m.side[0].empty() || m.side[1].empty())
                return;
            SideStats sum[2];
            for (int s = 0; s < 2; ++s)
                for (Fighter const& f : m.side[s])
                {
                    sum[s].decisions += f.stats.decisions;
                    sum[s].reward += f.stats.reward;
                    sum[s].interruptsOk += f.stats.interruptsOk;
                    sum[s].interruptsBad += f.stats.interruptsBad;
                    sum[s].ccDone += f.stats.ccDone;
                    sum[s].ccTaken += f.stats.ccTaken;
                    sum[s].damageDealt += f.stats.damageDealt;
                    sum[s].damageTaken += f.stats.damageTaken;
                    sum[s].objectives += f.stats.objectives;
                }
            Fighter const& a = m.side[0].front();
            Fighter const& b = m.side[1].front();
            std::string names[2] = { a.name, b.name };
            PlayerbotsDatabase.EscapeString(names[0]);
            PlayerbotsDatabase.EscapeString(names[1]);
            uint32 duration = m.startedMs && m.endedMs >= m.startedMs ? (m.endedMs - m.startedMs) / 1000 : 0;
            std::shared_ptr<QTable> c = Candidate();
            PlayerbotsDatabase.Execute(
                "INSERT INTO adaptive_match (id, fecha, fuente, mapa, tipo, modelo, "
                "bot_a, nombre_a, clase_a, spec_a, modo_a, rating_a, bot_b, nombre_b, clase_b, spec_b, modo_b, rating_b, "
                "version_a, version_b, "
                "ganador, duracion, dec_a, rec_a, int_ok_a, int_mal_a, cc_a, cc_suf_a, dano_a, dano_suf_a, "
                "dec_b, rec_b, int_ok_b, int_mal_b, cc_b, cc_suf_b, dano_b, dano_suf_b) VALUES "
                "({}, '{}', '{}', {}, '{}', {}, {}, '{}', {}, {}, '{}', {}, {}, '{}', {}, {}, '{}', {}, "
                "{}, {}, "
                "'{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
                m.id, Now(), SourceName(m.source), m.mapId, m.TypeName(), c ? c->Version() : 0,
                a.guid.GetCounter(), names[0], a.cls, a.spec, RecordedMode(a), a.ratingBefore,
                b.guid.GetCounter(), names[1], b.cls, b.spec, RecordedMode(b), b.ratingBefore,
                a.version, b.version,
                WinnerName(m.winner), duration,
                sum[0].decisions, sum[0].reward, sum[0].interruptsOk, sum[0].interruptsBad, sum[0].ccDone, sum[0].ccTaken, sum[0].damageDealt, sum[0].damageTaken,
                sum[1].decisions, sum[1].reward, sum[1].interruptsOk, sum[1].interruptsBad, sum[1].ccDone, sum[1].ccTaken, sum[1].damageDealt, sum[1].damageTaken);

            // Y cada bot, para las partidas de equipo y los campos
            SQLTransaction<PlayerbotsDatabaseConnection> trans = PlayerbotsDatabase.BeginTransaction();
            for (int s = 0; s < 2; ++s)
                for (Fighter const& f : m.side[s])
                {
                    std::string name = f.name;
                    PlayerbotsDatabase.EscapeString(name);
                    trans->Append(Acore::StringFormat(
                        "REPLACE INTO adaptive_match_bot (partida, lado, guid, nombre, clase, spec, modo, rating, decisiones, recompensa, int_ok, int_mal, cc, cc_suf, dano, dano_suf, objetivos) "
                        "VALUES ({}, {}, {}, '{}', {}, {}, '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
                        m.id, s, f.guid.GetCounter(), name, f.cls, f.spec, RecordedMode(f), f.ratingBefore,
                        f.stats.decisions, f.stats.reward, f.stats.interruptsOk, f.stats.interruptsBad, f.stats.ccDone, f.stats.ccTaken,
                        f.stats.damageDealt, f.stats.damageTaken, f.stats.objectives).c_str());
                }
            PlayerbotsDatabase.CommitTransaction(trans);
        }

        // ¿Todas las clases que se juzgan tienen ya partidas de sobra por los dos
        // lados? Es la condición que el informe (tools/progreso-adaptive.sh)
        // enseña como "ya se puede juzgar": PartidasPorClase por lado. Las
        // aprobadas juegan pero no se juzgan (su candidata y su validada son la
        // misma tabla), así que no cuentan. Si no queda ninguna por juzgar
        // devuelve false: el examen agota su ventana como siempre, para que una
        // ronda con las diez aprobadas no produzca exámenes de duración cero.
        // ¿A esta clase le siguen haciendo falta partidas de examen? No, si está
        // aprobada (juega, pero no se la juzga) o si ya llegó al umbral por los
        // dos lados. Llamar con s_lock cogido.
        bool ClassNeedsExam(uint8 cls, uint32 need)
        {
            if (s_approved.count(cls))
                return false;
            auto c = s_calib.byClass.find(cls);
            auto v = s_calib.byClassValidated.find(cls);
            uint32 nc = c == s_calib.byClass.end() ? 0 : c->second.first + c->second.second;
            uint32 nv = v == s_calib.byClassValidated.end() ? 0 : v->second.first + v->second.second;
            return nc < need || nv < need;
        }

        // Una pareja del examen deja de lanzar cuando NINGUNA de sus dos clases
        // necesita ya partidas: sus huecos se los quedan las parejas de las
        // clases que van atrasadas. Se exige que sobren las DOS: soltar una
        // pareja porque una de sus clases ya cumplió dejaría a la otra con menos
        // rivales de los que tiene, que es justo lo contrario de lo que se
        // busca. Con SaltarSaturadas quitando parejas sin senal, hay clases que
        // se quedan con 3 rivales de 9 (04/09/2026: el cazador) y son las que
        // marcan cuándo se puede cortar el examen.
        bool CompStillUseful(CompSpec const& comp)
        {
            // Calibrar.TodasJuegan (05/09/2026): ninguna pareja deja de lanzar
            // porque sus clases hayan aprobado o llegado al tope; el examen
            // sigue repartiendo entre todas hasta que corta o cierra por reloj.
            if (cfg.calibrateAllPlay)
                return true;
            uint32 need = cfg.calibrateCutMatches ? cfg.calibrateCutMatches : cfg.calibratePerClassMin;
            for (auto const& side : { std::cref(comp.a), std::cref(comp.b) })
                for (ClassSpec const& cs : side.get())
                    if (!cs.cls || ClassNeedsExam(cs.cls, need))
                        return true;   // clase "cualquiera" (0): no se puede descartar
            return false;
        }

        // ¿Puede esta clase llegar a 'need' por lado antes de que cierre la
        // ventana, con las arenas que tiene? Con SaltarSaturadas una clase puede
        // quedarse con un solo rival (05/09/2026: el CdM, 8 de sus 9 parejas
        // fuera): el examen 2 estuvo 28 min con nueve clases listas y el servidor
        // casi parado esperándola, y al cerrar por reloj la juzgó igual con el
        // acumulado de la ventana de exámenes. El corte no debe esperar a quien
        // no puede llegar. Se mide por capacidad, no por el ritmo que llevaba:
        // las partidas simultáneas de sus parejas del examen (que ahora crecen
        // para las clases con pocos rivales, ver StartCalibration), a una partida
        // por hueco y minuto (duran 35-40 s más entrar y salir), y de cada tres
        // partidas una cuenta para el lado corto (los tres brazos de
        // Calibrar.MismoRival). Llamar con s_lock cogido.
        bool ClassCanStillReach(uint8 cls, uint32 need, time_t clock)
        {
            if (!s_calib.endsAt || clock >= s_calib.endsAt)
                return true;   // sin ventana, o ya cerrada: decide el reloj
            auto c = s_calib.byClass.find(cls);
            auto v = s_calib.byClassValidated.find(cls);
            uint32 nc = c == s_calib.byClass.end() ? 0 : c->second.first + c->second.second;
            uint32 nv = v == s_calib.byClassValidated.end() ? 0 : v->second.first + v->second.second;
            uint32 shortSide = std::min(nc, nv);
            uint32 slots = 0;
            for (Series const& s : s_series)
                if (s.source == SOURCE_CALIBRATION && s.comp.Size() == 1)
                    for (auto const* side : { &s.comp.a, &s.comp.b })
                        if (!side->empty() && (*side)[0].cls == cls)
                        {
                            slots += s.simultaneous;
                            break;
                        }
            uint64 minutesLeft = uint64(s_calib.endsAt - clock) / 60;
            uint64 possible = uint64(slots) * minutesLeft / 3;
            return uint64(shortSide) + possible >= uint64(need);
        }

        // leftOut: las clases que aún necesitarían partidas pero no pueden llegar
        // (ClassCanStillReach) y por eso no frenan el corte.
        bool AllJudgedClassesReady(std::string* leftOut = nullptr)
        {
            if (!s_calib.running)
                return false;
            uint32 need = cfg.calibrateCutMatches ? cfg.calibrateCutMatches : cfg.calibratePerClassMin;
            time_t clock = GameTime::GetGameTime().count();
            uint32 judged = 0, ready = 0;
            std::string skipped;
            for (uint8 cls : TrainingClasses())
            {
                if (s_approved.count(cls))
                    continue;
                ++judged;
                if (!ClassNeedsExam(cls, need))
                {
                    ++ready;
                    continue;
                }
                if (ClassCanStillReach(cls, need, clock))
                    return false;
                auto c = s_calib.byClass.find(cls);
                auto v = s_calib.byClassValidated.find(cls);
                skipped += Acore::StringFormat("{}{} ({}+{})", skipped.empty() ? "" : ", ", ClassName(cls),
                                               c == s_calib.byClass.end() ? 0 : c->second.first + c->second.second,
                                               v == s_calib.byClassValidated.end() ? 0 : v->second.first + v->second.second);
            }
            // Alguna tiene que estar lista de verdad: si todas las que quedan
            // "no pueden llegar", el examen sigue hasta su hora.
            if (leftOut)
                *leftOut = skipped;
            return judged > 0 && ready > 0;
        }

        // Reparto del examen por CLASE (05/09/2026): cuanto lleva una clase,
        // contando los lados ya jugados por los dos brazos y las arenas en vuelo,
        // relativo a lo que necesita (need por lado, dos lados). Las aprobadas no
        // necesitan nada. Con el reparto por pareja, las clases a las que
        // SaltarSaturadas deja 3 rivales de 9 no llegaban a las 100 por lado en
        // los 45 min y el corte no se disparaba (3 de los 4 examenes de la noche
        // del 04 al 05/09 agotaron la ventana). Llamar con s_lock cogido.
        float ClassProgress(uint8 cls, uint32 need)
        {
            if (!cls)
                return 0.0f;
            // Con Calibrar.TodasJuegan (05/09/2026, peticion del usuario) la
            // aprobada se reparte como las demas: al mandarla al final, las
            // suspensas jugaban menos contra ella.
            if (!cfg.calibrateAllPlay && s_approved.count(cls))
                return 1.0e9f;
            uint32 done = 0;
            if (auto it = s_calib.byClass.find(cls); it != s_calib.byClass.end())
                done += it->second.first + it->second.second;
            if (auto it = s_calib.byClassValidated.find(cls); it != s_calib.byClassValidated.end())
                done += it->second.first + it->second.second;
            for (auto const& [id, m] : s_matches)
                if (m.source == SOURCE_CALIBRATION && !m.ended)
                    for (int s = 0; s < 2; ++s)
                        for (Fighter const& f : m.side[s])
                            if (f.cls == cls)
                                ++done;
            return float(done) / float(std::max<uint32>(1, 2 * need));
        }

        float SeriesProgress(Series const& s, std::map<uint8, float> const& progress)
        {
            float k = 1.0e9f;
            for (auto const& side : { std::cref(s.comp.a), std::cref(s.comp.b) })
                for (ClassSpec const& cs : side.get())
                    if (cs.cls)
                        if (auto it = progress.find(cs.cls); it != progress.end())
                            k = std::min(k, it->second);
            return k;
        }

        void ConcludeCalibration()
        {
            uint32 decided = s_calib.candidateWins + s_calib.validatedWins;
            float winrate = decided ? float(s_calib.candidateWins) / float(decided) : 0.0f;
            bool promote = decided >= std::max<uint32>(1, s_calib.total / 2) && winrate >= cfg.calibrateMargin;
            // Por clase del lado candidata (1c1) y por tipo: el global esconde
            // que una clase gane el 80 % y la otra el 20 % (anoche: 54 % era eso).
            // Cada clase se compara consigo misma: lo que gana con la candidata
            // contra lo que gana con la validada en las mismas parejas. La
            // nota absoluta engañaba (el CdM gana el 95 % sin aprender nada; el
            // pícaro no llega al 60 % ni aprendiendo el doble).
            std::string detail;
            for (auto const& [cls, wl] : s_calib.byClass)
            {
                uint32 n = wl.first + wl.second;
                auto vit = s_calib.byClassValidated.find(cls);
                uint32 vn = vit == s_calib.byClassValidated.end() ? 0 : vit->second.first + vit->second.second;
                if (n)
                    detail += Acore::StringFormat("{}{} {:.0f}%/{}% de {}+{}", detail.empty() ? "" : ", ", ClassName(cls), 100.0f * wl.first / float(n),
                                                  vn ? std::to_string(100 * vit->second.first / vn) : "?", n, vn);
            }
            for (auto const& [type, wl] : s_calib.byType)
            {
                uint32 n = wl.first + wl.second;
                if (n && type != "1c1")
                    detail += Acore::StringFormat("{}{} {:.0f}% de {}", detail.empty() ? "" : ", ", type, 100.0f * wl.first / float(n), n);
            }
            Trace(Acore::StringFormat("Calibracion terminada: candidata {} - validada {} (empates {}), {:.0f}% [{}]: {}.",
                     s_calib.candidateWins, s_calib.validatedWins, s_calib.draws, winrate * 100.0f, detail,
                     promote ? "la candidata pasa a validada" : "la candidata sigue entrenando"));
            if (promote)
            {
                PromoteCandidate(winrate, decided, detail);
                // Todo aprobado de golpe: ronda nueva
                for (uint8 cls : TrainingClasses())
                    s_approved.insert(cls);
                s_calibAcc.clear();
                s_classCycles.clear();
            }
            else
            {
                RecordCalibration(winrate, decided, false, detail);
                // Aprobado por clase: con el 60 % global la candidata no promociona
                // mientras el pícaro y el sacerdote de serie pierdan con todos.
                // Se suman los últimos Calibrar.VentanaCiclos exámenes por clase
                // (ventana móvil, no el acumulado de siempre: sumar sin límite
                // congelaba la nota de una clase con cientos de partidas).
                if (cfg.calibratePerClass)
                {
                    std::vector<uint8> toPromote, toRevert;
                    std::string promoteDetail, revertDetail;
                    std::set<uint8> seen;
                    for (auto const& [cls, wl] : s_calib.byClass) seen.insert(cls);
                    for (auto const& [cls, wl] : s_calib.byClassValidated) seen.insert(cls);
                    // La serie como vara (05/09/2026, N2+N6): el peldano de la
                    // candidata (partidas "sonda", sin explorar, contra la serie,
                    // relativo a la referencia por pareja: RefreshLadder) contra
                    // el mejor peldano actual de la clase. Se calcula siempre y se
                    // traza al lado del veredicto de hoy; con Calibrar.VaraSerie
                    // manda. El error tipico del rating sale del de su logit:
                    // 400/ln(10)/sqrt(n·w·(1-w)); el margen minimo es MejoraPorClase
                    // en unidades de rating (5 puntos a la par = 35).
                    std::map<uint8, std::vector<LadderRung>> ladder;
                    {
                        std::shared_lock<std::shared_mutex> lguard(s_ladderLock);
                        ladder = s_ladder;
                    }
                    uint32 candidateVersion = 0;
                    if (std::shared_ptr<QTable> c = Candidate())
                        candidateVersion = c->Version();
                    if (cfg.calibrateLadderRule)
                        for (uint8 cls : TrainingClasses())
                            seen.insert(cls);
                    auto seRating = [](LadderRung const& r)
                    {
                        float w = std::clamp(r.winrate, 0.05f, 0.95f);
                        return r.matches ? 173.7f / std::sqrt(float(r.matches) * w * (1.0f - w)) : 0.0f;
                    };
                    float const minGain = 400.0f * std::log10((0.5f + cfg.calibrateClassGain) / (0.5f - std::min(cfg.calibrateClassGain, 0.45f)));
                    std::string ladderVerdicts;
                    for (uint8 cls : seen)
                    {
                        // Veredicto por escalera de esta clase (sombra o vara)
                        bool ladderImproved = false;
                        std::string ladderLine;
                        {
                            LadderRung const* cand = nullptr;
                            LadderRung best;   // la serie, 1500, si no hay otro
                            auto lit = ladder.find(cls);
                            if (lit != ladder.end())
                                for (LadderRung const& r : lit->second)
                                {
                                    if (r.version == candidateVersion) cand = &r;
                                    else if (r.rating > best.rating)   best = r;
                                }
                            if (cand && cand->matches >= cfg.calibratePerClassMin)
                            {
                                float se = std::sqrt(seRating(*cand) * seRating(*cand) + seRating(best) * seRating(best));
                                float needed = std::max(minGain, cfg.calibrateZ * se);
                                ladderImproved = cand->rating - best.rating >= needed;
                                ladderLine = Acore::StringFormat("{} candidata {:.0f} ({} partidas) contra {} {:.0f}: {} (hacian falta {:.0f})",
                                                                 ClassName(cls), cand->rating, cand->matches,
                                                                 best.version ? "v" + std::to_string(best.version) : std::string("serie"), best.rating,
                                                                 ladderImproved ? "APRUEBA" : "no", needed);
                            }
                            else
                                ladderLine = Acore::StringFormat("{} candidata sin peldano ({} partidas de {})", ClassName(cls), cand ? cand->matches : 0, cfg.calibratePerClassMin);
                            if (!s_approved.count(cls))
                                ladderVerdicts += (ladderVerdicts.empty() ? "" : ", ") + ladderLine;
                        }
                        // Lo de ESTE examen entra en la ventana; se decide con
                        // la suma de los ultimos Calibrar.VentanaCiclos.
                        ClassWL exam;
                        if (auto it = s_calib.byClass.find(cls); it != s_calib.byClass.end()) { exam.cw = it->second.first; exam.cl = it->second.second; }
                        if (auto it = s_calib.byClassValidated.find(cls); it != s_calib.byClassValidated.end()) { exam.vw = it->second.first; exam.vl = it->second.second; }
                        WindowPush(cls, exam);
                        ClassWL sum = WindowSum(cls);
                        if (s_approved.count(cls))
                            continue;
                        uint32 nc = sum.cw + sum.cl, nv = sum.vw + sum.vl;
                        if (cfg.calibrateLadderRule)
                        {
                            // Manda la escalera: la nota del examen solo informa
                            std::string line = Acore::StringFormat("{} [escalera] {}", ClassName(cls), ladderLine);
                            if (ladderImproved)
                            {
                                toPromote.push_back(cls);
                                promoteDetail += (promoteDetail.empty() ? "" : ", ") + line;
                                continue;
                            }
                            if (ladderLine.find("sin peldano") != std::string::npos)
                            {
                                SaveApproval(cls, false, 0);
                                continue;   // sin medida suficiente no cuenta como suspenso
                            }
                            ++s_classCycles[cls];
                            SaveApproval(cls, false, 0);
                            continue;   // sin vuelta a lo anterior por escalera: la candidata sigue
                        }
                        if (nc < cfg.calibratePerClassMin || nv < cfg.calibratePerClassMin)
                        {
                            // Sin partidas suficientes no cuenta como examen
                            // suspendido (05/09/2026): el tope de ciclos se
                            // llevaba por delante clases que apenas se habian
                            // podido juzgar.
                            SaveApproval(cls, false, 0);   // el acumulado sobrevive al reinicio
                            continue;
                        }
                        float wc = float(sum.cw) / float(nc), wv = float(sum.vw) / float(nv);
                        // Error tipico de la diferencia de dos porcentajes: con
                        // 200-300 por lado ronda los 4-4,5 puntos, asi que un
                        // umbral fijo de 5 dejaba pasar un falso aprobado por
                        // clase cada ocho examenes. Se exige ademas z veces el
                        // error (Calibrar.Z, 1,64 = 95 % a una cola).
                        float se = std::sqrt(wc * (1.0f - wc) / float(nc) + wv * (1.0f - wv) / float(nv));
                        float needed = std::max(cfg.calibrateClassGain, cfg.calibrateZ * se);
                        std::string line = Acore::StringFormat("{} candidata {:.0f}% / validada {:.0f}% ({}+{}, hacen falta {:.1f} puntos)", ClassName(cls), wc * 100.0f, wv * 100.0f, nc, nv, needed * 100.0f);
                        // Aprueba si mejora claramente, o si ya gana casi todo sin
                        // empeorar (saturada: no hay nada que aprender y sus arenas
                        // valen más en las clases que van atrás).
                        bool improved = wc >= wv + needed;
                        bool saturated = wc >= 0.90f && wc >= wv;
                        if (improved || saturated)
                        {
                            toPromote.push_back(cls);
                            promoteDetail += (promoteDetail.empty() ? "" : ", ") + line + (saturated && !improved ? " [saturada]" : "");
                            continue;
                        }
                        // Tope de ciclos sin aprobar: si no mejora, vuelve a lo
                        // anterior (las filas de la clase se copian de la validada)
                        // y empieza de nuevo; si iba mejorando pero sin llegar al
                        // margen, sigue con lo que tiene.
                        uint32 cycles = ++s_classCycles[cls];
                        SaveApproval(cls, false, 0);
                        if (cfg.calibrateMaxCycles && cycles >= cfg.calibrateMaxCycles)
                        {
                            ResetAccumulated(cls);
                            SaveApproval(cls, false, 0);
                            if (wc <= wv)
                            {
                                toRevert.push_back(cls);
                                revertDetail += (revertDetail.empty() ? "" : ", ") + line;
                            }
                            else
                                Trace(Acore::StringFormat("{} lleva {} calibraciones sin aprobar pero mejora ({}): sigue con lo aprendido.", ClassName(cls), cycles, line));
                        }
                    }
                    if (!ladderVerdicts.empty())
                        Trace(Acore::StringFormat("Veredicto por escalera ({}): {}.", cfg.calibrateLadderRule ? "manda" : "sombra, solo informa", ladderVerdicts));
                    if (!toRevert.empty())
                    {
                        std::shared_ptr<QTable> c = Candidate();
                        std::shared_ptr<QTable> v = Validated();
                        // La guarda que faltaba: si la validada no tiene filas de
                        // esa clase, revertir la deja a CERO. El 04/09 a las 02:00
                        // el tope de ciclos borró así cinco clases que nunca habían
                        // aprobado, y por eso CiclosMaximos estaba a 0.
                        std::string reverted;
                        if (c && v)
                            for (auto it = toRevert.begin(); it != toRevert.end();)
                            {
                                if (v->ClassRows(*it))
                                {
                                    c->CopyClassFrom(*v, *it);
                                    reverted += (reverted.empty() ? "" : ", ") + std::string(ClassName(*it));
                                    ++it;
                                }
                                else
                                {
                                    Trace(Acore::StringFormat("{} lleva {} calibraciones sin aprobar, pero la validada v{} no tiene ni una fila suya: se queda con lo que ha aprendido (revertir la dejaria a cero).",
                                             ClassName(*it), cfg.calibrateMaxCycles, v->Version()));
                                    it = toRevert.erase(it);
                                }
                            }
                        // Solo lo que de verdad se ha revertido: el 05/09/2026 esta
                        // traza salia con el druida aunque la guarda lo hubiera
                        // salvado, y el informe lo pintaba en rojo.
                        if (!reverted.empty())
                            Trace(Acore::StringFormat("Vuelta a lo anterior tras {} calibraciones sin mejorar (ronda {}): {} [{}]. Sus filas de la candidata vuelven a ser las de la validada.",
                                                      cfg.calibrateMaxCycles, s_round, reverted, revertDetail));
                    }
                    if (!toPromote.empty())
                    {
                        PromoteClasses(toPromote, Acore::StringFormat("aprobado por clase, ronda {}: {}", s_round, promoteDetail));
                        std::shared_ptr<QTable> v = Validated();
                        for (uint8 cls : toPromote)
                        {
                            s_approved.insert(cls);
                            ResetAccumulated(cls);
                            SaveApproval(cls, true, v ? v->Version() : 0);
                        }
                        Trace(Acore::StringFormat("Aprobado por clase (ronda {}): {}. Sus arenas pasan a las clases que siguen entrenando.", s_round, promoteDetail));
                        RefreshGenerations();
                    }
                }
            }
            // Todas aprobadas: ronda nueva, todas vuelven a entrenar contra lo que
            // acaban de dejar en la validada (y contra las generaciones anteriores).
            //
            // Y si no aprueban todas, la ronda se cierra igual a los
            // Calibrar.RondaMaxExamenes: mientras no cierra, las clases YA
            // aprobadas no vuelven a aprender en 1c1 (AssignModes les da siempre
            // el lado no adaptativo y CompAllowed no lanza las parejas de dos
            // aprobadas), así que una sola clase atascada congela a todas las
            // demás. El 04/09 eran cuatro clases paradas por el brujo.
            {
                ++s_classCycles[0];   // un examen más en esta ronda
                SaveApproval(0, false, 0);   // y a la base: sin esto el reinicio diario ponia la cuenta a cero y la ronda no cerraba nunca
                std::vector<uint8> all = TrainingClasses();
                bool everyone = !all.empty();
                for (uint8 cls : all)
                    everyone = everyone && s_approved.count(cls);
                bool porTope = !everyone && cfg.roundMaxExams && s_classCycles[0] >= cfg.roundMaxExams;
                if (everyone || porTope)
                {
                    ++s_round;
                    s_approved.clear();
                    s_calibAcc.clear();
                    s_classCycles.clear();
                    PlayerbotsDatabase.Execute("UPDATE adaptive_clase SET aprobada = 0, ronda = {}, cand_gana = 0, cand_pierde = 0, val_gana = 0, val_pierde = 0, ciclos = 0, historial = ''", s_round);
                    SaveApproval(0, false, 0);
                    if (everyone)
                        Trace(Acore::StringFormat("Ronda {}: todas las clases han aprobado. Vuelven a entrenar todas contra la validada nueva.", s_round));
                    else
                        Trace(Acore::StringFormat("Ronda {}: se cierra por tope ({} examenes) sin que aprobaran todas. Las aprobadas vuelven a entrenar en vez de quedarse de sparring.",
                                 s_round, cfg.roundMaxExams));
                }
            }
            if (auto c = Candidate())
                s_updatesAtLastCalib = c->Updates();
            s_calib = Calibration();
            s_series.erase(std::remove_if(s_series.begin(), s_series.end(), [](Series const& s) { return s.source == SOURCE_CALIBRATION; }), s_series.end());
        }

        // Elo por equipos: la media de cada lado, y el mismo delta para todos.
        void EloTeams(Match& m, float scoreA)
        {
            std::vector<BotProfile*> pa, pb;
            float ra = 0.0f, rb = 0.0f;
            for (Fighter const& f : m.side[0])
                if (BotProfile* p = FindProfile(f.guid.GetCounter())) { pa.push_back(p); ra += p->rating; }
            for (Fighter const& f : m.side[1])
                if (BotProfile* p = FindProfile(f.guid.GetCounter())) { pb.push_back(p); rb += p->rating; }
            if (pa.empty() || pb.empty())
                return;
            ra /= pa.size();
            rb /= pb.size();
            float const k = 32.0f;
            float expectedA = 1.0f / (1.0f + std::pow(10.0f, (rb - ra) / 400.0f));
            float delta = k * (scoreA - expectedA);
            for (BotProfile* p : pa)
            {
                p->rating += delta;
                if (scoreA > 0.75f) ++p->wins; else if (scoreA < 0.25f) ++p->losses; else ++p->draws;
                SaveProfile(*p);
            }
            for (BotProfile* p : pb)
            {
                p->rating -= delta;
                if (scoreA > 0.75f) ++p->losses; else if (scoreA < 0.25f) ++p->wins; else ++p->draws;
                SaveProfile(*p);
            }
        }

        // Llamar con s_lock. Cierra la partida: cerebros, Elo, registro.
        void FinishMatch(Match& m, int8 winner, char const* reason)
        {
            if (m.ended)
                return;
            m.ended = true;
            m.endedMs = getMSTime();
            m.winner = winner;

            for (int s = 0; s < 2; ++s)
                for (Fighter& f : m.side[s])
                {
                    if (std::shared_ptr<Brain> brain = FindBrain(f.guid))
                    {
                        std::lock_guard<std::recursive_mutex> guard(brain->lock);
                        f.stats.decisions = brain->decisions;
                        f.stats.interruptsOk = brain->interruptsOk;
                        f.stats.interruptsBad = brain->interruptsBad;
                        f.stats.ccDone = brain->ccDone;
                        f.stats.ccTaken = brain->ccTaken;
                        f.stats.damageDealt = brain->damageDealt;
                        f.stats.damageTaken = brain->damageTaken;
                        f.stats.reward = brain->totalReward;
                    }
                    float terminal = 0.0f;
                    if (winner == s)          terminal = cfg.rWin;
                    else if (winner == 1 - s) terminal = cfg.rLoss;

                    // Muerte y kill, que por el camino normal no llegan nunca.
                    // En Unit::Kill el core llama antes a bg->HandleKillPlayer
                    // —que termina la arena y trae hasta aquí, y aquí se borran
                    // los cerebros— y sólo después a OnUnitDeath y a
                    // OnPlayerPVPKill. Resultado medido el 04/09/2026: ganar y
                    // perder valían +20/-20 en vez de +50/-50, la mitad de lo
                    // que dice el .conf. La bandera brain->dead evita el doble
                    // cobro cuando el enganche sí llegó (muertes que no acaban
                    // la partida, en 2c2 y campos).
                    // Ojo con IsAlive(): la arena resucita al perdedor antes de
                    // que llegue este hook, asi que preguntarlo daba "vivo" y la
                    // muerte no se cobraba (perder valia -20 en vez de -50,
                    // medido el 04/09/2026 a las 18:55). En 1c1 el perdedor
                    // murio por definicion; en 2c2 y campos se sigue mirando,
                    // porque alli hay muertes que no acaban la partida.
                    if (Player* p = ObjectAccessor::FindPlayer(f.guid))
                    {
                        bool murio = m.size == 1 ? (winner == 1 - s) : !p->IsAlive();
                        if (murio)
                            if (std::shared_ptr<Brain> brain = FindBrain(f.guid))
                            {
                                bool first = false;
                                {
                                    std::lock_guard<std::recursive_mutex> guard(brain->lock);
                                    if (!brain->dead)
                                    {
                                        brain->dead = true;
                                        first = true;
                                    }
                                }
                                if (first)
                                {
                                    terminal += cfg.rDeath;
                                    for (Fighter& mate : m.side[s])
                                        if (mate.guid != f.guid)
                                            Reward(mate.guid, cfg.rAllyDeath);
                                }
                            }
                        // El premio por matar sólo se puede atribuir en 1c1: en
                        // 2c2 y más no se sabe quién remató, y la última muerte
                        // de la partida se pierde.
                        if (winner == s && m.size == 1)
                            terminal += cfg.rKill;
                    }

                    f.stats.reward += terminal;
                    EndBrain(f.guid, terminal);
                    OwnWarlockPet(ObjectAccessor::FindPlayer(f.guid), false);
                }

            if (winner >= -1)
            {
                // El Elo por bot solo con partidas donde nadie asigna el modelo a
                // dedo: en el examen y en la escalera el lado con modelo se
                // elige por rueda, y eso es ruido para el rating del bot.
                if (m.source != SOURCE_CALIBRATION && m.rule != "escalera")
                    EloTeams(m, winner == 0 ? 1.0f : (winner == 1 ? 0.0f : 0.5f));
                ++s_finishedToday;
            }
            else
                ++s_abortedToday;

            RecordMatch(m);

            if (Series* s = FindSeries(m.seriesId))
            {
                if (winner >= -1) ++s->finished; else ++s->aborted;
                if (winner == -1) ++s->draws;
                else if (winner >= 0)
                {
                    // lado 0 de la serie = el que decide con la candidata
                    bool candidateWon = m.sideMode[winner] == BRAIN_CANDIDATE;
                    ++s->wins[candidateWon ? 0 : 1];
                }
            }
            if (m.source == SOURCE_CALIBRATION && s_calib.running)
            {
                // El brazo lo dice m.calibArm (el papel), no lo que juega cada bot:
                // un lado "validada" sin filas de su clase juega de serie (N1) y
                // sigue contando para la nota "con la validada" de esa clase.
                bool const bothValidated = m.calibArm == 2;
                if (winner >= 0 && bothValidated)
                {
                    // Tercer brazo (validada contra validada): la nota "con la
                    // validada" de las DOS clases, contra un rival con la
                    // validada, el mismo que tienen enfrente cuando llevan la
                    // candidata. No entra en el global candidata-validada.
                    if (m.size == 1 && !m.side[0].empty() && !m.side[1].empty())
                        for (int s = 0; s < 2; ++s)
                        {
                            auto& vl = s_calib.byClassValidated[m.side[s].front().cls];
                            if (winner == s) ++vl.first; else ++vl.second;
                        }
                }
                else if (winner >= 0)
                {
                    int candidateSide = m.calibArm >= 0 ? m.calibArm : (m.sideMode[0] == BRAIN_CANDIDATE ? 0 : 1);
                    if (winner == candidateSide) ++s_calib.candidateWins;
                    else ++s_calib.validatedWins;
                    if (m.size == 1 && !m.side[candidateSide].empty() && !m.side[1 - candidateSide].empty())
                    {
                        auto& wl = s_calib.byClass[m.side[candidateSide].front().cls];
                        if (winner == candidateSide) ++wl.first; else ++wl.second;
                        // Sin el tercer brazo, la nota "con la validada" sale de
                        // aqui (contra un rival con la candidata: es lo que
                        // mezclaba las mejoras de los dos).
                        if (!cfg.calibrateSameRival)
                        {
                            auto& vl = s_calib.byClassValidated[m.side[1 - candidateSide].front().cls];
                            if (winner != candidateSide) ++vl.first; else ++vl.second;
                        }
                    }
                    auto& wt = s_calib.byType[m.TypeName()];
                    if (winner == candidateSide) ++wt.first; else ++wt.second;
                }
                else if (winner == -1)
                    ++s_calib.draws;
                ++s_calib.finished;
                if (s_calib.finished >= s_calib.total)
                    ConcludeCalibration();
            }

            std::string result = winner >= 0 ? Acore::StringFormat("gana {} ({})", SideNames(m, winner), winner == 0 ? "lado a" : "lado b")
                                             : (winner == -1 ? "empate" : "abortada");
            std::string sides;
            for (int s = 0; s < 2; ++s)
            {
                SideStats sum;
                for (Fighter const& f : m.side[s])
                {
                    sum.decisions += f.stats.decisions; sum.interruptsOk += f.stats.interruptsOk;
                    sum.ccDone += f.stats.ccDone; sum.reward += f.stats.reward; sum.objectives += f.stats.objectives;
                }
                sides += Acore::StringFormat("{}{}: {} decisiones, {} interrupciones, {} controles{}, recompensa {:.1f}", s ? "; " : "",
                                             SideNames(m, s), sum.decisions, sum.interruptsOk, sum.ccDone,
                                             m.isBg ? Acore::StringFormat(", {} objetivos", sum.objectives) : "", sum.reward);
            }
            Trace(Acore::StringFormat("Partida #{} ({}) terminada: {} [{}] en {} s. {}.",
                     m.id, m.TypeName(), result, reason, m.startedMs ? (m.endedMs - m.startedMs) / 1000 : 0, sides));
        }

        // Llamar con s_lock. Saca a los bots de la instancia (vuelven a su sitio).
        void IssueLeave(Match& m, Battleground* bg)
        {
            if (m.leaveIssued)
                return;
            m.leaveIssued = true;
            DisbandGroups(m);
            std::vector<Player*> present;
            for (auto const& pair : bg->GetPlayers())
                if (pair.second && SideOf(m, pair.first) >= 0)
                    present.push_back(pair.second);
            for (Player* p : present)
                if (p->InBattleground() && p->GetBattlegroundId() == m.instanceId && !p->IsBeingTeleported())
                    p->LeaveBattleground(bg);

            for (int s = 0; s < 2; ++s)
                for (Fighter& f : m.side[s])
                {
                    if (f.arrived)
                        continue;
                    if (f.invited)
                    {
                        bg->DecreaseInvitedCount(m.team[s]);
                        f.invited = false;
                    }
                    if (Player* p = ObjectAccessor::FindPlayer(f.guid))
                        if (p->GetBattlegroundId() == m.instanceId && !p->IsBeingTeleported())
                            p->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE, PLAYER_MAX_BATTLEGROUND_QUEUES, false, false, TEAM_NEUTRAL);
                    ReleaseFighter(f);
                }
        }

        // Campos: objetivos cobrados desde la puntuación del campo (banderas,
        // bases, torres...), al que los hizo y un poco a sus compañeros.
        void ScoreObjectives(Match& m, Battleground* bg)
        {
            Battleground::BattlegroundScoreMap const* scores = bg->GetPlayerScores();
            if (!scores)
                return;
            for (int s = 0; s < 2; ++s)
                for (Fighter& f : m.side[s])
                {
                    auto it = scores->find(f.guid.GetCounter());
                    if (it == scores->end() || !it->second)
                        continue;
                    BattlegroundScore const* sc = it->second;
                    uint32 total = ObjectiveScore(sc);
                    if (total <= f.lastObjectives)
                        continue;
                    uint32 gained = total - f.lastObjectives;
                    f.lastObjectives = total;
                    f.stats.objectives += gained;
                    Reward(f.guid, cfg.rObjective * gained);
                    for (Fighter const& ally : m.side[s])
                        if (ally.guid != f.guid)
                            Reward(ally.guid, cfg.rObjectiveTeam * gained);
                    Trace(Acore::StringFormat("Partida #{}: {} suma {} objetivo(s).", m.id, f.name, gained));
                }
        }

        uint32 RunningArenas()
        {
            uint32 n = 0;
            for (auto const& [id, m] : s_matches)
                if (!m.isBg)
                    ++n;
            return n;
        }

        uint32 BotsInArenas()
        {
            uint32 n = 0;
            for (auto const& [id, m] : s_matches)
                if (!m.isBg)
                    n += uint32(m.side[0].size() + m.side[1].size());
            return n;
        }

        // Cabe otra partida de este tamaño sin pasar del tope de bots en arenas
        bool ArenaRoom(uint8 size)
        {
            return RunningArenas() < std::max<uint32>(1, cfg.arenaSimultaneous) && BotsInArenas() + size * 2 <= cfg.arenaBotsMax;
        }

        uint32 RunningBgs()
        {
            uint32 n = 0;
            for (auto const& [id, m] : s_matches)
                if (m.isBg)
                    ++n;
            return n;
        }

        uint32 RunningOfSeries(uint32 seriesId)
        {
            uint32 n = 0;
            for (auto const& [id, m] : s_matches)
                if (m.seriesId == seriesId)
                    ++n;
            return n;
        }

        bool TypeEnabled(uint8 size)
        {
            return std::find(cfg.arenaTypes.begin(), cfg.arenaTypes.end(), size) != cfg.arenaTypes.end();
        }

        bool CompAllowed(CompSpec const& comp)
        {
            if (!TypeEnabled(comp.Size()))
                return false;
            if (cfg.objectivesStop && s_reached.count(comp.Name()))
                return false;
            for (ClassSpec const& c : comp.a) if (c.cls && !ClassEnabled(c.cls)) return false;
            for (ClassSpec const& c : comp.b) if (c.cls && !ClassEnabled(c.cls)) return false;
            // Las dos clases aprobadas en esta ronda: la arena es para las suspensas
            // (solo si las aprobadas no siguen entrenando, Arena.AprobadasEntrenan = 0)
            if (!cfg.approvedKeepLearning && comp.Size() == 1 && comp.a[0].cls && comp.b[0].cls && s_approved.count(comp.a[0].cls) && s_approved.count(comp.b[0].cls))
                return false;
            return true;
        }

        // Objetivos por emparejamiento (AdaptiveAI.Objetivos): un 1c1 explícito
        // está alcanzado cuando, en sus últimas partidas de contraste, el
        // adaptativo gana al menos objetivo - margen en LAS DOS orientaciones
        // con partidas suficientes. Se comprueba cada 10 minutos y, si
        // Objetivos.Parar, el automático deja de lanzarlo (a mano sigue).
        void FinishObjectives()
        {
            std::set<std::string> reached;
            std::string text;
            for (CompSpec const& comp : cfg.arenaPairs)
            {
                if (comp.Size() != 1 || !comp.a[0].cls || !comp.b[0].cls)
                    continue;
                uint8 x = comp.a[0].cls, y = comp.b[0].cls;
                if (TargetFor(x, y) < 0)
                    continue;
                bool all = true;
                std::string detail;
                for (int o = 0; o < 2; ++o)
                {
                    uint8 me = o == 0 ? x : y, other = o == 0 ? y : x;
                    int target = TargetFor(me, other);
                    // Solo la candidata contra la serie: las partidas de la
                    // escalera (validada o generacion contra serie) miden otra
                    // cosa y desviarian el objetivo.
                    PairWL c;
                    if (auto it = s_objContrast.find({ me, other }); it != s_objContrast.end())
                        c = it->second;
                    uint32 n = c.n, wins = c.wins;
                    uint32 pct = n ? 100 * wins / n : 0;
                    // Y no por debajo de lo que saca playerbots de serie en la
                    // misma pareja: un objetivo que el de serie ya supera no
                    // dice nada del aprendizaje (mago 77 % con serie al 87 %).
                    uint32 refN = 0, refWins = 0;
                    if (auto it = s_objRef.find({ std::min(me, other), std::max(me, other) }); it != s_objRef.end())
                    {
                        refN = it->second.n;
                        refWins = me <= other ? it->second.wins : it->second.n - it->second.wins;
                    }
                    uint32 refPct = refN ? 100 * refWins / refN : 0;
                    bool belowRef = refN >= 20 && pct + cfg.objectiveMargin < refPct;
                    bool ok = n >= cfg.objectiveMinMatches && pct + cfg.objectiveMargin >= uint32(target) && !belowRef;
                    all = all && ok;
                    detail += Acore::StringFormat("{}{} {}% de {} (objetivo {}%, serie {}%{})", detail.empty() ? "" : ", ", ClassName(me), pct, n, target,
                                                  refN >= 20 ? std::to_string(refPct) : "?", ok ? ", ok" : (belowRef ? ", por debajo de serie" : ""));
                }
                text += Acore::StringFormat("{}{}: {}{}", text.empty() ? "" : "; ", comp.Name(), detail, all ? " -> ALCANZADO" : "");
                if (all)
                {
                    reached.insert(comp.Name());
                    if (!s_reached.count(comp.Name()))
                        Trace(Acore::StringFormat("Objetivo alcanzado en {}: {}. {}", comp.Name(), detail,
                                 cfg.objectivesStop ? "El automatico deja de lanzarlo (a mano sigue)." : ""));
                }
            }
            s_reached = reached;
            s_objectivesText = text.empty() ? "sin objetivos definidos para los 1c1 configurados" : text;
        }

        void ObjectivesQueryDone()
        {
            if (s_objPending && --s_objPending == 0)
                FinishObjectives();
        }

        // Lanza las dos consultas (una recorrida de la tabla cada una, con
        // ROW_NUMBER para quedarse con las ultimas N de cada pareja) al hilo de
        // la base; FinishObjectives las junta cuando vuelven las dos.
        uint32 s_objLaunchedMs = 0;
        uint32 s_objGen = 0;   // pasada en curso: una respuesta de otra pasada (perdida y relanzada) se ignora
        void CheckObjectives()
        {
            // La pasada anterior aun no ha vuelto: se espera, salvo que lleve
            // mas de diez minutos (una consulta perdida no puede dejar los
            // objetivos congelados hasta el reinicio).
            if (s_objPending && getMSTime() - s_objLaunchedMs < 10 * 60000)
                return;
            s_objContrast.clear();
            s_objRef.clear();
            s_objPending = 2;
            s_objLaunchedMs = getMSTime();
            uint32 gen = ++s_objGen;
            std::string contrast = Acore::StringFormat(
                "SELECT me, rival, COUNT(*), IFNULL(SUM(g), 0) FROM ("
                "SELECT IF(modo_a = 'ninguna', clase_b, clase_a) me, IF(modo_a = 'ninguna', clase_a, clase_b) rival, "
                "((ganador = 'a' AND modo_a <> 'ninguna') OR (ganador = 'b' AND modo_b <> 'ninguna')) g, "
                "ROW_NUMBER() OVER (PARTITION BY IF(modo_a = 'ninguna', clase_b, clase_a), IF(modo_a = 'ninguna', clase_a, clase_b) ORDER BY id DESC) rn "
                "FROM adaptive_match WHERE tipo = '1c1' AND fuente IN ('auto','manual') AND ganador IN ('a','b') "
                "AND (modo_a = 'ninguna') <> (modo_b = 'ninguna') AND (modo_a = 'candidata' OR modo_b = 'candidata') "
                "AND clase_a > 0 AND clase_b > 0) t WHERE rn <= {} GROUP BY me, rival",
                cfg.objectiveMinMatches * 2);
            std::string ref = Acore::StringFormat(
                "SELECT lo, hi, COUNT(*), IFNULL(SUM(g), 0) FROM ("
                "SELECT LEAST(clase_a, clase_b) lo, GREATEST(clase_a, clase_b) hi, IF(clase_a <= clase_b, ganador = 'a', ganador = 'b') g, "
                "ROW_NUMBER() OVER (PARTITION BY LEAST(clase_a, clase_b), GREATEST(clase_a, clase_b) ORDER BY id DESC) rn "
                "FROM adaptive_match WHERE tipo = '1c1' AND ganador IN ('a','b') AND modo_a = 'ninguna' AND modo_b = 'ninguna' "
                "AND clase_a > 0 AND clase_b > 0) t WHERE rn <= {} GROUP BY lo, hi",
                cfg.objectiveMinMatches);
            s_dbQueue.AddCallback(PlayerbotsDatabase.AsyncQuery(contrast).WithCallback([gen](QueryResult r)
            {
                std::lock_guard<std::recursive_mutex> guard(s_lock);
                if (gen != s_objGen)
                    return;
                if (r)
                    do
                    {
                        Field* f = r->Fetch();
                        s_objContrast[{ f[0].Get<uint8>(), f[1].Get<uint8>() }] = { uint32(f[2].Get<uint64>()), uint32(f[3].Get<uint64>()) };
                    } while (r->NextRow());
                ObjectivesQueryDone();
            }));
            s_dbQueue.AddCallback(PlayerbotsDatabase.AsyncQuery(ref).WithCallback([gen](QueryResult r)
            {
                std::lock_guard<std::recursive_mutex> guard(s_lock);
                if (gen != s_objGen)
                    return;
                if (r)
                    do
                    {
                        Field* f = r->Fetch();
                        s_objRef[{ f[0].Get<uint8>(), f[1].Get<uint8>() }] = { uint32(f[2].Get<uint64>()), uint32(f[3].Get<uint64>()) };
                    } while (r->NextRow());
                ObjectivesQueryDone();
            }));
        }

        // La escalera: por cada peldaño (la validada y las generaciones), sus
        // últimas partidas contra playerbots de serie, agrupadas por clase. El
        // % de victorias se convierte a rating con la fórmula de Elo tomando la
        // serie como 1500. Una consulta conjunta asíncrona cada diez minutos.
        // La escalera medida, a la base: se pisa entera en cada pasada porque es
        // una foto, no un acumulado.
        void SaveLadder(std::map<uint8, std::vector<LadderRung>> const& built)
        {
            SQLTransaction<PlayerbotsDatabaseConnection> trans = PlayerbotsDatabase.BeginTransaction();
            trans->Append("DELETE FROM adaptive_escalera");
            for (auto const& [cls, rungList] : built)
                for (LadderRung const& r : rungList)
                {
                    if (!r.version)
                        continue;   // el peldaño 0 es la serie: siempre 1500, no se guarda
                    trans->Append(Acore::StringFormat(
                        "REPLACE INTO adaptive_escalera (clase, version, partidas, victorias, rating, actualizado) VALUES ({}, {}, {}, {}, {}, '{}')",
                        cls, r.version, r.matches, r.winrate, r.rating, Now()).c_str());
                }
            // También una medida vacía debe retirar los peldaños antiguos.
            PlayerbotsDatabase.CommitTransaction(trans);
        }

        void LoadLadder()
        {
            s_ladderSaved.clear();
            if (cfg.ladderSince)
            {
                // La foto guardada no identifica el periodo medido. Recalcular
                // desde las partidas, sin heredar ratings de antes del corte.
                std::unique_lock<std::shared_mutex> guard(s_ladderLock);
                s_ladder.clear();
                s_ladderTimerMs = 10 * 60000;
                return;
            }
            if (QueryResult r = PlayerbotsDatabase.Query("SELECT clase, version, partidas, victorias, rating FROM adaptive_escalera"))
                do
                {
                    Field* f = r->Fetch();
                    LadderRung rung;
                    rung.version = f[1].Get<uint32>();
                    rung.matches = f[2].Get<uint32>();
                    rung.winrate = f[3].Get<float>();
                    rung.rating  = f[4].Get<float>();
                    if (rung.version)
                        s_ladderSaved[f[0].Get<uint8>()].push_back(rung);
                } while (r->NextRow());
            if (!s_ladderSaved.empty())
            {
                std::unique_lock<std::shared_mutex> guard(s_ladderLock);
                s_ladder = s_ladderSaved;   // vale desde el arranque, sin esperar a la primera medida
                for (auto& [cls, list] : s_ladder)
                {
                    if (std::none_of(list.begin(), list.end(), [](LadderRung const& x) { return x.version == 0; }))
                        list.push_back(LadderRung());   // la serie, el peldaño 0
                    std::sort(list.begin(), list.end(), [](LadderRung const& x, LadderRung const& y) { return x.rating > y.rating; });
                }
                LOG_INFO("module", "[adaptive-ai] Escalera cargada de la base: {} clases con peldanos medidos.", s_ladder.size());
            }
        }

        uint32 s_ladderRequest = 0;
        bool s_ladderPending = false;
        uint32 s_ladderLaunchedMs = 0;

        void RefreshLadder()
        {
            uint32 now = getMSTime();
            if (s_ladderPending && now - s_ladderLaunchedMs < 10 * 60000)
                return;
            auto candidate = Candidate();
            auto validated = Validated();
            if (!candidate || !validated)
                return;
            uint32 candidateVersion = candidate->Version(), validatedVersion = validated->Version();
            uint32 since = cfg.ladderSince, window = cfg.ladderWindow, minimum = cfg.ladderMinMatches;
            uint32 request = ++s_ladderRequest;
            s_ladderPending = true;
            s_ladderLaunchedMs = now;
            // Una sola sentencia: referencia, muestras y existencia de filas ven
            // la misma instant?nea. No carga Q ni consulta modelos en el mundo.
            std::string query = Acore::StringFormat(
                "WITH modelos AS (SELECT version FROM adaptive_model WHERE version > 1 "
                "AND (generacion = 1 OR validada = 1 OR version = {})), "
                "clases AS (SELECT 1 cls UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 "
                "UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 "
                "UNION ALL SELECT 9 UNION ALL SELECT 11), "
                "base AS (SELECT id, clase_a, clase_b, modo_a, modo_b, version_a, version_b, ganador "
                "FROM adaptive_match WHERE tipo = '1c1' AND ganador IN ('a','b') "
                "AND clase_a > 0 AND clase_b > 0 AND fecha >= FROM_UNIXTIME({}) "
                "AND fecha > NOW() - INTERVAL 7 DAY), "
                "lados AS (SELECT id, IF(modo_a = 'ninguna', version_b, version_a) version, "
                "IF(modo_a = 'ninguna', clase_b, clase_a) clase, "
                "IF(modo_a = 'ninguna', clase_a, clase_b) rival, "
                "IF(modo_a = 'ninguna', ganador = 'b', ganador = 'a') g "
                "FROM base WHERE (modo_a = 'ninguna') <> (modo_b = 'ninguna') "
                "AND IF(modo_a = 'ninguna', modo_b, modo_a) <> 'candidata'), "
                "ranked AS (SELECT l.*, ROW_NUMBER() OVER (PARTITION BY l.version, clase ORDER BY id DESC) rn "
                "FROM lados l JOIN modelos m ON m.version = l.version) "
                "SELECT 0 kind, 0 version, clase_a clase, clase_b rival, COUNT(*) n, SUM(ganador = 'a') w "
                "FROM base WHERE modo_a = 'ninguna' AND modo_b = 'ninguna' GROUP BY clase_a, clase_b "
                "UNION ALL SELECT 1, version, clase, rival, COUNT(*), SUM(g) FROM ranked "
                "WHERE rn <= {} GROUP BY version, clase, rival "
                "UNION ALL SELECT 2, m.version, c.cls, 0, 0, 0 FROM modelos m CROSS JOIN clases c "
                "WHERE EXISTS (SELECT 1 FROM adaptive_q q WHERE q.modelo = m.version AND q.clase = c.cls) "
                "UNION ALL SELECT 3, 0, 0, 0, 0, 0",
                candidateVersion, since, window);
            s_dbQueue.AddCallback(PlayerbotsDatabase.AsyncQuery(query).WithCallback(
                [request, now, candidateVersion, validatedVersion, since, window, minimum](QueryResult r)
            {
                std::lock_guard<std::recursive_mutex> guard(s_lock);
                if (request != s_ladderRequest)
                    return;
                s_ladderPending = false;
                auto c = Candidate();
                auto v = Validated();
                if (!c || !v || c->Version() != candidateVersion || v->Version() != validatedVersion ||
                    cfg.ladderSince != since || cfg.ladderWindow != window || cfg.ladderMinMatches != minimum)
                {
                    s_ladderTimerMs = 10 * 60000;
                    return;   // promoci?n, importaci?n o recarga: no publicar una foto obsoleta
                }
                if (!r)
                {
                    LOG_WARN("module", "[adaptive-ai] Escalera: consulta sin resultado; se conserva la ultima medida.");
                    return;   // la fila centinela distingue ?xito vac?o de error
                }
                uint32 applyStart = getMSTime();
                struct Pair { uint32 version; uint8 cls, rival; uint32 n, wins; };
                std::vector<Pair> pairs;
                std::map<std::pair<uint8, uint8>, std::pair<uint32, uint32>> ref;
                std::set<std::pair<uint32, uint8>> eligible;
                do
                {
                    Field* f = r->Fetch();
                    uint8 kind = f[0].Get<uint8>(), cls = f[2].Get<uint8>(), rival = f[3].Get<uint8>();
                    uint32 version = f[1].Get<uint32>();
                    uint32 n = uint32(f[4].Get<uint64>()), wins = uint32(f[5].Get<double>());
                    if (kind == 0)
                    {
                        auto& ab = ref[{cls, rival}]; ab.first += n; ab.second += wins;
                        auto& ba = ref[{rival, cls}]; ba.first += n; ba.second += n - wins;
                    }
                    else if (kind == 1) pairs.push_back({version, cls, rival, n, wins});
                    else if (kind == 2) eligible.insert({version, cls});
                } while (r->NextRow());
                struct Acc { uint32 n = 0, wins = 0; float expected = 0; bool missing = false; };
                std::map<std::pair<uint32, uint8>, Acc> acc;
                for (Pair const& p : pairs)
                {
                    if (!eligible.count({p.version, p.cls})) continue;
                    auto& a = acc[{p.version, p.cls}];
                    a.n += p.n; a.wins += p.wins;
                    auto it = ref.find({p.cls, p.rival});
                    if (it == ref.end() || it->second.first < 20) a.missing = true;
                    else a.expected += float(p.n) * float(it->second.second) / float(it->second.first);
                }
                auto logit = [](float w) { w = std::clamp(w, 0.05f, 0.95f); return std::log10(w / (1.0f - w)); };
                std::map<uint8, std::vector<LadderRung>> built;
                for (uint8 cls = 1; cls <= 11; ++cls)
                    if (cls != 10) built[cls].push_back(LadderRung());
                for (auto const& [key, a] : acc)
                {
                    if (a.n < minimum || a.missing) continue;
                    LadderRung rung;
                    rung.version = key.first; rung.matches = a.n;
                    rung.winrate = float(a.wins) / float(a.n);
                    rung.rating = 1500.0f + 400.0f * (logit(rung.winrate) - logit(a.expected / float(a.n)));
                    built[key.second].push_back(rung);
                }
                if (!since)
                    for (auto const& [cls, saved] : s_ladderSaved)
                        for (LadderRung const& old : saved)
                        {
                            if (!old.version || !eligible.count({old.version, cls})) continue;
                            auto& list = built[cls];
                            if (std::none_of(list.begin(), list.end(), [&](LadderRung const& x) { return x.version == old.version; }))
                                list.push_back(old);
                        }
                uint32 count = 0;
                for (auto& [cls, list] : built)
                {
                    std::sort(list.begin(), list.end(), [](LadderRung const& x, LadderRung const& y) { return x.rating > y.rating; });
                    count += uint32(list.size() - 1);
                }
                SaveLadder(built);   // CommitTransaction encola la escritura
                {
                    std::unique_lock<std::shared_mutex> lguard(s_ladderLock);
                    s_ladder.swap(built);
                }
                LOG_INFO("module", "[adaptive-ai] Escalera asincrona: {} peldanos, {} ms hasta respuesta, {} ms publicacion en mundo.",
                         count, getMSTime() - now, getMSTime() - applyStart);
            }));
        }

        uint32 TimeoutMs(Match const& m) { return (m.isBg ? cfg.bgTimeoutSecs : cfg.arenaTimeoutSecs) * 1000; }
        uint32 MaxMs(Match const& m)     { return m.isBg ? cfg.bgMaxMinutes * 60000 : cfg.arenaMaxSecs * 1000; }
#endif // ADAPTIVE_WITH_PLAYERBOTS
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  API pública
    // ─────────────────────────────────────────────────────────────────────────
    bool IsTrainingBot(ObjectGuid guid)
    {
        std::shared_lock<std::shared_mutex> guard(s_trainingLock);
        return s_trainingBots.count(guid) != 0;
    }

    // Al apagar: el examen que un reinicio pillaba a medias se perdía entero
    // (el reinicio diario de las 00:00 cae justo al final de la ventana de las
    // 23:00). Si ya lleva al menos media ventana, se concluye con lo que hay,
    // que es el mismo criterio que aplica el tope de minutos.
    void ArenaShutdown()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        ++s_ladderRequest;
        s_ladderPending = false;
        if (!s_calib.running)
            return;
        time_t clock = GameTime::GetGameTime().count();
        time_t span = s_calib.endsAt ? s_calib.endsAt - s_calib.startedAt : time_t(cfg.calibrateMaxMinutes * 60);
        if (span <= 0 || clock - s_calib.startedAt < span / 2)
        {
            Trace(Acore::StringFormat("Apagado con el examen a medias ({} de {} min): se descarta.",
                                      uint32((clock - s_calib.startedAt) / 60), uint32(span / 60)));
            return;
        }
        uint32 decided = s_calib.candidateWins + s_calib.validatedWins;
        Trace(Acore::StringFormat("Apagado con el examen a los {} de {} min y {} partidas: se concluye con lo que hay.",
                                  uint32((clock - s_calib.startedAt) / 60), uint32(span / 60), s_calib.finished));
        s_calib.total = std::max<uint32>(2, decided);
        ConcludeCalibration();
    }

    void ArenaInit()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        ++s_ladderRequest;
        s_ladderPending = false;
        if (QueryResult result = PlayerbotsDatabase.Query("SELECT IFNULL(MAX(id), 0) FROM adaptive_match"))
            s_nextMatchId = result->Fetch()[0].Get<uint32>() + 1;
        s_autoEnabled = true;
        if (auto c = Candidate())
            s_updatesAtLastCalib = c->Updates();
        s_approved.clear();
        s_round = 1;
        s_calibAcc.clear();
        s_classCycles.clear();
        if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT clase, aprobada, ronda, cand_gana, cand_pierde, val_gana, val_pierde, ciclos, historial FROM adaptive_clase"))
            do
            {
                Field* f = result->Fetch();
                uint8 cls = f[0].Get<uint8>();
                s_round = std::max<uint32>(s_round, f[2].Get<uint32>());
                if (!cls)
                {
                    // La fila de la clase 0 lleva la ronda y, en su columna de
                    // ciclos, cuántos exámenes van en ella (Calibrar.RondaMaxExamenes)
                    s_classCycles[0] = f[7].Get<uint32>();
                    continue;
                }
                if (f[1].Get<uint8>())
                    s_approved.insert(cls);
                // La ventana, examen a examen. De una base anterior al 04/09
                // (sin historial) se recupera el acumulado como un solo examen.
                std::string history = f[8].Get<std::string>();
                std::deque<ClassWL> window;
                size_t at = 0;
                while (at < history.size())
                {
                    size_t comma = history.find(',', at);
                    std::string one = history.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    at = comma == std::string::npos ? history.size() : comma + 1;
                    ClassWL w;
                    if (std::sscanf(one.c_str(), "%u:%u:%u:%u", &w.cw, &w.cl, &w.vw, &w.vl) == 4)
                        window.push_back(w);
                }
                while (window.size() > std::max<size_t>(1, cfg.calibrateWindow))
                    window.pop_front();
                if (window.empty())
                {
                    ClassWL acc;
                    acc.cw = f[3].Get<uint32>();
                    acc.cl = f[4].Get<uint32>();
                    acc.vw = f[5].Get<uint32>();
                    acc.vl = f[6].Get<uint32>();
                    if (acc.cw || acc.cl || acc.vw || acc.vl)
                        window.push_back(acc);
                }
                if (!window.empty())
                    s_calibAcc[cls] = window;
                if (uint32 cycles = f[7].Get<uint32>())
                    s_classCycles[cls] = cycles;
            } while (result->NextRow());
        RefreshGenerations();
        LoadLadder();
        if (!s_approved.empty() || !s_generations.empty())
            LOG_INFO("module", "[adaptive-ai] Ronda {}: {} clases aprobadas, {} generaciones disponibles.", s_round, s_approved.size(), s_generations.size());
    }

    // Para RevertClass: al devolverle a una clase las filas de la validada, su
    // historial de exámenes ya no describe al modelo que va a jugar.
    void ArenaResetClassHistory(uint8 cls)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        ResetAccumulated(cls);
        SaveApproval(cls, s_approved.count(cls) > 0, 0);
    }

    std::vector<LadderRung> LadderFor(uint8 cls)
    {
        std::shared_lock<std::shared_mutex> guard(s_ladderLock);
        auto it = s_ladder.find(cls);
        if (it == s_ladder.end())
            return std::vector<LadderRung>{ LadderRung() };   // sin medida, solo la serie
        return it->second;
    }

    float LadderRating(uint8 cls, uint32 version)
    {
        for (LadderRung const& r : LadderFor(cls))
            if (r.version == version)
                return r.rating;
        return 1500.0f;
    }

    std::string LadderStatus()
    {
        uint32 candidateVersion = 0;
        if (std::shared_ptr<QTable> c = Candidate())
            candidateVersion = c->Version();
        std::shared_lock<std::shared_mutex> guard(s_ladderLock);
        std::string out;
        for (auto const& [cls, rungs] : s_ladder)
        {
            std::string line;
            for (LadderRung const& r : rungs)
                line += Acore::StringFormat("{}{} {:.0f}{}", line.empty() ? "" : " > ",
                                            r.version ? "v" + std::to_string(r.version) + (r.version == candidateVersion ? " (candidata, no sale al mundo)" : "") : std::string("serie"),
                                            r.rating, r.version ? Acore::StringFormat(" ({:.0f}% de {})", r.winrate * 100.0f, r.matches) : std::string());
            out += Acore::StringFormat("{}  {}: {}", out.empty() ? "" : "\n", ClassName(cls), line);
        }
        return out.empty() ? "  todavia sin medidas (AdaptiveAI.Arena.Escalera)" : out;
    }

    bool ClassApproved(uint8 cls)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        return s_approved.count(cls) > 0;
    }

    std::string ApprovalStatus()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        std::string approved, pending;
        for (uint8 cls : TrainingClasses())
            (s_approved.count(cls) ? approved : pending) += (s_approved.count(cls) ? (approved.empty() ? "" : ", ") : (pending.empty() ? "" : ", ")) + std::string(ClassName(cls));
        std::string gens;
        for (uint32 v : s_generations)
            gens += (gens.empty() ? "v" : ", v") + std::to_string(v);
        return Acore::StringFormat("Ronda {}: aprobadas {}; entrenando {}. Generaciones: {} ({} % de las partidas automaticas)",
                                   s_round, approved.empty() ? "ninguna" : approved, pending.empty() ? "ninguna" : pending,
                                   gens.empty() ? "ninguna" : gens, cfg.arenaGenerationsPct);
    }

#ifdef ADAPTIVE_WITH_PLAYERBOTS
    void ArenaTick(uint32 diff)
    {
        s_tickAccum += diff;
        if (s_tickAccum < 1000)
            return;
        uint32 elapsed = s_tickAccum;
        s_tickAccum = 0;

        std::lock_guard<std::recursive_mutex> guard(s_lock);
        uint32 now = getMSTime();
        s_dbQueue.ProcessReadyCallbacks();   // objetivos y escalera, sin esperar al hilo de BD

        // Tiempos de espera, objetivos y limpieza. Va aquí y no en
        // OnBattlegroundUpdate porque el core no llama a ese hook mientras la
        // instancia está vacía (y si los bots nunca llegan, está vacía).
        for (auto it = s_matches.begin(); it != s_matches.end();)
        {
            Match& m = it->second;
            Battleground* bg = sBattlegroundMgr->GetBattleground(m.instanceId, BattlegroundTypeId(m.bgTypeId));
            if (!bg)
            {
                if (!m.ended)
                    FinishMatch(m, -2, "instancia desaparecida");
                DisbandGroups(m);
                for (int s = 0; s < 2; ++s)
                    for (Fighter& f : m.side[s])
                    {
                        EndBrain(f.guid, 0.0f);
                        if (Player* p = ObjectAccessor::FindPlayer(f.guid))
                            if (p->GetBattlegroundId() == m.instanceId && !p->IsBeingTeleported())
                                p->SetBattlegroundId(0, BATTLEGROUND_TYPE_NONE, PLAYER_MAX_BATTLEGROUND_QUEUES, false, false, TEAM_NEUTRAL);
                        ReleaseFighter(f);
                    }
                it = s_matches.erase(it);
                continue;
            }
            if (!m.started && !m.ended && now > m.createdMs + TimeoutMs(m))
            {
                FinishMatch(m, -2, "no llegaron todos los bots");
                IssueLeave(m, bg);
            }
            else if (m.started && !m.ended && now > m.startedMs + MaxMs(m))
            {
                FinishMatch(m, -1, "tiempo maximo");
                IssueLeave(m, bg);
            }
            else if (m.ended && !m.leaveIssued && now > m.endedMs + 3000)
                IssueLeave(m, bg);
            else if (m.isBg && m.started && !m.ended && now >= m.lastScoreMs + 5000)
            {
                m.lastScoreMs = now;
                ScoreObjectives(m, bg);
            }
            ++it;
        }

        // El examen dura Calibrar.MinutosMaximos y concluye con lo que haya
        // medido: así el ciclo es fijo (una hora entrenando, una midiendo)
        // aunque la capacidad de arenas cambie.
        time_t clock = GameTime::GetGameTime().count();

        // Corte anticipado: en cuanto TODAS las clases que se juzgan tienen ya
        // partidas de sobra por los dos lados, el resto de la ventana vuelve al
        // entrenamiento. Con Exclusiva = 1 el examen se come una hora de cada
        // dos y el mínimo por clase se alcanza en 15-20 min: los otros 40 son
        // arenas que no entrenan y no cambian el veredicto. La ventana ya está
        // marcada como atendida (s_lastExamWindow), así que no se relanza.
        std::string leftOut;
        if (s_calib.running && cfg.calibrateCutWhenJudged && AllJudgedClassesReady(&leftOut))
        {
            uint32 decided = s_calib.candidateWins + s_calib.validatedWins;
            Trace(Acore::StringFormat("Examen: cortado a los {} min de {}, todas las clases ya se pueden juzgar ({} partidas terminadas){}.",
                                      uint32((clock - s_calib.startedAt) / 60),
                                      uint32(s_calib.endsAt ? (s_calib.endsAt - s_calib.startedAt) / 60 : cfg.calibrateMaxMinutes),
                                      s_calib.finished,
                                      leftOut.empty() ? std::string() : "; sin esperar a " + leftOut + ", que ni con todas sus arenas llegaba y se juzga con su acumulado"));
            s_calib.total = std::max<uint32>(2, decided);
            ConcludeCalibration();
        }
        if (s_calib.running && s_calib.endsAt && clock >= s_calib.endsAt)
        {
            uint32 decided = s_calib.candidateWins + s_calib.validatedWins;
            Trace(Acore::StringFormat("Examen: se cumple la hora ({} min) con {} de {} partidas terminadas.",
                                      uint32((s_calib.endsAt - s_calib.startedAt) / 60), s_calib.finished, s_calib.total));
            s_calib.total = std::max<uint32>(2, decided);   // el criterio global se mide sobre lo jugado
            ConcludeCalibration();
        }

        // Calibración automática. Por el reloj del sistema (Calibrar.Reloj): el
        // día se parte en periodos de CadaMinutos + MinutosMaximos desde la
        // medianoche local, y los últimos MinutosMaximos de cada periodo son
        // de examen (con 60 y 60: de una a dos, de tres a cuatro...). Así el
        // examen no depende de cuándo se reinició el servidor: antes el
        // temporizador arrancaba de cero con cada reinicio y un día con siete
        // reinicios se quedó sin ningún examen (03/09/2026). Un reinicio en
        // mitad de una ventana retoma el examen si queda al menos un tercio;
        // si no, espera a la siguiente. Cada ventana se atiende una sola vez.
        if (cfg.enabled && cfg.arenaEnable && cfg.calibrateMinutes && !s_calib.running)
        {
            if (cfg.calibrateClock && cfg.calibrateMaxMinutes)
            {
                uint32 period = cfg.calibrateMinutes + cfg.calibrateMaxMinutes;
                tm lt;
                localtime_r(&clock, &lt);
                uint32 minuteOfDay = uint32(lt.tm_hour) * 60 + uint32(lt.tm_min);
                uint32 inPeriod = minuteOfDay % period;
                if (inPeriod >= cfg.calibrateMinutes)
                {
                    time_t windowStart = clock - time_t((inPeriod - cfg.calibrateMinutes) * 60 + uint32(lt.tm_sec));
                    time_t windowEnd = windowStart + time_t(cfg.calibrateMaxMinutes * 60);
                    if (windowStart != s_lastExamWindow)
                    {
                        tm ws;
                        localtime_r(&windowStart, &ws);
                        std::string label = Acore::StringFormat("{:02}:{:02}", ws.tm_hour, ws.tm_min);
                        uint32 left = uint32((windowEnd - clock) / 60);
                        std::shared_ptr<QTable> c = Candidate();
                        if (left < std::max<uint32>(1, cfg.calibrateMaxMinutes / 3))
                        {
                            s_lastExamWindow = windowStart;
                            Trace(Acore::StringFormat("Examen de las {}: solo quedan {} min de su ventana (arranque reciente), se espera al siguiente.", label, left));
                        }
                        // Sin decisiones nuevas suficientes NO se da la ventana por
                        // atendida: se reintenta en los ticks siguientes y el examen
                        // empieza en cuanto las haya. Marcarla aquí es lo que hizo que
                        // un reinicio a las 13:00:45 (candidata recién cargada, 0
                        // decisiones) se llevara por delante toda la ventana de las
                        // 13:00 del 04/09/2026. El aviso sale una sola vez.
                        else if (!c || c->Updates() - s_updatesAtLastCalib < cfg.calibrateMinNew)
                        {
                            if (windowStart != s_lastExamSkipLog)
                            {
                                s_lastExamSkipLog = windowStart;
                                Trace(Acore::StringFormat("Examen de las {}: esperando decisiones nuevas ({} de {}); empezara en cuanto las haya.",
                                                          label, c ? c->Updates() - s_updatesAtLastCalib : 0, cfg.calibrateMinNew));
                            }
                        }
                        else
                        {
                            s_lastExamWindow = windowStart;
                            std::string error;
                            if (CalibrationStart(error))
                            {
                                s_calib.endsAt = windowEnd;   // termina con la ventana, no una hora después de empezar
                                Trace(Acore::StringFormat("Examen de las {}: empieza ahora, hasta las {:02}:{:02} ({} min).",
                                                          label, (ws.tm_hour + (ws.tm_min + cfg.calibrateMaxMinutes) / 60) % 24,
                                                          (ws.tm_min + cfg.calibrateMaxMinutes) % 60, left));
                            }
                            else
                                Trace(Acore::StringFormat("Examen de las {}: no se pudo empezar ({}).", label, error));
                        }
                    }
                }
            }
            else
            {
                s_calibTimerMs += elapsed;
                if (s_calibTimerMs >= cfg.calibrateMinutes * 60000)
                {
                    s_calibTimerMs = 0;
                    std::shared_ptr<QTable> c = Candidate();
                    if (c && c->Updates() - s_updatesAtLastCalib >= cfg.calibrateMinNew)
                    {
                        std::string error;
                        CalibrationStart(error);
                    }
                }
            }
        }

        bool yield = cfg.arenaYield && HumanInPvpQueue();
        bool humanOnline = HumanOnline();
        if (now >= s_generationsRefreshMs + 5 * 60000)
        {
            uint32 t0 = getMSTime();
            RefreshGenerations();
            WarnIfSlow("las generaciones", t0);
        }

        // Escalera y objetivos: encolar consultas cada diez minutos. Las
        // respuestas se publican desde ProcessReadyCallbacks, sin esperar a BD.
        s_ladderTimerMs += elapsed;
        if (s_ladderTimerMs >= 10 * 60000)
        {
            s_ladderTimerMs = 0;
            uint32 t0 = getMSTime();
            RefreshLadder();
            WarnIfSlow("la escalera", t0);
        }

        s_objectivesTimerMs += elapsed;
        if (s_objectivesTimerMs >= 10 * 60000)
        {
            s_objectivesTimerMs = 0;
            uint32 t0 = getMSTime();
            CheckObjectives();
            WarnIfSlow("los objetivos", t0);
        }

        // Campo de batalla automático
        if (cfg.enabled && cfg.bgEnable && s_autoEnabled && !cfg.bgTypes.empty() && cfg.bgEveryMinutes)
        {
            s_bgTimerMs += elapsed;
            if (s_bgTimerMs >= cfg.bgEveryMinutes * 60000 && !yield && RunningBgs() < cfg.bgSimultaneous &&
                !(cfg.calibrateExclusive && s_calib.running) &&   // durante el examen, las arenas son para medir
                (!humanOnline || RunningBgs() < cfg.bgWithPlayer))
            {
                s_bgTimerMs = 0;
                uint8 type = cfg.bgTypes[s_bgTypeIndex % cfg.bgTypes.size()];
                ++s_bgTypeIndex;
                std::string error;
                if (!LaunchBattleground(type, 0, SOURCE_AUTO, cfg.bgMode, error))
                    LOG_INFO("module", "[adaptive-ai] Campo de batalla automatico ({}): {}.", BgName(type), error);
            }
        }

        if (now < s_lastLaunchMs + cfg.arenaPauseSecs * 1000)
            return;

        uint32 cap = std::max<uint32>(1, cfg.arenaSimultaneous);
        // Lanzamientos por tick (el tick es de 1 s). Con dos fijos, un aforo
        // grande no llega a llenarse: 90 arenas de ~60 s piden 1,5 lanzamientos
        // por segundo, y las series del examen comparten este contador. Se
        // escala con el aforo, con el mínimo de 2 que había (04/09/2026).
        uint32 const perTick = std::max<uint32>(2, cap / 30);
        uint32 launched = 0;

        // 1) series (calibración primero, luego manuales)
        if (s_calib.running)
        {
            // Durante el examen, la pareja cuya clase mas atrasada va mas atrasada
            // primero (reparto por clase, 05/09/2026): asi una clase con 3
            // rivales recibe tantas arenas como una con 7. El cursor no hace
            // falta: el orden ya reparte, y se recalcula en cada tick.
            uint32 need = cfg.calibrateCutMatches ? cfg.calibrateCutMatches : cfg.calibratePerClassMin;
            std::map<uint8, float> progress;
            for (Series const& s : s_series)
                if (s.source == SOURCE_CALIBRATION)
                    for (auto const& side : { std::cref(s.comp.a), std::cref(s.comp.b) })
                        for (ClassSpec const& cs : side.get())
                            if (cs.cls && !progress.count(cs.cls))
                                progress[cs.cls] = ClassProgress(cs.cls, need);
            std::stable_sort(s_series.begin(), s_series.end(), [&](Series const& x, Series const& y)
            {
                bool cx = x.source == SOURCE_CALIBRATION, cy = y.source == SOURCE_CALIBRATION;
                if (cx != cy)
                    return cx > cy;
                if (!cx)
                    return false;
                return SeriesProgress(x, progress) < SeriesProgress(y, progress);
            });
            s_seriesCursor = 0;
        }
        else
            std::stable_sort(s_series.begin(), s_series.end(), [](Series const& x, Series const& y)
            {
                return (x.source == SOURCE_CALIBRATION) > (y.source == SOURCE_CALIBRATION);
            });
        // El recorrido empieza donde lo dejo el tick anterior, no siempre por la
        // primera. El examen crea una serie por pareja (45) con Calibrar.PorPareja
        // arenas a la vez; si el producto pasa del aforo real (Arena.BotsMax / 2
        // = 100 arenas de 1c1), las ultimas de la lista no arrancaban NI UNA VEZ
        // en los 45 minutos, siempre las mismas. Y como Arena.Pares va agrupado
        // por clase, el bloque que se quedaba fuera podia llevarse 8 de las 9
        // parejas de una clase: el 04/09/2026 el paladin jugo 40 lados y el
        // caballero de la muerte 397, y sin sus 100 por lado el examen no pudo
        // juzgarlo ni cortarse. Con el cursor, todas las series avanzan a la par.
        size_t const nSeries = s_series.size();
        size_t seen = 0;
        for (; seen < nSeries; ++seen)
        {
            Series& s = s_series[(s_seriesCursor + seen) % nSeries];
            if (launched >= perTick || RunningArenas() >= cap)
                break;
            if (!s.remaining || RunningOfSeries(s.id) >= s.simultaneous)
                continue;
            if (s.source == SOURCE_CALIBRATION && !CompStillUseful(s.comp))
                continue;   // las dos clases de la pareja ya cumplen: el hueco es para otra
            if (!ArenaRoom(s.comp.Size()))
                break;   // tope de bots en arenas: se reintenta cuando acabe alguna
            if (s.source == SOURCE_AUTO && !WithPlayerAllows(s.comp.Size(), humanOnline))
                continue;
            if (yield && s.source == SOURCE_AUTO)
                continue;
            std::string error;
            if (LaunchArena(s.comp, s.source, s.id, s.mode, error))
            {
                --s.remaining;
                ++s.launched;
                ++launched;
                if (s.source == SOURCE_CALIBRATION)
                    ++s_calib.launched;
            }
            else
            {
                LOG_DEBUG("module", "[adaptive-ai] Serie {}: {}.", s.id, error);
                break;   // sin bots libres ahora: se reintenta en el siguiente tick
            }
        }
        // Se avanza SIEMPRE, tambien cuando el bucle corto por aforo o por falta
        // de bots: si no, la serie que corta seria la nueva que bloquea a todas.
        if (nSeries)
            s_seriesCursor = (s_seriesCursor + seen + 1) % nSeries;
        s_series.erase(std::remove_if(s_series.begin(), s_series.end(), [](Series const& s)
        {
            return !s.remaining && s.launched == s.finished + s.aborted && s.source != SOURCE_CALIBRATION;
        }), s_series.end());

        // 2) entrenamiento automático: las composiciones permitidas, en rueda.
        // Mientras hay examen no se lanza nada (Calibrar.Exclusiva): así el
        // examen ocupa todas las arenas y una hora da 150 partidas por clase y
        // lado en vez de 18. Se entrena una hora, se mide la siguiente.
        if (cfg.enabled && cfg.arenaEnable && s_autoEnabled && !yield && !cfg.arenaPairs.empty() &&
            !(cfg.calibrateExclusive && s_calib.running))
        {
            // Arenas de equipo a la vez (AdaptiveAI.Arena.EquipoSimultaneas): el
            // grueso del entrenamiento va a 1c1 y las de equipo se turnan entre
            // ellas. Cada grupo lleva SU rueda: con una sola rueda, el 2c2 (que
            // va delante en la lista) se llevaba todos los turnos y 3c3 y 5c5 no
            // se lanzaban nunca (03/09/2026: 319 duelos, 7 de 2c2, 0 de las otras).
            uint32 teamRunning = 0;
            for (auto const& [id, m] : s_matches)
                if (!m.isBg && m.size > 1 && !m.ended)
                    ++teamRunning;
            std::vector<CompSpec const*> solo, team;
            for (CompSpec const& comp : cfg.arenaPairs)
                if (CompAllowed(comp) && WithPlayerAllows(comp.Size(), humanOnline))
                    (comp.Size() == 1 ? solo : team).push_back(&comp);
            while ((!solo.empty() || !team.empty()) && launched < perTick && RunningArenas() < cap)
            {
                // Si hay hueco de equipo, la siguiente de su rueda; si no, un duelo
                bool wantTeam = !team.empty() && teamRunning < cfg.arenaTeamSimultaneous;
                if (!wantTeam && solo.empty())
                    break;
                CompSpec const& comp = wantTeam ? *team[s_autoTeamIndex % team.size()]
                                                : *solo[s_autoPairIndex % solo.size()];
                if (!ArenaRoom(comp.Size()))
                    break;
                if (wantTeam)
                {
                    ++s_autoTeamIndex;
                    ++teamRunning;
                }
                else
                    ++s_autoPairIndex;
                // Una parte de las partidas es serie contra serie: la línea base
                // con la que ".adaptive" compara el contraste
                std::string rule = cfg.arenaMode;
                uint32 roll = urand(1, 100);
                if (cfg.arenaReferencePct && roll <= cfg.arenaReferencePct)
                    rule = "referencia";
                else if (cfg.arenaGenerationsPct && !s_generations.empty() && roll <= cfg.arenaReferencePct + cfg.arenaGenerationsPct)
                    rule = "generacion";   // contra una generación anterior: variedad, que no se estanque
                else if (cfg.arenaLadderPct && roll <= cfg.arenaReferencePct + cfg.arenaGenerationsPct + cfg.arenaLadderPct)
                    rule = "escalera";     // un peldaño contra la serie: la medida que coloca la escalera
                else if (cfg.arenaProbePct && roll <= cfg.arenaReferencePct + cfg.arenaGenerationsPct + cfg.arenaLadderPct + cfg.arenaProbePct)
                    rule = "sonda";        // la candidata sin explorar contra la serie: su peldaño (N2+N6)
                std::string error;
                if (!LaunchArena(comp, SOURCE_AUTO, 0, rule, error))
                {
                    LOG_DEBUG("module", "[adaptive-ai] Entrenamiento automatico ({}): {}.", comp.Name(), error);
                    break;
                }
                ++launched;
            }
        }
    }

    uint32 ArenaLaunchSeries(CompSpec const& comp, uint32 count, uint32 simultaneous, std::string const& mode, std::string& error)
    {
        if (mode != "entrenar" && mode != "contraste" && mode != "mixto" && mode != "referencia" && mode != "generacion" && mode != "escalera")
        {
            error = "el modo tiene que ser entrenar, contraste, mixto, referencia, generacion o escalera";
            return 0;
        }
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        Series s;
        s.id = s_nextSeriesId++;
        s.comp = comp;
        s.mode = mode;
        s.source = SOURCE_MANUAL;
        s.remaining = std::max<uint32>(1, count);
        s.simultaneous = std::clamp<uint32>(simultaneous, 1, std::max<uint32>(1, cfg.arenaSimultaneous));
        s_series.push_back(s);
        s_lastLaunchMs = 0;   // que salga ya
        return s.id;
    }

    bool BgLaunch(uint8 bgType, uint32 perTeam, std::string const& mode, std::string& error)
    {
        if (mode != "entrenar" && mode != "contraste" && mode != "mixto" && mode != "referencia" && mode != "generacion")
        {
            error = "el modo tiene que ser entrenar, contraste, mixto, referencia o generacion";
            return false;
        }
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        return LaunchBattleground(bgType, perTeam, SOURCE_MANUAL, mode, error);
    }

    uint32 ArenaStopAll()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        s_series.clear();
        s_calib = Calibration();
        uint32 stopped = 0;
        for (auto& [id, m] : s_matches)
        {
            if (m.ended)
                continue;
            if (Battleground* bg = sBattlegroundMgr->GetBattleground(m.instanceId, BattlegroundTypeId(m.bgTypeId)))
            {
                FinishMatch(m, -2, "parada por comando");
                IssueLeave(m, bg);
                ++stopped;
            }
        }
        return stopped;
    }

    ArenaStatus ArenaGetStatus()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        ArenaStatus st;
        uint32 now = getMSTime();
        st.running = RunningArenas();
        st.bgRunning = RunningBgs();
        st.lines.push_back(Acore::StringFormat("  bots en arenas: {} de {} (AdaptiveAI.Arena.BotsMax){}", BotsInArenas(), cfg.arenaBotsMax,
            HumanOnline() && !cfg.arenaWithPlayer.empty() ? "; hay un jugador conectado: el automatico se limita a " + WithPlayerText() + " (AdaptiveAI.Arena.ConJugador)" : ""));
        st.finishedToday = s_finishedToday;
        for (auto const& [id, m] : s_matches)
        {
            std::string phase = m.ended ? "terminada" : (m.started ? "en curso" : Acore::StringFormat("esperando {}/{}", ArrivedCount(m), m.size * 2));
            uint32 secs = (now - (m.started ? m.startedMs : m.createdMs)) / 1000;
            st.lines.push_back(Acore::StringFormat("  #{} {} {} {}({}) vs {}({}) - {} {} s [{}]",
                m.id, SourceName(m.source), m.TypeName(), SideNames(m, 0), ModeName(m.sideMode[0]), SideNames(m, 1), ModeName(m.sideMode[1]),
                phase, secs, m.mapId));
        }
        for (Series const& s : s_series)
        {
            st.waiting += s.remaining;
            st.lines.push_back(Acore::StringFormat("  serie {} ({}, {}): {} pendientes, {} lanzadas, {} terminadas, candidata {} - {} otro, {} empates, {} abortadas",
                s.id, s.comp.Name(), s.mode, s.remaining, s.launched, s.finished, s.wins[0], s.wins[1], s.draws, s.aborted));
        }
        if (!s_autoEnabled)
            st.lines.push_back("  entrenamiento automatico: parado por comando (.adaptive arena auto on)");
        else if (cfg.arenaYield && HumanInPvpQueue())
            st.lines.push_back("  entrenamiento automatico: cediendo los bots a un jugador en cola PvP");
        if (cfg.bgEnable && cfg.bgEveryMinutes)
            st.lines.push_back(Acore::StringFormat("  campos de batalla automaticos: siguiente en {} min", (cfg.bgEveryMinutes * 60000 - std::min(s_bgTimerMs, cfg.bgEveryMinutes * 60000)) / 60000));
        return st;
    }

    bool CalibrationStart(std::string& error)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        if (s_calib.running)
        {
            error = "ya hay una calibracion en marcha";
            return false;
        }
        std::shared_ptr<QTable> c = Candidate();
        std::shared_ptr<QTable> v = Validated();
        if (!c || !v)
        {
            error = "no hay modelos cargados";
            return false;
        }
        // El examen solo con los tamaños de AdaptiveAI.Calibrar.Tipos (1c1 por
        // defecto): en un duelo el resultado se atribuye a una clase, en un 5c5
        // no se sabe de quién fue el mérito, y el aprobado va por clase. El
        // equipo se sigue midiendo con las partidas de contraste del automático,
        // que salen en el informe sin gastar examen.
        // Parejas sin señal: donde playerbots DE SERIE ya gana (o pierde) casi
        // todas, la candidata y la validada no se pueden distinguir, y el
        // examen gasta ahí sus combates para nada. El caballero de la muerte
        // gana el 97-100 % a ocho clases en referencia (serie contra serie,
        // 04/09/2026, y con el ilvl medio más bajo de las diez), así que sus
        // parejas no dicen nada de lo aprendido; el brujo, por abajo, lo mismo.
        // Se miran las de referencia, que son las que no llevan modelo en
        // ninguno de los dos lados.
        std::set<std::pair<uint8, uint8>> saturated;
        if (cfg.calibrateSkipSaturated && s_saturatedValid && s_saturatedRound == s_round)
            saturated = s_saturated;   // congelada por ronda (05/09/2026): la misma mezcla de rivales en toda la ventana
        else if (cfg.calibrateSkipSaturated)
        {
            std::map<std::pair<uint8, uint8>, std::pair<uint32, uint32>> ref;   // (menor, mayor) -> (partidas, gana el menor)
            // Solo la referencia reciente: la de antes de un cambio de equipo,
            // spec o premio describe otro juego.
            if (QueryResult r = PlayerbotsDatabase.Query(
                "SELECT clase_a, clase_b, COUNT(*), SUM(ganador = 'a') FROM adaptive_match "
                "WHERE tipo = '1c1' AND modo_a = 'ninguna' AND modo_b = 'ninguna' AND ganador IN ('a','b') "
                "AND clase_a > 0 AND clase_b > 0 AND fecha > NOW() - INTERVAL 3 DAY GROUP BY clase_a, clase_b"))
            {
                do
                {
                    Field* f = r->Fetch();
                    uint8 x = f[0].Get<uint8>(), y = f[1].Get<uint8>();
                    uint32 n = uint32(f[2].Get<uint64>()), winsX = uint32(f[3].Get<double>());
                    if (x == y)
                        continue;
                    std::pair<uint8, uint8> key = x < y ? std::make_pair(x, y) : std::make_pair(y, x);
                    auto& acc = ref[key];
                    acc.first += n;
                    acc.second += (x < y) ? winsX : (n - winsX);   // siempre las victorias de la clase menor
                } while (r->NextRow());
            }
            uint32 withReference = 0;
            for (auto const& [key, acc] : ref)
            {
                if (acc.first < 20)
                    continue;   // sin referencia suficiente no se descarta nada
                ++withReference;
                uint32 pct = 100 * acc.second / acc.first;
                if (pct <= cfg.calibrateSkipSaturated || pct + cfg.calibrateSkipSaturated >= 100)
                    saturated.insert(key);
            }
            // Solo se congela cuando hay referencia con la que decidir: recien
            // puesto a cero (05/09/2026 09:27) la lista salia vacia y se quedaba
            // asi para toda la ronda.
            s_saturated = saturated;
            s_saturatedRound = s_round;
            s_saturatedValid = withReference >= 10;
            Trace(Acore::StringFormat("Examen: parejas sin senal recalculadas para la ronda {} ({} parejas, {} con referencia){}.", s_round, saturated.size(), withReference,
                     s_saturatedValid ? "; no cambian hasta la ronda siguiente" : "; se recalculan en el proximo examen (poca referencia)"));
        }

        std::vector<CompSpec const*> allowed;
        std::string skipped;
        uint32 skippedCount = 0;
        for (CompSpec const& comp : cfg.arenaPairs)
        {
            if (!CompAllowed(comp) ||
                std::find(cfg.calibrateTypes.begin(), cfg.calibrateTypes.end(), comp.Size()) == cfg.calibrateTypes.end())
                continue;
            if (comp.Size() == 1 && comp.a[0].cls && comp.b[0].cls)
            {
                uint8 x = comp.a[0].cls, y = comp.b[0].cls;
                if (saturated.count(x < y ? std::make_pair(x, y) : std::make_pair(y, x)))
                {
                    skipped += (skipped.empty() ? "" : ", ") + comp.Name();
                    ++skippedCount;
                    continue;
                }
            }
            allowed.push_back(&comp);
        }
        // Si el filtro se lo lleva todo (poblacion rara, referencia escasa), se
        // examina con todo antes que no examinar.
        if (allowed.empty() && !skipped.empty())
        {
            for (CompSpec const& comp : cfg.arenaPairs)
                if (CompAllowed(comp) &&
                    std::find(cfg.calibrateTypes.begin(), cfg.calibrateTypes.end(), comp.Size()) != cfg.calibrateTypes.end())
                    allowed.push_back(&comp);
            Trace("Examen: todas las parejas estaban saturadas; se examinan igual.");
            skipped.clear();
            skippedCount = 0;
        }
        if (allowed.empty())
        {
            error = "no hay composiciones permitidas (AdaptiveAI.Calibrar.Tipos, Arena.Pares, .Clases)";
            return false;
        }
        if (!skipped.empty())
            Trace(Acore::StringFormat("Examen: {} parejas sin senal (la serie gana mas del {} %) fuera del examen: {}.",
                     skippedCount, 100 - cfg.calibrateSkipSaturated, skipped));
        s_calib = Calibration();
        s_calib.running = true;
        s_calib.total = std::max<uint32>(2, cfg.calibrateMatches);
        s_calib.startedMs = getMSTime();
        s_calib.startedAt = GameTime::GetGameTime().count();
        s_calib.endsAt = cfg.calibrateMaxMinutes ? s_calib.startedAt + time_t(cfg.calibrateMaxMinutes * 60) : 0;
        uint32 perComp = std::max<uint32>(1, s_calib.total / uint32(allowed.size()));
        // El planificador recorre las series en orden y cada una podia ocupar el
        // aforo entero (simultaneous = Arena.Simultaneas): las primeras parejas
        // de Arena.Pares agotaban su cuota antes de que a las ultimas les
        // tocara, y en una hora el examen solo llegaba a la mitad de la lista
        // (04/09/2026: guerrero 574 partidas medidas, druida 137, y al druida
        // no se le pudo juzgar). Con una a la vez por pareja todas avanzan a la
        // par, y la lista rota con la hora de la ventana para que el orden no
        // favorezca siempre a las mismas (no se reinicia con el servidor).
        std::rotate(allowed.begin(), allowed.begin() + int32((s_calib.startedAt / 3600) % int64(allowed.size())), allowed.end());
        // Cuántas parejas 1c1 le quedan a cada clase en este examen. Con
        // SaltarSaturadas una clase puede quedarse con 1 de 9 (05/09/2026: el CdM)
        // y a PorPareja partidas a la vez no llega a PartidasPorClase en la
        // ventana (52+50 en 45 min): sus parejas corren con más partidas
        // simultáneas, en proporción (1 de 9 → ×9, tope ×8) y con más cuota, para
        // que le dé tiempo a juzgarse como a las demás.
        std::map<uint8, uint32> pairsOf;
        uint32 mostPairs = 0;
        for (CompSpec const* comp : allowed)
            if (comp->Size() == 1 && comp->a[0].cls && comp->b[0].cls)
            {
                ++pairsOf[comp->a[0].cls];
                ++pairsOf[comp->b[0].cls];
            }
        for (auto const& [cls, n] : pairsOf)
            mostPairs = std::max(mostPairs, n);
        std::string boosted;
        uint32 assigned = 0;
        for (CompSpec const* comp : allowed)
        {
            Series s;
            s.id = s_nextSeriesId++;
            s.comp = *comp;
            s.mode = "calibracion";
            s.source = SOURCE_CALIBRATION;
            uint32 factor = 1;
            if (comp->Size() == 1 && comp->a[0].cls && comp->b[0].cls && mostPairs)
            {
                uint32 fewest = std::max<uint32>(1, std::min(pairsOf[comp->a[0].cls], pairsOf[comp->b[0].cls]));
                factor = std::clamp<uint32>((mostPairs + fewest - 1) / fewest, 1, 8);
            }
            s.remaining = perComp * factor;
            s.simultaneous = std::max<uint32>(1, cfg.calibratePerPair) * factor;
            if (factor > 1)
                boosted += Acore::StringFormat("{}{} x{}", boosted.empty() ? "" : ", ", comp->Name(), factor);
            s_series.push_back(s);
            assigned += s.remaining;
        }
        if (!boosted.empty())
            Trace(Acore::StringFormat("Examen: parejas con mas partidas a la vez por tener pocos rivales: {}.", boosted));
        s_calib.total = assigned;
        s_calib.seriesId = s_series.back().id;
        s_lastLaunchMs = 0;
        Trace(Acore::StringFormat("Calibracion: {} combates candidata v{} contra validada v{}.", assigned, c->Version(), v->Version()));
        return true;
    }

    std::string ObjectivesStatus()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        if (cfg.matchupTargets.empty())
            return "objetivos: ninguno (AdaptiveAI.Objetivos)";
        return "objetivos: " + (s_objectivesText.empty() ? std::string("se comprueban al minuto del arranque y cada diez; la consulta va en segundo plano") : s_objectivesText);
    }

    bool CalibrationRunning()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        return s_calib.running;
    }

    void ArenaSetAuto(bool on)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        s_autoEnabled = on;
    }

    bool ArenaAutoEnabled()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        return s_autoEnabled;
    }

    std::string CalibrationStatus()
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        if (!s_calib.running)
        {
            uint32 have = Candidate() ? Candidate()->Updates() - s_updatesAtLastCalib : 0;
            if (cfg.calibrateClock && cfg.calibrateMinutes && cfg.calibrateMaxMinutes)
            {
                // Siguiente ventana de examen por el reloj (local)
                uint32 period = cfg.calibrateMinutes + cfg.calibrateMaxMinutes;
                time_t clock = GameTime::GetGameTime().count();
                tm lt;
                localtime_r(&clock, &lt);
                uint32 minuteOfDay = uint32(lt.tm_hour) * 60 + uint32(lt.tm_min);
                uint32 toNext = cfg.calibrateMinutes - std::min(minuteOfDay % period, cfg.calibrateMinutes);
                if (minuteOfDay % period >= cfg.calibrateMinutes)   // dentro de una ventana ya atendida: la siguiente
                    toNext = period - minuteOfDay % period + cfg.calibrateMinutes;
                uint32 at = (minuteOfDay + toNext) % 1440;
                return Acore::StringFormat("sin examen en marcha (siguiente a las {:02}:{:02} hora del servidor, en {} min; hacen falta {} decisiones nuevas, hay {})",
                                           at / 60, at % 60, toNext, cfg.calibrateMinNew, have);
            }
            return Acore::StringFormat("sin calibracion en marcha (siguiente automatica en {} min, hacen falta {} decisiones nuevas; hay {})",
                cfg.calibrateMinutes ? (cfg.calibrateMinutes * 60000 - std::min(s_calibTimerMs, cfg.calibrateMinutes * 60000)) / 60000 : 0,
                cfg.calibrateMinNew, have);
        }
        return Acore::StringFormat("calibracion en marcha: {}/{} combates, candidata {} - validada {}, {} empates",
            s_calib.finished, s_calib.total, s_calib.candidateWins, s_calib.validatedWins, s_calib.draws);
    }

    // ─── Hooks de campo de batalla ──────────────────────────────────────────
    // La cuenta atrás de verdad no empieza hasta que han llegado todos: el
    // core prepara la instancia con el primero que llega y, si abre las
    // puertas con un lado vacío, CheckWinConditions da la victoria al presente.
    void OnBgSetup(Battleground* bg)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        if (Match* m = FindMatch(bg))
            bg->SetStartDelayTime(int32((AllArrived(*m) ? cfg.arenaPrepSecs : (m->isBg ? cfg.bgTimeoutSecs : cfg.arenaTimeoutSecs)) * 1000));
    }

    void OnBgAddPlayer(Battleground* bg, Player* player)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        Match* m = FindMatch(bg);
        if (!m)
            return;
        int side = SideOf(*m, player->GetGUID());
        Fighter* f = FighterOf(*m, player->GetGUID());
        if (side < 0 || !f)
            return;
        f->arrived = true;
        Trace(Acore::StringFormat("Partida #{}: {} ha llegado a la instancia {} ({}/{}).", m->id, player->GetName(), m->instanceId,
                 ArrivedCount(*m), m->size * 2));
        if (f->invited)
        {
            bg->DecreaseInvitedCount(m->team[side]);
            f->invited = false;
        }
        if (AllArrived(*m) && bg->GetStatus() == STATUS_WAIT_JOIN)
            bg->SetStartDelayTime(int32(cfg.arenaPrepSecs * 1000));
    }

    // El core solo llama a OnBattlegroundStart en los campos de batalla, no en
    // las arenas (Battleground::_ProcessJoin): el arranque se detecta también
    // desde OnBgUpdate mirando el estado.
    static void StartMatch(Match* m, Battleground* bg)
    {
        if (!m || m->started || m->ended)
            return;
        std::vector<Player*> present[2];
        for (int s = 0; s < 2; ++s)
            for (Fighter const& f : m->side[s])
            {
                Player* p = ObjectAccessor::FindPlayer(f.guid);
                if (p && p->GetBattlegroundId() == m->instanceId)
                    present[s].push_back(p);
            }
        if (present[0].empty() || present[1].empty() || (m->size == 1 && (present[0].size() != 1 || present[1].size() != 1)))
        {
            FinishMatch(*m, -2, "falta un bot al empezar");
            IssueLeave(*m, bg);
            return;
        }
        m->started = true;
        m->startedMs = getMSTime();
        m->lastScoreMs = m->startedMs;
        Trace(Acore::StringFormat("Partida #{} ({}): puertas abiertas, {} ({}) contra {} ({}).", m->id, m->TypeName(),
                 SideNames(*m, 0), RecordedMode(m->side[0].front()), SideNames(*m, 1), RecordedMode(m->side[1].front())));
        std::vector<ObjectGuid> guids[2];
        for (int s = 0; s < 2; ++s)
            for (Player* p : present[s])
                guids[s].push_back(p->GetGUID());
        bool team = m->size > 1;
        for (int s = 0; s < 2; ++s)
            for (Player* p : present[s])
            {
                Fighter* f = FighterOf(*m, p->GetGUID());
                if (!f || (f->mode == BRAIN_NONE && !f->learn))
                    continue;
                Player* enemy = present[1 - s][urand(0, uint32(present[1 - s].size() - 1))];
                std::shared_ptr<Brain> brain = StartBrain(p, enemy, f->mode, f->learn, f->learn ? cfg.epsilon : 0.0f, m->id,
                                                          f->mode == BRAIN_VERSION ? GenerationTable(f->version) : nullptr, !m->isBg && m->size == 1);
                if (team && brain)
                {
                    std::vector<ObjectGuid> allies;
                    for (ObjectGuid g : guids[s])
                        if (g != p->GetGUID())
                            allies.push_back(g);
                    SetBrainTeam(brain, int8(s), allies, guids[1 - s]);
                }
            }
    }

    void OnBgStart(Battleground* bg)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        StartMatch(FindMatch(bg), bg);
    }

    void OnBgEnd(Battleground* bg, TeamId winner)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        Match* m = FindMatch(bg);
        if (!m || m->ended)
            return;
        if (!m->started)
        {
            FinishMatch(*m, -2, "la instancia acabo sin empezar");
            return;
        }
        if (m->isBg)
            ScoreObjectives(*m, bg);
        int8 side = -1;
        if (winner == m->team[0]) side = 0;
        else if (winner == m->team[1]) side = 1;
        FinishMatch(*m, side, m->isBg ? "fin del campo" : "fin de arena");
    }

    void OnBgUpdate(Battleground* bg, uint32 /*diff*/)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        Match* m = FindMatch(bg);
        if (!m)
            return;
        if (!m->started && !m->ended && bg->GetStatus() == STATUS_IN_PROGRESS)
            StartMatch(m, bg);
        uint32 now = getMSTime();
        if (!m->started && !m->ended && now > m->createdMs + TimeoutMs(*m))
        {
            FinishMatch(*m, -2, "no llegaron todos los bots");
            IssueLeave(*m, bg);
            return;
        }
        if (m->started && !m->ended && now > m->startedMs + MaxMs(*m))
        {
            FinishMatch(*m, -1, "tiempo maximo");
            IssueLeave(*m, bg);
            return;
        }
        if (m->ended && !m->leaveIssued && now > m->endedMs + 3000)
            IssueLeave(*m, bg);
    }

    void OnBgRemovePlayer(Battleground* bg, Player* player)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        Match* m = FindMatch(bg);
        if (!m)
            return;
        Fighter* f = FighterOf(*m, player->GetGUID());
        if (!f)
            return;
        if (!m->ended)
            Trace(Acore::StringFormat("Partida #{}: {} sale de la instancia {} antes de acabar (estado {}, vivo {}).",
                     m->id, player->GetName(), m->instanceId, uint32(bg->GetStatus()), player->IsAlive() ? 1 : 0));
        EndBrain(player->GetGUID(), 0.0f);
        ReleaseFighter(*f);
    }

    void OnBgDestroy(Battleground* bg)
    {
        std::lock_guard<std::recursive_mutex> guard(s_lock);
        auto it = s_matches.find(bg->GetInstanceID());
        if (it == s_matches.end())
            return;
        Match& m = it->second;
        if (!m.ended)
            FinishMatch(m, -2, "instancia destruida");
        DisbandGroups(m);
        for (int s = 0; s < 2; ++s)
            for (Fighter& f : m.side[s])
            {
                EndBrain(f.guid, 0.0f);
                ReleaseFighter(f);
            }
        s_matches.erase(it);
    }

    class mod_adaptive_ai_bg : public AllBattlegroundScript
    {
    public:
        mod_adaptive_ai_bg() : AllBattlegroundScript("mod_adaptive_ai_bg",
            { ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_SETUP, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER,
              ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_START, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END,
              ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_UPDATE, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_REMOVE_PLAYER_AT_LEAVE,
              ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_DESTROY }) { }

        // Todas las instancias pasan por aquí; FindMatch descarta las que no son nuestras
        void OnBattlegroundSetup(Battleground* bg) override                         { if (bg) OnBgSetup(bg); }
        void OnBattlegroundAddPlayer(Battleground* bg, Player* player) override      { if (bg && player) OnBgAddPlayer(bg, player); }
        void OnBattlegroundStart(Battleground* bg) override                         { if (bg) OnBgStart(bg); }
        void OnBattlegroundEnd(Battleground* bg, TeamId winner) override            { if (bg) OnBgEnd(bg, winner); }
        void OnBattlegroundUpdate(Battleground* bg, uint32 diff) override           { if (bg) OnBgUpdate(bg, diff); }
        void OnBattlegroundRemovePlayerAtLeave(Battleground* bg, Player* player) override { if (bg && player) OnBgRemovePlayer(bg, player); }
        void OnBattlegroundDestroy(Battleground* bg) override                       { if (bg) OnBgDestroy(bg); }
    };
#else
    void ArenaTick(uint32 /*diff*/) {}
    uint32 ArenaLaunchSeries(CompSpec const&, uint32, uint32, std::string const&, std::string& error) { error = "sin mod-playerbots"; return 0; }
    bool BgLaunch(uint8, uint32, std::string const&, std::string& error) { error = "sin mod-playerbots"; return false; }
    uint32 ArenaStopAll() { return 0; }
    ArenaStatus ArenaGetStatus() { return ArenaStatus(); }
    bool CalibrationStart(std::string& error) { error = "sin mod-playerbots"; return false; }
    bool CalibrationRunning() { return false; }
    void ArenaShutdown() {}
    void ArenaSetAuto(bool) {}
    bool ArenaAutoEnabled() { return false; }
    std::string CalibrationStatus() { return "sin mod-playerbots"; }
    std::string ObjectivesStatus() { return "sin mod-playerbots"; }
    std::vector<LadderRung> LadderFor(uint8) { return std::vector<LadderRung>{ LadderRung() }; }
    float LadderRating(uint8, uint32) { return 1500.0f; }
    std::string LadderStatus() { return "sin mod-playerbots"; }
    void OnBgSetup(Battleground*) {}
    void OnBgAddPlayer(Battleground*, Player*) {}
    void OnBgStart(Battleground*) {}
    void OnBgEnd(Battleground*, TeamId) {}
    void OnBgUpdate(Battleground*, uint32) {}
    void OnBgRemovePlayer(Battleground*, Player*) {}
    void OnBgDestroy(Battleground*) {}
#endif
}

void AddSC_mod_adaptive_ai_arena()
{
#ifdef ADAPTIVE_WITH_PLAYERBOTS
    new AdaptiveAI::mod_adaptive_ai_bg();
#endif
}
