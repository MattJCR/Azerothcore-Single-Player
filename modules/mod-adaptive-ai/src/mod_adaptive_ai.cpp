// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-adaptive-ai — núcleo: configuración, catálogo de acciones, estado,
 * tabla Q, modelos, perfiles, cerebro por bot y hooks de recompensa.
 *
 * Ver AdaptiveAI.h para la idea general y CHANGELOG.md anexo A1 para el diseño.
 *
 * EL CICLO DE UNA DECISIÓN
 *   1. OnPlayerUpdate (hilo del mapa) llama a BrainTick cada 200 ms.
 *   2. Se calcula el estado resumido (vida propia y enemiga, distancia, si el
 *      enemigo castea, controles, recursos, snare): 864 combinaciones.
 *   3. Si el estado cambió (o pasó el intervalo) es un punto de decisión: se
 *      cierra la decisión anterior con lo cobrado desde entonces
 *      (Q-learning), y se elige la siguiente acción entre las disponibles
 *      (CanCastSpell) por su valor Q, con exploración epsilon.
 *   4. La acción se ejecuta con PlayerbotAI::DoSpecificAction: playerbots
 *      comprueba rango, visión, cooldown, GCD, y la lanza. NONE = no hacer
 *      nada especial (el motor de playerbots sigue con su rotación).
 *   5. Los hooks del core van sumando recompensas: daño hecho y recibido,
 *      interrupción lograda o desperdiciada, control hecho o sufrido, muerte,
 *      kill; y al acabar el combate, victoria o derrota.
 */

#include "AdaptiveAI.h"

#include "Battleground.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SlowTick.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"
#include "Unit.h"
#include "World.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#ifdef ADAPTIVE_WITH_PLAYERBOTS
#  include "AiFactory.h"
#  include "Creature.h"
#  include "Pet.h"
#  include "PlayerbotAI.h"
#  include "PlayerbotAIConfig.h"
#  include "PlayerbotFactory.h"
#  include "Playerbots.h"
#  include "RandomPlayerbotMgr.h"
#endif

namespace AdaptiveAI
{
    Config cfg;

    // ─────────────────────────────────────────────────────────────────────────
    //  Utilidades
    // ─────────────────────────────────────────────────────────────────────────
    std::string Now()
    {
        time_t t = GameTime::GetGameTime().count();
        tm* lt = localtime(&t);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", lt);
        return buf;
    }

    static std::atomic<bool> s_traces{true};

    void SetTraces(bool on) { s_traces.store(on); }
    bool TracesOn() { return s_traces.load(); }

    // Al log siempre; a los GM conectados con .gm on si las trazas están activas.
    void Trace(std::string const& text)
    {
        LOG_INFO("module", "[adaptive-ai] {}", text);
        if (!cfg.tracesGM || !s_traces.load())
            return;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!p || !p->IsInWorld() || !p->GetSession() || IsBot(p) || !p->IsGameMaster())
                continue;
            ChatHandler(p->GetSession()).PSendSysMessage("|cff8fd8ff[adaptive]|r {}", text);
        }
    }

    bool IsBot(Player* player)
    {
#ifdef ADAPTIVE_WITH_PLAYERBOTS
        return player && GET_PLAYERBOT_AI(player) != nullptr && !IsSelfBot(player);
#else
        (void)player;
        return false;
#endif
    }

    Player* PlayerOf(Unit* unit)
    {
        if (!unit)
            return nullptr;
        return unit->GetCharmerOrOwnerPlayerOrPlayerItself();
    }

    char const* ClassName(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "warrior";
            case CLASS_PALADIN:      return "paladin";
            case CLASS_HUNTER:       return "hunter";
            case CLASS_ROGUE:        return "rogue";
            case CLASS_PRIEST:       return "priest";
            case CLASS_DEATH_KNIGHT: return "deathknight";
            case CLASS_SHAMAN:       return "shaman";
            case CLASS_MAGE:         return "mage";
            case CLASS_WARLOCK:      return "warlock";
            case CLASS_DRUID:        return "druid";
            default:                 return "?";
        }
    }

    // "warrior", "guerrero", "mage.2", "mago.frost", "warrior.arms"...
    uint8 ParseClass(std::string const& raw, int8& spec)
    {
        spec = -1;
        std::string text = raw;
        std::transform(text.begin(), text.end(), text.begin(), ::tolower);
        std::string specText;
        size_t dot = text.find('.');
        if (dot != std::string::npos)
        {
            specText = text.substr(dot + 1);
            text = text.substr(0, dot);
        }

        uint8 cls = 0;
        if (text == "warrior" || text == "guerrero")            cls = CLASS_WARRIOR;
        else if (text == "paladin" || text == "paladín")        cls = CLASS_PALADIN;
        else if (text == "hunter" || text == "cazador")         cls = CLASS_HUNTER;
        else if (text == "rogue" || text == "picaro" || text == "pícaro") cls = CLASS_ROGUE;
        else if (text == "priest" || text == "sacerdote")       cls = CLASS_PRIEST;
        else if (text == "deathknight" || text == "dk" || text == "caballero") cls = CLASS_DEATH_KNIGHT;
        else if (text == "shaman" || text == "chaman" || text == "chamán") cls = CLASS_SHAMAN;
        else if (text == "mage" || text == "mago")              cls = CLASS_MAGE;
        else if (text == "warlock" || text == "brujo")          cls = CLASS_WARLOCK;
        else if (text == "druid" || text == "druida")           cls = CLASS_DRUID;
        if (!cls)
            return 0;

        if (!specText.empty())
        {
            if (specText == "0" || specText == "1" || specText == "2")
                spec = int8(specText[0] - '0');
            else if (specText == "arms" || specText == "armas" || specText == "arcane" || specText == "arcano" ||
                     specText == "holy" || specText == "sagrado" || specText == "beast" || specText == "bestias" ||
                     specText == "assassination" || specText == "asesinato" || specText == "discipline" || specText == "disciplina" ||
                     specText == "blood" || specText == "sangre" || specText == "elemental" || specText == "affliction" || specText == "aflicción" ||
                     specText == "balance" || specText == "equilibrio")
                spec = 0;
            else if (specText == "fury" || specText == "furia" || specText == "fire" || specText == "fuego" ||
                     specText == "protection" || specText == "proteccion" || specText == "marksmanship" || specText == "punteria" ||
                     specText == "combat" || specText == "combate" || (specText == "frost" && cls == CLASS_DEATH_KNIGHT) ||
                     specText == "enhancement" || specText == "mejora" || specText == "demonology" || specText == "demonologia" ||
                     specText == "feral")
                spec = 1;
            else if (specText == "prot" || specText == "frost" || specText == "escarcha" || specText == "retribution" || specText == "reprension" ||
                     specText == "survival" || specText == "supervivencia" || specText == "subtlety" || specText == "sutileza" ||
                     specText == "shadow" || specText == "sombras" || specText == "unholy" || specText == "profano" ||
                     specText == "restoration" || specText == "restauracion" || specText == "destruction" || specText == "destruccion")
                spec = 2;
            else
                return 0;
        }
        return cls;
    }

    std::string ClassSpec::Name() const
    {
        std::ostringstream out;
        out << (cls ? ClassName(cls) : "*");
        if (spec >= 0) out << "." << int(spec);
        return out.str();
    }

    static std::string TeamName(std::vector<ClassSpec> const& team)
    {
        std::string out;
        for (ClassSpec const& c : team)
            out += (out.empty() ? "" : "+") + c.Name();
        return out;
    }

    std::string CompSpec::Name() const     { return TeamName(a) + ":" + TeamName(b); }
    std::string CompSpec::TypeName() const { return std::to_string(Size()) + "c" + std::to_string(Size()); }

    static std::string Trim(std::string_view v)
    {
        size_t b = v.find_first_not_of(" \t");
        size_t e = v.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : std::string(v.substr(b, e - b + 1));
    }

    // "warrior+priest", "*+*", "mage.frost"
    bool ParseTeam(std::string const& text, std::vector<ClassSpec>& out)
    {
        out.clear();
        for (std::string_view item : Acore::Tokenize(text, '+', false))
        {
            std::string t = Trim(item);
            ClassSpec c;
            if (t == "*" || t == "cualquiera" || t == "any")
                c.cls = 0;
            else
            {
                c.cls = ParseClass(t, c.spec);
                if (!c.cls)
                    return false;
            }
            out.push_back(c);
        }
        return !out.empty() && out.size() <= 5;
    }

    // "warrior:mage, warrior+priest:mage+rogue, *+*+*:*+*+*"
    bool ParseComps(std::string const& text, std::vector<CompSpec>& out)
    {
        out.clear();
        for (std::string_view item : Acore::Tokenize(text, ',', false))
        {
            std::string comp = Trim(item);
            if (comp.empty())
                continue;
            size_t colon = comp.find(':');
            if (colon == std::string::npos)
                return false;
            CompSpec c;
            if (!ParseTeam(comp.substr(0, colon), c.a) || !ParseTeam(comp.substr(colon + 1), c.b))
                return false;
            if (c.a.size() != c.b.size() || (c.a.size() != 1 && c.a.size() != 2 && c.a.size() != 3 && c.a.size() != 5))
                return false;
            out.push_back(c);
        }
        return !out.empty();
    }

    uint8 ParseBgType(std::string const& raw)
    {
        std::string t = raw;
        std::transform(t.begin(), t.end(), t.begin(), ::toupper);
        if (t == "WS" || t == "WSG" || t == "GARGANTA")  return BATTLEGROUND_WS;
        if (t == "AB" || t == "ARATHI" || t == "CUENCA") return BATTLEGROUND_AB;
        if (t == "EY" || t == "EOTS" || t == "OJO")      return BATTLEGROUND_EY;
        if (t == "AV" || t == "ALTERAC")                 return BATTLEGROUND_AV;
        if (t == "SA" || t == "SOTA" || t == "PLAYA")    return BATTLEGROUND_SA;
        if (t == "IC" || t == "IOC" || t == "ISLA")      return BATTLEGROUND_IC;
        return 0;
    }

    char const* BgName(uint8 bgType)
    {
        switch (bgType)
        {
            case BATTLEGROUND_WS: return "WS";
            case BATTLEGROUND_AB: return "AB";
            case BATTLEGROUND_EY: return "EY";
            case BATTLEGROUND_AV: return "AV";
            case BATTLEGROUND_SA: return "SA";
            case BATTLEGROUND_IC: return "IC";
        }
        return "?";
    }

    bool ClassEnabled(uint8 cls)
    {
        if (cfg.classes.empty())
            return true;
        return std::find(cfg.classes.begin(), cfg.classes.end(), cls) != cfg.classes.end();
    }

    bool IsCasterClass(uint8 cls)
    {
        return cls == CLASS_MAGE || cls == CLASS_WARLOCK || cls == CLASS_PRIEST || cls == CLASS_HUNTER || cls == CLASS_SHAMAN || cls == CLASS_DRUID;
    }

    bool IsHealerSpec(uint8 cls, uint8 spec)
    {
        switch (cls)
        {
            case CLASS_PRIEST:  return spec == 0 || spec == 1;   // disciplina, sagrado
            case CLASS_PALADIN: return spec == 0;                // sagrado
            case CLASS_DRUID:   return spec == 2;                // restauración
            case CLASS_SHAMAN:  return spec == 2;                // restauración
        }
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Configuración
    // ─────────────────────────────────────────────────────────────────────────
    static Difficulty ParseDifficulty(std::string const& text, Difficulty fallback)
    {
        std::vector<std::string_view> parts = Acore::Tokenize(text, ',', false);
        if (parts.size() != 2)
            return fallback;
        Difficulty d;
        d.miss   = Acore::StringTo<float>(parts[0]).value_or(fallback.miss);
        d.second = Acore::StringTo<float>(parts[1]).value_or(fallback.second);
        return d;
    }

    void LoadConfig()
    {
        cfg.enabled          = sConfigMgr->GetOption<bool>("AdaptiveAI.Enable", true);
        cfg.learn            = sConfigMgr->GetOption<bool>("AdaptiveAI.Learn", true);
        cfg.learnOnlyArena   = sConfigMgr->GetOption<bool>("AdaptiveAI.Learn.SoloEnArena", false);
        cfg.realEnable       = sConfigMgr->GetOption<bool>("AdaptiveAI.Real.Enable", true);
        cfg.realMode         = sConfigMgr->GetOption<std::string>("AdaptiveAI.Real.Modo", "mejor");
        if (cfg.realMode != "validada" && cfg.realMode != "mejor" && cfg.realMode != "espejo")
        {
            LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Real.Modo: '{}' no vale (validada, mejor, espejo); se usa mejor.", cfg.realMode);
            cfg.realMode = "mejor";
        }
        cfg.mirrorSpread     = std::min<uint32>(5, sConfigMgr->GetOption<uint32>("AdaptiveAI.Real.Espejo.Dispersion", 1));
        cfg.mirrorHours      = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AdaptiveAI.Real.Espejo.Horas", 6), 1, 240);

        cfg.arenaEnable      = sConfigMgr->GetOption<bool>("AdaptiveAI.Arena.Enable", true);
        cfg.arenaSimultaneous = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.Simultaneas", 20);
        cfg.arenaTeamSimultaneous = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.EquipoSimultaneas", 1);
        cfg.arenaBotsMax     = std::max<uint32>(2, sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.BotsMax", 40));
        cfg.arenaWithPlayer.clear();
        std::string withPlayerText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Arena.ConJugador", "1c1:21");
        for (std::string_view t : Acore::Tokenize(withPlayerText, ',', false))
        {
            std::string item = Trim(t);
            size_t colon = item.find(':');
            if (item.size() < 3 || colon == std::string::npos)
                continue;
            uint8 size = uint8(item[0] - '0');
            Optional<uint32> n = Acore::StringTo<uint32>(item.substr(colon + 1));
            if ((size == 1 || size == 2 || size == 3 || size == 5) && n)
                cfg.arenaWithPlayer[size] = *n;
        }
        cfg.bgWithPlayer     = sConfigMgr->GetOption<uint32>("AdaptiveAI.Bg.ConJugador", 0);
        cfg.bracketsWithPlayer = sConfigMgr->GetOption<bool>("AdaptiveAI.FranjasConJugador", true);
        cfg.arenaPauseSecs   = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.PausaEntreCombates", 5);
        cfg.arenaYield       = sConfigMgr->GetOption<bool>("AdaptiveAI.Arena.CederAJugadores", true);
        cfg.arenaMap         = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.Mapa", 559);
        cfg.arenaPrepSecs    = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.PreparacionSegundos", 15);
        cfg.arenaLevel       = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.Nivel", 80);
        cfg.arenaGearScore   = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.NivelObjeto", 200);
        cfg.arenaGearTolerance = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.NivelObjetoTolerancia", 15);
        cfg.arenaTimeoutSecs = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.EsperaSegundos", 90);
        cfg.arenaMaxSecs     = sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.DuracionMaxSegundos", 600);
        cfg.arenaMode        = sConfigMgr->GetOption<std::string>("AdaptiveAI.Arena.Modo", "mixto");
        if (cfg.arenaMode != "entrenar" && cfg.arenaMode != "contraste" && cfg.arenaMode != "mixto")
        {
            LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Arena.Modo = '{}' no es entrenar|contraste|mixto: se usa mixto.", cfg.arenaMode);
            cfg.arenaMode = "mixto";
        }
        std::string pairs = sConfigMgr->GetOption<std::string>("AdaptiveAI.Arena.Pares", "warrior:mage");
        if (!ParseComps(pairs, cfg.arenaPairs))
        {
            LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Arena.Pares = '{}' no se entiende: se usa warrior:mage.", pairs);
            ParseComps("warrior:mage", cfg.arenaPairs);
        }
        cfg.arenaTypes.clear();
        std::string typesText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Arena.Tipos", "1c1,2c2,3c3,5c5");
        for (std::string_view t : Acore::Tokenize(typesText, ',', false))
        {
            std::string x = Trim(t);
            if (!x.empty() && (x[0] == '1' || x[0] == '2' || x[0] == '3' || x[0] == '5'))
                cfg.arenaTypes.push_back(uint8(x[0] - '0'));
        }
        cfg.classes.clear();
        std::string classesText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Clases", "");
        for (std::string_view t : Acore::Tokenize(classesText, ',', false))
        {
            int8 spec;
            if (uint8 c = ParseClass(Trim(t), spec))
                cfg.classes.push_back(c);
            else
                LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Clases: '{}' no es una clase.", std::string(t));
        }

        // Acciones que no se ofrecen a una clase aunque las tenga. No se borran
        // del catálogo a propósito: los índices no se mueven y no se pierde ni
        // una fila de lo aprendido, así que quitar o poner una es reversible en
        // caliente. Sirve para el kit de otra forma o de otra rama, que el bot
        // conoce (así que la pieza A no lo filtra) pero que en su spec de arena
        // sólo dispersa el aprendizaje.
        cfg.excludedActions.clear();
        std::string excludeText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Acciones.Excluir", "");
        for (std::string_view t : Acore::Tokenize(excludeText, ',', false))
        {
            std::string item = Trim(t);
            size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            int8 spec;
            uint8 cls = ParseClass(item.substr(0, colon), spec);
            std::string action = Trim(item.substr(colon + 1));
            if (cls && !action.empty())
                cfg.excludedActions.insert({ cls, action });
            else
                LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Acciones.Excluir: no entiendo '{}'.", item);
        }

        cfg.level80Targets.clear();
        std::string level80Text = sConfigMgr->GetOption<std::string>("AdaptiveAI.Bots.Nivel80", "");
        for (std::string_view t : Acore::Tokenize(level80Text, ',', false))
        {
            std::string item = Trim(t);
            size_t colon = item.find(':');
            int8 spec;
            uint8 cls = colon == std::string::npos ? 0 : ParseClass(item.substr(0, colon), spec);
            Optional<uint32> n = colon == std::string::npos ? std::nullopt : Acore::StringTo<uint32>(item.substr(colon + 1));
            if (cls && n)
                cfg.level80Targets[cls] = std::min<uint32>(*n, 60);
            else if (!item.empty())
                LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Bots.Nivel80: '{}' no es clase:numero.", item);
        }

        cfg.arenaSpecs.clear();
        std::string specsText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Arena.Specs", "");
        for (std::string_view t : Acore::Tokenize(specsText, ',', false))
        {
            std::string item = Trim(t);
            size_t colon = item.find(':');
            int8 spec;
            uint8 cls = colon == std::string::npos ? 0 : ParseClass(item.substr(0, colon), spec);
            if (cls && colon + 1 < item.size())
                cfg.arenaSpecs[cls] = Trim(item.substr(colon + 1));
            else if (!item.empty())
                LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Arena.Specs: '{}' no es clase:nombre de spec.", item);
        }
        std::string specsPvpText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Loadout.SpecPvP", "");
        if (!specsPvpText.empty())
        {
            cfg.arenaSpecs.clear();
            for (std::string_view t : Acore::Tokenize(specsPvpText, ',', false))
            {
                std::string item = Trim(t);
                size_t colon = item.find(':');
                int8 spec;
                uint8 cls = colon == std::string::npos ? 0 : ParseClass(item.substr(0, colon), spec);
                if (cls && colon + 1 < item.size())
                    cfg.arenaSpecs[cls] = Trim(item.substr(colon + 1));
            }
        }
        cfg.loadoutEnable    = sConfigMgr->GetOption<bool>("AdaptiveAI.Loadout.Enable", true);
        cfg.loadoutPerPass   = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AdaptiveAI.Loadout.PorPasada", 5), 1, 50);
        cfg.loadoutCapLevels.clear();
        std::string capText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Loadout.NivelesTope", "60,70,80");
        for (std::string_view t : Acore::Tokenize(capText, ',', false))
            if (Optional<uint32> n = Acore::StringTo<uint32>(t); n && *n >= 1 && *n <= 80)
                cfg.loadoutCapLevels.push_back(uint8(*n));
        cfg.loadoutPvpIlvl.clear();
        std::string pvpIlvlText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Loadout.EquipoPvP", "0:232,1600:251,1800:264,2200:270");
        for (std::string_view t : Acore::Tokenize(pvpIlvlText, ',', false))
        {
            std::string item = Trim(t);
            size_t colon = item.find(':');
            Optional<uint32> r = colon == std::string::npos ? std::nullopt : Acore::StringTo<uint32>(item.substr(0, colon));
            Optional<uint32> il = colon == std::string::npos ? std::nullopt : Acore::StringTo<uint32>(item.substr(colon + 1));
            if (r && il)
                cfg.loadoutPvpIlvl[*r] = *il;
        }
        if (cfg.loadoutPvpIlvl.empty())
            cfg.loadoutPvpIlvl[0] = 232;
        std::string pveIlvlText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Loadout.EquipoPvE", "normal:187,heroica:200,banda10:219,banda25:232,mundo:0");
        for (std::string_view t : Acore::Tokenize(pveIlvlText, ',', false))
        {
            std::string item = Trim(t);
            size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = item.substr(0, colon);
            Optional<uint32> il = Acore::StringTo<uint32>(item.substr(colon + 1));
            if (!il) continue;
            if (key == "normal") cfg.loadoutPveNormal = *il;
            else if (key == "heroica") cfg.loadoutPveHeroic = *il;
            else if (key == "banda10") cfg.loadoutPveRaid10 = *il;
            else if (key == "banda25") cfg.loadoutPveRaid25 = *il;
            else if (key == "mundo") cfg.loadoutPveWorld = *il;
        }

        cfg.matchupTargets.clear();
        std::string targetsText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Objetivos", "");
        for (std::string_view t : Acore::Tokenize(targetsText, ',', false))
        {
            std::string item = Trim(t);
            size_t colon = item.find(':'), eq = item.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon)
                continue;
            int8 spec;
            uint8 a = ParseClass(Trim(item.substr(0, colon)), spec);
            uint8 b = ParseClass(Trim(item.substr(colon + 1, eq - colon - 1)), spec);
            Optional<uint32> pct = Acore::StringTo<uint32>(Trim(item.substr(eq + 1)));
            if (a && b && pct && *pct <= 100)
                cfg.matchupTargets[{ a, b }] = uint8(*pct);
            else
                LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Objetivos: '{}' no es clase:clase=porcentaje.", item);
        }
        cfg.objectivesStop     = sConfigMgr->GetOption<bool>("AdaptiveAI.Objetivos.Parar", true);
        cfg.objectiveMinMatches = std::max<uint32>(20, sConfigMgr->GetOption<uint32>("AdaptiveAI.Objetivos.PartidasMinimas", 200));
        cfg.objectiveMargin    = sConfigMgr->GetOption<uint32>("AdaptiveAI.Objetivos.Margen", 5);

        cfg.bgEnable         = sConfigMgr->GetOption<bool>("AdaptiveAI.Bg.Enable", false);
        cfg.bgTypes.clear();
        std::string bgText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Bg.Mapas", "WS,AB,EY");
        for (std::string_view t : Acore::Tokenize(bgText, ',', false))
        {
            if (uint8 type = ParseBgType(Trim(t)))
                cfg.bgTypes.push_back(type);
            else
                LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Bg.Mapas: '{}' no es WS, AB, EY, AV, SA ni IC.", std::string(t));
        }
        cfg.bgEveryMinutes   = sConfigMgr->GetOption<uint32>("AdaptiveAI.Bg.CadaMinutos", 30);
        cfg.bgSimultaneous   = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AdaptiveAI.Bg.Simultaneos", 1));
        cfg.bgPlayersPerTeam = std::min<uint32>(40, sConfigMgr->GetOption<uint32>("AdaptiveAI.Bg.PorEquipo", 0));
        cfg.bgLevel          = sConfigMgr->GetOption<uint32>("AdaptiveAI.Bg.Nivel", 80);
        cfg.bgMaxMinutes     = std::max<uint32>(5, sConfigMgr->GetOption<uint32>("AdaptiveAI.Bg.DuracionMaxMinutos", 30));
        cfg.bgTimeoutSecs    = std::max<uint32>(60, sConfigMgr->GetOption<uint32>("AdaptiveAI.Bg.EsperaSegundos", 150));
        cfg.bgMode           = sConfigMgr->GetOption<std::string>("AdaptiveAI.Bg.Modo", "contraste");
        if (cfg.bgMode != "entrenar" && cfg.bgMode != "contraste" && cfg.bgMode != "mixto" && cfg.bgMode != "referencia")
            cfg.bgMode = "contraste";
        cfg.exportFile       = sConfigMgr->GetOption<std::string>("AdaptiveAI.Exportar.Fichero", "adaptive_entrenado.sql");

        cfg.calibrateMinutes = sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.CadaMinutos", 60);
        cfg.calibrateMatches = sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.Combates", 5000);
        cfg.calibrateMargin  = sConfigMgr->GetOption<float>("AdaptiveAI.Calibrar.MargenMinimo", 0.60f);
        cfg.calibrateMinNew  = sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.DecisionesNuevasMinimas", 200);
        cfg.calibrateSkipSaturated = sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.SaltarSaturadas", 5);
        cfg.roundMaxExams        = sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.RondaMaxExamenes", 6);
        cfg.calibratePerClass = sConfigMgr->GetOption<bool>("AdaptiveAI.Calibrar.PorClase", true);
        cfg.calibrateTypes.clear();
        std::string calibTypesText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Calibrar.Tipos", "1c1");
        for (std::string_view t : Acore::Tokenize(calibTypesText, ',', false))
        {
            std::string x = Trim(t);
            if (!x.empty() && (x[0] == '1' || x[0] == '2' || x[0] == '3' || x[0] == '5'))
                cfg.calibrateTypes.push_back(uint8(x[0] - '0'));
        }
        if (cfg.calibrateTypes.empty())
            cfg.calibrateTypes.push_back(1);
        cfg.calibrateMaxMinutes = sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.MinutosMaximos", 60);
        cfg.calibrateClock   = sConfigMgr->GetOption<bool>("AdaptiveAI.Calibrar.Reloj", true);
        cfg.calibrateExclusive = sConfigMgr->GetOption<bool>("AdaptiveAI.Calibrar.Exclusiva", true);
        cfg.calibratePerClassMin = std::max<uint32>(10, sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.PartidasPorClase", 100));
        cfg.calibrateCutWhenJudged = sConfigMgr->GetOption<bool>("AdaptiveAI.Calibrar.CortarAlJuzgar", true);
        cfg.calibrateCutMatches = sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.PartidasCorte", 0);
        cfg.calibrateClassGain = std::clamp(sConfigMgr->GetOption<float>("AdaptiveAI.Calibrar.MejoraPorClase", 5.0f), 0.0f, 50.0f) / 100.0f;
        cfg.calibrateMaxCycles = sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.CiclosMaximos", 6);
        cfg.calibratePerPair = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.PorPareja", 4));
        cfg.calibrateWindow  = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AdaptiveAI.Calibrar.VentanaCiclos", 2), 1, 10);
        cfg.calibrateSameRival = sConfigMgr->GetOption<bool>("AdaptiveAI.Calibrar.MismoRival", true);
        cfg.calibrateZ       = std::clamp(sConfigMgr->GetOption<float>("AdaptiveAI.Calibrar.Z", 1.64f), 0.0f, 5.0f);
        cfg.calibrateLadderRule = sConfigMgr->GetOption<bool>("AdaptiveAI.Calibrar.VaraSerie", false);
        cfg.arenaProbePct    = std::min<uint32>(30, sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.EscaleraCandidata", 5));
        cfg.greedyMinVisits  = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("AdaptiveAI.Decision.VisitasMinimas", 5));
        cfg.rejectLearns     = sConfigMgr->GetOption<bool>("AdaptiveAI.Decision.RechazoAprende", false);
        cfg.approvedKeepLearning = sConfigMgr->GetOption<bool>("AdaptiveAI.Arena.AprobadasEntrenan", true);
        cfg.approvedWithModelPct = std::min<uint32>(100, sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.AprobadaConModelo", 50));
        cfg.calibrateAllPlay = sConfigMgr->GetOption<bool>("AdaptiveAI.Calibrar.TodasJuegan", true);
        cfg.arenaGenerationsPct = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.Generaciones", 15));
        cfg.arenaLadderPct   = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.Escalera", 15));
        cfg.ladderMinMatches = std::max<uint32>(10, sConfigMgr->GetOption<uint32>("AdaptiveAI.Escalera.PartidasMinimas", 30));
        cfg.ladderWindow     = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AdaptiveAI.Escalera.Ventana", 200), 50, 5000);
        cfg.ladderSince      = sConfigMgr->GetOption<uint32>("AdaptiveAI.Escalera.DesdeUnix", 0);
        cfg.generationsKeep  = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AdaptiveAI.Generaciones.Guardar", 12), 1, 20);
        cfg.generalize       = sConfigMgr->GetOption<bool>("AdaptiveAI.Generalizar", true);

        cfg.arenaReferencePct = std::min<uint32>(50, sConfigMgr->GetOption<uint32>("AdaptiveAI.Arena.Referencia", 10));

        cfg.epsilon          = sConfigMgr->GetOption<float>("AdaptiveAI.Epsilon", 0.05f);
        cfg.alpha            = sConfigMgr->GetOption<float>("AdaptiveAI.Alpha", 0.10f);
        cfg.alphaDecay       = sConfigMgr->GetOption<bool>("AdaptiveAI.Alpha.Decreciente", true);
        cfg.gamma            = sConfigMgr->GetOption<float>("AdaptiveAI.Gamma", 0.90f);
        cfg.explorationBonus = sConfigMgr->GetOption<float>("AdaptiveAI.Exploracion.Bonus", 3.0f);
        cfg.returnWeight     = sConfigMgr->GetOption<float>("AdaptiveAI.Retorno.Peso", 0.5f);
        cfg.interruptOnlyCasting = sConfigMgr->GetOption<bool>("AdaptiveAI.Interrupcion.SoloCasteando", true);
        cfg.kiteDistance     = std::clamp(sConfigMgr->GetOption<float>("AdaptiveAI.Kitear.Distancia", 20.0f), 10.0f, 35.0f);
        cfg.kiteMs           = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AdaptiveAI.Kitear.Ms", 3000), 1000, 8000);
        cfg.decisionIntervalMs = sConfigMgr->GetOption<uint32>("AdaptiveAI.Decision.IntervaloMs", 1500);
        cfg.decisionMinGapMs = sConfigMgr->GetOption<uint32>("AdaptiveAI.Decision.MinimoMs", 400);
        cfg.tickMs           = sConfigMgr->GetOption<uint32>("AdaptiveAI.Decision.TickMs", 200);
        cfg.respectOwnCast   = sConfigMgr->GetOption<bool>("AdaptiveAI.Decision.RespetarCasteo", true);
        cfg.respectForm      = sConfigMgr->GetOption<bool>("AdaptiveAI.Decision.RespetarForma", true);

        cfg.defaultDifficulty = uint8(std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AdaptiveAI.Dificultad.PorDefecto", 3), 1, 6));
        Difficulty const defaults[7] = { {0, 0}, {0.50f, 0.40f}, {0.35f, 0.30f}, {0.20f, 0.20f}, {0.10f, 0.10f}, {0.03f, 0.03f}, {0, 0} };
        for (uint32 i = 1; i <= 6; ++i)
        {
            std::string key = "AdaptiveAI.Dificultad." + std::to_string(i);
            cfg.difficulty[i] = ParseDifficulty(sConfigMgr->GetOption<std::string>(key, ""), defaults[i]);
        }

        cfg.brackets.clear();
        std::string bracketsText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Brackets", "1000,2000,3000,4000,5000");
        for (std::string_view b : Acore::Tokenize(bracketsText, ',', false))
            if (Optional<float> v = Acore::StringTo<float>(b))
                cfg.brackets.push_back(*v);
        if (cfg.brackets.size() != 5)
            cfg.brackets = { 1000.f, 2000.f, 3000.f, 4000.f, 5000.f };

        cfg.logDecisions     = sConfigMgr->GetOption<bool>("AdaptiveAI.Log.Decisiones", false);
        cfg.auditBot         = sConfigMgr->GetOption<uint32>("AdaptiveAI.Log.AuditarBot", 0);
        cfg.auditClassMask = 0;
        std::string auditClassesText = sConfigMgr->GetOption<std::string>("AdaptiveAI.Log.AuditarClases", "");
        for (std::string_view t : Acore::Tokenize(auditClassesText, ',', false))
        {
            int8 spec;
            if (uint8 cls = ParseClass(Trim(t), spec)) cfg.auditClassMask |= uint32(1) << cls;
        }
        cfg.auditSessions = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("AdaptiveAI.Log.AuditarSesiones", 3), 1, 20);
        cfg.logAuras         = sConfigMgr->GetOption<bool>("AdaptiveAI.Log.Auras", false);
        cfg.warlockPvpPet    = sConfigMgr->GetOption<std::string>("AdaptiveAI.Brujo.MascotaPvP", "felhunter");
        cfg.logMatches       = sConfigMgr->GetOption<bool>("AdaptiveAI.Log.Partidas", true);
        cfg.tracesGM         = sConfigMgr->GetOption<bool>("AdaptiveAI.Trazas.GM", true);
        cfg.matchRetentionDays = sConfigMgr->GetOption<uint32>("AdaptiveAI.Log.PartidasDias", 30);

        cfg.rDamage          = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.Dano", 10.0f);
        cfg.rInterruptOk     = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.InterrupcionOk", 5.0f);
        cfg.rInterruptBad    = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.InterrupcionMal", -2.0f);
        cfg.rCcDone          = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.ControlHecho", 3.0f);
        cfg.rCcTaken         = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.ControlSufrido", -3.0f);
        cfg.rSnareDone       = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.SnareHecho", 1.0f);
        cfg.rSnareTaken      = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.SnareSufrido", -1.0f);
        cfg.rKill            = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.Kill", 30.0f);
        cfg.rDeath           = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.Muerte", -30.0f);
        cfg.rWin             = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.Victoria", 20.0f);
        cfg.rLoss            = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.Derrota", -20.0f);
        cfg.rWastedDefensive = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.DefensivaDesperdiciada", -1.0f);
        cfg.rFailedAction    = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.AccionFallida", -0.2f);
        cfg.rTime            = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.Tiempo", -0.05f);
        cfg.rDistance        = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.Distancia", 0.3f);
        cfg.rShapingCap      = std::max(0.0f, sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.TopeSucesos", 40.0f));
        cfg.rTeamKill        = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.KillEquipo", 10.0f);
        cfg.rAllyDeath       = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.MuerteAliado", -10.0f);
        cfg.rObjective       = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.Objetivo", 15.0f);
        cfg.rObjectiveTeam   = sConfigMgr->GetOption<float>("AdaptiveAI.Recompensa.ObjetivoEquipo", 5.0f);
        cfg.personalityWeight = sConfigMgr->GetOption<float>("AdaptiveAI.Personalidad.Peso", 0.5f);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Catálogo de acciones por clase
    //
    //  Los nombres son los de las acciones de playerbots (fase 0: inventario de
    //  WarriorAiObjectContext.cpp, MageAiObjectContext.cpp y ActionContext.h).
    //  La primera siempre es NONE: dejar hacer al motor de playerbots.
    // ─────────────────────────────────────────────────────────────────────────
    static std::vector<ActionDef> const s_none = {
        { "none", KIND_NONE, true, false },
    };

    static std::vector<ActionDef> const s_warrior = {
        { "none",                 KIND_NONE,      true,  false },
        { "charge",               KIND_MOBILITY,  false, true  },
        { "intercept",            KIND_MOBILITY,  false, true  },
        { "pummel",               KIND_INTERRUPT, false, true  },
        { "hamstring",            KIND_CONTROL,   false, true, true },
        { "piercing howl",        KIND_CONTROL,   true,  true  },
        { "intimidating shout",   KIND_CONTROL,   false, true  },
        { "berserker rage",       KIND_DEFENSIVE, true,  true  },
        { "bladestorm",           KIND_OFFENSIVE, true,  true  },
        { "mortal strike",        KIND_OFFENSIVE, false, true  },
        { "bloodthirst",          KIND_OFFENSIVE, false, true  },
        { "overpower",            KIND_OFFENSIVE, false, true  },
        { "execute",              KIND_OFFENSIVE, false, true  },
        { "rend",                 KIND_OFFENSIVE, false, true  },
        { "recklessness",         KIND_OFFENSIVE, true,  true  },
        { "death wish",           KIND_OFFENSIVE, true,  true  },
        { "enraged regeneration", KIND_DEFENSIVE, true,  true  },
        { "spell reflection",     KIND_DEFENSIVE, true,  true  },
        { "retaliation",          KIND_DEFENSIVE, true,  true  },
        { "shield wall",          KIND_DEFENSIVE, true,  true  },
        { "use trinket",          KIND_DEFENSIVE, true,  false },
        { "reach melee",          KIND_MOBILITY,  false, false },
    };

    static std::vector<ActionDef> const s_mage = {
        { "none",                    KIND_NONE,      true,  false },
        { "counterspell",            KIND_INTERRUPT, false, true  },
        { "polymorph",               KIND_CONTROL,   false, true  },
        { "frost nova",              KIND_CONTROL,   true,  true, true },
        { "deep freeze",             KIND_CONTROL,   false, true  },
        { "dragon's breath",         KIND_CONTROL,   true,  true  },
        { "blast wave",              KIND_CONTROL,   true,  true  },
        { "cone of cold",            KIND_CONTROL,   true,  true  },
        // "blink back" es como se llama en playerbots (CastBlinkBackAction);
        // con "blink" a secas DoSpecificAction no la encontraba nunca y cada
        // intento contaba como decisión desperdiciada (04/09/2026: 1.454
        // visitas con valor medio -0,01 en la candidata).
        { "blink back",              KIND_MOBILITY,  true,  true  },
        { "runaway",                 KIND_MOBILITY,  true,  false },
        { "ice block",               KIND_DEFENSIVE, true,  true  },
        { "ice barrier",             KIND_DEFENSIVE, true,  true  },
        { "mana shield",             KIND_DEFENSIVE, true,  true  },
        { "mirror image",            KIND_DEFENSIVE, true,  true  },
        { "cold snap",               KIND_DEFENSIVE, true,  true  },
        { "use trinket",             KIND_DEFENSIVE, true,  false },
        { "frostbolt",               KIND_OFFENSIVE, false, true  },
        { "ice lance",               KIND_OFFENSIVE, false, true  },
        { "fire blast",              KIND_OFFENSIVE, false, true  },
        { "fireball",                KIND_OFFENSIVE, false, true  },
        { "scorch",                  KIND_OFFENSIVE, false, true  },
        { "arcane blast",            KIND_OFFENSIVE, false, true  },
        { "arcane barrage",          KIND_OFFENSIVE, false, true  },
        { "icy veins",               KIND_OFFENSIVE, true,  true  },
        { "summon water elemental",  KIND_OFFENSIVE, true,  true  },
        { "presence of mind",        KIND_OFFENSIVE, true,  true  },
        { "arcane power",            KIND_OFFENSIVE, true,  true  },
        { "combustion",              KIND_OFFENSIVE, true,  true  },
        { "evocation",               KIND_RESOURCE,  true,  true  },
        // Propia del módulo (no existe en playerbots): clavar si está pegado y
        // alejarse hasta AdaptiveAI.Kitear.Distancia. Siempre al final: el
        // índice de cada acción es lo que guarda la tabla Q.
        { "kitear",                  KIND_MOBILITY,  true,  false },
    };

    // Fase 2 (03/09/2026): las ocho clases restantes, con los nombres de sus
    // contextos de playerbots (Ai/Class/<Clase>/<Clase>AiObjectContext.cpp).
    static std::vector<ActionDef> const s_rogue = {
        { "none",              KIND_NONE,      true,  false },
        { "kick",              KIND_INTERRUPT, false, true  },
        { "kidney shot",       KIND_CONTROL,   false, true, true },
        { "cheap shot",        KIND_CONTROL,   false, true  },
        { "gouge",             KIND_CONTROL,   false, true  },
        { "blind",             KIND_CONTROL,   false, true  },
        { "sap",               KIND_CONTROL,   false, true  },
        { "vanish",            KIND_DEFENSIVE, true,  true  },
        { "evasion",           KIND_DEFENSIVE, true,  true  },
        { "cloak of shadows",  KIND_DEFENSIVE, true,  true  },
        { "feint",             KIND_DEFENSIVE, true,  true  },
        { "sprint",            KIND_MOBILITY,  true,  true  },
        { "stealth",           KIND_DEFENSIVE, true,  true  },
        { "sinister strike",   KIND_OFFENSIVE, false, true  },
        { "mutilate",          KIND_OFFENSIVE, false, true  },
        { "backstab",          KIND_OFFENSIVE, false, true  },
        { "ambush",            KIND_OFFENSIVE, false, true  },
        { "garrote",           KIND_OFFENSIVE, false, true  },
        { "eviscerate",        KIND_OFFENSIVE, false, true  },
        { "envenom",           KIND_OFFENSIVE, false, true  },
        { "rupture",           KIND_OFFENSIVE, false, true  },
        { "expose armor",      KIND_OFFENSIVE, false, true  },
        { "slice and dice",    KIND_OFFENSIVE, true,  true  },
        { "hunger for blood",  KIND_OFFENSIVE, true,  true  },
        { "adrenaline rush",   KIND_OFFENSIVE, true,  true  },
        { "blade flurry",      KIND_OFFENSIVE, true,  true  },
        { "killing spree",     KIND_OFFENSIVE, true,  true  },
        { "cold blood",        KIND_OFFENSIVE, true,  true  },
        { "fan of knives",     KIND_OFFENSIVE, true,  true  },
        { "use trinket",       KIND_DEFENSIVE, true,  false },
        { "reach melee",       KIND_MOBILITY,  false, false },
    };

    static std::vector<ActionDef> const s_priest = {
        { "none",                    KIND_NONE,      true,  false },
        { "silence",                 KIND_INTERRUPT, false, true  },
        { "psychic scream",          KIND_CONTROL,   true,  true, true },
        { "power word: shield",      KIND_DEFENSIVE, true,  true  },
        { "renew",                   KIND_DEFENSIVE, true,  true  },
        { "flash heal",              KIND_DEFENSIVE, true,  true  },
        { "greater heal",            KIND_DEFENSIVE, true,  true  },
        { "binding heal",            KIND_DEFENSIVE, true,  true  },
        { "desperate prayer",        KIND_DEFENSIVE, true,  true  },
        { "pain suppression",        KIND_DEFENSIVE, true,  true  },
        { "dispersion",              KIND_DEFENSIVE, true,  true  },
        { "fade",                    KIND_DEFENSIVE, true,  true  },
        { "inner fire",              KIND_DEFENSIVE, true,  true  },
        { "power infusion",          KIND_OFFENSIVE, true,  true  },
        { "shadowform",              KIND_OFFENSIVE, true,  true  },
        { "shadow word: pain",       KIND_OFFENSIVE, false, true  },
        { "devouring plague",        KIND_OFFENSIVE, false, true  },
        { "vampiric touch",          KIND_OFFENSIVE, false, true  },
        { "mind flay",               KIND_OFFENSIVE, false, true  },
        { "mind blast",              KIND_OFFENSIVE, false, true  },
        { "shadow word: death",      KIND_OFFENSIVE, false, true  },
        { "holy fire",               KIND_OFFENSIVE, false, true  },
        { "smite",                   KIND_OFFENSIVE, false, true  },
        { "mana burn",               KIND_OFFENSIVE, false, true  },
        { "dispel magic on target",  KIND_OFFENSIVE, false, true  },
        { "mass dispel",             KIND_OFFENSIVE, false, true  },
        { "shadowfiend",             KIND_RESOURCE,  false, true  },
        { "hymn of hope",            KIND_RESOURCE,  true,  true  },
        { "use trinket",             KIND_DEFENSIVE, true,  false },
        { "kitear",                  KIND_MOBILITY,  true,  false },
    };

    static std::vector<ActionDef> const s_hunter = {
        { "none",                    KIND_NONE,      true,  false },
        { "silencing shot",          KIND_INTERRUPT, false, true  },
        { "concussive shot",         KIND_CONTROL,   false, true, true },
        { "wyvern sting",            KIND_CONTROL,   false, true  },
        { "wing clip",               KIND_CONTROL,   false, true  },
        { "intimidation",            KIND_CONTROL,   true,  true  },
        { "freezing trap",           KIND_CONTROL,   true,  true  },
        { "scare beast",             KIND_CONTROL,   false, true  },
        { "disengage",               KIND_MOBILITY,  true,  true  },
        { "feign death",             KIND_DEFENSIVE, true,  true  },
        { "deterrence",              KIND_DEFENSIVE, true,  true  },
        { "mend pet",                KIND_DEFENSIVE, true,  true  },
        { "aspect of the viper",     KIND_RESOURCE,  true,  true  },
        { "aspect of the dragonhawk",KIND_OFFENSIVE, true,  true  },
        { "rapid fire",              KIND_OFFENSIVE, true,  true  },
        { "bestial wrath",           KIND_OFFENSIVE, true,  true  },
        { "readiness",               KIND_OFFENSIVE, true,  true  },
        { "kill command",            KIND_OFFENSIVE, true,  true  },
        { "hunter's mark",           KIND_OFFENSIVE, false, true  },
        { "serpent sting",           KIND_OFFENSIVE, false, true  },
        { "viper sting",             KIND_OFFENSIVE, false, true  },
        { "scorpid sting",           KIND_OFFENSIVE, false, true  },
        { "aimed shot",              KIND_OFFENSIVE, false, true  },
        { "chimera shot",            KIND_OFFENSIVE, false, true  },
        { "arcane shot",             KIND_OFFENSIVE, false, true  },
        { "steady shot",             KIND_OFFENSIVE, false, true  },
        { "multi-shot",              KIND_OFFENSIVE, false, true  },
        { "kill shot",               KIND_OFFENSIVE, false, true  },
        { "explosive shot base",     KIND_OFFENSIVE, false, true  },
        { "black arrow",             KIND_OFFENSIVE, false, true  },
        { "raptor strike",           KIND_OFFENSIVE, false, true  },
        { "mongoose bite",           KIND_OFFENSIVE, false, true  },
        { "use trinket",             KIND_DEFENSIVE, true,  false },
        { "kitear",                  KIND_MOBILITY,  true,  false },
    };

    static std::vector<ActionDef> const s_paladin = {
        { "none",                      KIND_NONE,      true,  false },
        { "hammer of justice",         KIND_CONTROL,   false, true, true },
        { "repentance",                KIND_CONTROL,   false, true  },
        { "divine shield",             KIND_DEFENSIVE, true,  true  },
        { "divine protection",         KIND_DEFENSIVE, true,  true  },
        { "lay on hands",              KIND_DEFENSIVE, true,  true  },
        { "flash of light",            KIND_DEFENSIVE, true,  true  },
        { "holy light",                KIND_DEFENSIVE, true,  true  },
        { "holy shock",                KIND_DEFENSIVE, true,  true  },
        { "cleanse magic",             KIND_DEFENSIVE, true,  true  },
        { "divine plea",               KIND_RESOURCE,  true,  true  },
        { "divine favor",              KIND_OFFENSIVE, true,  true  },
        { "avenging wrath",            KIND_OFFENSIVE, true,  true  },
        { "seal of command",           KIND_OFFENSIVE, true,  true  },
        { "judgement of light",        KIND_OFFENSIVE, false, true  },
        { "judgement of wisdom",       KIND_OFFENSIVE, false, true  },
        { "judgement of justice",      KIND_OFFENSIVE, false, true  },
        { "crusader strike",           KIND_OFFENSIVE, false, true  },
        { "divine storm",              KIND_OFFENSIVE, true,  true  },
        { "exorcism",                  KIND_OFFENSIVE, false, true  },
        { "hammer of wrath",           KIND_OFFENSIVE, false, true  },
        { "consecration",              KIND_OFFENSIVE, true,  true  },
        { "holy wrath",                KIND_OFFENSIVE, true,  true  },
        { "shield of righteousness",   KIND_OFFENSIVE, false, true  },
        { "hammer of the righteous",   KIND_OFFENSIVE, false, true  },
        { "avenger's shield",          KIND_OFFENSIVE, false, true  },
        { "use trinket",               KIND_DEFENSIVE, true,  false },
        { "reach melee",               KIND_MOBILITY,  false, false },
    };

    static std::vector<ActionDef> const s_dk = {
        { "none",                  KIND_NONE,      true,  false },
        { "mind freeze",           KIND_INTERRUPT, false, true  },
        { "strangulate",           KIND_INTERRUPT, false, true  },
        { "death grip",            KIND_CONTROL,   false, true  },
        { "chains of ice",         KIND_CONTROL,   false, true, true },
        { "hungering cold",        KIND_CONTROL,   true,  true  },
        { "icebound fortitude",    KIND_DEFENSIVE, true,  true  },
        { "anti magic shell",      KIND_DEFENSIVE, true,  true  },
        { "death pact",            KIND_DEFENSIVE, true,  true  },
        { "vampiric blood",        KIND_DEFENSIVE, true,  true  },
        { "rune tap",              KIND_DEFENSIVE, true,  true  },
        { "bone shield",           KIND_DEFENSIVE, true,  true  },
        { "unbreakable armor",     KIND_DEFENSIVE, true,  true  },
        { "death strike",          KIND_OFFENSIVE, false, true  },
        { "icy touch",             KIND_OFFENSIVE, false, true  },
        { "plague strike",         KIND_OFFENSIVE, false, true  },
        { "blood strike",          KIND_OFFENSIVE, false, true  },
        { "heart strike",          KIND_OFFENSIVE, false, true  },
        { "obliterate",            KIND_OFFENSIVE, false, true  },
        { "frost strike",          KIND_OFFENSIVE, false, true  },
        { "howling blast",         KIND_OFFENSIVE, false, true  },
        { "scourge strike",        KIND_OFFENSIVE, false, true  },
        { "death coil",            KIND_OFFENSIVE, false, true  },
        { "blood boil",            KIND_OFFENSIVE, true,  true  },
        { "pestilence",            KIND_OFFENSIVE, false, true  },
        { "death and decay",       KIND_OFFENSIVE, false, true  },
        { "unholy blight",         KIND_OFFENSIVE, true,  true  },
        { "raise dead",            KIND_OFFENSIVE, true,  true  },
        { "summon gargoyle",       KIND_OFFENSIVE, true,  true  },
        { "army of the dead",      KIND_OFFENSIVE, true,  true  },
        { "dancing rune weapon",   KIND_OFFENSIVE, true,  true  },
        { "horn of winter",        KIND_OFFENSIVE, true,  true  },
        { "empower rune weapon",   KIND_RESOURCE,  true,  true  },
        { "blood tap",             KIND_RESOURCE,  true,  true  },
        { "use trinket",           KIND_DEFENSIVE, true,  false },
        { "reach melee",           KIND_MOBILITY,  false, false },
    };

    static std::vector<ActionDef> const s_shaman = {
        { "none",                  KIND_NONE,      true,  false },
        { "wind shear",            KIND_INTERRUPT, false, true  },
        { "frost shock",           KIND_CONTROL,   false, true, true },
        { "earthbind totem",       KIND_CONTROL,   true,  true  },
        { "thunderstorm",          KIND_CONTROL,   true,  true  },
        { "tremor totem",          KIND_DEFENSIVE, true,  true  },
        { "stoneclaw totem",       KIND_DEFENSIVE, true,  true  },
        { "earth shield",          KIND_DEFENSIVE, true,  true  },
        { "lightning shield",      KIND_DEFENSIVE, true,  true  },
        { "healing wave",          KIND_DEFENSIVE, true,  true  },
        { "lesser healing wave",   KIND_DEFENSIVE, true,  true  },
        { "riptide",               KIND_DEFENSIVE, true,  true  },
        { "water shield",          KIND_RESOURCE,  true,  true  },
        { "shamanistic rage",      KIND_RESOURCE,  true,  true  },
        { "purge",                 KIND_OFFENSIVE, false, true  },
        { "elemental mastery",     KIND_OFFENSIVE, true,  true  },
        { "bloodlust",             KIND_OFFENSIVE, true,  true  },
        { "heroism",               KIND_OFFENSIVE, true,  true  },
        { "feral spirit",          KIND_OFFENSIVE, true,  true  },
        { "flame shock",           KIND_OFFENSIVE, false, true  },
        { "earth shock",           KIND_OFFENSIVE, false, true  },
        { "lava burst",            KIND_OFFENSIVE, false, true  },
        { "lightning bolt",        KIND_OFFENSIVE, false, true  },
        { "chain lightning",       KIND_OFFENSIVE, false, true  },
        { "stormstrike",           KIND_OFFENSIVE, false, true  },
        { "lava lash",             KIND_OFFENSIVE, false, true  },
        { "fire nova",             KIND_OFFENSIVE, true,  true  },
        { "searing totem",         KIND_OFFENSIVE, true,  true  },
        { "magma totem",           KIND_OFFENSIVE, true,  true  },
        { "fire elemental totem",  KIND_OFFENSIVE, true,  true  },
        { "use trinket",           KIND_DEFENSIVE, true,  false },
        { "kitear",                KIND_MOBILITY,  true,  false },
    };

    static std::vector<ActionDef> const s_warlock = {
        { "none",                  KIND_NONE,      true,  false },
        { "spell lock",            KIND_INTERRUPT, false, true  },
        { "shadowfury",            KIND_CONTROL,   false, true, true },
        { "curse of exhaustion",   KIND_CONTROL,   false, true, true },
        { "curse of tongues",      KIND_CONTROL,   false, true  },
        { "shadow ward",           KIND_DEFENSIVE, true,  true  },
        { "drain life",            KIND_DEFENSIVE, false, true  },
        { "fel armor",             KIND_DEFENSIVE, true,  true  },
        { "life tap",              KIND_RESOURCE,  true,  true  },
        { "drain mana",            KIND_OFFENSIVE, false, true  },
        { "curse of agony",        KIND_OFFENSIVE, false, true  },
        { "curse of the elements", KIND_OFFENSIVE, false, true  },
        { "curse of doom",         KIND_OFFENSIVE, false, true  },
        { "corruption",            KIND_OFFENSIVE, false, true  },
        { "immolate",              KIND_OFFENSIVE, false, true  },
        { "unstable affliction",   KIND_OFFENSIVE, false, true  },
        { "haunt",                 KIND_OFFENSIVE, false, true  },
        { "shadow bolt",           KIND_OFFENSIVE, false, true  },
        { "incinerate",            KIND_OFFENSIVE, false, true  },
        { "conflagrate",           KIND_OFFENSIVE, false, true  },
        { "chaos bolt",            KIND_OFFENSIVE, false, true  },
        { "soul fire",             KIND_OFFENSIVE, false, true  },
        { "shadowburn",            KIND_OFFENSIVE, false, true  },
        { "searing pain",          KIND_OFFENSIVE, false, true  },
        { "drain soul",            KIND_OFFENSIVE, false, true  },
        { "metamorphosis",         KIND_OFFENSIVE, true,  true  },
        { "demonic empowerment",   KIND_OFFENSIVE, true,  true  },
        // Sin invocaciones: preguntar por ellas aturdia a la propia mascota en
        // cada decision (ver CanCast). La mascota de PvP la garantiza
        // OwnWarlockPet al empezar la partida.
        { "use trinket",           KIND_DEFENSIVE, true,  false },
        { "kitear",                KIND_MOBILITY,  true,  false },
    };

    static std::vector<ActionDef> const s_druid = {
        { "none",                  KIND_NONE,      true,  false },
        { "bash",                  KIND_CONTROL,   false, true, true },
        { "maim",                  KIND_CONTROL,   false, true, true },
        { "pounce",                KIND_CONTROL,   false, true  },
        { "entangling roots",      KIND_CONTROL,   false, true, true },
        { "typhoon",               KIND_CONTROL,   true,  true  },
        { "feral charge - bear",   KIND_MOBILITY,  false, true  },
        { "feral charge - cat",    KIND_MOBILITY,  false, true  },
        { "dash",                  KIND_MOBILITY,  true,  true  },
        { "travel form",           KIND_MOBILITY,  true,  true  },
        { "barkskin",              KIND_DEFENSIVE, true,  true  },
        { "survival instincts",    KIND_DEFENSIVE, true,  true  },
        { "frenzied regeneration", KIND_DEFENSIVE, true,  true  },
        { "nature's grasp",        KIND_DEFENSIVE, true,  true  },
        { "regrowth",              KIND_DEFENSIVE, true,  true  },
        { "rejuvenation",          KIND_DEFENSIVE, true,  true  },
        { "healing touch",         KIND_DEFENSIVE, true,  true  },
        { "nature's swiftness",    KIND_DEFENSIVE, true,  true  },
        { "prowl",                 KIND_DEFENSIVE, true,  true  },
        { "innervate",             KIND_RESOURCE,  true,  true  },
        { "bear form",             KIND_DEFENSIVE, true,  true  },
        { "dire bear form",        KIND_DEFENSIVE, true,  true  },
        { "cat form",              KIND_OFFENSIVE, true,  true  },
        { "moonkin form",          KIND_OFFENSIVE, true,  true  },
        { "caster form",           KIND_OFFENSIVE, true,  true  },
        { "wrath",                 KIND_OFFENSIVE, false, true  },
        { "starfire",              KIND_OFFENSIVE, false, true  },
        { "moonfire",              KIND_OFFENSIVE, false, true  },
        { "insect swarm",          KIND_OFFENSIVE, false, true  },
        { "starfall",              KIND_OFFENSIVE, true,  true  },
        { "force of nature",       KIND_OFFENSIVE, true,  true  },
        { "faerie fire",           KIND_OFFENSIVE, false, true  },
        { "claw",                  KIND_OFFENSIVE, false, true  },
        { "shred",                 KIND_OFFENSIVE, false, true  },
        { "mangle (cat)",          KIND_OFFENSIVE, false, true  },
        { "rake",                  KIND_OFFENSIVE, false, true  },
        { "rip",                   KIND_OFFENSIVE, false, true  },
        { "ferocious bite",        KIND_OFFENSIVE, false, true  },
        { "ravage",                KIND_OFFENSIVE, false, true  },
        { "tiger's fury",          KIND_OFFENSIVE, true,  true  },
        { "savage roar",           KIND_OFFENSIVE, true,  true  },
        { "berserk",               KIND_OFFENSIVE, true,  true  },
        { "enrage",                KIND_OFFENSIVE, true,  true  },
        { "mangle (bear)",         KIND_OFFENSIVE, false, true  },
        { "maul",                  KIND_OFFENSIVE, false, true  },
        { "lacerate",              KIND_OFFENSIVE, false, true  },
        { "use trinket",           KIND_DEFENSIVE, true,  false },
        { "kitear",                KIND_MOBILITY,  true,  false },
    };

    // Acciones de equipo (2c2 o más y campos de batalla), las mismas para todas
    // las clases, siempre al final del catálogo. Cambian el objetivo del bot.
    static std::vector<ActionDef> const s_team = {
        { "asistir",       KIND_TEAM, false, false },   // el rival que más aliados están atacando
        { "foco sanador",  KIND_TEAM, false, false },   // el sanador enemigo vivo más cercano
        { "proteger",      KIND_TEAM, false, false },   // el rival que pega al aliado más bajo
    };

    std::vector<ActionDef> const& ActionsFor(uint8 cls)
    {
        static std::vector<ActionDef> full[MAX_CLASSES + 1];
        static std::once_flag once;
        std::call_once(once, []
        {
            for (uint8 c = 0; c <= MAX_CLASSES; ++c)
            {
                std::vector<ActionDef> const* base = &s_none;
                switch (c)
                {
                    case CLASS_WARRIOR:      base = &s_warrior; break;
                    case CLASS_MAGE:         base = &s_mage;    break;
                    case CLASS_ROGUE:        base = &s_rogue;   break;
                    case CLASS_PRIEST:       base = &s_priest;  break;
                    case CLASS_HUNTER:       base = &s_hunter;  break;
                    case CLASS_PALADIN:      base = &s_paladin; break;
                    case CLASS_DEATH_KNIGHT: base = &s_dk;      break;
                    case CLASS_SHAMAN:       base = &s_shaman;  break;
                    case CLASS_WARLOCK:      base = &s_warlock; break;
                    case CLASS_DRUID:        base = &s_druid;   break;
                    default: break;
                }
                full[c] = *base;
                if (base != &s_none)
                    full[c].insert(full[c].end(), s_team.begin(), s_team.end());
            }
        });
        return full[std::min<uint8>(cls, MAX_CLASSES)];
    }

    char const* KindName(ActionKind kind)
    {
        switch (kind)
        {
            case KIND_NONE:      return "nada";
            case KIND_OFFENSIVE: return "ofensiva";
            case KIND_DEFENSIVE: return "defensiva";
            case KIND_CONTROL:   return "control";
            case KIND_INTERRUPT: return "interrupcion";
            case KIND_MOBILITY:  return "movilidad";
            case KIND_RESOURCE:  return "recursos";
            case KIND_TEAM:      return "equipo";
        }
        return "?";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Estado
    // ─────────────────────────────────────────────────────────────────────────
    uint16 StateInfo::Key() const
    {
        uint16 k = selfHp;
        k = k * 3 + enemyHp;
        k = k * 3 + dist;
        k = k * 2 + enemyCasting;
        k = k * 2 + selfControlled;
        k = k * 2 + enemyControlled;
        k = k * 2 + lowResource;
        k = k * 2 + selfSnared;
        // Contextos por encima de la base de 864: equipo (0-8) y situación (0-7).
        // Situación 0 = las claves de antes del 03/09 (siguen valiendo).
        uint16 situation = uint16(keyReady) | uint16(mobilityReady << 1) | uint16(enemySnared << 2);
        return uint16(k + STATE_SOLO * (team + 9 * situation));
    }

    std::string StateInfo::Describe() const
    {
        static char const* hp[] = { "baja", "media", "alta" };
        static char const* ds[] = { "melee", "media", "lejos" };
        std::ostringstream out;
        out << "vida " << hp[selfHp] << "/enemigo " << hp[enemyHp] << ", dist " << ds[dist];
        if (enemyCasting)    out << ", enemigo casteando";
        if (selfControlled)  out << ", controlado";
        if (enemyControlled) out << ", enemigo controlado";
        if (lowResource)     out << ", sin recursos";
        if (selfSnared)      out << ", snare";
        if (keyReady)        out << ", control clave listo";
        if (mobilityReady)   out << ", movilidad lista";
        if (enemySnared)     out << ", enemigo ralentizado";
        if (team)
        {
            uint8 t = team - 1;
            out << " [equipo";
            if (t & 1) out << ", aliado bajo";
            if (t & 2) out << ", me enfocan";
            if (t & 4) out << ", sanador enemigo";
            out << "]";
        }
        return out.str();
    }

    static uint8 HpBucket(float pct)
    {
        return pct < 35.0f ? 0 : (pct < 70.0f ? 1 : 2);
    }

    static bool IsControlled(Unit* u)
    {
        return u->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED | UNIT_STATE_FLEEING | UNIT_STATE_ROOT) ||
               u->HasBreakableByDamageCrowdControlAura();
    }

    static uint8 SpecOf(Player* p)
    {
#ifdef ADAPTIVE_WITH_PLAYERBOTS
        return AiFactory::GetPlayerSpecTab(p);
#else
        (void)p;
        return 0;
#endif
    }

    static Player* AlivePlayer(ObjectGuid guid, Player* near)
    {
        Player* p = ObjectAccessor::FindPlayer(guid);
        if (!p || !p->IsInWorld() || !p->IsAlive() || p->GetMap() != near->GetMap())
            return nullptr;
        return p;
    }

    static TeamInfo ComputeTeam(Player* bot, Brain const* brain)
    {
        TeamInfo t;
        if (!brain || !brain->teamMatch)
            return t;
        t.inTeam = true;
        for (ObjectGuid guid : brain->allies)
            if (Player* a = AlivePlayer(guid, bot))
                if (a->GetHealthPct() < 35.0f)
                    t.allyLow = 1;
        uint32 onMe = 0;
        for (ObjectGuid guid : brain->enemies)
            if (Player* e = AlivePlayer(guid, bot))
            {
                if (e->GetVictim() == bot)
                    ++onMe;
                if (IsHealerSpec(e->getClass(), SpecOf(e)))
                    t.enemyHealer = 1;
            }
        t.focused = onMe >= 2 ? 1 : 0;
        return t;
    }

#ifdef ADAPTIVE_WITH_PLAYERBOTS
    // Preguntarle a playerbots si un hechizo se puede lanzar NO es gratis:
    // PlayerbotAI::CanCastSpell monta un Spell de verdad y llama a
    // CheckCast(strict = true). Y el core, en el caso SPELL_EFFECT_SUMMON_PET,
    // aprovecha esa comprobacion para aturdir a la mascota del brujo
    // (Summoning Disorientation, 32752) "para que no le pegue al dueno mientras
    // se reinvoca" (Spell.cpp ~6453). Como el modulo pregunta por cada accion
    // del catalogo en cada decision, el brujo se pasaba el combate entero con su
    // demonio aturdido y sin pegar: 498 aturdimientos en cinco minutos, y el
    // menor dano de las diez clases. Medido el 04/09/2026. Aqui no se pregunta
    // nunca por una invocacion de mascota; del demonio se encarga OwnWarlockPet.
    static bool IsSummonPetAction(PlayerbotAI* botAI, ActionDef const& def)
    {
        if (!def.isSpell || !botAI)
            return false;
        uint32 id = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", std::string(def.name))->Get();
        SpellInfo const* info = id ? sSpellMgr->GetSpellInfo(id) : nullptr;
        return info && info->HasEffect(SPELL_EFFECT_SUMMON_PET);
    }

    static bool CanCast(PlayerbotAI* botAI, ActionDef const& def, Player* bot, Unit* enemy)
    {
        if (IsSummonPetAction(botAI, def))
            return false;
        return botAI->CanCastSpell(def.name, def.selfTarget ? bot : enemy);
    }
#endif

    StateInfo ComputeState(Player* bot, Unit* enemy, Brain const* brain)
    {
        StateInfo s;
        s.team = ComputeTeam(bot, brain).Ctx();
        s.selfHp  = HpBucket(bot->GetHealthPct());
        s.enemyHp = HpBucket(enemy->GetHealthPct());
        float d = bot->GetDistance(enemy);
        s.dist = d <= 8.0f ? 0 : (d <= 20.0f ? 1 : 2);
        s.enemyCasting = enemy->IsNonMeleeSpellCast(false, false, true) ? 1 : 0;
        s.selfControlled = IsControlled(bot) ? 1 : 0;
        s.enemyControlled = IsControlled(enemy) ? 1 : 0;
        switch (bot->getPowerType())
        {
            case POWER_RAGE:   s.lowResource = bot->GetPower(POWER_RAGE) < 200 ? 1 : 0; break;      // 20 de ira
            case POWER_ENERGY: s.lowResource = bot->GetPower(POWER_ENERGY) < 30 ? 1 : 0; break;
            case POWER_RUNIC_POWER: s.lowResource = bot->GetPower(POWER_RUNIC_POWER) < 200 ? 1 : 0; break;
            default:           s.lowResource = bot->GetPowerPct(POWER_MANA) < 25.0f ? 1 : 0; break;
        }
        s.selfSnared = (bot->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) || bot->HasUnitState(UNIT_STATE_ROOT)) ? 1 : 0;
        s.enemySnared = enemy->HasAuraType(SPELL_AURA_MOD_DECREASE_SPEED) ? 1 : 0;
        // Situación: ¿tengo lista la herramienta clave (Nova, Tendón, Cadenas...)
        // y un hechizo de movilidad (Parpadeo, Carga...)? Es lo que un jugador
        // mira antes de huir o de cerrar. Mismo CanCastSpell que ActionAvailable:
        // dos a siete comprobaciones por tick según la clase.
#ifdef ADAPTIVE_WITH_PLAYERBOTS
        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            for (ActionDef const& def : ActionsFor(bot->getClass()))
            {
                if (!def.isSpell)
                    continue;
                bool wantKey = def.key && !s.keyReady;
                bool wantMob = def.kind == KIND_MOBILITY && !s.mobilityReady;
                if (!wantKey && !wantMob)
                    continue;
                if (!CanCast(botAI, def, bot, enemy))
                    continue;
                if (wantKey) s.keyReady = 1;
                if (wantMob) s.mobilityReady = 1;
            }
#endif
        return s;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Tabla Q
    // ─────────────────────────────────────────────────────────────────────────
    float QTable::Get(uint8 cls, uint8 enemyCls, uint16 state, uint8 action)
    {
        std::lock_guard<std::mutex> guard(_lock);
        auto it = _map.find(Key(cls, enemyCls, state, action));
        return it == _map.end() ? 0.0f : it->second.q;
    }

    QEntry QTable::Entry(uint8 cls, uint8 enemyCls, uint16 state, uint8 action)
    {
        std::lock_guard<std::mutex> guard(_lock);
        auto it = _map.find(Key(cls, enemyCls, state, action));
        return it == _map.end() ? QEntry() : it->second;
    }

    std::vector<QEntry> QTable::Row(uint8 cls, uint8 enemyCls, uint16 state, uint8 actionCount)
    {
        std::vector<QEntry> out(actionCount);
        std::lock_guard<std::mutex> guard(_lock);
        for (uint8 a = 0; a < actionCount; ++a)
        {
            auto it = _map.find(Key(cls, enemyCls, state, a));
            if (it != _map.end())
                out[a] = it->second;
        }
        return out;
    }

    float QTable::MaxQ(uint8 cls, uint8 enemyCls, uint16 state, uint8 actionCount, bool generalFallback)
    {
        std::lock_guard<std::mutex> guard(_lock);
        float best = 0.0f;   // las acciones sin entrada valen 0
        for (uint8 a = 0; a < actionCount; ++a)
        {
            auto it = _map.find(Key(cls, enemyCls, state, a));
            if ((it == _map.end() || !it->second.visits) && generalFallback && enemyCls)
                it = _map.find(Key(cls, 0, state, a));
            if (it != _map.end() && it->second.visits && it->second.q > best)
                best = it->second.q;
        }
        return best;
    }

    void QTable::Update(uint8 cls, uint8 enemyCls, uint16 state, uint8 action, float target, float alpha, bool countVisit)
    {
        std::lock_guard<std::mutex> guard(_lock);
        uint64 key = Key(cls, enemyCls, state, action);
        auto [it, inserted] = _map.try_emplace(key);
        if (inserted && cls < 16)
            ++_classRows[cls];
        QEntry& e = it->second;
        e.q += alpha * (target - e.q);
        if (countVisit)
            ++e.visits;
        if (!e.dirty)
        {
            e.dirty = true;
            _dirty.push_back(key);
        }
        ++_updates;
        ++_updatesSinceSave;
    }

    void QTable::Load()
    {
        std::lock_guard<std::mutex> guard(_lock);
        _map.clear();
        std::fill(std::begin(_classRows), std::end(_classRows), 0u);
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT clase, clase_enemiga, estado, accion, q, visitas FROM adaptive_q WHERE modelo = {}", _version);
        if (!result)
            return;
        do
        {
            Field* f = result->Fetch();
            QEntry e;
            e.q = f[4].Get<float>();
            e.visits = f[5].Get<uint32>();
            uint8 cls = f[0].Get<uint8>();
            if (_map.emplace(Key(cls, f[1].Get<uint8>(), f[2].Get<uint16>(), f[3].Get<uint8>()), e).second && cls < 16)
                ++_classRows[cls];
        } while (result->NextRow());
    }

    void QTable::SaveDirty(uint32 maxRows)
    {
        std::lock_guard<std::mutex> guard(_lock);
        SQLTransaction<PlayerbotsDatabaseConnection> trans = PlayerbotsDatabase.BeginTransaction();
        uint32 count = 0;
        // Se guarda por el final de la lista (lo último cambiado) y se para en
        // maxRows: lo que queda se lleva la siguiente llamada. Una clave puede
        // haber desaparecido (CopyClassFrom borra la clase) o estar ya limpia
        // (se guardó entera con maxRows = 0): se salta.
        while (!_dirty.empty() && (!maxRows || count < maxRows))
        {
            uint64 key = _dirty.back();
            _dirty.pop_back();
            auto it = _map.find(key);
            if (it == _map.end() || !it->second.dirty)
                continue;
            QEntry& e = it->second;
            e.dirty = false;
            uint8 cls = uint8(key >> 40), enemyCls = uint8(key >> 32), action = uint8(key);
            uint16 state = uint16((key >> 8) & 0xFFFF);
            trans->Append(Acore::StringFormat(
                "REPLACE INTO adaptive_q (modelo, clase, clase_enemiga, estado, accion, q, visitas) VALUES ({}, {}, {}, {}, {}, {}, {})",
                _version, cls, enemyCls, state, action, e.q, e.visits).c_str());
            ++count;
        }
        if (count)
            PlayerbotsDatabase.CommitTransaction(trans);
        if (_dirty.empty())
            _updatesSinceSave = 0;
    }

    size_t QTable::DirtyRows()
    {
        std::lock_guard<std::mutex> guard(_lock);
        return _dirty.size();
    }

    void QTable::CopyFrom(QTable& other)
    {
        std::scoped_lock guard(_lock, other._lock);
        _map = other._map;
        std::copy(std::begin(other._classRows), std::end(other._classRows), std::begin(_classRows));
        _dirty.clear();
        _dirty.reserve(_map.size());
        for (auto& [key, e] : _map)
        {
            e.dirty = true;
            _dirty.push_back(key);
        }
    }

    size_t QTable::ClassRows(uint8 cls)
    {
        std::lock_guard<std::mutex> guard(_lock);
        return cls < 16 ? _classRows[cls] : 0;
    }

    void QTable::CopyClassFrom(QTable& other, uint8 cls)
    {
        std::scoped_lock guard(_lock, other._lock);
        for (auto it = _map.begin(); it != _map.end();)
        {
            if (uint8(it->first >> 40) == cls)
            {
                // La fila desaparece de la memoria; en la base la pisa el REPLACE de
                // la copia (misma clave) o queda huérfana con visitas viejas, que
                // Load() vuelve a traer: se borra explícitamente.
                it = _map.erase(it);
            }
            else
                ++it;
        }
        PlayerbotsDatabase.Execute("DELETE FROM adaptive_q WHERE modelo = {} AND clase = {}", _version, cls);
        if (cls < 16)
            _classRows[cls] = 0;
        for (auto const& [key, e] : other._map)
            if (uint8(key >> 40) == cls)
            {
                QEntry copy = e;
                copy.dirty = true;
                _map[key] = copy;
                _dirty.push_back(key);
                if (cls < 16)
                    ++_classRows[cls];
            }
    }

    size_t QTable::Size()
    {
        std::lock_guard<std::mutex> guard(_lock);
        return _map.size();
    }

    std::vector<std::pair<uint64, QEntry>> QTable::Snapshot()
    {
        std::lock_guard<std::mutex> guard(_lock);
        std::vector<std::pair<uint64, QEntry>> out;
        out.reserve(_map.size());
        for (auto const& [key, e] : _map)
            out.emplace_back(key, e);
        return out;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Modelos: candidata y validada
    // ─────────────────────────────────────────────────────────────────────────
    static std::mutex s_modelLock;
    static std::shared_ptr<QTable> s_candidate;
    static std::shared_ptr<QTable> s_validated;

    std::shared_ptr<QTable> Candidate()
    {
        std::lock_guard<std::mutex> guard(s_modelLock);
        return s_candidate;
    }

    std::shared_ptr<QTable> Validated()
    {
        std::lock_guard<std::mutex> guard(s_modelLock);
        return s_validated;
    }

    std::vector<ModelInfo> ListModels()
    {
        std::vector<ModelInfo> models;
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT version, creado, validada, tasa_victoria, combates, nota, generacion FROM adaptive_model ORDER BY version");
        if (!result)
            return models;
        do
        {
            Field* f = result->Fetch();
            ModelInfo m;
            m.version = f[0].Get<uint32>();
            m.created = f[1].Get<std::string>();
            m.validated = f[2].Get<uint8>() != 0;
            m.winrate = f[3].Get<float>();
            m.matches = f[4].Get<uint32>();
            m.note = f[5].Get<std::string>();
            m.generation = f[6].Get<uint8>() != 0;
            models.push_back(m);
        } while (result->NextRow());
        return models;
    }

    static void InsertModel(uint32 version, bool validated, std::string note)
    {
        PlayerbotsDatabase.EscapeString(note);
        PlayerbotsDatabase.Execute(
            "INSERT IGNORE INTO adaptive_model (version, creado, validada, nota) VALUES ({}, '{}', {}, '{}')",
            version, Now(), validated ? 1 : 0, note);
    }

    void LoadModels()
    {
        std::vector<ModelInfo> models = ListModels();
        uint32 validated = 0, latest = 0;
        for (ModelInfo const& m : models)
        {
            latest = std::max(latest, m.version);
            if (m.validated)
                validated = std::max(validated, m.version);
        }
        if (!latest)
        {
            InsertModel(1, true, "inicial: tabla vacia, equivale a playerbots de serie");
            validated = latest = 1;
        }
        if (!validated)
            validated = latest;

        // La candidata es la version mas alta que no es validada ni generacion.
        // Antes era "la mas alta a secas", y una copia antigua cargada como
        // generacion con un numero por encima (05/09/2026, N8) habria pasado a
        // ser la candidata en el siguiente reinicio. Si no hay ninguna, una por
        // encima de la validada: lo que entrena nunca es lo que ven los jugadores.
        uint32 candidate = 0;
        for (ModelInfo const& m : models)
            if (!m.validated && !m.generation && m.version > validated)
                candidate = std::max(candidate, m.version);
        if (!candidate)
        {
            candidate = latest + 1;
            InsertModel(candidate, false, Acore::StringFormat("candidata a partir de la v{}", validated));
        }

        auto v = std::make_shared<QTable>(validated);
        v->Load();
        auto c = std::make_shared<QTable>(candidate);
        c->Load();
        if (c->Size() == 0 && v->Size() > 0)
        {
            c->CopyFrom(*v);
            c->SaveDirty();
        }

        std::lock_guard<std::mutex> guard(s_modelLock);
        s_validated = v;
        s_candidate = c;
        LOG_INFO("module", "[adaptive-ai] Modelo validado v{} ({} entradas), candidata v{} ({} entradas).",
                 validated, v->Size(), candidate, c->Size());
    }

    void RecordCalibration(float winrate, uint32 matches, bool promoted, std::string const& detail)
    {
        std::shared_ptr<QTable> c = Candidate();
        if (!c)
            return;
        std::string escaped = detail;
        PlayerbotsDatabase.EscapeString(escaped);
        // La nota es un registro que crece con cada calibracion. Si desborda la
        // columna, MySQL tumba el UPDATE ENTERO (errno 1406) y se pierden tambien
        // la tasa y los combates de esa calibracion: pasó el 03/09/2026 a las
        // 20:03. RIGHT() se queda con la cola, que es lo reciente.
        PlayerbotsDatabase.Execute(
            "UPDATE adaptive_model SET tasa_victoria = {}, combates = {}, nota = RIGHT(CONCAT(nota, ' | calibracion {}: {:.0f}% en {} combates{}{}'), 4000) WHERE version = {}",
            winrate, matches, Now(), winrate * 100.0f, matches, escaped.empty() ? std::string() : " (" + escaped + ")", promoted ? ", validada" : "", c->Version());
    }

    void PromoteCandidate(float winrate, uint32 matches, std::string const& detail)
    {
        std::shared_ptr<QTable> oldCandidate = Candidate();
        if (!oldCandidate)
            return;
        oldCandidate->SaveDirty();
        RecordCalibration(winrate, matches, true, detail);
        PlayerbotsDatabase.Execute("UPDATE adaptive_model SET validada = 0 WHERE version <> {}", oldCandidate->Version());
        PlayerbotsDatabase.Execute("UPDATE adaptive_model SET validada = 1, generacion = 1 WHERE version = {}", oldCandidate->Version());

        // Siempre por encima de TODAS las versiones (tambien de las generaciones
        // cargadas a mano), no solo de la candidata: INSERT IGNORE callaria el choque.
        uint32 next = oldCandidate->Version() + 1;
        for (ModelInfo const& m : ListModels())
            next = std::max(next, m.version + 1);
        InsertModel(next, false, Acore::StringFormat("candidata a partir de la v{}", oldCandidate->Version()));
        auto c = std::make_shared<QTable>(next);
        c->CopyFrom(*oldCandidate);
        c->SaveDirty();

        std::lock_guard<std::mutex> guard(s_modelLock);
        s_validated = oldCandidate;
        s_candidate = c;
        LOG_INFO("module", "[adaptive-ai] Calibracion: la candidata v{} gana el {:.0f}% de {} combates a la validada: pasa a validada. Nueva candidata v{}.",
                 oldCandidate->Version(), winrate * 100.0f, matches, next);
    }

    // Aprobado por clase. La validada nueva (versión N) es la validada de ahora
    // con las filas de esas clases sustituidas por las de la candidata; la
    // candidata sigue entrenando con un número de versión nuevo (N+1), para
    // que siga siendo "una por encima" de la validada. La validada anterior
    // se queda como generación (adaptive_model.generacion = 1).
    void PromoteClasses(std::vector<uint8> const& classes, std::string const& detail)
    {
        std::shared_ptr<QTable> c = Candidate();
        std::shared_ptr<QTable> v = Validated();
        if (!c || !v || classes.empty())
            return;
        c->SaveDirty();
        v->SaveDirty();
        uint32 latest = 0;
        for (ModelInfo const& m : ListModels())
            latest = std::max(latest, m.version);
        uint32 newValidated = latest + 1, newCandidate = latest + 2;

        std::string names;
        for (uint8 cls : classes)
            names += (names.empty() ? "" : ", ") + std::string(ClassName(cls));

        auto nv = std::make_shared<QTable>(newValidated);
        nv->CopyFrom(*v);
        for (uint8 cls : classes)
            nv->CopyClassFrom(*c, cls);
        InsertModel(newValidated, true, Acore::StringFormat("validada: v{} con {} de la candidata v{}", v->Version(), names, c->Version()));
        std::string escaped = detail;
        PlayerbotsDatabase.EscapeString(escaped);
        PlayerbotsDatabase.Execute("UPDATE adaptive_model SET validada = 0 WHERE version <> {}", newValidated);
        PlayerbotsDatabase.Execute("UPDATE adaptive_model SET generacion = 1, nota = RIGHT(CONCAT(nota, ' | {}'), 4000) WHERE version = {}", escaped, newValidated);
        nv->SaveDirty();

        InsertModel(newCandidate, false, Acore::StringFormat("candidata a partir de la v{} (sigue entrenando)", c->Version()));
        auto nc = std::make_shared<QTable>(newCandidate);
        nc->CopyFrom(*c);
        nc->SaveDirty();
        // La candidata vieja ya no es de nadie (ni validada ni generacion): sus
        // filas se van ahora y no en la purga del siguiente arranque, que dejaba
        // un cuarto de millon de filas por cada aprobado (1,27 M el 05/09/2026).
        PlayerbotsDatabase.Execute("DELETE FROM adaptive_q WHERE modelo = {}", c->Version());

        {
            std::lock_guard<std::mutex> guard(s_modelLock);
            s_validated = nv;
            s_candidate = nc;
        }
        LOG_INFO("module", "[adaptive-ai] Aprobado por clase: {} pasan a la validada v{} ({}). La candidata sigue como v{}.",
                 names, newValidated, detail, newCandidate);
    }

    // Devolverle a una clase las filas de la validada: se tira lo que esa clase
    // haya aprendido desde la última aprobación. Es el gatillo manual de lo que
    // Calibrar.CiclosMaximos hace solo, para cuando una clase se ha metido en un
    // pozo y bloquea la ronda (el brujo del 04/09: 15 % con la candidata donde
    // la validada saca 31 %).
    //
    // La guarda importante: si la validada NO tiene filas de esa clase, revertir
    // la deja a CERO y es peor que dejarla como está. Eso es exactamente lo que
    // pasó el 04/09 a las 02:00, cuando el tope de ciclos borró cinco clases que
    // nunca habían aprobado.
    bool RevertClass(uint8 cls, std::string& detail, bool force)
    {
        std::shared_ptr<QTable> c = Candidate();
        std::shared_ptr<QTable> v = Validated();
        if (!c || !v)
        {
            detail = "no hay modelos cargados";
            return false;
        }
        size_t rows = v->ClassRows(cls);
        if (!rows && !force)
        {
            detail = Acore::StringFormat("la validada v{} no tiene ni una fila de {}: revertir la dejaria a cero (anade \"forzar\" si es lo que quieres)", v->Version(), ClassName(cls));
            return false;
        }
        size_t before = c->ClassRows(cls);
        c->CopyClassFrom(*v, cls);
        c->SaveDirty();
        ArenaResetClassHistory(cls);
        detail = rows
            ? Acore::StringFormat("{}: las {} filas de la candidata v{} pasan a ser las {} de la validada v{}, y se borra su historial de examenes",
                                  ClassName(cls), before, c->Version(), rows, v->Version())
            : Acore::StringFormat("{}: las {} filas de la candidata v{} se VACIAN (la validada v{} tampoco tiene ninguna), asi que la clase vuelve a jugar de serie",
                                  ClassName(cls), before, c->Version(), v->Version());
        LOG_INFO("module", "[adaptive-ai] Vuelta a lo anterior a mano: {}.", detail);
        return true;
    }

    // ─── Generaciones ────────────────────────────────────────────────────────
    static std::mutex s_generationLock;
    static std::map<uint32, std::shared_ptr<QTable>> s_generationTables;

    std::vector<uint32> Generations()
    {
        std::vector<uint32> out;
        std::shared_ptr<QTable> c = Candidate();
        for (ModelInfo const& m : ListModels())
            if (m.generation && m.version > 1 && (!c || m.version != c->Version()))
                out.push_back(m.version);
        return out;
    }

    std::shared_ptr<QTable> GenerationTable(uint32 version)
    {
        if (std::shared_ptr<QTable> v = Validated())
            if (v->Version() == version)
                return v;
        std::lock_guard<std::mutex> guard(s_generationLock);
        auto it = s_generationTables.find(version);
        if (it != s_generationTables.end())
            return it->second;
        auto t = std::make_shared<QTable>(version);
        t->Load();
        if (!t->Size())
            return nullptr;
        // Como mucho las que se guardan en memoria; la más vieja fuera
        while (s_generationTables.size() >= cfg.generationsKeep)
            s_generationTables.erase(s_generationTables.begin());
        s_generationTables[version] = t;
        LOG_INFO("module", "[adaptive-ai] Generacion v{} cargada ({} entradas) como rival de la candidata.", version, t->Size());
        return t;
    }

    std::shared_ptr<QTable> TableOfVersion(uint32 version)
    {
        if (!version)
            return nullptr;
        if (std::shared_ptr<QTable> c = Candidate())
            if (c->Version() == version)
                return c;
        return GenerationTable(version);   // la validada o una generacion (cargada y cacheada)
    }

    bool UseModel(uint32 version, std::string& error)
    {
        bool found = false;
        for (ModelInfo const& m : ListModels())
            if (m.version == version)
                found = true;
        if (!found)
        {
            error = "no existe esa version";
            return false;
        }
        auto v = std::make_shared<QTable>(version);
        v->Load();
        PlayerbotsDatabase.Execute("UPDATE adaptive_model SET validada = 0 WHERE version > {}", version);
        PlayerbotsDatabase.Execute("UPDATE adaptive_model SET validada = 1 WHERE version = {}", version);
        std::lock_guard<std::mutex> guard(s_modelLock);
        s_validated = v;
        return true;
    }

    void SaveAllDirty(uint32 maxRowsPerTable)
    {
        if (auto c = Candidate()) c->SaveDirty(maxRowsPerTable);
        if (auto v = Validated()) v->SaveDirty(maxRowsPerTable);
    }

    // Delegado en SlowTick.h (modules/shared/): misma cabecera que usan ahora
    // mod-bot-operations y los módulos que reparten bots, en vez de esta copia local que fue la primera en tenerlo.
    void WarnIfSlow(char const* step, uint32 startMs)
    {
        SlowTick::WarnIfSlow("adaptive-ai", step, startMs);
    }

    // Limpieza: partidas de más de N días (y sus decisiones, si se guardaron) y
    // las tablas Q de versiones que ni son la validada ni la candidata. Lo
    // aprendido que importa (validada, candidata, perfiles) no se toca.
    // Columnas del loadout en adaptive_bot para bases anteriores al 03/09/2026
    void EnsureColumns()
    {
        // Aprobados por clase, ronda y acumulado de calibraciones (la fila
        // clase = 0 guarda la ronda en curso). Se crea antes de comprobar las
        // columnas, que abajo se le añaden si viene de una versión anterior.
        // El rating del jugador: el centro de la escalera en el modo espejo.
        PlayerbotsDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `adaptive_jugador` ("
            "`guid` INT UNSIGNED NOT NULL, `nombre` VARCHAR(12) NOT NULL DEFAULT '', `rating` FLOAT NOT NULL DEFAULT 1500, "
            "`victorias` INT UNSIGNED NOT NULL DEFAULT 0, `derrotas` INT UNSIGNED NOT NULL DEFAULT 0, `actualizado` DATETIME NULL, "
            "PRIMARY KEY (`guid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        PlayerbotsDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `adaptive_clase` ("
            "`clase` TINYINT UNSIGNED NOT NULL, `aprobada` TINYINT UNSIGNED NOT NULL DEFAULT 0, `ronda` INT UNSIGNED NOT NULL DEFAULT 1, "
            "`version` INT UNSIGNED NOT NULL DEFAULT 0, `fecha` DATETIME NULL, PRIMARY KEY (`clase`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        static char const* columns[][3] = {
            { "adaptive_bot",   "spec_pve",   "TINYINT NOT NULL DEFAULT -1" },
            { "adaptive_bot",   "spec_pvp",   "TINYINT NOT NULL DEFAULT -1" },
            { "adaptive_bot",   "proposito",  "VARCHAR(4) NOT NULL DEFAULT 'pve'" },
            { "adaptive_bot",   "ilvl",       "SMALLINT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_model", "generacion", "TINYINT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_clase", "cand_gana",   "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_clase", "cand_pierde", "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_clase", "val_gana",    "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_clase", "val_pierde",  "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_clase", "ciclos",      "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_clase", "historial",   "VARCHAR(255) NOT NULL DEFAULT ''" },
            { "adaptive_match", "version_a",   "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_match", "version_b",   "INT UNSIGNED NOT NULL DEFAULT 0" },
            { "adaptive_bot",   "desvio",       "TINYINT NOT NULL DEFAULT 0" },
            { "adaptive_bot",   "desvio_fecha", "INT UNSIGNED NOT NULL DEFAULT 0" },
        };
        for (auto const& col : columns)
        {
            QueryResult r = PlayerbotsDatabase.Query(
                "SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}' AND COLUMN_NAME = '{}'", col[0], col[1]);
            if (!r)
            {
                PlayerbotsDatabase.DirectExecute(Acore::StringFormat("ALTER TABLE {} ADD COLUMN `{}` {}", col[0], col[1], col[2]).c_str());
                LOG_INFO("module", "[adaptive-ai] {}: columna {} anadida.", col[0], col[1]);
            }
        }
    }

    void PurgeOldData()
    {
        std::shared_ptr<QTable> c = Candidate();
        std::shared_ptr<QTable> v = Validated();
        if (cfg.matchRetentionDays)
        {
            PlayerbotsDatabase.Execute("DELETE FROM adaptive_experience WHERE partida IN (SELECT id FROM adaptive_match WHERE fecha < NOW() - INTERVAL {} DAY)", cfg.matchRetentionDays);
            PlayerbotsDatabase.Execute("DELETE FROM adaptive_match_bot WHERE partida IN (SELECT id FROM adaptive_match WHERE fecha < NOW() - INTERVAL {} DAY)", cfg.matchRetentionDays);
            PlayerbotsDatabase.Execute("DELETE FROM adaptive_match WHERE fecha < NOW() - INTERVAL {} DAY", cfg.matchRetentionDays);
        }
        // Generaciones: se guardan las últimas Generaciones.Guardar; las demás
        // dejan de serlo y sus filas se borran con las versiones descartadas.
        std::vector<uint32> generations;
        for (ModelInfo const& m : ListModels())
            if (m.generation)
                generations.push_back(m.version);
        std::sort(generations.rbegin(), generations.rend());
        for (size_t i = cfg.generationsKeep; i < generations.size(); ++i)
            if (!v || generations[i] != v->Version())
                PlayerbotsDatabase.Execute("UPDATE adaptive_model SET generacion = 0 WHERE version = {}", generations[i]);
        if (c && v)
            PlayerbotsDatabase.Execute("DELETE FROM adaptive_q WHERE modelo NOT IN ({}, {}) AND modelo NOT IN (SELECT version FROM adaptive_model WHERE validada = 1 OR generacion = 1)", c->Version(), v->Version());
        LOG_INFO("module", "[adaptive-ai] Limpieza: partidas de mas de {} dias y tablas de versiones descartadas ({} generaciones guardadas).",
                 cfg.matchRetentionDays, std::min<size_t>(generations.size(), cfg.generationsKeep));
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Perfiles
    // ─────────────────────────────────────────────────────────────────────────
    static std::mutex s_profileLock;
    static std::unordered_map<uint32, BotProfile> s_profiles;

    // ─────────────────────────────────────────────────────────────────────────
    //  Exportar e importar lo aprendido. Formato: SQL de una sentencia por
    //  línea (REPLACE), con dos líneas de cabecera comentadas; la segunda,
    //  "-- adaptive-visitas: N", la lee el instalador para no pisar una base
    //  que tenga más entrenamiento que el fichero. Nunca partidas ni
    //  experiencias: solo modelos, tabla Q y perfiles.
    // ─────────────────────────────────────────────────────────────────────────
    static std::string SqlString(std::string text)
    {
        PlayerbotsDatabase.EscapeString(text);
        return "'" + text + "'";
    }

    bool ExportTraining(std::string const& path, std::string& summary)
    {
        SaveAllDirty();
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            summary = "no se puede escribir " + path;
            return false;
        }
        std::shared_ptr<QTable> c = Candidate();
        std::shared_ptr<QTable> v = Validated();
        std::set<uint32> versions;
        if (c) versions.insert(c->Version());
        if (v) versions.insert(v->Version());
        // Y todas las generaciones: son los peldanos de la escalera, y sin
        // ellas una instalacion limpia recibe la validada pero no con que
        // compararla (05/09/2026: hasta entonces solo iban dos versiones y
        // tools/exportar-adaptive.sh y este comando no exportaban lo mismo).
        for (uint32 g : Generations())
            versions.insert(g);

        // Todo desde la base (SaveAllDirty acaba de volcar lo de memoria): asi
        // entran tambien las generaciones, que no estan cargadas.
        uint64 visits = 0;
        uint32 rows = 0;
        std::ostringstream q;
        for (uint32 version : versions)
        {
            QueryResult r = PlayerbotsDatabase.Query("SELECT clase, clase_enemiga, estado, accion, q, visitas FROM adaptive_q WHERE modelo = {}", version);
            if (!r)
                continue;
            uint32 inStatement = 0;
            do
            {
                Field* f = r->Fetch();
                if (!inStatement)
                    q << "REPLACE INTO adaptive_q (modelo, clase, clase_enemiga, estado, accion, q, visitas) VALUES ";
                else
                    q << ",";
                q << "(" << version << "," << uint32(f[0].Get<uint8>()) << "," << uint32(f[1].Get<uint8>()) << "," << f[2].Get<uint16>() << ","
                  << uint32(f[3].Get<uint8>()) << "," << f[4].Get<float>() << "," << f[5].Get<uint32>() << ")";
                visits += f[5].Get<uint32>();
                ++rows;
                if (++inStatement >= 500)
                {
                    q << ";\n";
                    inStatement = 0;
                }
            } while (r->NextRow());
            if (inStatement)
                q << ";\n";
        }

        out << "-- mod-adaptive-ai: entrenamiento exportado " << Now()
            << "; validada v" << (v ? v->Version() : 0) << ", candidata v" << (c ? c->Version() : 0) << "\n";
        out << "-- adaptive-visitas: " << visits << "\n";

        uint32 models = 0;
        if (QueryResult result = PlayerbotsDatabase.Query("SELECT version, creado, validada, tasa_victoria, combates, nota, generacion FROM adaptive_model ORDER BY version"))
            do
            {
                Field* f = result->Fetch();
                if (!versions.count(f[0].Get<uint32>()))
                    continue;
                out << "REPLACE INTO adaptive_model (version, creado, validada, tasa_victoria, combates, nota, generacion) VALUES ("
                    << f[0].Get<uint32>() << "," << SqlString(f[1].Get<std::string>()) << "," << uint32(f[2].Get<uint8>()) << ","
                    << f[3].Get<float>() << "," << f[4].Get<uint32>() << "," << SqlString(f[5].Get<std::string>()) << "," << uint32(f[6].Get<uint8>()) << ");\n";
                ++models;
            } while (result->NextRow());
        out << q.str();

        // Las tablas pequenas, enteras y columna a columna; todo entre comillas
        // (MySQL convierte). Perfiles con el loadout y el desvio de la escalera,
        // aprobados por clase, la escalera medida y el rating del jugador: lo
        // mismo que tools/exportar-adaptive.sh.
        auto dump = [&](char const* table, std::vector<char const*> const& cols) -> uint32
        {
            std::string list;
            for (char const* col : cols)
                list += (list.empty() ? "" : ", ") + std::string("`") + col + "`";
            QueryResult r = PlayerbotsDatabase.Query("SELECT {} FROM {}", list, table);
            if (!r)
                return 0;
            uint32 n = 0;
            do
            {
                Field* f = r->Fetch();
                out << "REPLACE INTO " << table << " (" << list << ") VALUES (";
                for (size_t i = 0; i < cols.size(); ++i)
                    out << (i ? "," : "") << (f[i].IsNull() ? std::string("NULL") : SqlString(f[i].Get<std::string>()));
                out << ");\n";
                ++n;
            } while (r->NextRow());
            return n;
        };
        uint32 bots = dump("adaptive_bot", { "guid", "nombre", "clase", "spec", "rating", "victorias", "derrotas", "empates", "dificultad",
                                             "agresividad", "riesgo", "defensa", "prioridad", "movilidad", "xp", "modelo",
                                             "spec_pve", "spec_pvp", "proposito", "ilvl", "desvio", "desvio_fecha", "actualizado" });
        uint32 classes = dump("adaptive_clase", { "clase", "aprobada", "ronda", "version", "fecha", "cand_gana", "cand_pierde", "val_gana", "val_pierde", "ciclos", "historial" });
        uint32 ladder = dump("adaptive_escalera", { "clase", "version", "partidas", "victorias", "rating", "actualizado" });
        uint32 players = dump("adaptive_jugador", { "guid", "nombre", "rating", "victorias", "derrotas", "actualizado" });
        out.close();
        summary = Acore::StringFormat("{} modelos, {} filas Q ({} visitas), {} perfiles, {} clases, {} peldanos, {} jugadores -> {}",
                                      models, rows, visits, bots, classes, ladder, players, path);
        LOG_INFO("module", "[adaptive-ai] Exportado: {}.", summary);
        return true;
    }

    bool ImportTraining(std::string const& path, std::string& summary)
    {
        std::ifstream in(path);
        if (!in)
        {
            summary = "no se puede leer " + path;
            return false;
        }
        uint64 fileVisits = 0, dbVisits = 0;
        if (QueryResult result = PlayerbotsDatabase.Query("SELECT IFNULL(SUM(visitas), 0) FROM adaptive_q"))
            dbVisits = result->Fetch()[0].Get<uint64>();
        std::string line;
        uint32 statements = 0;
        while (std::getline(in, line))
        {
            if (line.empty())
                continue;
            if (line.rfind("-- adaptive-visitas:", 0) == 0)
            {
                fileVisits = std::strtoull(line.c_str() + 20, nullptr, 10);
                continue;
            }
            if (line.rfind("--", 0) == 0)
                continue;
            if (line.back() == ';')
                line.pop_back();
            PlayerbotsDatabase.DirectExecute(line.c_str());
            ++statements;
        }
        {
            std::lock_guard<std::mutex> guard(s_profileLock);
            s_profiles.clear();
        }
        LoadModels();
        {
            std::lock_guard<std::mutex> guard(s_generationLock);
            s_generationTables.clear();   // las generaciones importadas se cargan de nuevo al pedirlas
        }
        // Aprobados, ronda, escalera y contador de partidas: lo que trae el
        // fichero, no lo que habia en memoria (05/09/2026: sin esto, hasta el
        // siguiente reinicio el examen seguia con la ronda y las clases de antes).
        ArenaInit();
        summary = Acore::StringFormat("{} sentencias de {} (el fichero traia {} visitas; la base tenia {} antes). Modelos, aprobados y escalera recargados.",
                                      statements, path, fileVisits, dbVisits);
        LOG_INFO("module", "[adaptive-ai] Importado: {}.", summary);
        return true;
    }

    static BotProfile* LoadProfileFromDb(uint32 guidLow)
    {
        QueryResult result = PlayerbotsDatabase.Query(
            "SELECT nombre, clase, spec, rating, victorias, derrotas, empates, dificultad, agresividad, riesgo, defensa, prioridad, movilidad, xp, modelo, spec_pve, spec_pvp, proposito, ilvl, desvio, desvio_fecha "
            "FROM adaptive_bot WHERE guid = {}", guidLow);
        if (!result)
            return nullptr;
        Field* f = result->Fetch();
        BotProfile& p = s_profiles[guidLow];
        p.guid = guidLow;
        p.name = f[0].Get<std::string>();
        p.cls = f[1].Get<uint8>();
        p.spec = f[2].Get<uint8>();
        p.rating = f[3].Get<float>();
        p.wins = f[4].Get<uint32>();
        p.losses = f[5].Get<uint32>();
        p.draws = f[6].Get<uint32>();
        p.difficulty = f[7].Get<uint8>();
        p.aggression = f[8].Get<float>();
        p.risk = f[9].Get<float>();
        p.defense = f[10].Get<float>();
        p.priority = f[11].Get<float>();
        p.mobility = f[12].Get<float>();
        p.xp = f[13].Get<uint32>();
        p.model = f[14].Get<uint32>();
        p.specPve = f[15].Get<int8>();
        p.specPvp = f[16].Get<int8>();
        p.purpose = f[17].Get<std::string>();
        p.ilvl = f[18].Get<uint32>();
        p.rungOffset = f[19].Get<int8>();
        p.rungAt = f[20].Get<uint32>();
        return &p;
    }

    BotProfile* FindProfile(uint32 guidLow)
    {
        std::lock_guard<std::mutex> guard(s_profileLock);
        auto it = s_profiles.find(guidLow);
        if (it != s_profiles.end())
            return &it->second;
        return LoadProfileFromDb(guidLow);
    }

    BotProfile& GetProfile(Player* bot)
    {
        uint32 guidLow = bot->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> guard(s_profileLock);
            auto it = s_profiles.find(guidLow);
            if (it != s_profiles.end())
                return it->second;
            if (BotProfile* loaded = LoadProfileFromDb(guidLow))
                return *loaded;

            BotProfile& p = s_profiles[guidLow];
            p.guid = guidLow;
            p.name = bot->GetName();
            p.cls = bot->getClass();
#ifdef ADAPTIVE_WITH_PLAYERBOTS
            p.spec = AiFactory::GetPlayerSpecTab(bot);
#endif
            p.difficulty = cfg.defaultDifficulty;
            p.aggression = frand(0.3f, 0.7f);
            p.risk       = frand(0.3f, 0.7f);
            p.defense    = frand(0.3f, 0.7f);
            p.priority   = frand(0.3f, 0.7f);
            p.mobility   = frand(0.3f, 0.7f);
            p.dirty = true;
        }
        BotProfile* p = FindProfile(guidLow);
        SaveProfile(*p);
        return *p;
    }

    void SaveProfile(BotProfile& p)
    {
        std::string name = p.name;
        PlayerbotsDatabase.EscapeString(name);
        std::string purpose = p.purpose == "pvp" ? "pvp" : "pve";
        PlayerbotsDatabase.Execute(
            "REPLACE INTO adaptive_bot (guid, nombre, clase, spec, rating, victorias, derrotas, empates, dificultad, "
            "agresividad, riesgo, defensa, prioridad, movilidad, xp, modelo, spec_pve, spec_pvp, proposito, ilvl, desvio, desvio_fecha, actualizado) "
            "VALUES ({}, '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, '{}', {}, {}, {}, '{}')",
            p.guid, name, p.cls, p.spec, p.rating, p.wins, p.losses, p.draws, p.difficulty,
            p.aggression, p.risk, p.defense, p.priority, p.mobility, p.xp, p.model, int32(p.specPve), int32(p.specPvp), purpose, p.ilvl,
            int32(p.rungOffset), p.rungAt, Now());
        p.dirty = false;
    }

    // ─── Rating del jugador ──────────────────────────────────────────────────
    static std::mutex s_playerRatingLock;
    static std::unordered_map<uint32, float> s_playerRatings;

    float PlayerRating(uint32 guidLow)
    {
        {
            std::lock_guard<std::mutex> guard(s_playerRatingLock);
            auto it = s_playerRatings.find(guidLow);
            if (it != s_playerRatings.end())
                return it->second;
        }
        float rating = 1500.0f;
        if (QueryResult r = PlayerbotsDatabase.Query("SELECT rating FROM adaptive_jugador WHERE guid = {}", guidLow))
            rating = r->Fetch()[0].Get<float>();
        std::lock_guard<std::mutex> guard(s_playerRatingLock);
        s_playerRatings[guidLow] = rating;
        return rating;
    }

    // Una baja contra un bot (o a manos de uno) mueve el rating del jugador con
    // la misma formula que el Elo de los bots, tomando como fuerza del rival el
    // rating del peldano con el que jugaba el bot.
    void PlayerRatingUpdate(Player* human, float opponentRating, float score)
    {
        if (!human)
            return;
        uint32 guidLow = human->GetGUID().GetCounter();
        float rating = PlayerRating(guidLow);
        float const k = 32.0f;
        float expected = 1.0f / (1.0f + std::pow(10.0f, (opponentRating - rating) / 400.0f));
        rating += k * (score - expected);
        {
            std::lock_guard<std::mutex> guard(s_playerRatingLock);
            s_playerRatings[guidLow] = rating;
        }
        uint32 win = score > 0.5f ? 1 : 0;
        std::string name = human->GetName();
        PlayerbotsDatabase.EscapeString(name);
        PlayerbotsDatabase.Execute(
            "INSERT INTO adaptive_jugador (guid, nombre, rating, victorias, derrotas, actualizado) VALUES ({}, '{}', {}, {}, {}, '{}') "
            "ON DUPLICATE KEY UPDATE nombre = VALUES(nombre), rating = VALUES(rating), victorias = victorias + {}, derrotas = derrotas + {}, actualizado = VALUES(actualizado)",
            guidLow, name, rating, win, 1 - win, Now(), win, 1 - win);
    }

    void EloUpdate(BotProfile& a, BotProfile& b, float scoreA)
    {
        float const k = 32.0f;
        float expectedA = 1.0f / (1.0f + std::pow(10.0f, (b.rating - a.rating) / 400.0f));
        float delta = k * (scoreA - expectedA);
        a.rating += delta;
        b.rating -= delta;
        if (scoreA > 0.75f)      { ++a.wins; ++b.losses; }
        else if (scoreA < 0.25f) { ++a.losses; ++b.wins; }
        else                     { ++a.draws; ++b.draws; }
        a.dirty = b.dirty = true;
    }

    uint8 BracketOf(float rating)
    {
        uint8 bracket = 0;
        for (float threshold : cfg.brackets)
            if (rating >= threshold)
                ++bracket;
        return bracket;
    }

    char const* BracketName(uint8 bracket)
    {
        static char const* names[] = { "Novato", "Principiante", "Intermedio", "Avanzado", "Experto", "Elite" };
        return names[std::min<uint8>(bracket, 5)];
    }

    char const* DifficultyName(uint8 difficulty)
    {
        static char const* names[] = { "?", "Novato", "Facil", "Normal", "Dificil", "Experto", "Elite" };
        return names[std::clamp<uint8>(difficulty, 0, 6)];
    }

    char const* ModeName(BrainMode mode)
    {
        switch (mode)
        {
            case BRAIN_NONE:      return "ninguna";
            case BRAIN_CANDIDATE: return "candidata";
            case BRAIN_VALIDATED: return "validada";
            case BRAIN_VERSION:   return "generacion";
        }
        return "?";
    }

    char const* SourceName(MatchSource source)
    {
        switch (source)
        {
            case SOURCE_AUTO:        return "auto";
            case SOURCE_MANUAL:      return "manual";
            case SOURCE_CALIBRATION: return "calibracion";
        }
        return "?";
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Cerebros
    // ─────────────────────────────────────────────────────────────────────────
    static std::shared_mutex s_brainLock;
    static std::unordered_map<ObjectGuid, std::shared_ptr<Brain>> s_brains;
    static std::atomic<uint32> s_activeBrains{0};

    static std::mutex s_lastDecisionsLock;
    static std::unordered_map<uint32, std::vector<Decision>> s_lastDecisions;

    static bool SelectAuditSession(Brain const& b, bool eligible)
    {
        if (b.mode == BRAIN_NONE) return false;
        if (cfg.auditBot && cfg.auditBot == b.bot.GetCounter()) return true;
        if (!eligible || !b.matchId || b.mode != BRAIN_CANDIDATE || !b.learn) return false;
        static std::mutex lock;
        static uint32 previousMask = 0, previousQuota = 0;
        static std::map<uint8, uint32> used;
        std::lock_guard<std::mutex> guard(lock);
        if (previousMask != cfg.auditClassMask || previousQuota != cfg.auditSessions)
        {
            used.clear(); previousMask = cfg.auditClassMask; previousQuota = cfg.auditSessions;
        }
        if (!(cfg.auditClassMask & (uint32(1) << b.myClass))) return false;
        uint32& count = used[b.myClass];
        if (count >= cfg.auditSessions) return false;
        ++count;
        LOG_INFO("module", "[adaptive-ai] Auditoria automatica: clase {}, bot {}, partida {}, sesion {}/{}.",
                 b.myClass, b.bot.GetCounter(), b.matchId, count, cfg.auditSessions);
        return true;
    }

    // Solo Server.log, nunca chat ni BD. El número de secuencia permite detectar
    // pérdidas; los valores son contabilidad, no una atribución causal del hechizo.
    static void AuditBrain(Brain& b, char const* event, float value = 0.0f, uint32 site = 0)
    {
        if (!b.audit || b.auditEvents > 4000)
            return;
        if (b.auditEvents == 4000)
            event = "truncated";
        LOG_INFO("module", "[adaptive-audit] bot={} match={} start={} seq={} event={} decision={} action={} pending={:.9f} total={:.9f} value={:.9f} learn={} model={} site={} class={}",
                 b.bot.GetCounter(), b.matchId, b.startMs, ++b.auditEvents, event,
                 b.decisions, b.lastAction, b.pendingReward, b.totalReward, value,
                 b.learn ? 1 : 0, b.learnTable ? b.learnTable->Version() : 0, site, b.myClass);
    }

    uint32 ActiveBrains() { return s_activeBrains.load(std::memory_order_relaxed); }

    std::shared_ptr<Brain> FindBrain(ObjectGuid guid)
    {
        if (!ActiveBrains())
            return nullptr;
        std::shared_lock<std::shared_mutex> guard(s_brainLock);
        auto it = s_brains.find(guid);
        return it == s_brains.end() ? nullptr : it->second;
    }

    std::vector<Decision> LastDecisions(uint32 guidLow)
    {
        std::lock_guard<std::mutex> guard(s_lastDecisionsLock);
        auto it = s_lastDecisions.find(guidLow);
        return it == s_lastDecisions.end() ? std::vector<Decision>() : it->second;
    }

    std::shared_ptr<Brain> StartBrain(Player* bot, Unit* enemy, BrainMode mode, bool learn, float epsilon, uint32 matchId,
                                      std::shared_ptr<QTable> tableOverride, bool auditEligible)
    {
        auto brain = std::make_shared<Brain>();
        brain->bot = bot->GetGUID();
        brain->enemy = enemy->GetGUID();
        brain->myClass = bot->getClass();
        brain->enemyClass = enemy->getClass();
        brain->role = CombatRole(bot);
        brain->mode = mode;
        brain->learn = learn && mode != BRAIN_NONE;
        brain->epsilon = epsilon;
        brain->profile = &GetProfile(bot);
        brain->difficulty = matchId ? Difficulty{} : cfg.difficulty[std::clamp<uint8>(brain->profile->difficulty, 1, 6)];
        brain->table = mode == BRAIN_CANDIDATE ? Candidate() : (mode == BRAIN_VALIDATED ? Validated() : (mode == BRAIN_VERSION ? tableOverride : nullptr));
        if (!brain->table)
            brain->mode = BRAIN_NONE;
        if (mode == BRAIN_VERSION)
            brain->learn = false;   // una generación anterior no cambia
        // Se aprende siempre en la candidata, se actúe con la que se actúe
        // (Q-learning es fuera de política): así los bots que se cruzan con el
        // jugador usan la validada y lo que aprenden va a la candidata.
        brain->learnTable = brain->learn ? Candidate() : nullptr;
        if (!brain->learnTable)
            brain->learn = false;
        brain->matchId = matchId;
        brain->startMs = getMSTime();
        brain->lastDecisionMs = brain->startMs;
        brain->audit = SelectAuditSession(*brain, auditEligible);
        AuditBrain(*brain, "start");

        // El brujo entra con la mascota de PvP y sin los disparadores que la
        // reinvocaban sin parar (ver OwnWarlockPet).
        if (matchId)
            OwnWarlockPet(bot, true);

        std::unique_lock<std::shared_mutex> guard(s_brainLock);
        s_brains[bot->GetGUID()] = brain;
        s_activeBrains.store(uint32(s_brains.size()), std::memory_order_relaxed);
        return brain;
    }

    // Paso de aprendizaje: media muestral al principio (1, 1/2, 1/3...) y un
    // suelo (AdaptiveAI.Alpha) para seguir adaptándose después.
    static float StepSize(QTable& table, uint8 cls, uint8 enemyCls, uint16 state, uint8 action)
    {
        if (!cfg.alphaDecay)
            return cfg.alpha;
        uint32 visits = table.Entry(cls, enemyCls, state, action).visits;
        return std::max(cfg.alpha, 1.0f / float(visits + 1));
    }

    void SetBrainTeam(std::shared_ptr<Brain> const& brain, int8 side, std::vector<ObjectGuid> const& allies, std::vector<ObjectGuid> const& enemies)
    {
        if (!brain)
            return;
        std::lock_guard<std::recursive_mutex> guard(brain->lock);
        brain->side = side;
        brain->allies = allies;
        brain->enemies = enemies;
        brain->teamMatch = !allies.empty() || enemies.size() > 1;
    }

    // Toda actualización pasa por aquí: la fila contra este rival y, si se
    // generaliza, la fila "contra cualquiera" (clase enemiga 0) con su propio
    // paso. Esa fila es el punto de partida de lo que aún no se ha probado
    // contra un rival concreto: con 45 parejas, cada clase aprende de sus
    // nueve rivales a la vez en vez de empezar de cero con cada uno.
    static void Learn(Brain& b, uint16 state, uint8 action, float target, float alphaScale, bool countVisit)
    {
        float alpha = StepSize(*b.learnTable, b.myClass, b.enemyClass, state, action) * alphaScale;
        b.learnTable->Update(b.myClass, b.enemyClass, state, action, target, alpha, countVisit);
        if (cfg.generalize && b.enemyClass)
        {
            float alphaGeneral = StepSize(*b.learnTable, b.myClass, 0, state, action) * alphaScale;
            b.learnTable->Update(b.myClass, 0, state, action, target, alphaGeneral, countVisit);
        }
    }

    static void LearnStep(Brain& b, float target, bool terminal)
    {
        if (!b.learn || b.lastAction < 0 || !b.learnTable)
            return;
        Learn(b, b.lastState, uint8(b.lastAction), target, 1.0f, true);
        AuditBrain(b, terminal ? "learn_terminal" : "learn_step", target);
        if (b.profile)
        {
            ++b.profile->xp;
            b.profile->dirty = true;
        }
    }

    // Al acabar: el retorno descontado de la partida entera, repartido a cada
    // decisión (Monte Carlo). El paso a paso ya se hizo; esto lleva la
    // victoria o la derrota hasta las primeras decisiones sin esperar a que
    // el bootstrap las alcance visita a visita.
    static void LearnReturns(Brain& b)
    {
        if (!b.learn || !b.learnTable || cfg.returnWeight <= 0.0f || b.history.size() < 2)
            return;
        float G = 0.0f;
        for (auto it = b.history.rbegin(); it != b.history.rend(); ++it)
        {
            G = it->reward + cfg.gamma * G;
            Learn(b, it->state, it->action, G, cfg.returnWeight, false);
        }
    }

    static void LogExperience(Brain& b, Decision const& d)
    {
        AuditBrain(b, "close", d.reward);
        if (!cfg.logDecisions)
            return;
        PlayerbotsDatabase.Execute(
            "INSERT INTO adaptive_experience (partida, bot, t, estado, accion, q, recompensa) VALUES ({}, {}, {}, {}, {}, {}, {})",
            b.matchId, b.bot.GetCounter(), d.timeMs - b.startMs, d.state, d.action, d.q, d.reward);
    }

    void EndBrain(ObjectGuid guid, float terminalReward)
    {
        std::shared_ptr<Brain> brain;
        {
            std::unique_lock<std::shared_mutex> guard(s_brainLock);
            auto it = s_brains.find(guid);
            if (it == s_brains.end())
                return;
            brain = it->second;
            s_brains.erase(it);
            s_activeBrains.store(uint32(s_brains.size()), std::memory_order_relaxed);
        }

        std::lock_guard<std::recursive_mutex> guard(brain->lock);
        brain->ended = true;   // si estamos dentro de Decide(), que no reabra la decision al volver
        brain->pendingReward += terminalReward;
        brain->totalReward += terminalReward;
        AuditBrain(*brain, "reward", terminalReward, __LINE__);
        if (brain->lastAction >= 0)
        {
            if (!brain->history.empty())
            {
                brain->history.back().reward += brain->pendingReward;
                LogExperience(*brain, brain->history.back());
            }
            LearnStep(*brain, brain->pendingReward, true);
            brain->lastAction = -1;
        }
        brain->pendingReward = 0.0f;
        brain->kiteUntilMs = 0;
        LearnReturns(*brain);
        AuditBrain(*brain, "end");

        if (brain->profile)
        {
            brain->profile->model = brain->learnTable ? brain->learnTable->Version() : (brain->table ? brain->table->Version() : 0);
            if (brain->profile->dirty)
                SaveProfile(*brain->profile);
        }

        std::lock_guard<std::mutex> lguard(s_lastDecisionsLock);
        s_lastDecisions[guid.GetCounter()] = std::vector<Decision>(brain->history.begin(), brain->history.end());
    }

    // Suma una recompensa a la decisión en curso del bot (desde cualquier hilo).
    // SIN tope: sólo para los terminales (Kill, Muerte, MuerteAliado, KillEquipo)
    // y para los objetivos de campo de batalla. Todo lo demás va por
    // RewardShaped, que respeta AdaptiveAI.Recompensa.TopeSucesos.
    void Reward(ObjectGuid guid, float amount)
    {
        std::shared_ptr<Brain> brain = FindBrain(guid);
        if (!brain)
            return;
        std::lock_guard<std::recursive_mutex> guard(brain->lock);
        brain->pendingReward += amount;
        brain->totalReward += amount;
        AuditBrain(*brain, "reward", amount, __LINE__);
    }

    // Recorta lo que se puede cobrar todavía de premio de sucesos en este
    // combate. Se topa el ACUMULADO, no cada cobro, así que un bot que va
    // sumando puede seguir restando (y al revés): lo que no puede es que el
    // "cómo" acabe pesando más que el resultado. Llamar con brain->lock cogido.
    static float ShapedAmount(Brain& b, float amount)
    {
        if (cfg.rShapingCap <= 0.0f)
            return amount;
        amount = std::clamp(amount, -cfg.rShapingCap - b.shaping, cfg.rShapingCap - b.shaping);
        b.shaping += amount;
        return amount;
    }

    // Como Reward(), pero contra el tope de sucesos.
    void RewardShaped(ObjectGuid guid, float amount)
    {
        std::shared_ptr<Brain> brain = FindBrain(guid);
        if (!brain)
            return;
        std::lock_guard<std::recursive_mutex> guard(brain->lock);
        float const given = ShapedAmount(*brain, amount);
        brain->pendingReward += given;
        brain->totalReward += given;
        AuditBrain(*brain, "reward", given, __LINE__);
    }

#ifdef ADAPTIVE_WITH_PLAYERBOTS
    // ─── Elección y ejecución ───────────────────────────────────────────────
    struct Scored
    {
        uint8 index;
        float q;
        float score;
        bool  prior;   // el valor viene de la fila "contra cualquiera" (en explicar: "~=")
    };

    static float PersonalityBias(BotProfile const* p, ActionKind kind)
    {
        if (!p || cfg.personalityWeight <= 0.0f)
            return 0.0f;
        float w = cfg.personalityWeight;
        switch (kind)
        {
            case KIND_NONE:      return -(p->risk - 0.5f) * w;
            case KIND_OFFENSIVE:
            case KIND_INTERRUPT:
            case KIND_CONTROL:   return (p->aggression - 0.5f) * w;
            case KIND_DEFENSIVE:
            case KIND_RESOURCE:  return (p->defense - 0.5f) * w;
            case KIND_MOBILITY:  return (p->mobility - 0.5f) * w;
            case KIND_TEAM:      return (p->priority - 0.5f) * w;
        }
        return 0.0f;
    }

    // ─── Acciones de equipo: elegir objetivo con los compañeros ────────────
    static bool IsEnemyOf(Brain const& b, ObjectGuid guid)
    {
        return std::find(b.enemies.begin(), b.enemies.end(), guid) != b.enemies.end();
    }

    static Player* NearestEnemy(Brain const& b, Player* bot, float maxDist)
    {
        Player* best = nullptr;
        float bestDist = maxDist;
        for (ObjectGuid guid : b.enemies)
            if (Player* e = AlivePlayer(guid, bot))
            {
                float d = bot->GetDistance(e);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = e;
                }
            }
        return best;
    }

    static Player* TeamTarget(Brain const& b, Player* bot, ActionDef const& def)
    {
        if (!b.teamMatch)
            return nullptr;
        std::string name = def.name;
        if (name == "foco sanador")
        {
            Player* best = nullptr;
            float bestDist = 100.0f;
            for (ObjectGuid guid : b.enemies)
                if (Player* e = AlivePlayer(guid, bot))
                    if (IsHealerSpec(e->getClass(), SpecOf(e)) && bot->GetDistance(e) < bestDist)
                    {
                        bestDist = bot->GetDistance(e);
                        best = e;
                    }
            return best;
        }
        if (name == "asistir")
        {
            std::map<ObjectGuid, uint32> votes;
            for (ObjectGuid guid : b.allies)
                if (Player* a = AlivePlayer(guid, bot))
                    if (Player* v = PlayerOf(a->GetVictim()))
                        if (IsEnemyOf(b, v->GetGUID()) && v->IsAlive())
                            ++votes[v->GetGUID()];
            Player* best = nullptr;
            uint32 bestVotes = 0;
            for (auto const& [guid, n] : votes)
                if (n > bestVotes)
                    if (Player* e = AlivePlayer(guid, bot))
                    {
                        bestVotes = n;
                        best = e;
                    }
            return best;
        }
        if (name == "proteger")
        {
            Player* lowest = nullptr;
            for (ObjectGuid guid : b.allies)
                if (Player* a = AlivePlayer(guid, bot))
                    if (a->GetHealthPct() < 70.0f && (!lowest || a->GetHealthPct() < lowest->GetHealthPct()))
                        lowest = a;
            if (!lowest)
                return nullptr;
            Player* best = nullptr;
            float bestDist = 100.0f;
            for (ObjectGuid guid : b.enemies)
                if (Player* e = AlivePlayer(guid, bot))
                    if (e->GetVictim() == lowest && bot->GetDistance(e) < bestDist)
                    {
                        bestDist = bot->GetDistance(e);
                        best = e;
                    }
            return best;
        }
        return nullptr;
    }

    static void SwitchTarget(Brain& b, PlayerbotAI* botAI, Player* bot, Player* target)
    {
        b.enemy = target->GetGUID();
        b.enemyClass = target->getClass();
        bot->SetSelection(target->GetGUID());
        if (Value<Unit*>* current = botAI->GetAiObjectContext()->GetValue<Unit*>("current target"))
            current->Set(target);
        bot->Attack(target, !IsCasterClass(bot->getClass()));
    }

    static bool IsKite(ActionDef const& def) { return def.kind == KIND_MOBILITY && !def.isSpell && std::string(def.name) == "kitear"; }

    // Abalorio con uso y sin cooldown en alguno de los dos huecos. Sin esto,
    // "use trinket" siempre parecía disponible y el optimismo lo proponía una
    // y otra vez (8.000 intentos fallidos del guerrero en una hora).
    static bool TrinketAvailable(Player* bot)
    {
        for (uint8 slot : { uint8(EQUIPMENT_SLOT_TRINKET1), uint8(EQUIPMENT_SLOT_TRINKET2) })
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            for (auto const& spell : item->GetTemplate()->Spells)
                if (spell.SpellId && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE && !bot->HasSpellCooldown(spell.SpellId))
                    return true;
        }
        return false;
    }

    // Contadores del 05/09/2026, para medir las dos guardas nuevas (.adaptive estado)
    static std::atomic<uint32> s_castWaits{ 0 };      // decisiones pospuestas porque el propio bot casteaba
    static std::atomic<uint32> s_formFiltered{ 0 };   // hechizos no ofrecidos porque la forma actual no los permite
    static std::atomic<uint32> s_feralFiltered{ 0 };  // hechizos de lanzador quitados del catalogo de un feral (por combate)

    // El propio bot esta lanzando un hechizo con tiempo de casteo. Es la misma
    // comprobacion que hace UpdateAIInternal de playerbots antes de dejar correr
    // su motor; DoSpecificAction se la salta, y por eso la hace este modulo.
    static bool OwnCastInProgress(Player* bot)
    {
        Spell const* s = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        return s && s->getState() == SPELL_STATE_PREPARING && s->GetCastTime() > 0;
    }

    // La forma actual permite este hechizo. CanCastSpell de playerbots devuelve
    // true con SPELL_FAILED_NOT_SHAPESHIFT (sus propias acciones de druida
    // cambian de forma antes), asi que al felino se le ofrecian Colera o
    // Recrecimiento y el core los rechazaba al lanzar: decision perdida y premio
    // de accion fallida. Un CheckShapeshift sobre el SpellInfo ya resuelto.
    static bool FormAllows(Brain const& b, Player* bot, ActionDef const& def)
    {
        if (!cfg.respectForm || !def.isSpell || b.spells.empty())
            return true;
        std::vector<ActionDef> const& actions = ActionsFor(b.myClass);
        size_t i = size_t(&def - actions.data());
        if (i >= b.spells.size() || !b.spells[i])
            return true;
        if (b.spells[i]->CheckShapeshift(bot->GetShapeshiftForm()) == SPELL_CAST_OK)
            return true;
        ++s_formFiltered;
        return false;
    }

    static bool ActionAvailable(Brain const& b, PlayerbotAI* botAI, Player* bot, Unit* enemy, ActionDef const& def, float distance, StateInfo const& state)
    {
        if (def.kind == KIND_NONE)
            return true;
        if (def.kind == KIND_TEAM)
        {
            Player* t = TeamTarget(b, bot, def);
            return t && t->GetGUID() != b.enemy;
        }
        // Interrumpir sin nada que interrumpir no es una opción: el estado ya
        // sabe si el enemigo castea (anoche: 750 contrahechizos al aire por 6 buenos).
        if (cfg.interruptOnlyCasting && def.kind == KIND_INTERRUPT && !state.enemyCasting)
            return false;
        if (!def.isSpell)
        {
            if (std::string(def.name) == "reach melee")
                return distance > 5.5f;
            if (IsKite(def))
            {
                // Huir sólo si se puede huir. Un bot ralentizado que se aleja de
                // un cuerpo a cuerpo no gana distancia: pierde el hechizo que
                // estaba lanzando y come golpes gratis. Y sin nada con que
                // clavar al enemigo ni movilidad propia, tampoco hay huida.
                // Medido el 04/09/2026: el brujo elegía kitear con la ralentización
                // encima y a distancia de melé, y kitear le salía a -0,74 de valor
                // medio (18.308 visitas) contra +1,88 del chamán, que sí clava.
                if (state.selfSnared || state.selfControlled)
                    return false;
                if (!state.enemySnared && !state.keyReady && !state.mobilityReady)
                    return false;
                return b.role > 0 && distance < cfg.kiteDistance - 4.0f;
            }
            if (std::string(def.name) == "use trinket")
                return TrinketAvailable(bot);
            return true;
        }
        if (!FormAllows(b, bot, def))
            return false;
        return CanCast(botAI, def, bot, enemy);
    }

    // Rechazada dos veces en este estado durante la sesión: no se vuelve a ofrecer
    static bool FailedHere(Brain const& b, uint16 state, uint8 action)
    {
        auto it = b.failedHere.find((uint32(state) << 8) | action);
        return it != b.failedHere.end() && it->second >= 2;
    }

    // ─── Kitear (acción propia) ─────────────────────────────────────────────
    // Un paso: un punto a AdaptiveAI.Kitear.Distancia del enemigo, en la
    // dirección enemigo -> bot, hasta la primera colisión. No se mueve si está
    // casteando (que acabe) ni controlado. La ventana (kiteUntilMs) hace que
    // BrainTick repita el paso hasta llegar a la distancia o agotar el tiempo;
    // playerbots sigue con su rotación entre paso y paso.
    static bool KiteStep(Player* bot, Unit* enemy)
    {
        if (bot->HasUnitState(UNIT_STATE_CASTING) || IsControlled(bot))
            return false;
        float distance = bot->GetDistance(enemy);
        float step = std::clamp(cfg.kiteDistance - distance + 3.0f, 5.0f, cfg.kiteDistance);
        float away = enemy->GetAngle(bot);   // ángulo absoluto enemigo -> bot
        Position dest = bot->GetPosition();
        // MovePositionToFirstCollision suma la orientación del bot al ángulo
        bot->MovePositionToFirstCollision(dest, step, Position::NormalizeOrientation(away - bot->GetOrientation()));
        if (bot->GetExactDist2d(&dest) < 2.0f)
            return false;
        bot->GetMotionMaster()->MovePoint(0, dest, FORCED_MOVEMENT_NONE, 0.0f, true, true);
        return true;
    }

    static bool StartKite(Brain& b, PlayerbotAI* botAI, Player* bot, Unit* enemy, uint32 now)
    {
        // Clavar primero, si el enemigo está encima y la herramienta clave de la
        // clase está lista. Esto lo hacía sólo el mago, con Nova de Escarcha
        // escrita a pelo; el catálogo marca la de cada clase con key = true
        // (guerrero Tendón, chamán Choque de Escarcha, brujo Furia de las
        // Sombras o Maldición de Agotamiento, CdM Cadenas de Hielo...), así que
        // ahora vale para todas. Sin clavar, kitear es regalar la espalda.
        if (bot->GetDistance(enemy) <= 10.0f)
            for (ActionDef const& def : ActionsFor(bot->getClass()))
            {
                if (!def.key || !def.isSpell)
                    continue;
                if (!CanCast(botAI, def, bot, enemy))
                    continue;
                botAI->DoSpecificAction(def.name, Event(), true);
                break;
            }
        b.kiteUntilMs = now + cfg.kiteMs;
        b.kiteLastMoveMs = now;
        KiteStep(bot, enemy);   // si ahora mismo no puede (casteando), la ventana lo reintenta
        return true;
    }

    static bool ExecuteAction(Brain& b, PlayerbotAI* botAI, Player* bot, Unit* enemy, ActionDef const& def, uint32 now)
    {
        if (def.kind == KIND_NONE)
            return true;
        if (def.kind == KIND_TEAM)
        {
            Player* t = TeamTarget(b, bot, def);
            if (!t)
                return false;
            SwitchTarget(b, botAI, bot, t);
            return true;
        }
        if (IsKite(def))
            return StartKite(b, botAI, bot, enemy, now);
        if (!def.selfTarget)
        {
            if (bot->GetSelectedUnit() != enemy)
                bot->SetSelection(enemy->GetGUID());
            if (Value<Unit*>* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target"))
                if (target->Get() != enemy)
                    target->Set(enemy);
        }
        return botAI->DoSpecificAction(def.name, Event(), true);
    }

    // Recompensa de posición por decisión: los lanzadores quieren distancia,
    // los cuerpo a cuerpo quieren estar pegados. Pequeña: orienta, no decide.
    // Rol de combate de verdad, por spec y forma, no por clase: un druida
    // felino o un chamán de mejora pegan cuerpo a cuerpo aunque su clase
    // "lance". 1 = a distancia, -1 = cuerpo a cuerpo, 0 = neutro (sanadores).
    // Decide si "kitear" está disponible y el signo de la recompensa de posición
    // (03/09/2026: el druida adaptativo, felino con kiteo, iba al 16 % contra
    // el 52 % del de serie).
    int8 CombatRole(Player* bot)
    {
        uint8 cls = bot->getClass();
        uint8 spec = SpecOf(bot);
        switch (cls)
        {
            case CLASS_MAGE: case CLASS_WARLOCK: case CLASS_HUNTER:
                return 1;
            case CLASS_PRIEST:
                return spec == 2 ? 1 : 0;              // sombra a distancia; disciplina y sagrado, sanadores
            case CLASS_WARRIOR: case CLASS_ROGUE: case CLASS_DEATH_KNIGHT:
                return -1;
            case CLASS_PALADIN:
                return spec == 0 ? 0 : -1;             // sagrado sana; protección y reprensión pegan
            case CLASS_SHAMAN:
                return spec == 1 ? -1 : (spec == 2 ? 0 : 1);   // mejora pega, restauración sana, elemental lanza
            case CLASS_DRUID:
            {
                ShapeshiftForm form = bot->GetShapeshiftForm();
                if (form == FORM_CAT || form == FORM_BEAR || form == FORM_DIREBEAR)
                    return -1;
                return spec == 1 ? -1 : (spec == 2 ? 0 : 1);   // feral pega, restauración sana, equilibrio lanza
            }
            default:
                return 0;
        }
    }

    // ─── Pieza A: lo que este bot puede hacer de verdad ──────────────────
    //
    // El catálogo de acciones es por clase y es el mismo para un guerrero de 20
    // que para uno de 80. Había dos motivos muy distintos por los que una acción
    // no sale, y hasta ahora eran el mismo:
    //
    //   "ahora no sirve"  la tiene, pero está en enfriamiento, sin objetivo
    //                     válido o sin maná. Señal legítima: se ofrece y, si
    //                     playerbots la rechaza, puntúa.
    //   "no la tiene"     no conoce el hechizo (no llega al nivel, no tiene el
    //                     talento) o playerbots no tiene esa acción para este
    //                     bot. NO es señal: ni se ofrece ni puntúa, porque el
    //                     castigo caía en una casilla que comparten todos los
    //                     niveles y todos los bots de la clase.
    //
    // Las dos preguntas se las sabe playerbots y no hay que declarar nada a
    // mano: el valor "spell id" recorre el mapa de hechizos del bot y devuelve
    // 0 si no lo conoce, y GetAction(nombre) devuelve nulo si esa acción no
    // existe en su contexto. La segunda es además una autocomprobación del
    // catálogo: así se cazó "blink", que en playerbots es "blink back".
    //
    // Se calcula una vez por combate y en el hilo del mapa (el contexto de
    // acciones del bot no es de otro hilo). Dentro de una arena el nivel y la
    // spec no cambian, así que vale para todo el combate.
    static std::map<std::pair<uint8, std::string>, uint32> s_missingAction;   // (clase, acción) que no existe en playerbots
    static std::mutex s_missingLock;
    static std::atomic<uint32> s_usableFiltered{ 0 };   // acciones descartadas por "no la tiene"

    static void ComputeUsable(Brain& b, PlayerbotAI* botAI)
    {
        std::vector<ActionDef> const& actions = ActionsFor(b.myClass);
        Player* bot = botAI->GetBot();
        // El feral (05/09/2026): un druida de spec feral no lleva en su catalogo
        // el dano de forma de lanzador. Se le ofrecia cuando playerbots salia de
        // la forma a curarse, y el modulo llenaba ese hueco con Colera y Fuego
        // estelar en vez de volver al gato: 60.000 de sus 360.000 visitas.
        // Las curas y los controles se quedan (un feral sale de forma a curarse
        // o a enraizar); solo se quita lo OFENSIVO que no vale ni en gato ni en
        // oso.
        bool const feral = cfg.respectForm && b.myClass == CLASS_DRUID && bot && SpecOf(bot) == 1;
        uint64 mask = 0;
        b.spells.assign(actions.size(), nullptr);
        for (size_t i = 0; i < actions.size() && i < 64; ++i)
        {
            ActionDef const& def = actions[i];
            bool usable = true;
            // Excluida a mano (AdaptiveAI.Acciones.Excluir)
            if (cfg.excludedActions.count({ b.myClass, std::string(def.name) }))
                usable = false;
            // "none", las de equipo y "kitear" las ejecuta este módulo
            else if (def.kind != KIND_NONE && def.kind != KIND_TEAM && !IsKite(def))
            {
                if (def.isSpell)
                {
                    uint32 spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", std::string(def.name))->Get();
                    if (!spellId)
                        usable = false;   // no conoce el hechizo
                    else
                        b.spells[i] = sSpellMgr->GetSpellInfo(spellId);
                }
                if (usable && !botAI->GetAiObjectContext()->GetAction(def.name))
                {
                    usable = false;   // playerbots no tiene esa acción para este bot
                    std::lock_guard<std::mutex> guard(s_missingLock);
                    if (++s_missingAction[{ b.myClass, std::string(def.name) }] == 1)
                        LOG_WARN("module", "[adaptive-ai] La accion '{}' del catalogo de {} no existe en playerbots: no se ofrecera.",
                                 def.name, ClassName(b.myClass));
                }
                if (usable && feral && def.kind == KIND_OFFENSIVE && b.spells[i] &&
                    b.spells[i]->CheckShapeshift(FORM_CAT) != SPELL_CAST_OK &&
                    b.spells[i]->CheckShapeshift(FORM_BEAR) != SPELL_CAST_OK &&
                    b.spells[i]->CheckShapeshift(FORM_DIREBEAR) != SPELL_CAST_OK)
                {
                    usable = false;   // dano de lanzador en un feral
                    ++s_feralFiltered;
                }
            }
            if (usable)
                mask |= (uint64(1) << i);
            else
                ++s_usableFiltered;
        }
        b.usable = mask;
        b.usableReady = true;
    }

    std::string UsableStatus()
    {
        std::string missing;
        {
            std::lock_guard<std::mutex> guard(s_missingLock);
            for (auto const& [key, n] : s_missingAction)
                missing += Acore::StringFormat("{}{} de {} ({} veces)", missing.empty() ? "" : ", ", key.second, ClassName(key.first), n);
        }
        std::string excluidas;
        for (auto const& [cls, action] : cfg.excludedActions)
            excluidas += Acore::StringFormat("{}{} de {}", excluidas.empty() ? "" : ", ", action, ClassName(cls));
        return Acore::StringFormat("acciones por bot: {} descartadas por no tenerlas desde el arranque; {}{}; "
                                   "decisiones pospuestas por casteo propio: {}; hechizos no ofrecidos por la forma: {}; "
                                   "dano de lanzador quitado a ferales (por combate): {}",
                                   s_usableFiltered.load(),
                                   missing.empty() ? "todo el catalogo existe en playerbots" : "NO EXISTEN en playerbots: " + missing,
                                   excluidas.empty() ? "" : "; excluidas a mano: " + excluidas,
                                   s_castWaits.load(), s_formFiltered.load(), s_feralFiltered.load());
    }

    static float DistanceReward(int8 role, StateInfo const& state)
    {
        if (cfg.rDistance == 0.0f || role == 0)
            return 0.0f;
        if (role > 0) return state.dist >= 1 ? cfg.rDistance : -cfg.rDistance;
        return state.dist == 0 ? cfg.rDistance : -cfg.rDistance;
    }

    // onlyInterrupt: el propio bot castea y el rival tambien; solo se elige entre
    // dejar hacer e interrumpir (cortar el propio hechizo para cortar el suyo).
    static void Decide(Brain& b, Player* bot, Unit* enemy, StateInfo const& state, uint32 now, bool onlyInterrupt = false)
    {
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;

        std::vector<ActionDef> const& actions = ActionsFor(b.myClass);
        if (!b.usableReady)
            ComputeUsable(b, botAI);
        uint16 key = state.Key();

        // Dificultad: no reaccionar en este punto de decisión. Se anota como
        // decisión propia (none): sin anotarla, lo que se cobrara hasta la
        // siguiente iba a parar a la decision ANTERIOR del historial.
        if (b.difficulty.miss > 0.0f && frand(0.0f, 1.0f) < b.difficulty.miss)
        {
            Decision miss;
            miss.timeMs = now;
            miss.state = key;
            miss.action = 0;
            miss.explain = state.Describe() + " | none (no reacciona: dificultad)";
            b.history.push_back(miss);
            if (b.history.size() > HISTORY_MAX)
                b.history.pop_front();
            ++b.decisions;
            b.lastState = key;
            b.lastAction = 0;
            b.lastDecisionMs = now;
            return;
        }

        float distance = bot->GetDistance(enemy);
        // Las filas del estado, con un solo bloqueo de la tabla en vez de uno
        // por accion (200 bots decidiendo contra el mismo mutex, 05/09/2026)
        std::vector<QEntry> const row = b.table->Row(b.myClass, b.enemyClass, key, uint8(actions.size()));
        std::vector<QEntry> const rowGeneral = (cfg.generalize && b.enemyClass)
            ? b.table->Row(b.myClass, 0, key, uint8(actions.size())) : std::vector<QEntry>();
        std::vector<Scored> candidates;
        std::vector<uint32> visits;
        candidates.reserve(actions.size());
        float bestVisitedQ = 0.0f;
        bool anyVisited = false;
        for (uint8 i = 0; i < actions.size(); ++i)
        {
            // "No la tiene": ni se ofrece ni puntúa (pieza A)
            if (!(b.usable & (uint64(1) << i)))
                continue;
            // Casteo propio en marcha: solo dejar hacer o interrumpir al rival
            if (onlyInterrupt && actions[i].kind != KIND_NONE && actions[i].kind != KIND_INTERRUPT)
                continue;
            if (FailedHere(b, key, i) || !ActionAvailable(b, botAI, bot, enemy, actions[i], distance, state))
                continue;
            QEntry e = row[i];
            bool prior = false;
            // Sin visitas contra este rival: lo aprendido contra cualquiera es el
            // punto de partida (y cuenta como visitado para el optimismo).
            if (!e.visits && !rowGeneral.empty())
            {
                QEntry const& g = rowGeneral[i];
                if (g.visits)
                {
                    e = g;
                    prior = true;
                }
            }
            // Sin aprender (validada, generacion, mundo) solo cuenta lo que se
            // ha probado de verdad: una accion con menos de Decision.VisitasMinimas
            // no es candidata. Hasta el 05/09/2026 la "mejor" accion de un
            // estado tenia UNA visita en el 52-58 % de los estados (N4), y con la
            // tabla sin filas todas valian 0 y decidia la personalidad (N1).
            // "none" siempre es candidata: es playerbots de serie.
            if (!b.learn && actions[i].kind != KIND_NONE && e.visits < cfg.greedyMinVisits)
                continue;
            // La personalidad solo desempata entre acciones con datos: sobre
            // valores a cero convertia la tabla vacia en una politica fija
            // ("siempre huir", "siempre la primera defensiva"), que gana el 42 %
            // donde la serie gana el 50 % (medido el 05/09/2026, N1).
            float const bias = e.visits ? PersonalityBias(b.profile, actions[i].kind) : 0.0f;
            candidates.push_back({ i, e.q, e.q + bias, prior });
            visits.push_back(e.visits);
            if (e.visits && (!anyVisited || e.q > bestVisitedQ))
            {
                bestVisitedQ = e.q;
                anyVisited = true;
            }
        }
        if (candidates.empty())
        {
            candidates.push_back({ 0, 0.0f, 0.0f, false });
            visits.push_back(0);
        }

        // Exploración dirigida mientras se aprende: lo no probado vale el mejor
        // Q del estado más el bonus (se prueba todo al menos una vez), lo poco
        // probado lleva bonus/sqrt(visitas), y un ruido mínimo deshace los
        // empates al azar en vez de a favor de NONE. Anoche NONE se llevó el
        // 85-92 % de las decisiones y el adaptativo jugaba como el de serie.
        if (b.learn && cfg.explorationBonus > 0.0f)
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                if (!visits[i])
                    candidates[i].score = (anyVisited ? bestVisitedQ : 0.0f) + cfg.explorationBonus + PersonalityBias(b.profile, actions[candidates[i].index].kind);
                else
                    candidates[i].score += cfg.explorationBonus / std::sqrt(float(visits[i]));
                candidates[i].score += frand(-0.01f, 0.01f);
            }

        // Orden por puntuación; empates (sin aprender) hacia NONE, playerbots de serie
        std::stable_sort(candidates.begin(), candidates.end(), [](Scored const& x, Scored const& y)
        {
            if (x.score != y.score)
                return x.score > y.score;
            return x.index < y.index;
        });

        std::string why;
        size_t pick = 0;
        if (b.epsilon > 0.0f && candidates.size() > 1 && frand(0.0f, 1.0f) < b.epsilon)
        {
            pick = urand(0, uint32(candidates.size() - 1));
            why = "exploracion";
        }
        else if (b.difficulty.second > 0.0f && candidates.size() > 1 && frand(0.0f, 1.0f) < b.difficulty.second)
        {
            pick = 1;
            why = "error de dificultad";
        }
        else
            why = "mejor valor";

        // Se intenta la elegida y, si playerbots la rechaza, las siguientes
        // El goteo posicional y el del reloj van por SEGUNDO, no por decisión:
        // cobrados por decisión, un lanzador que decide más se llevaba más por
        // el mismo combate (+19,4 con 3 decisiones contra +31,5 con 112,
        // auditoría del 04/09/2026). DefensivaDesperdiciada y AccionFallida
        // siguen siendo por suceso.
        float dtSec = std::clamp(float(now - b.lastDecisionMs) / 1000.0f, 0.0f, 3.0f);
        float immediate = (cfg.rTime + DistanceReward(b.role, state)) * dtSec;
        uint8 chosen = 0;
        float chosenQ = 0.0f;
        size_t tries = 0;

        // La decision se anota ANTES de ejecutar la accion, y con la accion que
        // se esta intentando. En este core un instantaneo se resuelve dentro de
        // la misma llamada (Spell::prepare -> cast, sin esperar al GCD), asi que
        // la interrupcion lograda, el control aplicado, el dano y hasta el fin de
        // la partida (si la accion mata, Arena::HandleKillPlayer termina la arena
        // y llega a EndBrain) saltan en los hooks mientras DoSpecificAction
        // todavia no ha vuelto. Hasta el 05/09/2026 lastAction era -1 en ese
        // momento y pendingReward se ASIGNABA al final: todo lo que la accion
        // provocaba en el acto se perdia para la tabla Q (no para `rec`, que
        // suma en totalReward, por eso ninguna auditoria por base lo veia).
        Decision d;
        d.timeMs = now;
        d.state = key;
        d.action = 0;
        b.history.push_back(d);
        if (b.history.size() > HISTORY_MAX)
            b.history.pop_front();
        ++b.decisions;
        b.lastState = key;
        b.lastAction = 0;
        b.lastDecisionMs = now;

        AuditBrain(b, "open");
        for (size_t i = pick; i < candidates.size() && tries < 3; ++i, ++tries)
        {
            ActionDef const& def = actions[candidates[i].index];
            b.lastAction = candidates[i].index;
            b.history.back().action = candidates[i].index;
            b.history.back().q = candidates[i].q;
            AuditBrain(b, "attempt");
            bool const done = ExecuteAction(b, botAI, bot, enemy, def, now);
            AuditBrain(b, b.ended ? "result_after_end" : "result", done ? 1.0f : 0.0f);
            if (b.ended)
                return;   // la partida acabo dentro de la accion: EndBrain ya cerro esta decision
            if (done)
            {
                chosen = candidates[i].index;
                chosenQ = candidates[i].q;
                if (def.kind == KIND_DEFENSIVE && state.selfHp == 2 && !state.selfControlled)
                    immediate += cfg.rWastedDefensive;
                // Una acción que castea corta el kiteo en marcha (hay que pararse)
                if (b.kiteUntilMs && (def.kind == KIND_OFFENSIVE || def.kind == KIND_CONTROL || def.kind == KIND_INTERRUPT))
                    b.kiteUntilMs = 0;
                if (i != pick)
                    why += ", tras " + std::to_string(i - pick) + " fallida(s)";
                break;
            }
            // Rechazada: no se ha lanzado nada, la decision vuelve a "none"
            b.lastAction = 0;
            b.history.back().action = 0;
            b.history.back().q = 0.0f;
            immediate += cfg.rFailedAction;
            ++b.failedActions;
            ++b.failedHere[(uint32(key) << 8) | candidates[i].index];
            // La acción que playerbots rechaza aquí (inútil ahora) contaba como
            // visita y valía el mejor valor del estado menos uno (un valor
            // absoluto, -0,2, parecía BUENO en los estados en los que se va
            // perdiendo). Pero "mejor menos uno" la dejaba como SEGUNDA mejor
            // del estado sin haberse ejecutado nunca, y la siguiente en cuanto
            // fallaba la primera: el 3-8 % de las casillas de una visita
            // llevaban esa huella exacta (05/09/2026, N4). Apagado por defecto
            // (Decision.RechazoAprende): el rechazo solo cuenta en failedHere
            // (a la segunda deja de ofrecerse en esta sesión) y en el premio.
            if (cfg.rejectLearns && b.learn && b.learnTable)
                Learn(b, key, candidates[i].index, (anyVisited ? bestVisitedQ : 0.0f) + cfg.rFailedAction * 5.0f, 1.0f, true);
        }

        std::ostringstream explain;
        explain << state.Describe() << " | ";
        for (size_t i = 0; i < candidates.size() && i < 4; ++i)
            explain << actions[candidates[i].index].name << (candidates[i].prior ? "~=" : "=") << Acore::StringFormat("{:.2f}", candidates[i].score) << " ";
        explain << "| " << actions[chosen].name << " (" << why << ")";

        b.history.back().action = chosen;
        b.history.back().q = chosenQ;
        b.history.back().explain = explain.str();
        b.lastAction = chosen;
        // El tope se aplica aquí, con la defensiva desperdiciada y las acciones
        // fallidas ya dentro de `immediate`: los tres son premio de sucesos. Se
        // SUMA a lo que los hooks hayan cobrado ya durante la accion.
        immediate = ShapedAmount(b, immediate);
        b.pendingReward += immediate;
        b.totalReward += immediate;
        AuditBrain(b, "reward", immediate, __LINE__);
    }

    void BrainTick(Player* bot)
    {
        std::shared_ptr<Brain> brain = FindBrain(bot->GetGUID());
        if (!brain)
            return;

        uint32 now = getMSTime();
        std::lock_guard<std::recursive_mutex> guard(brain->lock);
        if (now < brain->nextTickMs)
            return;
        brain->nextTickMs = now + cfg.tickMs;

        if (!bot->IsAlive() || brain->dead)
            return;

        // En combate real el enemigo puede cambiar; en equipo, se sigue al
        // rival que el bot está atacando y, si el suyo muere, al más cercano
        Unit* enemy = ObjectAccessor::GetUnit(*bot, brain->enemy);
        if (!brain->matchId)
        {
            if (Player* victim = PlayerOf(bot->GetVictim()))
                if (victim != bot && victim->GetGUID() != brain->enemy)
                {
                    enemy = victim;
                    brain->enemy = victim->GetGUID();
                    brain->enemyClass = victim->getClass();
                }
        }
        else if (brain->teamMatch)
        {
            if (Player* victim = PlayerOf(bot->GetVictim()))
                if (victim != bot && victim->GetGUID() != brain->enemy && victim->IsAlive() && IsEnemyOf(*brain, victim->GetGUID()))
                {
                    enemy = victim;
                    brain->enemy = victim->GetGUID();
                    brain->enemyClass = victim->getClass();
                }
            bool valid = enemy && enemy->IsAlive() && enemy->IsInWorld() && enemy->GetMap() == bot->GetMap();
            if (!valid && now >= brain->lastEnemySearchMs + 1000)
            {
                brain->lastEnemySearchMs = now;
                if (Player* e = NearestEnemy(*brain, bot, 80.0f))
                {
                    enemy = e;
                    brain->enemy = e->GetGUID();
                    brain->enemyClass = e->getClass();
                }
            }
        }
        if (!enemy || !enemy->IsAlive() || !enemy->IsInWorld() || enemy->GetMap() != bot->GetMap())
            return;

        if (Battleground* bg = bot->GetBattleground())
            if (bg->GetStatus() != STATUS_IN_PROGRESS)
                return;

        // Kiteo en marcha: repetir el paso hasta llegar a la distancia o agotar la ventana
        if (brain->kiteUntilMs)
        {
            if (now >= brain->kiteUntilMs || bot->GetDistance(enemy) >= cfg.kiteDistance - 2.0f)
                brain->kiteUntilMs = 0;
            else if (now >= brain->kiteLastMoveMs + 600)
            {
                brain->kiteLastMoveMs = now;
                KiteStep(bot, enemy);
            }
        }

        StateInfo state = ComputeState(bot, enemy, brain.get());
        uint16 key = state.Key();
        bool decide = (key != brain->lastState && now >= brain->lastDecisionMs + cfg.decisionMinGapMs) ||
                      now >= brain->lastDecisionMs + cfg.decisionIntervalMs;
        if (!decide)
            return;

        // Casteo propio (05/09/2026): mientras el bot lanza un hechizo con tiempo
        // de casteo no se decide. Una orden de hechizo del modulo por
        // DoSpecificAction se salta la espera del UpdateAI de playerbots; el core
        // no la frena (Spell::prepare solo corta "spell in progress" con
        // m_cast_count, que ponen los paquetes del cliente) y
        // SetCurrentCastedSpell interrumpe el que estaba en curso. Con 1,2-1,4
        // decisiones/s y el 45 % que no eran "none", los lanzadores perdian el
        // final de sus casteos (la ventana entre el fin del GCD y el fin del
        // cast). Brujo, chaman elemental y mago llevaban cinco examenes sin
        // mejorar; las cinco clases de instantaneos aprobaron en horas.
        // Se pospone la decision: la anterior sigue cobrando lo que su hechizo
        // haga (asi el credito del dano va a quien lo lanzo). Excepcion: si el
        // rival castea y esta clase interrumpe, se decide solo entre none e
        // interrumpir, que cortar el propio para cortar el suyo es legitimo.
        bool onlyInterrupt = false;
        if (cfg.respectOwnCast && brain->mode != BRAIN_NONE && brain->table && OwnCastInProgress(bot))
        {
            bool canInterrupt = false;
            if (state.enemyCasting && brain->usableReady)
            {
                std::vector<ActionDef> const& actions = ActionsFor(brain->myClass);
                for (size_t i = 0; i < actions.size() && i < 64; ++i)
                    if (actions[i].kind == KIND_INTERRUPT && (brain->usable & (uint64(1) << i)))
                        canInterrupt = true;
            }
            if (!canInterrupt)
            {
                ++s_castWaits;
                AuditBrain(*brain, "wait_cast");
                return;
            }
            onlyInterrupt = true;
        }

        // Cierre de la decisión anterior con lo cobrado desde entonces
        if (brain->lastAction >= 0)
        {
            if (!brain->history.empty())
            {
                brain->history.back().reward += brain->pendingReward;
                LogExperience(*brain, brain->history.back());
            }
            if (brain->learn && brain->learnTable)
            {
                float bootstrap = cfg.gamma * brain->learnTable->MaxQ(brain->myClass, brain->enemyClass, key, uint8(ActionsFor(brain->myClass).size()), cfg.generalize);
                AuditBrain(*brain, "bootstrap", bootstrap);
                LearnStep(*brain, brain->pendingReward + bootstrap, false);
            }
        }
        brain->pendingReward = 0.0f;
        brain->lastAction = -1;

        if (brain->mode == BRAIN_NONE || !brain->table)
        {
            brain->lastState = key;
            brain->lastDecisionMs = now;
            return;
        }

        Decide(*brain, bot, enemy, state, now, onlyInterrupt);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  La mascota del brujo, en PvP
    //
    //  playerbots elige la mascota por rama de talentos (AiFactory.cpp): la
    //  aflicción lleva manáfago, la demonología abisario y la destrucción
    //  diablillo. En PvP la que vale es el manáfago (o la súcubo), así que aquí
    //  se garantiza una sola, la de AdaptiveAI.Brujo.MascotaPvP.
    //
    //  Y de paso se corta un bucle: los disparadores "no pet" y "wrong pet" del
    //  motor NO-COMBATE de playerbots se quedaban encendidos y el brujo
    //  reinvocaba su demonio varias veces por segundo (152 veces en tres
    //  minutos, medido el 04/09/2026). Eso le costaba el 40 % de su daño —hacía
    //  12.184 de media perdiendo contra los 18.000-22.000 del resto— y, con el
    //  premio de entonces, le pagaba +3 por cada invocación. Mientras dura el
    //  combate el módulo se queda con la mascota: apagadas las cinco
    //  estrategias, no hay disparador que invoque. Al terminar,
    //  ResetStrategies() le devuelve las suyas.
    // ─────────────────────────────────────────────────────────────────────────
    void OwnWarlockPet(Player* bot, bool take)
    {
        if (!bot || bot->getClass() != CLASS_WARLOCK)
            return;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return;
        static char const* const kPets[] = { "imp", "voidwalker", "succubus", "felhunter", "felguard" };

        if (!take)
        {
            botAI->ResetStrategies();
            return;
        }

        std::string want = cfg.warlockPvpPet;
        bool known = false;
        for (char const* name : kPets)
            if (want == name)
                known = true;
        if (!known)
            want = "felhunter";

        // Si no la lleva, encender su estrategia e invocarla UNA vez
        Pet* pet = bot->GetPet();
        CreatureTemplate const* ct = pet ? pet->GetCreatureTemplate() : nullptr;
        static std::unordered_map<std::string, uint32> const kEntry = {
            { "imp", 416 }, { "voidwalker", 1860 }, { "succubus", 1863 }, { "felhunter", 417 }, { "felguard", 17252 } };
        auto wantEntry = kEntry.find(want);
        bool correct = ct && wantEntry != kEntry.end() && ct->Entry == wantEntry->second;
        bool summon = !correct;

        // Y se deja encendida EXACTAMENTE la de la mascota que queremos: con una
        // sola, el disparador "wrong pet" de playerbots defiende nuestra
        // eleccion en vez de la de la rama de talentos (aflicción manáfago,
        // demonología abisario, destrucción diablillo), y si la mascota muere
        // se vuelve a invocar sola.
        std::string set;
        for (char const* name : kPets)
            set += (set.empty() ? "" : ",") + std::string(want == name ? "+" : "-") + name;
        botAI->ChangeStrategy(set, BOT_STATE_NON_COMBAT);
        if (summon)
            botAI->DoSpecificAction("summon " + want, Event(), true);
    }
#else
    void OwnWarlockPet(Player*, bool) {}
    void BrainTick(Player* /*bot*/) {}
    void MaintainLevel80() {}
    std::string Level80Status() { return "sin mod-playerbots"; }
    void SyncLevelBrackets() {}
    std::string LevelBracketsStatus() { return "sin mod-playerbots"; }
    bool EnsureSpec(Player*) { return false; }
    bool EnsureDualSpec(Player*) { return false; }
    bool ApplyPurpose(Player*, bool, uint32, bool) { return false; }
    void QueuePurpose(ObjectGuid, bool, uint32, bool) {}
    uint32 PvpIlvlFor(float) { return 0; }
    void MaintainLoadout(uint32) {}
    std::string LoadoutStatus() { return "sin mod-playerbots"; }
    std::string UsableStatus() { return "sin mod-playerbots"; }
    int TargetFor(uint8, uint8) { return -1; }
#endif

    // ─────────────────────────────────────────────────────────────────────────
    //  Eventos de recompensa
    // ─────────────────────────────────────────────────────────────────────────
    static uint64 const CC_MECHANICS =
        (1ULL << MECHANIC_CHARM) | (1ULL << MECHANIC_DISORIENTED) | (1ULL << MECHANIC_FEAR) | (1ULL << MECHANIC_ROOT) |
        (1ULL << MECHANIC_SLEEP) | (1ULL << MECHANIC_STUN) | (1ULL << MECHANIC_FREEZE) | (1ULL << MECHANIC_KNOCKOUT) |
        (1ULL << MECHANIC_POLYMORPH) | (1ULL << MECHANIC_BANISH) | (1ULL << MECHANIC_HORROR) | (1ULL << MECHANIC_SAPPED) |
        (1ULL << MECHANIC_SILENCE);

    static bool IsCcSpell(SpellInfo const* info)
    {
        uint64 mask = info->GetAllEffectsMechanicMask() | (info->Mechanic ? (1ULL << info->Mechanic) : 0);
        if (mask & CC_MECHANICS)
            return true;
        return info->HasAura(SPELL_AURA_MOD_STUN) || info->HasAura(SPELL_AURA_MOD_FEAR) ||
               info->HasAura(SPELL_AURA_MOD_CONFUSE) || info->HasAura(SPELL_AURA_MOD_ROOT) ||
               info->HasAura(SPELL_AURA_MOD_SILENCE) || info->HasAura(SPELL_AURA_MOD_PACIFY_SILENCE);
    }

    static bool IsSnareSpell(SpellInfo const* info)
    {
        return info->HasAura(SPELL_AURA_MOD_DECREASE_SPEED);
    }

    static void CountStat(ObjectGuid guid, uint32 Brain::* field, uint32 add)
    {
        std::shared_ptr<Brain> brain = FindBrain(guid);
        if (!brain)
            return;
        std::lock_guard<std::recursive_mutex> guard(brain->lock);
        (*brain).*field += add;
    }

    // ¿El objetivo es un compañero del lanzador en esta partida? (2c2 y más)
    static bool SameSideAs(std::shared_ptr<Brain> const& brain, ObjectGuid target)
    {
        if (!brain)
            return false;
        std::lock_guard<std::recursive_mutex> guard(brain->lock);
        return std::find(brain->allies.begin(), brain->allies.end(), target) != brain->allies.end();
    }

    // Un control o un snare se cobra UNA vez por (hechizo, objetivo) mientras el
    // aura siga viva. Antes se cobraba en cada reaplicación: un DoT o una
    // maldición que se refresca volvía a pagar, y cada Descarga de Escarcha
    // valía +1 (284 cobros en tres minutos, auditoría del 04/09/2026). La
    // memoria vive en el cerebro de la víctima si lo tiene (así el cobro y el
    // cargo deciden lo mismo) y si no, en el del lanzador.
    static bool AlreadyPaidRecently(std::shared_ptr<Brain> const& holder, SpellInfo const* info, ObjectGuid target)
    {
        if (!holder)
            return false;
        uint32 now = getMSTime();
        uint64 key = (uint64(info->Id) << 32) | uint64(target.GetCounter());
        std::lock_guard<std::recursive_mutex> guard(holder->lock);
        auto it = holder->ccPaidUntilMs.find(key);
        if (it != holder->ccPaidUntilMs.end() && now < it->second)
            return true;
        holder->ccPaidUntilMs[key] = now + uint32(std::clamp<int32>(info->GetMaxDuration(), 1500, 30000));
        return false;
    }

    class mod_adaptive_ai_unit : public UnitScript
    {
    public:
        mod_adaptive_ai_unit() : UnitScript("mod_adaptive_ai_unit", true,
            { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_UNIT_DEATH }) { }

        // Las dos patas del daño van por el DUEÑO y con la MISMA fracción.
        // Antes el pago se normalizaba por la vida de la VÍCTIMA, así que pegar
        // a un gul o a un manáfago pagaba varias veces más que pegar a su dueño,
        // y el daño que encajaba tu mascota era gratis: entre el 3 % (druida) y
        // el 13 % (CdM) del daño de cada clase no lo sufría el rival.
        void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
        {
            if (!ActiveBrains() || !damage || !victim)
                return;
            Player* owner = PlayerOf(victim);
            Player* dealer = PlayerOf(attacker);
            if (!owner || owner == dealer)
                return;
            std::shared_ptr<Brain> dealerBrain = dealer ? FindBrain(dealer->GetGUID()) : nullptr;
            if (SameSideAs(dealerBrain, owner->GetGUID()))
                return;   // fuego amigo: ni paga ni cuesta
            float part = float(damage) / float(std::max<uint32>(1, owner->GetMaxHealth()));
            if (std::shared_ptr<Brain> brain = FindBrain(owner->GetGUID()))
            {
                std::lock_guard<std::recursive_mutex> guard(brain->lock);
                float const given = ShapedAmount(*brain, -cfg.rDamage * part);
                brain->pendingReward += given;
                brain->totalReward += given;
                AuditBrain(*brain, "reward", given, __LINE__);
                brain->damageTaken += damage;
            }
            if (dealerBrain)
            {
                std::lock_guard<std::recursive_mutex> guard(dealerBrain->lock);
                float const given = ShapedAmount(*dealerBrain, cfg.rDamage * part);
                dealerBrain->pendingReward += given;
                dealerBrain->totalReward += given;
                AuditBrain(*dealerBrain, "reward", given, __LINE__);
                dealerBrain->damageDealt += damage;
            }
        }

        void OnAuraApply(Unit* unit, Aura* aura) override
        {
            if (!ActiveBrains() || !unit || !aura)
                return;
            SpellInfo const* info = aura->GetSpellInfo();
            if (!info)
                return;
            bool cc = IsCcSpell(info);
            bool snare = !cc && IsSnareSpell(info);
            if (!cc && !snare)
                return;
            Player* caster = PlayerOf(aura->GetCaster());
            if (!caster)
                return;

            // Diagnostico (AdaptiveAI.Log.Auras): TODO control o snare que pasa
            // por aqui, se cobre o no, con el motivo. Va antes de los cortes a
            // proposito: si solo mirara lo cobrado no se veria lo que se esta
            // dejando de pagar, que es justo lo que hay que vigilar.
            auto trace = [&](char const* motivo)
            {
                if (cfg.logAuras)
                    LOG_INFO("module", "[adaptive-ai][aura] {} ({}) {} por {} ({}) sobre {} ({}) -> {}",
                             caster->GetName(), uint32(caster->getClass()), cc ? "control" : "snare",
                             info->Id, info->SpellName[0] ? info->SpellName[0] : "?",
                             unit->GetName(), unit->ToPlayer() ? "jugador" : "no-jugador", motivo);
            };

            // Sólo cuenta lo que cae sobre un JUGADOR RIVAL. Antes se pagaba al
            // lanzador cayera sobre quien cayera, y como aquí el lanzador es el
            // DUEÑO (PlayerOf) mientras el objetivo sigue siendo la mascota, la
            // guarda de "aura propia" no protegía: el brujo cobraba +3 por cada
            // `Summoning Disorientation` sobre su propio demonio, 28 controles
            // por combate contra 1 que sufría su rival (auditoría del 04/09/2026).
            Player* victim = unit->ToPlayer();
            if (!victim || victim == caster)
                return trace(victim ? "no cuenta: sobre uno mismo" : "NO cuenta: el objetivo no es un jugador");
            std::shared_ptr<Brain> casterBrain = FindBrain(caster->GetGUID());
            std::shared_ptr<Brain> victimBrain = FindBrain(victim->GetGUID());
            if (!casterBrain && !victimBrain)
                return trace("no cuenta: ninguno de los dos entrena");
            if (SameSideAs(casterBrain, victim->GetGUID()))
                return trace("no cuenta: es un companero");
            if (AlreadyPaidRecently(victimBrain ? victimBrain : casterBrain, info, victim->GetGUID()))
                return trace("no cuenta: reaplicacion");

            RewardShaped(victim->GetGUID(), cc ? cfg.rCcTaken : cfg.rSnareTaken);
            RewardShaped(caster->GetGUID(), cc ? cfg.rCcDone : cfg.rSnareDone);
            if (cc)
            {
                CountStat(victim->GetGUID(), &Brain::ccTaken, 1);
                CountStat(caster->GetGUID(), &Brain::ccDone, 1);
            }

            trace("cobrado");
        }

        void OnUnitDeath(Unit* unit, Unit* killer) override
        {
            if (!ActiveBrains() || !unit)
                return;
            Player* p = unit->ToPlayer();
            if (!p)
                return;
            std::shared_ptr<Brain> brain = FindBrain(p->GetGUID());
            if (!brain)
                return;
            std::vector<ObjectGuid> allies;
            {
                std::lock_guard<std::recursive_mutex> guard(brain->lock);
                if (brain->dead)
                    return;   // ya cobrada (FinishMatch pudo adelantarse)
                brain->pendingReward += cfg.rDeath;
                brain->totalReward += cfg.rDeath;
                AuditBrain(*brain, "reward", cfg.rDeath, __LINE__);
                brain->dead = true;
                allies = brain->allies;
            }
            // Fuera del cerrojo del muerto: cada aliado tiene el suyo
            for (ObjectGuid ally : allies)
                Reward(ally, cfg.rAllyDeath);

            // El premio por matar va aquí y no en OnPlayerPVPKill: aquel sólo
            // salta cuando el golpe final lo da el jugador en persona, así que
            // se perdía cada muerte rematada por una mascota o por un DoT.
            if (Player* k = PlayerOf(killer))
                if (k != p)
                {
                    std::vector<ObjectGuid> mates;
                    if (std::shared_ptr<Brain> kb = FindBrain(k->GetGUID()))
                    {
                        std::lock_guard<std::recursive_mutex> guard(kb->lock);
                        mates = kb->allies;
                    }
                    Reward(k->GetGUID(), cfg.rKill);
                    for (ObjectGuid ally : mates)
                        Reward(ally, cfg.rTeamKill);
                }
        }
    };

    // ─────────────────────────────────────────────────────────────────────────
    //  Bots de nivel 80 por clase. Con 1500 personajes y un 35 % de
    //  probabilidad de máximo nivel en la primera aleatorización hay unos 19
    //  guerreros y 19 magos de 80 en total, y 5-8 conectados: ese es el tope
    //  real de arenas 1c1. Aquí, cada 10 s, si hay menos de los pedidos
    //  conectados, se sube UN bot libre de esa clase a 80 con la fábrica de
    //  playerbots (nivel, talentos, hechizos, equipo: lo mismo que hace
    //  playerbots al aleatorizar). Uno por pasada para no atascar el mundo.
    // ─────────────────────────────────────────────────────────────────────────
    // ─────────────────────────────────────────────────────────────────────────
    //  Specs PvP. playerbots trae specs premade por clase con nombre
    //  ("arms pvp", "frost pvp"...; AiPlayerbot.PremadeSpecName.<clase>.<n>),
    //  pero los bots aleatorios nunca las cogen (RandomClassSpecProb a 0). Con
    //  la spec PvP puesta, la fábrica de playerbots pondera resiliencia y da
    //  el abalorio anti-control al equipar (IsSpecPvp). Aquí se pone la spec
    //  pedida a los bots que entran en arena y a los que se suben a 80; la
    //  tabla Q sigue indexada por clase, así que conviene una sola spec por
    //  clase mientras se entrena (el documento de referencia: PvP Matchup
    //  Reference.md).
    // ─────────────────────────────────────────────────────────────────────────
    static int PremadeSpecIndex(uint8 cls, std::string const& name)
    {
        for (int i = 0; i < MAX_SPECNO; ++i)
            if (sPlayerbotAIConfig.premadeSpecName[cls][i] == name)
                return i;
        return -1;
    }

    bool EnsureSpec(Player* bot)
    {
        auto it = cfg.arenaSpecs.find(bot->getClass());
        if (it == cfg.arenaSpecs.end())
            return false;
        int wanted = PremadeSpecIndex(bot->getClass(), it->second);
        if (wanted < 0)
        {
            static std::set<std::string> warned;
            if (warned.insert(it->second).second)
                LOG_WARN("module", "[adaptive-ai] AdaptiveAI.Arena.Specs: playerbots no tiene la spec '{}' para {} (AiPlayerbot.PremadeSpecName).", it->second, ClassName(bot->getClass()));
            return false;
        }
        uint32 current = sRandomPlayerbotMgr.GetValue(bot->GetGUID().GetCounter(), "specNo");   // guardado +1
        if (current == uint32(wanted) + 1)
            return false;
        PlayerbotFactory::InitTalentsBySpecNo(bot, wanted, true);
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitGlyphs(false);
        LOG_INFO("module", "[adaptive-ai] Spec: {} ({}) pasa a '{}'.", bot->GetName(), ClassName(bot->getClass()), it->second);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  Loadout: doble spec y equipo por propósito (03/09/2026)
    //
    //  Cada bot aleatorio lleva dos specs, como un jugador con doble
    //  especialización: la 0 es su spec PvE (la premade que playerbots le dio
    //  al aleatorizarlo, o una premade "pve" de su clase) y la 1 es la spec
    //  PvP premade de AdaptiveAI.Arena.Specs. Las dos completas: los enlaces
    //  premade de playerbots a nivel 80 reparten los 71 puntos, y si tras
    //  aplicarlos quedan puntos libres se avisa en el log. La doble spec se
    //  aprende como lo hace playerbots (hechizos 63680 y 63624) a partir de
    //  MinDualSpecLevel (40).
    //
    //  El equipo se regenera con la fábrica de playerbots según el propósito,
    //  no se guardan dos juegos (no cabrían en las bolsas): PvP por el rating
    //  Elo del bot (AdaptiveAI.Loadout.EquipoPvP: 232 Furioso, 251
    //  Implacable, 264/270 Colérico) y PvE por el contenido en el que entra
    //  (AdaptiveAI.Loadout.EquipoPvE: mazmorra normal, heroica, banda de 10 o
    //  de 25 de la expansión 2; en el mundo, lo que traiga). Con la spec PvP
    //  activa la fábrica pondera resiliencia (IsSpecPvp). Si el bot va en el
    //  grupo de un jugador de verdad, el equipo lo topa mod-queue-bots a la
    //  fase del jugador (BotGear) y aquí solo se cambia la spec.
    //
    //  Cambiar de spec y regenerar el equipo cuesta décimas de segundo por
    //  bot: los cambios que llegan desde hilos de mapa (OnMapChanged, duelo)
    //  se encolan y los hace el hilo del mundo de uno en uno.
    // ─────────────────────────────────────────────────────────────────────────
    static uint32 WornIlvl(Player* player)
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

    static int PvePremadeFor(uint8 cls)
    {
        std::vector<int> options;
        for (int i = 0; i < MAX_SPECNO; ++i)
        {
            std::string const& name = sPlayerbotAIConfig.premadeSpecName[cls][i];
            if (name.find("pve") != std::string::npos && !sPlayerbotAIConfig.premadeSpecLink[cls][i][80].empty())
                options.push_back(i);
        }
        if (options.empty())
            return -1;
        return options[urand(0, uint32(options.size() - 1))];
    }

    static void ApplyPremade(Player* bot, int idx, char const* what)
    {
        PlayerbotFactory::InitTalentsBySpecNo(bot, idx, true);
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitGlyphs(false);
        if (uint32 libres = bot->GetFreeTalentPoints())
            LOG_WARN("module", "[adaptive-ai] Loadout: {} ({}) se queda con {} puntos sin asignar en la spec {} '{}' (nivel {}).",
                     bot->GetName(), ClassName(bot->getClass()), libres, what, sPlayerbotAIConfig.premadeSpecName[bot->getClass()][idx], bot->GetLevel());
    }

    static uint32 s_loadoutDual = 0, s_loadoutSwitches = 0, s_loadoutRegears = 0;

    bool EnsureDualSpec(Player* bot)
    {
        if (!cfg.loadoutEnable || !bot || !bot->IsInWorld())
            return false;
        uint8 cls = bot->getClass();
        auto it = cfg.arenaSpecs.find(cls);
        if (it == cfg.arenaSpecs.end())
            return false;
        int pvpIdx = PremadeSpecIndex(cls, it->second);
        if (pvpIdx < 0)
            return false;
        if (bot->GetLevel() < sWorld->getIntConfig(CONFIG_MIN_DUALSPEC_LEVEL))
            return false;

        BotProfile& p = GetProfile(bot);
        uint32 low = bot->GetGUID().GetCounter();
        bool touched = false;

        // La spec PvE: la que playerbots eligió, salvo que sea una PvP (la
        // versión de esta mañana la ponía sobre la única spec)
        uint32 stored = sRandomPlayerbotMgr.GetValue(low, "specNo");   // 1-based
        int currentIdx = stored ? int(stored) - 1 : -1;
        if (p.specPve < 0 || p.specPve >= MAX_SPECNO)
        {
            bool currentIsPvp = currentIdx >= 0 && sPlayerbotAIConfig.premadeSpecName[cls][currentIdx].find("pvp") != std::string::npos;
            p.specPve = int8(currentIdx >= 0 && !currentIsPvp ? currentIdx : PvePremadeFor(cls));
            touched = true;
        }

        if (bot->GetSpecsCount() < 2)
        {
            bot->CastSpell(bot, 63680, true, nullptr, nullptr, bot->GetGUID());
            bot->CastSpell(bot, 63624, true, nullptr, nullptr, bot->GetGUID());
            if (bot->GetSpecsCount() < 2)
                bot->UpdateSpecCount(2);
            touched = true;
        }
        if (bot->GetSpecsCount() < 2)
            return false;

        uint8 before = bot->GetActiveSpec();
        // Spec 1 = PvP
        if (p.specPvp != pvpIdx || touched)
        {
            if (bot->GetActiveSpec() != 1)
                bot->ActivateSpec(1);
            ApplyPremade(bot, pvpIdx, "PvP");
            p.specPvp = int8(pvpIdx);
            touched = true;
        }
        // Spec 0 = PvE, completa
        if (bot->GetActiveSpec() != 0)
            bot->ActivateSpec(0);
        if (p.specPve >= 0 && (touched || bot->GetFreeTalentPoints() > 0))
        {
            bool currentIsPve = currentIdx == p.specPve && bot->GetFreeTalentPoints() == 0;
            if (!currentIsPve)
                ApplyPremade(bot, p.specPve, "PvE");
        }
        // Volver a la que estaba activa (el propósito lo decide ApplyPurpose)
        if (bot->GetActiveSpec() != before)
            bot->ActivateSpec(before);
        int activeIdx = before == 1 ? p.specPvp : p.specPve;
        if (activeIdx >= 0)
            sRandomPlayerbotMgr.SetValue(low, "specNo", uint32(activeIdx) + 1);
        p.purpose = before == 1 ? "pvp" : "pve";
        if (touched)
        {
            ++s_loadoutDual;
            p.dirty = true;
            SaveProfile(p);
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                botAI->ResetStrategies();
            LOG_INFO("module", "[adaptive-ai] Loadout: {} ({}) con doble spec: PvE '{}', PvP '{}'.", bot->GetName(), ClassName(cls),
                     p.specPve >= 0 ? sPlayerbotAIConfig.premadeSpecName[cls][p.specPve] : "?", it->second);
        }
        return touched;
    }

    uint32 PvpIlvlFor(float rating)
    {
        uint32 ilvl = 0;
        for (auto const& [minRating, il] : cfg.loadoutPvpIlvl)
            if (rating >= float(minRating))
                ilvl = il;
        return ilvl;
    }

    // Equipo PvP por el mundo. Lo normal es levear con equipo PvE; el PvP es
    // de arenas y campos, y solo al nivel tope de la etapa (60, 70, 80) es
    // normal verlo por el mundo. Un bot por debajo del tope que sale de un
    // campo con su equipo de resiliencia tiene ventaja sobre el jugador que
    // está leveando: se le vuelve a equipar PvE por nivel.
    static uint32 s_loadoutWorldRegears = 0;

    static bool WearsPvpGear(Player* bot)
    {
        return bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_CRIT_TAKEN_MELEE) > 0;   // resiliencia
    }

    static bool AtStageCap(Player* bot)
    {
        uint8 level = bot->GetLevel();
        if (level >= sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
            return true;
        return std::find(cfg.loadoutCapLevels.begin(), cfg.loadoutCapLevels.end(), level) != cfg.loadoutCapLevels.end();
    }

    static bool InOpenWorld(Player* bot)
    {
        Map* map = bot->GetMap();
        return map && !map->IsDungeon() && !map->IsBattlegroundOrArena();
    }

    static bool ItemHasResilience(Item* item)
    {
        ItemTemplate const* tmpl = item ? item->GetTemplate() : nullptr;
        if (!tmpl)
            return false;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
            if (tmpl->ItemStat[i].ItemStatValue > 0 &&
                (tmpl->ItemStat[i].ItemStatType == ITEM_MOD_RESILIENCE_RATING || tmpl->ItemStat[i].ItemStatType == ITEM_MOD_CRIT_TAKEN_RATING))
                return true;
        return false;
    }

    // true si le ha cambiado el equipo. Solo se quitan las piezas con
    // resiliencia y se rellenan los huecos por nivel; el equipo aleatorio por
    // nivel puede traer alguna pieza PvP suelta, así que un mismo bot no se
    // toca más de una vez cada 30 minutos (Foriti, nivel 76, se reequipó dos
    // veces en 30 s la primera tarde).
    static std::unordered_map<uint32, uint32> s_worldRegearAt;   // guid -> ms
    static bool RegearPveForWorld(Player* bot)
    {
        if (!InOpenWorld(bot) || AtStageCap(bot) || !WearsPvpGear(bot))
            return false;
        uint32 now = getMSTime();
        uint32 low = bot->GetGUID().GetCounter();
        if (auto it = s_worldRegearAt.find(low); it != s_worldRegearAt.end() && now < it->second + 30 * 60000)
            return false;
        s_worldRegearAt[low] = now;
        uint32 worn = WornIlvl(bot);
        uint32 removed = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (ItemHasResilience(item))
                {
                    bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
                    ++removed;
                }
        uint32 quality = sConfigMgr->GetOption<uint32>("AiPlayerbot.AutoGearQualityLimit", 4);
        PlayerbotFactory::AutoGear(bot, quality, 0, true, false);   // incremental: solo los huecos, por nivel
        BotProfile& p = GetProfile(bot);
        p.ilvl = 0;
        p.dirty = true;
        ++s_loadoutWorldRegears;
        LOG_INFO("module", "[adaptive-ai] Loadout: {} (nivel {}) vuelve al mundo con equipo PvE por nivel: {} piezas con resiliencia fuera (llevaba ~{}).",
                 bot->GetName(), bot->GetLevel(), removed, worn);
        return true;
    }

    bool ApplyPurpose(Player* bot, bool pvp, uint32 ilvl, bool forceGear)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive())
            return false;
        BotProfile& p = GetProfile(bot);
        bool dual = bot->GetSpecsCount() >= 2 && p.specPvp >= 0;
        if (!dual)
        {
            EnsureDualSpec(bot);
            dual = bot->GetSpecsCount() >= 2 && p.specPvp >= 0;
        }
        bool changed = false;
        uint32 low = bot->GetGUID().GetCounter();
        if (dual)
        {
            uint8 want = pvp ? 1 : 0;
            if (bot->GetActiveSpec() != want)
            {
                bot->ActivateSpec(want);
                changed = true;
                ++s_loadoutSwitches;
            }
            int idx = pvp ? p.specPvp : p.specPve;
            // playerbots aleatoriza a veces y borra talentos de la spec activa: reponer
            if (bot->GetFreeTalentPoints() > 0 && idx >= 0)
                ApplyPremade(bot, idx, pvp ? "PvP" : "PvE");
            if (idx >= 0)
                sRandomPlayerbotMgr.SetValue(low, "specNo", uint32(idx) + 1);
        }
        else if (pvp)
            changed = EnsureSpec(bot);   // sin doble spec (nivel bajo): la spec PvP sobre la única, como antes

        if (changed)
        {
            p.purpose = pvp ? "pvp" : "pve";
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                botAI->ResetStrategies();
        }

        // Con propósito PvP, la mascota del brujo la manda el módulo; con
        // propósito PvE se le devuelve la suya (la de su rama de talentos).
        if (bot->getClass() == CLASS_WARLOCK && (changed || pvp))
            OwnWarlockPet(bot, pvp);

        if (ilvl)
        {
            uint32 worn = WornIlvl(bot);
            bool off = worn + cfg.arenaGearTolerance < ilvl || worn > ilvl + cfg.arenaGearTolerance;
            // Se reequipa si el nivel de objeto no es el pedido, no por cambiar
            // de spec: con el mismo nivel de objeto el equipo que lleva sirve.
            if (forceGear || off || p.ilvl != ilvl)
            {
                uint32 quality = sConfigMgr->GetOption<uint32>("AiPlayerbot.AutoGearQualityLimit", 4);
                bool twoRounds = sConfigMgr->GetOption<bool>("AiPlayerbot.TwoRoundsGearInit", true);
                PlayerbotFactory::DestroyEquippedGear(bot);
                PlayerbotFactory::AutoGear(bot, quality, ilvl, false, twoRounds);
                p.ilvl = ilvl;
                ++s_loadoutRegears;
                LOG_DEBUG("module", "[adaptive-ai] Loadout: {} equipado {} a nivel de objeto {} (llevaba ~{}).", bot->GetName(), pvp ? "PvP" : "PvE", ilvl, worn);
            }
        }
        else if (!pvp)
            RegearPveForWorld(bot);   // mundo (o mazmorra con jugador, donde no aplica): fuera la resiliencia si no está al tope
        p.dirty = true;
        SaveProfile(p);
        return changed;
    }

    // Cola para lo que llega desde hilos de mapa
    struct PurposeJob { ObjectGuid guid; bool pvp; uint32 ilvl; bool force; };
    static std::mutex s_purposeLock;
    static std::deque<PurposeJob> s_purposeQueue;

    void QueuePurpose(ObjectGuid guid, bool pvp, uint32 ilvl, bool forceGear)
    {
        std::lock_guard<std::mutex> guard(s_purposeLock);
        for (PurposeJob& j : s_purposeQueue)
            if (j.guid == guid)
            {
                j.pvp = pvp; j.ilvl = ilvl; j.force = forceGear;
                return;
            }
        s_purposeQueue.push_back({ guid, pvp, ilvl, forceGear });
    }

    static uint32 s_loadoutPassMs = 20000;

    void MaintainLoadout(uint32 diff)
    {
        if (!cfg.loadoutEnable)
            return;
        // 1) un trabajo de la cola por tick
        PurposeJob job;
        bool have = false;
        {
            std::lock_guard<std::mutex> guard(s_purposeLock);
            if (!s_purposeQueue.empty())
            {
                job = s_purposeQueue.front();
                s_purposeQueue.pop_front();
                have = true;
            }
        }
        if (have)
            if (Player* bot = ObjectAccessor::FindPlayer(job.guid))
                if (IsBot(bot) && !IsTrainingBot(job.guid))
                    ApplyPurpose(bot, job.pvp, job.ilvl, job.force);

        // 2) cada 30 s, doble spec a unos cuantos bots que no la tengan
        s_loadoutPassMs += diff;
        if (s_loadoutPassMs < 30000)
            return;
        s_loadoutPassMs = 0;
        uint32 done = 0;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            if (done >= cfg.loadoutPerPass)
                break;
            Player* p = pair.second;
            if (!p || !p->IsInWorld() || !p->GetSession() || !IsBot(p) || !sRandomPlayerbotMgr.IsRandomBot(p))
                continue;
            if (p->GetLevel() < sWorld->getIntConfig(CONFIG_MIN_DUALSPEC_LEVEL) || !cfg.arenaSpecs.count(p->getClass()))
                continue;
            if (!p->IsAlive() || p->IsInCombat() || p->InBattleground() || p->IsBeingTeleported() || p->IsInFlight() || p->duel || IsTrainingBot(p->GetGUID()))
                continue;
            if (GameTime::GetGameTime().count() - p->m_logintime < 60)
                continue;
            // Equipo PvP por el mundo por debajo del tope de etapa (llegó así
            // al conectar, o de un duelo): PvE por nivel
            if (RegearPveForWorld(p))
            {
                ++done;
                continue;
            }
            if (p->GetSpecsCount() >= 2)
            {
                BotProfile* prof = FindProfile(p->GetGUID().GetCounter());
                if (prof && prof->specPvp >= 0)
                    continue;
            }
            if (EnsureDualSpec(p))
                ++done;
        }
    }

    std::string LoadoutStatus()
    {
        if (!cfg.loadoutEnable)
            return "loadout: apagado (AdaptiveAI.Loadout.Enable)";
        uint32 bots = 0, dual = 0;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!p || !p->IsInWorld() || !IsBot(p) || p->GetLevel() < sWorld->getIntConfig(CONFIG_MIN_DUALSPEC_LEVEL))
                continue;
            ++bots;
            if (p->GetSpecsCount() >= 2)
                ++dual;
        }
        size_t queued;
        {
            std::lock_guard<std::mutex> guard(s_purposeLock);
            queued = s_purposeQueue.size();
        }
        return Acore::StringFormat("loadout: {} de {} bots (nivel 40+) con doble spec; {} cambios de spec, {} reequipados y {} vueltos a PvE por el mundo desde el arranque; {} en cola",
                                   dual, bots, s_loadoutSwitches, s_loadoutRegears, s_loadoutWorldRegears, queued);
    }

    int TargetFor(uint8 cls, uint8 enemyCls)
    {
        auto it = cfg.matchupTargets.find({ cls, enemyCls });
        if (it != cfg.matchupTargets.end())
            return it->second;
        it = cfg.matchupTargets.find({ enemyCls, cls });
        if (it != cfg.matchupTargets.end())
            return 100 - it->second;
        return -1;
    }

    static std::map<uint8, uint32> s_level80Online;
    static uint32 s_level80Upgraded = 0;
    static std::string s_level80Last;

    // ─── Franjas de nivel de playerbots ─────────────────────────────
    //
    // El gestor de franjas de playerbots reparte la poblacion en nueve grupos de
    // nivel y BAJA DE NIVEL (Randomize completo) a los que sobran en un grupo.
    // Tiene dos repartos: el fijo del .conf (BOTS_LEVEL80_PCT, 36 % a la franja
    // de 80) y el automatico, que acerca la poblacion al nivel de los jugadores
    // conectados. El automatico PISA al fijo: ApplyBracketWeights() hace
    // pct = peso/total*100 y el porcentaje configurado no se usa ni de partida.
    //
    // Sin nadie dentro todos los pesos valen la linea base 1, asi que el
    // automatico reparte a partes iguales: 11 % a cada franja, o sea 28 bots de
    // nivel 80 por faccion contra los 200 que pide el entrenamiento. El
    // resultado era un tira y afloja: el gestor bajandolos y MaintainLevel80()
    // subiendolos, 12 bots rehechos por minuto (03/09/2026).
    //
    // Asi que se conmuta sola: con un jugador dentro manda el automatico, que es
    // cuando de verdad sirve para algo; con el servidor entrenando solo, el
    // reparto fijo. El gestor lee la bandera en cada pasada
    // (LevelBrackets.CheckFrequency, 300 s), de modo que basta con cambiarla.
    //
    // El valor de partida se lee del .conf y no del campo vivo, que es el que
    // nosotros movemos: asi un 'reload config' no puede dejarlo pegado.
    void SyncLevelBrackets()
    {
        if (!cfg.bracketsWithPlayer || !sPlayerbotAIConfig.levelBracketsEnabled)
            return;

        bool humanOnline = false;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (p && p->IsInWorld() && !IsBot(p))
            {
                humanOnline = true;
                break;
            }
        }

        bool base = sConfigMgr->GetOption<bool>("AiPlayerbot.LevelBrackets.Dynamic.UseDynamicDistribution", false);
        bool want = humanOnline || base;
        if (sPlayerbotAIConfig.levelBracketsDynamicDistribution == want)
            return;

        sPlayerbotAIConfig.levelBracketsDynamicDistribution = want;
        LOG_INFO("module", "[adaptive-ai] Franjas de nivel: reparto {} ({}).", want ? "automatico" : "fijo",
                 humanOnline ? "hay un jugador conectado" : "el servidor entrena solo");
    }

    std::string LevelBracketsStatus()
    {
        if (!sPlayerbotAIConfig.levelBracketsEnabled)
            return "franjas de nivel apagadas en playerbots";
        std::string reparto = sPlayerbotAIConfig.levelBracketsDynamicDistribution ? "automatico (sigue al jugador)"
                                                                                 : "fijo (manda BOTS_LEVEL80_PCT)";
        if (!cfg.bracketsWithPlayer)
            return reparto + ", sin conmutar (AdaptiveAI.FranjasConJugador = 0)";
        return reparto + ", se conmuta solo con jugador dentro";
    }

    void MaintainLevel80()
    {
        if (cfg.level80Targets.empty())
            return;
        std::map<uint8, uint32> online;
        std::map<uint8, std::vector<Player*>> candidates;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!p || !p->IsInWorld() || !p->GetSession() || !IsBot(p) || !sRandomPlayerbotMgr.IsRandomBot(p))
                continue;
            uint8 cls = p->getClass();
            if (!cfg.level80Targets.count(cls))
                continue;
            if (p->GetLevel() >= 80)
            {
                ++online[cls];
                continue;
            }
            if (!p->IsAlive() || p->IsInCombat() || p->GetGroup() || p->InBattleground() || p->InBattlegroundQueue() ||
                p->IsBeingTeleported() || p->IsInFlight() || p->duel || p->GetLevel() < 10)
                continue;
            if (GameTime::GetGameTime().count() - p->m_logintime < 60)
                continue;
            if (IsTrainingBot(p->GetGUID()))
                continue;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI || botAI->HasPlayerNearby(150.0f))
                continue;
            candidates[cls].push_back(p);
        }
        s_level80Online = online;
        // La clase a la que más le falta primero (antes iba en orden de clase y
        // los magos y sacerdotes esperaban a que guerreros y pícaros llegaran
        // a su cupo mientras playerbots rotaba fuera a los ya subidos). Con un
        // déficit grande (la oleada de desconexiones de los diez minutos
        // después de un reinicio se lleva 10-15 por minuto) se suben hasta
        // tres por pasada; con déficit pequeño, uno, que la fábrica pesa.
        int32 totalDeficit = 0;
        for (auto const& [cls, target] : cfg.level80Targets)
            totalDeficit += std::max<int32>(0, int32(target) - int32(online[cls]));
        uint32 perPass = totalDeficit >= 40 ? 3 : (totalDeficit >= 15 ? 2 : 1);
        for (uint32 n = 0; n < perPass; ++n)
        {
            uint8 pickCls = 0;
            int32 pickDeficit = 0;
            for (auto const& [cls, target] : cfg.level80Targets)
            {
                int32 deficit = int32(target) - int32(online[cls]);
                if (deficit > pickDeficit && !candidates[cls].empty())
                {
                    pickCls = cls;
                    pickDeficit = deficit;
                }
            }
            if (!pickCls)
                break;
            uint8 cls = pickCls;
            uint32 target = cfg.level80Targets.find(cls)->second;
            size_t idx = urand(0, uint32(candidates[cls].size() - 1));
            Player* bot = candidates[cls][idx];
            candidates[cls].erase(candidates[cls].begin() + idx);
            ++online[cls];   // cuenta para el siguiente de la pasada
            uint8 before = bot->GetLevel();
            PlayerbotFactory factory(bot, 80);
            factory.Randomize(false);
            {
                BotProfile& prof = GetProfile(bot);
                prof.specPve = -1;   // playerbots acaba de elegirle spec: se vuelve a leer
                prof.specPvp = -1;
                EnsureDualSpec(bot);
            }
            ++s_level80Upgraded;
            s_level80Last = Acore::StringFormat("{} ({}) de {} a {}", bot->GetName(), ClassName(cls), before, bot->GetLevel());
            LOG_INFO("module", "[adaptive-ai] Nivel 80: {} ({} conectados de {} pedidos).", s_level80Last, online[cls], target);
        }
    }

    std::string Level80Status()
    {
        if (cfg.level80Targets.empty())
            return "bots de nivel 80 por clase: no se mantienen (AdaptiveAI.Bots.Nivel80)";
        std::string out;
        for (auto const& [cls, target] : cfg.level80Targets)
            out += Acore::StringFormat("{}{} {}/{}", out.empty() ? "" : ", ", ClassName(cls), s_level80Online[cls], target);
        return Acore::StringFormat("bots de nivel 80 conectados: {} ({} subidos desde el arranque{})", out, s_level80Upgraded,
                                   s_level80Last.empty() ? "" : "; ultimo " + s_level80Last);
    }

    // ─── La escalera en el mundo ─────────────────────────────────────────────
    // Con que modelo sale un bot a un combate real. La serie es el peldano 0 y
    // el suelo: si en esa clase es lo mas fuerte que hay medido, el bot juega
    // de serie y el modulo no se mete. Nunca se sale del par serie/validada sin
    // numeros detras.
    static float BotRungRating(Player* bot)
    {
        if (!bot)
            return 1500.0f;
        uint32 version = 0;
        if (std::shared_ptr<Brain> brain = FindBrain(bot->GetGUID()))
        {
            std::lock_guard<std::recursive_mutex> guard(brain->lock);
            if (brain->table)
                version = brain->table->Version();
        }
        return LadderRating(bot->getClass(), version);
    }

    static BrainMode ChooseWorldBrain(Player* bot, Player* enemy, std::shared_ptr<QTable>& table, uint8& difficulty)
    {
        difficulty = 0;   // 0 = la del perfil del bot
        std::shared_ptr<QTable> validated = Validated();
        if (cfg.realMode == "validada")
        {
            // Sin filas de esta clase, la validada no es un modelo: es la serie (N1)
            if (validated && !validated->HasClass(bot->getClass()))
                return BRAIN_NONE;
            return BRAIN_VALIDATED;
        }
        std::vector<LadderRung> ladder = LadderFor(bot->getClass());
        // La candidata se mide en la escalera (modo "sonda") pero nunca sale al
        // mundo: lo que entrena no es lo que ven los jugadores.
        if (std::shared_ptr<QTable> c = Candidate())
            ladder.erase(std::remove_if(ladder.begin(), ladder.end(), [&](LadderRung const& r) { return r.version == c->Version(); }), ladder.end());
        if (ladder.empty())
            return validated && validated->HasClass(bot->getClass()) ? BRAIN_VALIDATED : BRAIN_NONE;

        // El centro: el peldano mas alto medido, o el mas cercano al rating del
        // jugador si se juega en espejo.
        size_t center = 0;
        float playerRating = 0.0f;
        bool mirror = cfg.realMode == "espejo" && enemy && !IsBot(enemy);
        if (mirror)
        {
            playerRating = PlayerRating(enemy->GetGUID().GetCounter());
            float best = 0.0f;
            for (size_t i = 0; i < ladder.size(); ++i)
            {
                float distance = std::fabs(ladder[i].rating - playerRating);
                if (!i || distance < best)
                {
                    best = distance;
                    center = i;
                }
            }
        }

        // Desvio estable por bot: el mundo tiene variedad (unos por encima del
        // jugador y otros por debajo) sin que el mismo bot cambie de nivel de
        // una pelea a la siguiente, que se leeria como que la IA falla.
        BotProfile& p = GetProfile(bot);
        uint32 now = uint32(GameTime::GetGameTime().count());
        if (cfg.mirrorSpread && (!p.rungAt || now - p.rungAt > cfg.mirrorHours * 3600))
        {
            p.rungOffset = int8(irand(-int32(cfg.mirrorSpread), int32(cfg.mirrorSpread)));
            p.rungAt = now;
            SaveProfile(p);
        }
        int32 index = std::clamp<int32>(int32(center) + p.rungOffset, 0, int32(ladder.size()) - 1);
        LadderRung const& rung = ladder[size_t(index)];

        if (mirror)
        {
            // La dificultad remata el ajuste fino: con un peldano mas fuerte
            // que el jugador el bot comete mas errores, y cada error lo acerca
            // a playerbots de serie; con uno mas flojo, juega a tope.
            float gap = (playerRating - rung.rating) / 100.0f;
            difficulty = uint8(std::clamp<int32>(3 + int32(std::lround(gap)), 1, 6));
        }
        if (!rung.version)
            return BRAIN_NONE;   // la serie es el peldano mas alto de esta clase
        // Un peldano sin filas de esta clase no es un modelo para ella: se juega
        // de serie, no con la personalidad del bot (N1).
        if (validated && rung.version == validated->Version())
            return validated->HasClass(bot->getClass()) ? BRAIN_VALIDATED : BRAIN_NONE;
        table = GenerationTable(rung.version);
        if (!table)
            return validated && validated->HasClass(bot->getClass()) ? BRAIN_VALIDATED : BRAIN_NONE;
        return table->HasClass(bot->getClass()) ? BRAIN_VERSION : BRAIN_NONE;
    }

    class mod_adaptive_ai_player : public PlayerScript
    {
    public:
        mod_adaptive_ai_player() : PlayerScript("mod_adaptive_ai_player",
            { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_PVP_KILL, PLAYERHOOK_ON_SPELL_CAST,
              PLAYERHOOK_ON_PLAYER_ENTER_COMBAT, PLAYERHOOK_ON_PLAYER_LEAVE_COMBAT, PLAYERHOOK_ON_LOGOUT,
              PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_DUEL_START }) { }

        // Loadout: al cambiar de mapa se decide el propósito. Arena o campo de
        // batalla = PvP con el equipo de su rating; mazmorra o banda = PvE con
        // el equipo del contenido (si va con un jugador, el equipo lo topa
        // mod-queue-bots); mundo = PvE con lo que traiga (o Loadout.EquipoPvE mundo).
        void OnPlayerMapChanged(Player* player) override
        {
            if (!cfg.loadoutEnable || !player || !IsBot(player) || !sRandomPlayerbotMgr.IsRandomBot(player))
                return;
            if (IsTrainingBot(player->GetGUID()))
                return;   // la arena de entrenamiento ya lo ha equipado
            Map* map = player->GetMap();
            if (!map)
                return;
            if (map->IsBattlegroundOrArena())
            {
                BotProfile& p = GetProfile(player);
                QueuePurpose(player->GetGUID(), true, PvpIlvlFor(p.rating), false);
                return;
            }
            // Al tope de etapa (80), un bot que vuelve de la arena al mundo se
            // queda con la spec y el equipo de PvP (05/09/2026): iba a volver a
            // la arena en un minuto, y cada ida y vuelta era un cambio de spec y
            // una regeneracion entera del equipo en el hilo del mundo, unas
            // 10.000 por hora con 120 arenas. Por debajo del tope sigue igual
            // (equipo PvE por nivel para el mundo).
            if (!map->IsDungeon() && AtStageCap(player))
            {
                BotProfile& p = GetProfile(player);
                if (p.purpose == "pvp")
                    return;
            }
            uint32 ilvl = cfg.loadoutPveWorld;
            if (map->IsDungeon())
            {
                bool withHuman = false;
                if (Group* g = player->GetGroup())
                    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                        if (Player* m = ref->GetSource())
                            if (!IsBot(m))
                                withHuman = true;
                if (withHuman)
                    ilvl = 0;   // BotGear (mod-queue-bots) lo topa a la fase del jugador
                else if (map->GetEntry() && map->GetEntry()->Expansion() >= 2)
                    ilvl = map->IsRaid() ? (map->Is25ManRaid() ? cfg.loadoutPveRaid25 : cfg.loadoutPveRaid10)
                                         : (map->IsHeroic() ? cfg.loadoutPveHeroic : cfg.loadoutPveNormal);
                else
                    ilvl = 0;
            }
            QueuePurpose(player->GetGUID(), false, ilvl, false);
        }

        void OnPlayerDuelStart(Player* player1, Player* player2) override
        {
            if (!cfg.loadoutEnable)
                return;
            for (Player* p : { player1, player2 })
                if (p && IsBot(p) && sRandomPlayerbotMgr.IsRandomBot(p) && !IsTrainingBot(p->GetGUID()))
                    QueuePurpose(p->GetGUID(), true, 0, false);
        }

        void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
        {
            if (!ActiveBrains() || !cfg.enabled)
                return;
            BrainTick(player);
        }

        void OnPlayerPVPKill(Player* killer, Player* killed) override
        {
            if (!killer || killer == killed)
                return;
            // Rating del jugador: sube cuando mata a un bot y baja cuando un bot
            // lo mata, con la fuerza del peldano del bot como rival. Es el
            // centro de la escalera en el modo espejo.
            if (killed && cfg.realMode == "espejo")
            {
                if (!IsBot(killer) && IsBot(killed))
                    PlayerRatingUpdate(killer, BotRungRating(killed), 1.0f);
                else if (IsBot(killer) && !IsBot(killed))
                    PlayerRatingUpdate(killed, BotRungRating(killer), 0.0f);
            }
            // El premio por matar ya NO se da aquí: este enganche sólo salta
            // cuando el golpe final lo da el jugador en persona, y además llega
            // después de que la arena haya terminado y borrado los cerebros, así
            // que en 1c1 no llegaba nunca. Vive en OnUnitDeath (auditoría del
            // 04/09/2026). Aquí se queda sólo el rating del jugador.
        }

        void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
        {
            if (!ActiveBrains() || !spell || !spell->GetSpellInfo())
                return;
            if (!spell->GetSpellInfo()->HasEffect(SPELL_EFFECT_INTERRUPT_CAST))
                return;
            std::shared_ptr<Brain> brain = FindBrain(player->GetGUID());
            if (!brain)
                return;
            Unit* target = spell->m_targets.GetUnitTarget();
            bool casting = target && target->IsNonMeleeSpellCast(false, false, true);
            std::lock_guard<std::recursive_mutex> guard(brain->lock);
            if (casting)
            {
                float const given = ShapedAmount(*brain, cfg.rInterruptOk);
                brain->pendingReward += given;
                brain->totalReward += given;
                AuditBrain(*brain, "reward", given, __LINE__);
                ++brain->interruptsOk;
            }
            else
            {
                float const given = ShapedAmount(*brain, cfg.rInterruptBad);
                brain->pendingReward += given;
                brain->totalReward += given;
                AuditBrain(*brain, "reward", given, __LINE__);
                ++brain->interruptsBad;
            }
        }

        // Fuente 1: combate real (bot contra jugador o contra otro bot, fuera de
        // las arenas de entrenamiento). Con que modelo sale al mundo lo decide
        // la escalera (AdaptiveAI.Real.Modo): puede ser la validada, una
        // generacion anterior o playerbots de serie, segun lo medido en esa
        // clase.
        void OnPlayerEnterCombat(Player* player, Unit* enemy) override
        {
            if (!cfg.enabled || !cfg.realEnable || !player || !enemy)
                return;
            if (!IsBot(player) || IsTrainingBot(player->GetGUID()))
                return;
            Player* ep = PlayerOf(enemy);
            if (!ep || ep == player)
                return;
            if (ActionsFor(player->getClass()).size() <= 1 || !ClassEnabled(player->getClass()))
                return;   // clase sin catálogo o desactivada en AdaptiveAI.Clases
            if (FindBrain(player->GetGUID()))
                return;
            std::shared_ptr<QTable> table;
            uint8 difficulty = 0;
            BrainMode mode = ChooseWorldBrain(player, ep, table, difficulty);
            if (mode == BRAIN_NONE)
                return;   // en esta clase la serie es lo mejor medido: no se toca nada
            std::shared_ptr<Brain> brain = StartBrain(player, ep, mode, cfg.learn && !cfg.learnOnlyArena, 0.0f, 0, table);
            if (brain && difficulty)
            {
                std::lock_guard<std::recursive_mutex> guard(brain->lock);
                brain->difficulty = cfg.difficulty[std::clamp<uint8>(difficulty, 1, 6)];
            }
        }

        void OnPlayerLeaveCombat(Player* player) override
        {
            if (!ActiveBrains() || !player)
                return;
            std::shared_ptr<Brain> brain = FindBrain(player->GetGUID());
            if (brain && !brain->matchId)
                EndBrain(player->GetGUID(), 0.0f);
        }

        void OnPlayerLogout(Player* player) override
        {
            if (ActiveBrains() && player)
                EndBrain(player->GetGUID(), 0.0f);
        }
    };

    class mod_adaptive_ai_world : public WorldScript
    {
    public:
        mod_adaptive_ai_world() : WorldScript("mod_adaptive_ai_world",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

        void OnAfterConfigLoad(bool reload) override
        {
            LoadConfig();
            if (reload)
                LOG_INFO("module", "[adaptive-ai] Configuracion recargada.");
        }

        void OnStartup() override
        {
#ifdef ADAPTIVE_WITH_PLAYERBOTS
            EnsureColumns();
            LoadModels();
            ArenaInit();
            PurgeOldData();
            LOG_INFO("module", "[adaptive-ai] Activo: decisor {}, aprendizaje {}, arena {} ({} simultaneas, pares: {}).",
                     cfg.enabled ? "si" : "no", cfg.learn ? "si" : "no", cfg.arenaEnable ? "si" : "no",
                     cfg.arenaSimultaneous, [] { std::string s; for (CompSpec const& p : cfg.arenaPairs) s += (s.empty() ? "" : ",") + p.Name(); return s; }());
            LOG_INFO("module", "[adaptive-ai] Clases: {}; tipos de arena: {}; campos de batalla: {}.",
                     cfg.classes.empty() ? std::string("todas") : [] { std::string s; for (uint8 c : cfg.classes) s += (s.empty() ? "" : ",") + std::string(ClassName(c)); return s; }(),
                     [] { std::string s; for (uint8 t : cfg.arenaTypes) s += (s.empty() ? "" : ",") + std::to_string(t) + "c" + std::to_string(t); return s; }(),
                     cfg.bgEnable ? [] { std::string s; for (uint8 t : cfg.bgTypes) s += (s.empty() ? "" : ",") + std::string(BgName(t)); return s + " cada " + std::to_string(cfg.bgEveryMinutes) + " min"; }() : std::string("no"));
#else
            LOG_INFO("module", "[adaptive-ai] mod-playerbots no esta: el modulo no hace nada.");
#endif
        }

        void OnUpdate(uint32 diff) override
        {
#ifdef ADAPTIVE_WITH_PLAYERBOTS
            uint32 t0 = getMSTime();
            ArenaTick(diff);
            WarnIfSlow("la arena (ArenaTick)", t0);
            // La tabla Q se guarda por tramos: hasta 500 filas por tabla y
            // segundo (05/09/2026; medido: unos 200 cambios por segundo entre
            // las dos, asi que va holgado). Antes iba todo lo cambiado de golpe
            // cada minuto; no esta demostrado que ese bloque fuera el tick de
            // 100-180 ms que ensena el core, que siguio tras el cambio.
            _saveTimer += diff;
            if (_saveTimer >= 1000)
            {
                _saveTimer = 0;
                t0 = getMSTime();
                SaveAllDirty(500);
                WarnIfSlow("el guardado de la tabla Q", t0);
            }
            _purgeTimer += diff;
            if (_purgeTimer >= 24 * 60 * 60 * 1000)
            {
                _purgeTimer = 0;
                t0 = getMSTime();
                PurgeOldData();
                WarnIfSlow("la limpieza diaria", t0);
            }
            _level80Timer += diff;
            if (_level80Timer >= 10000)   // uno cada 10 s: playerbots rota fuera a los subidos (MinRandomBotInWorldTime 600 s)
            {
                _level80Timer = 0;
                t0 = getMSTime();
                MaintainLevel80();
                WarnIfSlow("la subida a 80", t0);
            }
            _bracketsTimer += diff;
            if (_bracketsTimer >= 30000)   // cada 30 s: el gestor de franjas solo mira su bandera cada 5 min
            {
                _bracketsTimer = 0;
                t0 = getMSTime();
                SyncLevelBrackets();
                WarnIfSlow("las franjas de nivel", t0);
            }
            t0 = getMSTime();
            MaintainLoadout(diff);
            WarnIfSlow("el loadout", t0);
#else
            (void)diff;
#endif
        }

        void OnShutdown() override
        {
            ArenaShutdown();
            SaveAllDirty();
        }

    private:
        uint32 _saveTimer = 0;
        uint32 _purgeTimer = 0;
        uint32 _level80Timer = 25000;   // la primera pasada, a los 5 s
        uint32 _bracketsTimer = 25000;  // idem: conmutar el reparto de franjas nada mas arrancar
    };
}

void AddSC_mod_adaptive_ai()
{
    new AdaptiveAI::mod_adaptive_ai_unit();
    new AdaptiveAI::mod_adaptive_ai_player();
    new AdaptiveAI::mod_adaptive_ai_world();
}
