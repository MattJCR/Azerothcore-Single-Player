// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-adaptive-ai — comandos GM (.adaptive ...).
 *
 *   .adaptive                          estado general
 *   .adaptive on | off                 decisor
 *   .adaptive aprender on | off        aprendizaje
 *   .adaptive arena lanzar <equipoA> <equipoB> [cantidad] [simultaneas] [modo]
 *                                      equipos "warrior", "warrior+priest", "*+*+*"
 *   .adaptive arena parar              series y partidas en curso (también campos)
 *   .adaptive arena auto on | off      entrenamiento automático (arenas y campos)
 *   .adaptive arena estado
 *   .adaptive bg lanzar <WS|AB|EY|AV|SA|IC> [por equipo] [modo]
 *   .adaptive bg estado
 *   .adaptive exportar [fichero]       lo aprendido a un .sql (modelos, tabla Q, perfiles)
 *   .adaptive importar [fichero]
 *   .adaptive calibrar                 candidata contra validada, ahora
 *   .adaptive modelo lista
 *   .adaptive modelo usar <version>
 *   .adaptive bot <nombre> [dificultad <1-6>]
 *   .adaptive explicar <nombre>        últimas decisiones con sus valores
 *   .adaptive revertir <clase> [forzar] la candidata recupera las filas de la validada
 *   .adaptive guardar                  volcar la tabla a la base de datos
 *   .adaptive trazas on | off          trazas del entrenamiento a los GM con .gm on
 *   .adaptive escalera                 peldanos por clase (serie, generaciones, validada)
 *
 * Clases: warrior/guerrero, mage/mago, rogue/picaro, priest/sacerdote, ...
 * con especialización opcional: warrior.arms, mage.frost, mage.2.
 */

#include "AdaptiveAI.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Tokenize.h"

#include <algorithm>
#include <map>
#include <sstream>

using namespace Acore::ChatCommands;

namespace AdaptiveAI
{
    namespace
    {
        bool ParseOnOff(std::string const& text, bool& value)
        {
            std::string t = text;
            std::transform(t.begin(), t.end(), t.begin(), ::tolower);
            if (t == "on" || t == "si" || t == "1")  { value = true;  return true; }
            if (t == "off" || t == "no" || t == "0") { value = false; return true; }
            return false;
        }

        std::string ActionName(uint8 cls, uint8 action)
        {
            std::vector<ActionDef> const& actions = ActionsFor(cls);
            return action < actions.size() ? actions[action].name : "?";
        }

        uint32 FindGuidByName(std::string const& name)
        {
            if (Player* p = ObjectAccessor::FindPlayerByName(name))
                return p->GetGUID().GetCounter();
            std::string escaped = name;
            PlayerbotsDatabase.EscapeString(escaped);
            if (QueryResult result = PlayerbotsDatabase.Query("SELECT guid FROM adaptive_bot WHERE nombre = '{}' LIMIT 1", escaped))
                return result->Fetch()[0].Get<uint32>();
            return 0;
        }
    }

    class mod_adaptive_ai_command : public CommandScript
    {
    public:
        mod_adaptive_ai_command() : CommandScript("mod_adaptive_ai_command") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable arenaTable =
            {
                { "lanzar",  HandleArenaLaunch, SEC_GAMEMASTER, Console::Yes },
                { "parar",   HandleArenaStop,   SEC_GAMEMASTER, Console::Yes },
                { "auto",    HandleArenaAuto,   SEC_GAMEMASTER, Console::Yes },
                { "estado",  HandleArenaStatus, SEC_GAMEMASTER, Console::Yes },
                { "",        HandleArenaStatus, SEC_GAMEMASTER, Console::Yes },
            };
            static ChatCommandTable bgTable =
            {
                { "lanzar",  HandleBgLaunch,    SEC_GAMEMASTER, Console::Yes },
                { "estado",  HandleArenaStatus, SEC_GAMEMASTER, Console::Yes },
                { "",        HandleArenaStatus, SEC_GAMEMASTER, Console::Yes },
            };
            static ChatCommandTable modelTable =
            {
                { "lista",   HandleModelList,   SEC_GAMEMASTER, Console::Yes },
                { "usar",    HandleModelUse,    SEC_ADMINISTRATOR, Console::Yes },
                { "",        HandleModelList,   SEC_GAMEMASTER, Console::Yes },
            };
            static ChatCommandTable adaptiveTable =
            {
                { "on",       HandleOn,        SEC_GAMEMASTER, Console::Yes },
                { "off",      HandleOff,       SEC_GAMEMASTER, Console::Yes },
                { "aprender", HandleLearn,     SEC_GAMEMASTER, Console::Yes },
                { "arena",    arenaTable },
                { "bg",       bgTable },
                { "calibrar", HandleCalibrate, SEC_GAMEMASTER, Console::Yes },
                { "exportar", HandleExport,    SEC_ADMINISTRATOR, Console::Yes },
                { "importar", HandleImport,    SEC_ADMINISTRATOR, Console::Yes },
                { "modelo",   modelTable },
                { "bot",      HandleBot,       SEC_GAMEMASTER, Console::Yes },
                { "explicar", HandleExplain,   SEC_GAMEMASTER, Console::Yes },
                { "revertir", HandleRevert,    SEC_GAMEMASTER, Console::Yes },
                { "guardar",  HandleSave,      SEC_GAMEMASTER, Console::Yes },
                { "trazas",   HandleTraces,    SEC_GAMEMASTER, Console::Yes },
                { "escalera", HandleLadder,    SEC_GAMEMASTER, Console::Yes },
                { "estado",   HandleStatus,    SEC_GAMEMASTER, Console::Yes },
                { "",         HandleStatus,    SEC_GAMEMASTER, Console::Yes },
            };
            static ChatCommandTable commandTable =
            {
                { "adaptive", adaptiveTable },
            };
            return commandTable;
        }

        // La escalera: por clase, los modelos medidos contra playerbots de
        // serie, del mas fuerte al mas flojo. La serie es el peldano 0 y el
        // ancla en 1500: donde sale primera, el modulo deja jugar de serie.
        static bool HandleLadder(ChatHandler* handler)
        {
            handler->PSendSysMessage("Escalera (modo {}, dispersion {}, resorteo cada {} h). Peldano 0 = playerbots de serie, anclado en 1500:",
                cfg.realMode, cfg.mirrorSpread, cfg.mirrorHours);
            // Tokenize devuelve vistas al texto: el original tiene que seguir
            // vivo mientras se recorren (nada de pasarle un temporal).
            std::string text = LadderStatus();
            for (std::string_view line : Acore::Tokenize(text, '\n', false))
                handler->PSendSysMessage("{}", std::string(line));
            return true;
        }

        static bool HandleStatus(ChatHandler* handler)
        {
#ifndef ADAPTIVE_WITH_PLAYERBOTS
            handler->SendSysMessage("mod-adaptive-ai: mod-playerbots no esta compilado, el modulo no hace nada.");
            return true;
#endif
            std::shared_ptr<QTable> c = Candidate();
            std::shared_ptr<QTable> v = Validated();
            handler->PSendSysMessage("Adaptive AI: decisor {}, aprendizaje {}{}, combates reales {}, arena {} ({} simultaneas, modo {}).",
                cfg.enabled ? "ON" : "OFF", cfg.learn ? "ON" : "OFF", cfg.learnOnlyArena ? " (solo en arena)" : "",
                cfg.realEnable ? "si" : "no", cfg.arenaEnable && ArenaAutoEnabled() ? "ON" : "OFF", cfg.arenaSimultaneous, cfg.arenaMode);
            {
                std::string classes, types, comps, bgs;
                for (uint8 k : cfg.classes) classes += (classes.empty() ? "" : ",") + std::string(ClassName(k));
                for (uint8 t : cfg.arenaTypes) types += (types.empty() ? "" : ",") + std::to_string(t) + "c" + std::to_string(t);
                for (CompSpec const& p : cfg.arenaPairs) comps += (comps.empty() ? "" : " ") + p.Name();
                for (uint8 t : cfg.bgTypes) bgs += (bgs.empty() ? "" : ",") + std::string(BgName(t));
                handler->PSendSysMessage("Clases: {}. Tipos de arena: {}. Composiciones: {}. Campos de batalla: {}.",
                    classes.empty() ? "todas" : classes, types, comps,
                    cfg.bgEnable ? Acore::StringFormat("{} cada {} min, {} por equipo, modo {}", bgs, cfg.bgEveryMinutes, cfg.bgPlayersPerTeam ? std::to_string(cfg.bgPlayersPerTeam) : "el minimo del campo", cfg.bgMode) : "apagados (.adaptive bg lanzar para uno a mano)");
            }
            handler->PSendSysMessage("Modelos: validada v{} ({} entradas), candidata v{} ({} entradas, {} actualizaciones, {} sin guardar).",
                v ? v->Version() : 0, v ? v->Size() : 0, c ? c->Version() : 0, c ? c->Size() : 0, c ? c->Updates() : 0, c ? c->DirtySince() : 0);
            handler->PSendSysMessage("Cerebros activos: {}. {}", ActiveBrains(), CalibrationStatus());
            handler->PSendSysMessage("{}.", ApprovalStatus());
            handler->PSendSysMessage("En el mundo: modo {} (.adaptive escalera).", cfg.realMode);
            handler->PSendSysMessage("{}.", Level80Status());
            handler->PSendSysMessage("Franjas de nivel: {}.", LevelBracketsStatus());
            {
                std::string specs;
                for (auto const& [cls, name] : cfg.arenaSpecs) specs += (specs.empty() ? "" : ", ") + std::string(ClassName(cls)) + " " + name;
                handler->PSendSysMessage("Specs PvP: {}. Equipo de arena: nivel de objeto {}. {}.", specs.empty() ? "las que traigan" : specs, cfg.arenaGearScore, ObjectivesStatus());
                handler->PSendSysMessage("{}.", LoadoutStatus());
                handler->PSendSysMessage("{}.", UsableStatus());
            }

            ArenaStatus st = ArenaGetStatus();
            handler->PSendSysMessage("Arena: {} partidas en curso, {} en espera, {} campos de batalla en curso, {} terminadas desde el arranque.", st.running, st.waiting, st.bgRunning, st.finishedToday);

            // Contraste por clase del lado adaptativo (últimas 200 de cada clase)
            // y referencia (serie contra serie). El global mezcla emparejamientos
            // con líneas base muy distintas (guerrero-mago: 80 % y 20 %): solo
            // vale si está equilibrado, y aun así esconde dónde se aprende.
            struct WL { uint32 wins = 0, losses = 0; };
            std::map<uint8, WL> contrast;                     // 1c1: clase adaptativa -> resultado
            std::map<std::string, WL> contrastType;           // otros tipos (2c2, bg:WS...) -> resultado
            std::map<std::pair<uint8, uint8>, WL> reference;  // 1c1 serie contra serie: (clase a, clase b) -> gana a / gana b
            std::map<std::string, WL> referenceType;
            uint32 selfPlay = 0, adaptiveWins = 0, baselineWins = 0;
            if (QueryResult result = PlayerbotsDatabase.Query(
                "SELECT modo_a, modo_b, clase_a, clase_b, ganador, tipo FROM adaptive_match WHERE fuente IN ('auto','manual') AND ganador IN ('a','b') ORDER BY id DESC LIMIT 1500"))
            {
                do
                {
                    Field* f = result->Fetch();
                    std::string ma = f[0].Get<std::string>(), mb = f[1].Get<std::string>(), w = f[4].Get<std::string>(), type = f[5].Get<std::string>();
                    uint8 ca = f[2].Get<uint8>(), cb = f[3].Get<uint8>();
                    bool aNone = ma == "ninguna", bNone = mb == "ninguna";
                    bool solo = type == "1c1";
                    if (aNone && bNone)
                    {
                        WL& r = solo ? reference[{ ca, cb }] : referenceType[type];
                        if (r.wins + r.losses >= 200) continue;
                        if (w == "a") ++r.wins; else ++r.losses;
                    }
                    else if (aNone || bNone)
                    {
                        WL& c = solo ? contrast[aNone ? cb : ca] : contrastType[type];
                        if (c.wins + c.losses >= 200) continue;
                        bool adaptiveWon = (w == "a" && !aNone) || (w == "b" && !bNone);
                        if (adaptiveWon) { ++c.wins; ++adaptiveWins; } else { ++c.losses; ++baselineWins; }
                    }
                    else
                        ++selfPlay;
                } while (result->NextRow());
            }
            if (contrast.empty() && contrastType.empty())
                handler->SendSysMessage("Contraste: sin partidas decididas todavia.");
            else
            {
                std::string line;
                for (auto const& [cls, wl] : contrast)
                    line += Acore::StringFormat("{}{} adaptativo {:.0f}% ({}-{})", line.empty() ? "" : ", ", ClassName(cls), 100.0f * wl.wins / float(wl.wins + wl.losses), wl.wins, wl.losses);
                for (auto const& [type, wl] : contrastType)
                    line += Acore::StringFormat("{}{} adaptativo {:.0f}% ({}-{})", line.empty() ? "" : ", ", type, 100.0f * wl.wins / float(wl.wins + wl.losses), wl.wins, wl.losses);
                handler->PSendSysMessage("Contraste (ultimas 200 por clase o tipo): {}; global {:.0f}%. Autojuego: {}.",
                    line, 100.0f * adaptiveWins / float(std::max<uint32>(1, adaptiveWins + baselineWins)), selfPlay);
            }
            if (!reference.empty() || !referenceType.empty())
            {
                std::string line;
                for (auto const& [pair, wl] : reference)
                    line += Acore::StringFormat("{}{} gana {:.0f}% a {} ({} partidas)", line.empty() ? "" : ", ", ClassName(pair.first), 100.0f * wl.wins / float(wl.wins + wl.losses), ClassName(pair.second), wl.wins + wl.losses);
                for (auto const& [type, wl] : referenceType)
                    line += Acore::StringFormat("{}{}: lado a gana {:.0f}% ({} partidas)", line.empty() ? "" : ", ", type, 100.0f * wl.wins / float(wl.wins + wl.losses), wl.wins + wl.losses);
                handler->PSendSysMessage("Referencia (serie contra serie, ultimas 200): {}.", line);
            }
            return true;
        }

        static bool HandleOn(ChatHandler* handler)
        {
            cfg.enabled = true;
            handler->SendSysMessage("Adaptive AI: decisor ON.");
            return true;
        }

        static bool HandleOff(ChatHandler* handler)
        {
            cfg.enabled = false;
            handler->SendSysMessage("Adaptive AI: decisor OFF (los bots vuelven a ser playerbots de serie; las arenas siguen y registran resultados).");
            return true;
        }

        static bool HandleLearn(ChatHandler* handler, Optional<std::string> arg)
        {
            bool value;
            if (!arg || !ParseOnOff(*arg, value))
            {
                handler->SendSysMessage("Uso: .adaptive aprender on|off");
                return true;
            }
            cfg.learn = value;
            if (!value)
                SaveAllDirty();
            handler->PSendSysMessage("Adaptive AI: aprendizaje {}. (Las partidas ya en curso siguen como empezaron.)", value ? "ON" : "OFF");
            return true;
        }

        static bool HandleArenaLaunch(ChatHandler* handler, Optional<std::string> teamA, Optional<std::string> teamB,
                                      Optional<uint32> count, Optional<uint32> simultaneous, Optional<std::string> mode)
        {
            if (!teamA || !teamB)
            {
                handler->SendSysMessage("Uso: .adaptive arena lanzar <equipoA> <equipoB> [cantidad] [simultaneas] [entrenar|contraste|mixto|referencia]");
                handler->SendSysMessage("Equipos: una clase (warrior, mago.escarcha) o varias con + (warrior+priest, *+*+*); * = cualquiera. Ej.: .adaptive arena lanzar warrior+priest mage+rogue 4 2 contraste");
                return true;
            }
            CompSpec comp;
            if (!ParseTeam(*teamA, comp.a) || !ParseTeam(*teamB, comp.b))
            {
                handler->SendSysMessage("Clase desconocida. Valen: warrior/guerrero, paladin, hunter/cazador, rogue/picaro, priest/sacerdote, deathknight/dk, shaman/chaman, mage/mago, warlock/brujo, druid/druida, *; y .arms/.fury/.prot, .arcane/.fire/.frost o .0/.1/.2.");
                return true;
            }
            if (comp.a.size() != comp.b.size() || (comp.Size() != 1 && comp.Size() != 2 && comp.Size() != 3 && comp.Size() != 5))
            {
                handler->SendSysMessage("Los dos equipos tienen que ser del mismo tamano: 1, 2, 3 o 5.");
                return true;
            }
            std::string error;
            uint32 id = ArenaLaunchSeries(comp, count.value_or(1), simultaneous.value_or(1), mode.value_or(cfg.arenaMode), error);
            if (!id)
            {
                handler->PSendSysMessage("No se pudo lanzar: {}.", error);
                return true;
            }
            handler->PSendSysMessage("Serie {} lanzada: {} combates {} {} ({} a la vez, modo {}). Los bots se buscan en el siguiente tick; '.adaptive arena estado' para seguirla.",
                id, count.value_or(1), comp.TypeName(), comp.Name(), std::max<uint32>(1, simultaneous.value_or(1)), mode.value_or(cfg.arenaMode));
            return true;
        }

        static bool HandleBgLaunch(ChatHandler* handler, Optional<std::string> bg, Optional<uint32> perTeam, Optional<std::string> mode)
        {
            uint8 type = bg ? ParseBgType(*bg) : 0;
            if (!type)
            {
                handler->SendSysMessage("Uso: .adaptive bg lanzar <WS|AB|EY|AV|SA|IC> [bots por equipo] [entrenar|contraste|mixto|referencia]");
                handler->SendSysMessage("Sin 'bots por equipo' se usa AdaptiveAI.Bg.PorEquipo o el minimo del campo (WS 10, AB 15, EY 15, AV 40, SA 15, IC 20). Ej.: .adaptive bg lanzar WS 5 contraste");
                return true;
            }
            std::string error;
            if (!BgLaunch(type, perTeam.value_or(0), mode.value_or(cfg.bgMode), error))
            {
                handler->PSendSysMessage("No se pudo lanzar el campo: {}.", error);
                return true;
            }
            handler->PSendSysMessage("Campo de batalla {} lanzado (modo {}). '.adaptive arena estado' para seguirlo; con .gm on te llegan las trazas.", BgName(type), mode.value_or(cfg.bgMode));
            return true;
        }

        static bool HandleExport(ChatHandler* handler, Optional<std::string> file)
        {
            std::string summary;
            bool ok = ExportTraining(file.value_or(cfg.exportFile), summary);
            handler->PSendSysMessage("{}{}", ok ? "Exportado: " : "No se pudo exportar: ", summary);
            if (ok)
                handler->SendSysMessage("El fichero esta en el directorio del worldserver (bin/). Para que viaje con el instalador: modules/mod-adaptive-ai/data/entrenado/adaptive_entrenado.sql (tools/exportar-adaptive.sh).");
            return true;
        }

        static bool HandleImport(ChatHandler* handler, Optional<std::string> file)
        {
            std::string summary;
            bool ok = ImportTraining(file.value_or(cfg.exportFile), summary);
            handler->PSendSysMessage("{}{}", ok ? "Importado: " : "No se pudo importar: ", summary);
            return true;
        }

        static bool HandleArenaStop(ChatHandler* handler)
        {
            uint32 stopped = ArenaStopAll();
            handler->PSendSysMessage("Series borradas y {} partidas paradas. El entrenamiento automatico sigue como estaba (.adaptive arena auto off para pararlo).", stopped);
            return true;
        }

        static bool HandleArenaAuto(ChatHandler* handler, Optional<std::string> arg)
        {
            bool value;
            if (!arg || !ParseOnOff(*arg, value))
            {
                handler->PSendSysMessage("Entrenamiento automatico: {}. Uso: .adaptive arena auto on|off", ArenaAutoEnabled() ? "ON" : "OFF");
                return true;
            }
            ArenaSetAuto(value);
            handler->PSendSysMessage("Entrenamiento automatico {}.", value ? "ON" : "OFF");
            return true;
        }

        static bool HandleArenaStatus(ChatHandler* handler)
        {
            ArenaStatus st = ArenaGetStatus();
            handler->PSendSysMessage("Arena: {} partidas en curso, {} en espera, {} campos en curso, {} terminadas desde el arranque. {}", st.running, st.waiting, st.bgRunning, st.finishedToday, CalibrationStatus());
            for (std::string const& line : st.lines)
                handler->SendSysMessage(line.c_str());
            return true;
        }

        static bool HandleCalibrate(ChatHandler* handler)
        {
            std::string error;
            if (!CalibrationStart(error))
            {
                handler->PSendSysMessage("No se pudo calibrar: {}.", error);
                return true;
            }
            handler->PSendSysMessage("Calibracion lanzada: {} combates candidata contra validada, sin aprender. '.adaptive arena estado' para seguirla.", cfg.calibrateMatches);
            return true;
        }

        static bool HandleModelList(ChatHandler* handler)
        {
            std::shared_ptr<QTable> c = Candidate();
            std::shared_ptr<QTable> v = Validated();
            for (ModelInfo const& m : ListModels())
                handler->PSendSysMessage("  v{} {} {}{}{}{} - {:.0f}% en {} combates. {}", m.version, m.created,
                    m.validated ? "validada" : "no validada",
                    m.generation && !m.validated ? " [GENERACION]" : "",
                    v && v->Version() == m.version ? " [EN USO]" : "",
                    c && c->Version() == m.version ? " [CANDIDATA]" : "",
                    m.winrate * 100.0f, m.matches, m.note);
            return true;
        }

        static bool HandleModelUse(ChatHandler* handler, Optional<uint32> version)
        {
            if (!version)
            {
                handler->SendSysMessage("Uso: .adaptive modelo usar <version>");
                return true;
            }
            std::string error;
            if (!UseModel(*version, error))
            {
                handler->PSendSysMessage("No se pudo: {}.", error);
                return true;
            }
            handler->PSendSysMessage("Los bots que se crucen con jugadores usan ahora la v{}. La candidata sigue entrenando.", *version);
            return true;
        }

        static bool HandleBot(ChatHandler* handler, Optional<std::string> name, Optional<std::string> sub, Optional<uint32> value)
        {
            if (!name)
            {
                handler->SendSysMessage("Uso: .adaptive bot <nombre> [dificultad <1-6>]");
                return true;
            }
            uint32 guid = FindGuidByName(*name);
            BotProfile* p = guid ? FindProfile(guid) : nullptr;
            if (!p)
            {
                if (Player* online = ObjectAccessor::FindPlayerByName(*name))
                    p = &GetProfile(online);
            }
            if (!p)
            {
                handler->PSendSysMessage("No hay perfil de '{}' (todavia no ha luchado con Adaptive AI).", *name);
                return true;
            }
            if (sub && *sub == "dificultad")
            {
                if (!value || *value < 1 || *value > 6)
                {
                    handler->SendSysMessage("La dificultad va de 1 (Novato) a 6 (Elite).");
                    return true;
                }
                p->difficulty = uint8(*value);
                SaveProfile(*p);
                handler->PSendSysMessage("{}: dificultad {} ({}). Se aplica en combates reales, no en la arena de entrenamiento.", p->name, *value, DifficultyName(uint8(*value)));
                return true;
            }
            handler->PSendSysMessage("{} ({} spec {}): rating {:.0f} [{}], {} victorias, {} derrotas, {} empates, XP {}, modelo v{}.",
                p->name, ClassName(p->cls), p->spec, p->rating, BracketName(BracketOf(p->rating)), p->wins, p->losses, p->draws, p->xp, p->model);
            handler->PSendSysMessage("  dificultad {} ({}); personalidad: agresividad {:.2f}, riesgo {:.2f}, defensa {:.2f}, prioridad {:.2f}, movilidad {:.2f}.",
                p->difficulty, DifficultyName(p->difficulty), p->aggression, p->risk, p->defense, p->priority, p->mobility);
            handler->PSendSysMessage("  loadout: spec PvE {}, spec PvP {}, ahora {}, equipado a nivel de objeto {}.",
                p->specPve, p->specPvp, p->purpose, p->ilvl);
            return true;
        }

        // Tirar lo aprendido por una clase y volver a las filas de la validada.
        // Para cuando una clase se ha metido en un pozo y, de paso, bloquea la
        // ronda: mientras no aprueban todas, las que ya lo hicieron no vuelven a
        // entrenar en 1c1.
        static bool HandleRevert(ChatHandler* handler, Optional<std::string> name, Optional<std::string> force)
        {
            if (!name)
            {
                handler->SendSysMessage("Uso: .adaptive revertir <clase> [forzar]");
                return true;
            }
            int8 spec;
            uint8 cls = ParseClass(*name, spec);
            if (!cls)
            {
                handler->PSendSysMessage("'{}' no es una clase (warrior, mage, rogue...).", *name);
                return true;
            }
            // "forzar": vaciar aunque la validada no tenga filas de esa clase. Con la
            // tabla vacia la clase elige siempre la accion 0 (none), o sea deja hacer
            // a playerbots: es volver a la serie. Suena destructivo y a veces es la
            // mejora: el 04/09 el brujo sacaba 15 % con lo aprendido contra el 30 %
            // de su validada VACIA, que es exactamente jugar de serie.
            bool forzar = force && (*force == "forzar" || *force == "vaciar" || *force == "si");
            std::string detail;
            if (RevertClass(cls, detail, forzar))
                handler->PSendSysMessage("Revertida {}.", detail);
            else
                handler->PSendSysMessage("No se ha revertido: {}.", detail);
            return true;
        }

        static bool HandleExplain(ChatHandler* handler, Optional<std::string> name)
        {
            if (!name)
            {
                handler->SendSysMessage("Uso: .adaptive explicar <nombre>");
                return true;
            }
            uint32 guid = FindGuidByName(*name);
            if (!guid)
            {
                handler->PSendSysMessage("No se encuentra a '{}'.", *name);
                return true;
            }
            uint8 cls = 0;
            if (BotProfile* p = FindProfile(guid))
                cls = p->cls;
            else if (Player* online = ObjectAccessor::FindPlayerByName(*name))
                cls = online->getClass();

            std::vector<Decision> decisions;
            if (std::shared_ptr<Brain> brain = FindBrain(ObjectGuid::Create<HighGuid::Player>(guid)))
            {
                std::lock_guard<std::recursive_mutex> guard(brain->lock);
                decisions.assign(brain->history.begin(), brain->history.end());
                handler->PSendSysMessage("{} esta en combate ahora ({}, {}): {} decisiones, recompensa {:.1f}.",
                    *name, ModeName(brain->mode), brain->learn ? "aprendiendo" : "sin aprender", brain->decisions, brain->totalReward);
            }
            else
                decisions = LastDecisions(guid);
            if (decisions.empty())
            {
                handler->PSendSysMessage("{}: sin decisiones registradas.", *name);
                return true;
            }
            uint32 first = decisions.front().timeMs;
            size_t shown = std::min<size_t>(decisions.size(), 25);
            if (shown < decisions.size())
                handler->PSendSysMessage("  ({} decisiones; se enseñan las {} ultimas)", decisions.size(), shown);
            for (size_t i = decisions.size() - shown; i < decisions.size(); ++i)
            {
                Decision const& d = decisions[i];
                handler->PSendSysMessage("  +{:.1f}s {} -> {} (q {:.2f}, cobrado {:+.1f})", (d.timeMs - first) / 1000.0f, d.explain, ActionName(cls, d.action), d.q, d.reward);
            }
            return true;
        }

        static bool HandleTraces(ChatHandler* handler, Optional<std::string> arg)
        {
            bool value;
            if (!arg || !ParseOnOff(*arg, value))
            {
                handler->PSendSysMessage("Trazas a los GM: {}. Uso: .adaptive trazas on|off (con .gm on te llegan como mensajes de sistema).", TracesOn() ? "ON" : "OFF");
                return true;
            }
            SetTraces(value);
            handler->PSendSysMessage("Trazas a los GM {}.", value ? "ON" : "OFF");
            return true;
        }

        static bool HandleSave(ChatHandler* handler)
        {
            SaveAllDirty();
            handler->SendSysMessage("Tablas y perfiles volcados a acore_playerbots.");
            return true;
        }
    };
}

void AddSC_mod_adaptive_ai_commands()
{
    new AdaptiveAI::mod_adaptive_ai_command();
}
