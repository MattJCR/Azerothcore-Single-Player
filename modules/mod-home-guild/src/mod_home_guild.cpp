// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-home-guild — tu hermandad.
 *
 * EL PROBLEMA
 * Cada grupo es de desconocidos: otra mazmorra, otros nombres. En un servidor
 * de verdad tienes una hermandad, y con ella haces las bandas semana tras
 * semana. Las hermandades de bots de playerbots existen y te invitan, pero no
 * son tuyas, no se usan para formar grupos y sus miembros no siguen tu nivel.
 *
 * LA SOLUCIÓN
 * El módulo NO crea hermandades. Cuando un jugador funda una mediante el flujo
 * normal del núcleo, GuildScript::OnCreate la marca en la base de datos como
 * su hermandad de casa (si HomeGuild.AutoAdopt = 1, el defecto; con 0 se marca
 * a mano con ".hermandad activar"). ".hermandad estado" muestra el roster y
 * ".hermandad desactivar" expulsa a los bots sin disolver la hermandad. Sólo
 * esa hermandad, y sólo mientras siga liderándola su fundador, se rellena con
 * bots aleatorios de su facción y tramo de nivel (Guild::AddMember funciona
 * con el bot desconectado). Si el fundador deja de entrar durante
 * HomeGuild.InactiveOwnerReclaimDays días, sus bots se liberan al pool. Sus
 * miembros:
 *
 *   - se conectan contigo y se mantienen conectados mientras estés dentro
 *     (AddPlayerBot, como hacen queue-bots y world-bots al despertar);
 *   - no se mandan nunca a dormir para hacer sitio: los demás módulos lo
 *     consultan en el registro compartido (BotClaims::IsHomeGuildBot);
 *   - van los primeros cuando mod-queue-bots o mod-party-here forman un grupo;
 *   - se suben de nivel cuando se quedan atrás, con la fábrica de playerbots.
 *
 * Playerbots no les toca el nivel por su cuenta: LevelBrackets ignora por
 * defecto a las hermandades lideradas por un humano
 * (AiPlayerbot.LevelBrackets.IgnoreGuildBotsWithRealPlayers = 1), y el gestor
 * respeta la hermandad de un bot que ya tiene una (PlayerbotFactory::InitGuild
 * sale si GetGuildId() != 0). Lo único que hace el gestor es desconectarlos
 * cuando caduca su turno, y aquí se vuelven a conectar.
 *
 * DÓNDE SE HACE EL TRABAJO
 * Los hooks de jugador y hermandad sólo apuntan eventos; todo lo demás se hace
 * en WorldScript::OnUpdate. Al primer arranque de esta versión se disuelven,
 * una sola vez y con una firma conservadora, las hermandades que creó
 * automáticamente la versión antigua.
 *
 * DEPENDENCIA CON PLAYERBOTS (entre guardas)
 * IsRandomBot (sólo bots aleatorios), AddPlayerBot/LogoutPlayerBot (conectar
 * y hacer sitio) y PlayerbotFactory::Randomize para subirles el nivel: es lo
 * que hace el comando "rndbot init". Sin playerbots, compila y no hace nada.
 */

#include "BotClaims.h"
#include "BotEligibility.h"
#include "BotGear.h"
#include "BotOperations.h"
#include "BotPopulationCoordinator.h"
#include "BotWorldAge.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CharacterCache.h"
#include "CommandScript.h"
#include "Optional.h"
#include "Config.h"
#include "Containers.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SlowTick.h"
#include "TimeMs.h"
#include "Timer.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__has_include)
#  if __has_include("Playerbots.h") && __has_include("PlayerbotAI.h") && \
      __has_include("PlayerbotAIConfig.h") && __has_include("PlayerbotFactory.h")
#    include "PlayerbotAI.h"
#    include "PlayerbotAIConfig.h"
#    include "PlayerbotFactory.h"
#    include "Playerbots.h"
#    include "RandomPlayerbotMgr.h"
#    define HOME_GUILD_WITH_PLAYERBOTS 1
#  endif
#endif

namespace
{
    struct Config
    {
        bool        enabled          = true;
        std::string legacyName       = "Companeros de {name}";
        std::string legacyMotd       = "Bienvenido a casa.";
        bool        cleanupLegacy    = true;
        uint32      members          = 15;
        uint32      levelBelow       = 3;
        uint32      levelAbove       = 2;
        bool        keepOnline       = true;
        bool        reLevel          = true;
        uint32      reLevelBehind    = 4;
        bool        announce         = true;
        bool        announceWhenGuildless = true; // aviso "monta una hermandad" al entrar sin guild
        bool        autoAdopt        = true;   // fundar una guild la hace "de casa"
        uint32      inactiveReclaimDays = 30;  // owner sin entrar N dias -> liberar bots (0 = nunca)
        uint32      careIntervalSecs  = 30;    // cada cuanto se cuida cada hermandad
        uint32      recruitBackoffSecs = 300;  // espera si la hermandad ya esta llena
        uint32      keepOnlineIntervalSecs = 15; // cada cuanto se despiertan bots de la hermandad
        uint32      keepOnlineBatch   = 5;     // bots conectados por pasada
        uint32      reLevelPerPass    = 1;     // bots re-nivelados por pasada
        bool        recruitFromOfflinePool = true; // paso 2 de Recruit (tabla characters)
        uint32      maxOnlineBots     = 0;     // tope propio; 0 = AiPlayerbot.MaxRandomBots
        std::string allianceRaces     = "1,3,4,7,11";
        std::string hordeRaces        = "2,5,6,8,10";
        uint32      legacyNumberMax   = 100;   // "<100" en el nombre antiguo "Companeros N"
        uint32      reLevelTargetJitter = 0;   // 0 = usar LevelBelow como jitter
        uint32      reLevelMinWorldSecs = 60;  // asentamiento mínimo en el mundo antes de renivelar (M04)

        // Roster con roles equilibrados (A3/A4): objetivo de tanques y sanadores
        // entre los Members. 0 = según el tamaño.
        uint32      tanks             = 0;
        uint32      healers           = 0;

        // Equipo de los bots de casa a la fase del owner, aunque no estén en su
        // grupo, para que estén siempre listos (mejora 7). GearMode 0 = no tocar.
        BotGear::Settings gear;
        uint32      gearIntervalSecs  = 120;
    };

    Config cfg;

    // Espejos atómicos de los dos campos que leen un GuildScript y un
    // CommandScript (T7). ".reload config" corre en el hilo del mundo con los
    // mapas parados, igual que OnUpdate y los comandos, así que la limpieza de
    // g_homes/g_managedGuilds al recargar tampoco compite con nadie
    // (modules/README.md, regla 5; M05, 24/09/2026).
    std::atomic<bool> g_enabledHot{true};
    std::atomic<bool> g_autoAdoptHot{true};

    struct RecruitCountCache
    {
        uint32 total = 0;
        uint64_t expiresAt = 0;
    };

    // Acotado por diseno: la clave es (minLevel:maxLevel:races) y los tramos
    // salen del nivel del owner (1-80) con LevelBelow/Above fijos -> ~160
    // entradas como mucho, de pocos bytes. No necesita poda.
    std::unordered_map<std::string, RecruitCountCache> g_recruitCountCache;

    uint32 CachedRecruitCount(uint32 minLevel, uint32 maxLevel, std::string const& races)
    {
        std::string const key = std::to_string(minLevel) + ':' + std::to_string(maxLevel) + ':' + races;
        uint64_t const now = TimeMs::NowMs();
        auto const cached = g_recruitCountCache.find(key);
        if (cached != g_recruitCountCache.end() && now < cached->second.expiresAt)
            return cached->second.total;

        QueryResult result = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM characters c LEFT JOIN guild_member gm ON gm.guid = c.guid "
            "WHERE gm.guid IS NULL AND c.online = 0 AND c.level BETWEEN {} AND {} AND c.race IN ({})",
            minLevel, maxLevel, races);
        uint32 const total = result ? (*result)[0].Get<uint32>() : 0;
        g_recruitCountCache[key] = { total, now + TimeMs::SecsToMs(60) };
        return total;
    }

    enum EventKind : uint8
    {
        EVENT_LOGIN,
        EVENT_GUILD_CREATED,
        EVENT_GUILD_DISBANDED,
        EVENT_CMD_STATUS,      // ".hermandad estado"
        EVENT_CMD_ACTIVATE,    // ".hermandad activar"
        EVENT_CMD_DEACTIVATE,  // ".hermandad desactivar"
        EVENT_CMD_PIN,         // ".hermandad fijar <nombre>"
        EVENT_CMD_UNPIN,       // ".hermandad soltar <nombre>"
        EVENT_CMD_EXCLUDE,     // ".hermandad excluir <nombre>"
        EVENT_CMD_RENEW        // ".hermandad renovar"
    };

    struct PendingEvent
    {
        EventKind   kind      = EVENT_LOGIN;
        ObjectGuid  player;
        uint32      guildId   = 0;
        uint32      accountId = 0;
        std::string name = {};            // nombre de un bot (comandos por bot)
    };

    std::mutex        g_pendingLock;
    std::deque<PendingEvent> g_pending;

    struct ManagedGuild
    {
        uint32 ownerAccount = 0;
        uint32 ownerGuid    = 0;
    };

    // Sólo lo modifica mod_home_guild_world. GuildScript y PlayerScript
    // producen PendingEvent en g_pending y no tocan este estado.
    std::unordered_map<uint32, ManagedGuild> g_managedGuilds;

    // Roster propio: rol conocido y marcas por bot (tabla mod_home_guild_member).
    struct MemberInfo
    {
        uint8 role     = 0;   // 0 desconocido, 1 tanque, 2 sanador, 3 dano
        bool  pinned   = false;
        bool  excluded = false;
    };

    // Lo que se sabe de cada jugador conectado y su hermandad.
    struct Home
    {
        uint32                                   guildId   = 0;
        std::vector<ObjectGuid>                  bots;
        uint64                                   nextCareMs = 0;
        uint64                                   nextWakeMs = 0;
        uint64                                   nextGearMs = 0;
        size_t                                   wakeCursor = 0; // posición del roster por la que sigue el turno (M11)
        std::unordered_map<uint32, MemberInfo>   members;   // botLow -> info del roster
    };

    // Sólo lo modifica mod_home_guild_world.
    std::unordered_map<ObjectGuid, Home> g_homes;

    uint64 g_nextReclaimMs = 0;   // proxima pasada de ReclaimInactiveGuilds

    using BotEligibility::IsHuman;

    bool IsRandomBotGuid(uint32 lowGuid)
    {
#ifdef HOME_GUILD_WITH_PLAYERBOTS
        return sRandomPlayerbotMgr.IsRandomBot(lowGuid);
#else
        (void)lowGuid;
        return false;
#endif
    }

    bool IsRandomBotAccount(uint32 accountId)
    {
#ifdef HOME_GUILD_WITH_PLAYERBOTS
        if (sPlayerbotAIConfig.IsInRandomAccountList(accountId))
            return true;
#endif

        // Puede haber cuentas de bots que ya no estén dentro del límite
        // RandomBotAccountCount actual. La versión antigua pudo reclutarlas
        // cuando ese límite era mayor, así que también reconocemos el prefijo
        // reservado de Playerbots seguido exclusivamente por dígitos.
        QueryResult result = LoginDatabase.Query("SELECT username FROM account WHERE id = {}", accountId);
        if (!result)
            return false;

        std::string const username = (*result)[0].Get<std::string>();
        std::string const prefix = sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot");
        if (prefix.empty() || username.size() <= prefix.size())
            return false;

        for (size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(username[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;

        return std::all_of(username.begin() + prefix.size(), username.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    void Push(PendingEvent event)
    {
        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending.push_back(event);
    }

    std::string ReplaceAll(std::string text, std::string const& what, std::string const& with)
    {
        size_t pos = 0;
        while ((pos = text.find(what, pos)) != std::string::npos)
        {
            text.replace(pos, what.size(), with);
            pos += with.size();
        }
        return text;
    }

    // Los miembros bot de una hermandad, según la tabla (vale para desconectados).
    std::vector<ObjectGuid> LoadBotMembers(uint32 guildId)
    {
        std::vector<ObjectGuid> bots;
        QueryResult result = CharacterDatabase.Query("SELECT guid FROM guild_member WHERE guildid = {}", guildId);
        if (!result)
            return bots;
        do
        {
            uint32 const lowGuid = (*result)[0].Get<uint32>();
            if (IsRandomBotGuid(lowGuid))
                bots.push_back(ObjectGuid::Create<HighGuid::Player>(lowGuid));
        } while (result->NextRow());
        return bots;
    }

    // Rol de un bot, con el criterio de playerbots (como en queue-bots/party-here).
    uint8 BotRole(Player* bot)
    {
#ifdef HOME_GUILD_WITH_PLAYERBOTS
        if (PlayerbotAI::IsTank(bot, true))
            return 1;
        if (PlayerbotAI::IsHeal(bot, true))
            return 2;
        return 3;
#else
        (void)bot;
        return 3;
#endif
    }

    char const* RoleName(uint8 role)
    {
        switch (role)
        {
            case 1:  return "tanque";
            case 2:  return "sanador";
            case 3:  return "dano";
            default: return "?";
        }
    }

    // Carga el roster propio (rol, fijado, excluido) de mod_home_guild_member.
    void LoadRoster(uint32 guildId, std::unordered_map<uint32, MemberInfo>& out)
    {
        out.clear();
        QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, role, pinned, excluded FROM mod_home_guild_member WHERE guild_id = {}", guildId);
        if (!result)
            return;
        do
        {
            MemberInfo info;
            info.role     = (*result)[1].Get<uint8>();
            info.pinned   = (*result)[2].Get<uint8>() != 0;
            info.excluded = (*result)[3].Get<uint8>() != 0;
            out[(*result)[0].Get<uint32>()] = info;
        } while (result->NextRow());
    }

    void PublishHomeBots()
    {
        std::unordered_set<uint32_t> all;
        for (auto const& pair : g_homes)
            for (ObjectGuid const& bot : pair.second.bots)
                all.insert(bot.GetCounter());
        BotClaims::SetHomeGuildBots(all);
    }

    bool MatchesLegacyName(std::string const& guildName, std::string const& leaderName)
    {
        std::string const base = ReplaceAll(cfg.legacyName, "{name}", leaderName);
        if (guildName == base)
            return true;
        if (guildName.size() <= base.size() + 1 || guildName.compare(0, base.size(), base) != 0 || guildName[base.size()] != ' ')
            return false;

        std::string const suffix = guildName.substr(base.size() + 1);
        uint32 number = 0;
        for (char c : suffix)
        {
            if (c < '0' || c > '9')
                return false;
            number = number * 10 + static_cast<uint32>(c - '0');
            if (number >= cfg.legacyNumberMax)
                return false;
        }
        return number >= 2;
    }

    bool MatchesLegacyMotd(std::string const& motd)
    {
        // El fallback del código antiguo era el texto corto, mientras el
        // instalador escribía el largo. Aceptar ambos evita que el orden entre
        // instalar el .conf y arrancar el worldserver cambie la migración.
        return motd == cfg.legacyMotd || motd == "Bienvenido a casa." ||
               motd == "Bienvenido a casa. Escribe .grupo para salir de mazmorra con nosotros.";
    }

    // La versión antigua sólo añadía bots y personajes de la cuenta del líder.
    // Esta comprobación evita disolver por nombre una hermandad real compartida.
    bool HasOnlyLegacyMembers(uint32 guildId, uint32 ownerAccount)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT c.guid, c.account FROM guild_member gm "
            "JOIN characters c ON c.guid = gm.guid WHERE gm.guildid = {}", guildId);
        if (!result)
            return false;
        do
        {
            uint32 const account = (*result)[1].Get<uint32>();
            if (!IsRandomBotAccount(account) && account != ownerAccount)
                return false;
        } while (result->NextRow());
        return true;
    }

    void LoadManagedGuilds()
    {
        g_managedGuilds.clear();
        QueryResult result = CharacterDatabase.Query(
            "SELECT mh.guild_id, mh.owner_account, mh.owner_guid FROM mod_home_guild mh "
            "JOIN guild g ON g.guildid = mh.guild_id AND g.leaderguid = mh.owner_guid");
        if (result)
        {
            do
            {
                g_managedGuilds[(*result)[0].Get<uint32>()] =
                    { (*result)[1].Get<uint32>(), (*result)[2].Get<uint32>() };
            } while (result->NextRow());
        }

        // Una guild disuelta o que cambió de líder deja de pertenecer al módulo.
        // Async: es una limpieza, el SELECT de arriba ya filtró lo válido (B10).
        CharacterDatabase.Execute(
            "DELETE mh FROM mod_home_guild mh LEFT JOIN guild g ON g.guildid = mh.guild_id "
            "WHERE g.guildid IS NULL OR g.leaderguid <> mh.owner_guid");
        LOG_INFO("module", "[home-guild] {} hermandades de creación manual bajo gestión.", g_managedGuilds.size());
    }

    void CleanupLegacyGuilds()
    {
        if (!cfg.cleanupLegacy || CharacterDatabase.Query(
                "SELECT 1 FROM mod_home_guild_meta WHERE meta_key = 'legacy_cleanup_v2' LIMIT 1"))
            return;

        QueryResult result = CharacterDatabase.Query(
            "SELECT g.guildid, g.name, g.leaderguid, g.motd, c.name, c.account "
            "FROM guild g JOIN characters c ON c.guid = g.leaderguid");
        bool complete = true;
        uint32 removed = 0;

        if (result)
        {
            do
            {
                uint32 const guildId = (*result)[0].Get<uint32>();
                std::string const guildName = (*result)[1].Get<std::string>();
                uint32 const leaderGuid = (*result)[2].Get<uint32>();
                std::string const motd = (*result)[3].Get<std::string>();
                std::string const leaderName = (*result)[4].Get<std::string>();
                uint32 const leaderAccount = (*result)[5].Get<uint32>();

                if (g_managedGuilds.count(guildId) || IsRandomBotAccount(leaderAccount) ||
                    !MatchesLegacyName(guildName, leaderName))
                    continue;

                if (!MatchesLegacyMotd(motd) || !HasOnlyLegacyMembers(guildId, leaderAccount))
                {
                    LOG_WARN("module", "[home-guild] '{}' (id {}) parece una hermandad antigua por el nombre, "
                              "pero no coincide toda la firma: no se disuelve.", guildName, guildId);
                    continue;
                }

                Guild* guild = sGuildMgr->GetGuildById(guildId);
                if (!guild || guild->GetLeaderGUID().GetCounter() != leaderGuid)
                {
                    complete = false;
                    LOG_ERROR("module", "[home-guild] No se pudo cargar la hermandad antigua '{}' (id {}): "
                               "la limpieza se reintentará en el próximo arranque.", guildName, guildId);
                    continue;
                }

                LOG_WARN("module", "[home-guild] Se disuelve la hermandad automática antigua '{}' (id {}, líder {}).",
                         guildName, guildId, leaderName);
                // Disband() no libera el objeto; el core hace este mismo
                // 'delete guild' tras Disband() en cs_guild.cpp. Frágil ante un
                // cambio de GuildMgr::RemoveGuild río arriba, pero hoy correcto
                // (B12).
                guild->Disband();
                delete guild;
                ++removed;
            } while (result->NextRow());
        }

        if (!complete)
            return;

        CharacterDatabase.DirectExecute(
            "INSERT INTO mod_home_guild_meta (meta_key, meta_value) VALUES ('legacy_cleanup_v2', '1') "
            "ON DUPLICATE KEY UPDATE meta_value = VALUES(meta_value)");
        LOG_INFO("module", "[home-guild] Limpieza única terminada: {} hermandades automáticas antiguas disueltas.", removed);
    }

    // un owner que deja de jugar deja sus bots congelados en una guild
    // difunta -playerbots no los toca (InitGuild sale con GetGuildId()!=0) y
    // este modulo solo los cuida con el owner dentro-, y quedan fuera del pool
    // de bots libres para siempre. Si el owner lleva sin entrar mas de
    // cfg.inactiveReclaimDays y NO esta conectado ahora, se les saca de la guild
    // y se liberan.
    void ReclaimInactiveGuilds()
    {
        if (!cfg.inactiveReclaimDays)
            return;

        QueryResult rows = CharacterDatabase.Query(
            "SELECT guild_id, owner_account, owner_guid FROM mod_home_guild");
        if (!rows)
            return;

        uint32 reclaimed = 0;
        do
        {
            uint32 const guildId  = (*rows)[0].Get<uint32>();
            uint32 const ownerAcc = (*rows)[1].Get<uint32>();
            uint32 const ownerLow = (*rows)[2].Get<uint32>();

            if (ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(ownerLow)))
                continue;   // el owner esta dentro: no se toca

            QueryResult ageResult = LoginDatabase.Query(
                "SELECT DATEDIFF(NOW(), last_login) FROM account WHERE id = {}", ownerAcc);
            if (!ageResult || (*ageResult)[0].IsNull())
                continue;
            int32 const days = (*ageResult)[0].Get<int32>();
            if (days < 0 || static_cast<uint32>(days) < cfg.inactiveReclaimDays)
                continue;

            Guild* guild = sGuildMgr->GetGuildById(guildId);
            uint32 freed = 0;
            std::vector<ObjectGuid> const bots = LoadBotMembers(guildId);
            for (ObjectGuid const& bot : bots)
            {
                if (guild)
                    guild->DeleteMember(bot, false, true, false);
                BotClaims::RemoveHomeGuildBot(bot.GetCounter());
                ++freed;
            }

            CharacterDatabase.Execute("DELETE FROM mod_home_guild WHERE guild_id = {}", guildId);
            CharacterDatabase.Execute("DELETE FROM mod_home_guild_member WHERE guild_id = {}", guildId);
            g_managedGuilds.erase(guildId);
            for (auto it = g_homes.begin(); it != g_homes.end();)
                it = (it->second.guildId == guildId) ? g_homes.erase(it) : std::next(it);

            LOG_INFO("module", "[home-guild] Hermandad {} reclamada: owner (cuenta {}) sin entrar {} dias, "
                     "{} bots liberados al pool.", guildId, ownerAcc, days, freed);
            ++reclaimed;
        } while (rows->NextRow());

        if (reclaimed)
        {
            PublishHomeBots();
            LOG_INFO("module", "[home-guild] {} hermandades de owners inactivos reclamadas.", reclaimed);
        }
    }

    // Rellena la hermandad hasta cfg.members con bots del tramo y la facción.
    // Primero los que ya están conectados y libres (se ven al momento); luego
    // de la tabla de personajes, desconectados.
    uint32 Recruit(Player* player, Guild* guild, Home& home)
    {
        uint32 const have = static_cast<uint32>(home.bots.size());
        if (have >= cfg.members)
            return 0;

        uint32 wanted = cfg.members - have;
        uint32 const level = player->GetLevel();
        uint32 const maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
        uint32 const minLevel = level > cfg.levelBelow ? level - cfg.levelBelow : 1;
        uint32 const topLevel = std::min(level + cfg.levelAbove, maxLevel);
        TeamId const team = player->GetTeamId();
        std::string const races = (team == TEAM_ALLIANCE) ? cfg.allianceRaces : cfg.hordeRaces;

        uint32 joined = 0;
        auto tryAdd = [&](uint32 lowGuid) -> bool
        {
            if (!IsRandomBotGuid(lowGuid))
                return false;
            // Un bot reservado por otro módulo (queue-bots/party-here formando un
            // grupo) no entra en la guild, ni desde el pool offline (B3). El
            // paso 1 ya filtraba los conectados; el paso 2 no.
            if (BotClaims::IsClaimed(lowGuid))
                return false;
            // Excluido por el owner (".hermandad excluir"): nunca se re-recluta.
            if (auto ex = home.members.find(lowGuid); ex != home.members.end() && ex->second.excluded)
                return false;
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
            if (std::find(home.bots.begin(), home.bots.end(), guid) != home.bots.end())
                return false;
            if (!guild->AddMember(guid, GUILD_RANK_NONE))
                return false;
            home.bots.push_back(guid);
            BotClaims::AddHomeGuildBot(lowGuid);
            ++joined;
            return true;
        };

        // 1. Conectados y libres: sin grupo, sin hermandad, del tramo.
        std::vector<Player*> online;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* bot = pair.second;
            WorldSession* session = bot ? bot->GetSession() : nullptr;
            if (!session || !session->IsHeadless() || !bot->IsInWorld() || bot->GetGuildId() || bot->GetGroup())
                continue;
            if (bot->GetTeamId() != team || bot->GetLevel() < minLevel || bot->GetLevel() > topLevel)
                continue;
            if (BotClaims::IsClaimed(bot->GetGUID().GetCounter()))
                continue;
            online.push_back(bot);
        }
        Acore::Containers::RandomShuffle(online);

        // Roles equilibrados (A3): si al roster le faltan tanques o sanadores,
        // los candidatos que llenan ese hueco se prueban primero.
        {
            uint32 const size = std::max<uint32>(cfg.members, 1);
            uint32 const wantTanks   = cfg.tanks   ? cfg.tanks
                                                   : (size <= 10 ? 2u : (size <= 20 ? 3u : 4u));
            uint32 const wantHealers = cfg.healers ? cfg.healers
                                                   : (size <= 10 ? 3u : (size <= 20 ? 5u : 6u));
            uint32 haveTanks = 0, haveHealers = 0;
            for (ObjectGuid const& g : home.bots)
            {
                uint8 role = 0;
                if (auto info = home.members.find(g.GetCounter()); info != home.members.end() && info->second.role)
                    role = info->second.role;
                else if (Player* b = ObjectAccessor::FindPlayer(g))
                    role = BotRole(b);
                if (role == 1) ++haveTanks;
                else if (role == 2) ++haveHealers;
            }
            bool const gapT = haveTanks < wantTanks;
            bool const gapH = haveHealers < wantHealers;
            if (gapT || gapH)
            {
                auto score = [&](Player* p) -> int
                {
                    uint8 const r = BotRole(p);
                    if (r == 1 && gapT) return 0;
                    if (r == 2 && gapH) return 1;
                    return 2;
                };
                std::stable_sort(online.begin(), online.end(),
                                 [&](Player* a, Player* b) { return score(a) < score(b); });
            }
        }

        for (Player* bot : online)
        {
            if (joined >= wanted)
                break;
            uint32 const low = bot->GetGUID().GetCounter();
            if (auto ex = home.members.find(low); ex != home.members.end() && ex->second.excluded)
                continue;
            tryAdd(low);
        }

        // 2. De la tabla, desconectados y sin hermandad (HomeGuild.RecruitFromOfflinePool).
        //    "ORDER BY RAND()"
        // fuerza un filesort de toda la tabla characters en el hilo del
        // mundo; en su lugar, se cuenta cuántas filas cumplen el filtro y se
        // pide un tramo con LIMIT/OFFSET a partir de un punto al azar.
        if (joined < wanted && cfg.recruitFromOfflinePool)
        {
            // medir el coste real de este camino síncrono
            // (recuento cacheado 60s + LIMIT/OFFSET) antes de decidir si
            // hace falta precargar candidatos de forma asíncrona.
            uint32 const tRecruit0 = getMSTime();
            uint32 const need = (wanted - joined) * 3;
            uint32 const total = CachedRecruitCount(minLevel, topLevel, races);
            if (total)
            {
                uint32 const offset = total > need ? urand(0, total - need) : 0;
                QueryResult result = CharacterDatabase.Query(
                    "SELECT c.guid FROM characters c LEFT JOIN guild_member gm ON gm.guid = c.guid "
                    "WHERE gm.guid IS NULL AND c.online = 0 AND c.level BETWEEN {} AND {} AND c.race IN ({}) "
                    "LIMIT {} OFFSET {}",
                    minLevel, topLevel, races, need, offset);
                if (result)
                {
                    do
                    {
                        if (joined >= wanted)
                            break;
                        tryAdd((*result)[0].Get<uint32>());
                    } while (result->NextRow());
                }
            }
            SlowTick::WarnIfSlow("home-guild", "Recruit:offlinePool", tRecruit0);
        }

        if (joined)
            LOG_INFO("module", "[home-guild] {} bots de nivel {}-{} entran en '{}' ({}/{}).",
                     joined, minLevel, topLevel, guild->GetName(), home.bots.size(), cfg.members);
        else
            LOG_INFO("module", "[home-guild] Sin bots de nivel {}-{} ({}) libres para '{}': se reintenta.",
                     minLevel, topLevel, team == TEAM_ALLIANCE ? "Alianza" : "Horda", guild->GetName());
        return joined;
    }

    // Manda a dormir a bots ociosos que NO sean de ninguna hermandad de casa.
    uint32 MakeRoom(uint32 wanted)
    {
        uint32 freed = 0;
#ifdef HOME_GUILD_WITH_PLAYERBOTS
        // LogoutPlayerBot es SINCRONO (LogoutPlayer -> RemoveFromWorld ->
        // ObjectAccessor::RemoveObject): llamarlo dentro del range-for sobre
        // GetPlayers() muta el contenedor en plena iteracion -> UB en ++it.
        std::vector<ObjectGuid> victims;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            if (victims.size() >= wanted)
                break;
            Player* bot = pair.second;
            WorldSession* session = bot ? bot->GetSession() : nullptr;
            if (!session || !session->IsHeadless() || !bot->IsInWorld() || session->PlayerLoading())
                continue;
            if (bot->GetGroup() || bot->IsInCombat() || bot->InBattleground() || bot->InBattlegroundQueue())
                continue;
            if (Map* map = bot->GetMap())
                if (map->IsDungeon() || map->IsBattlegroundOrArena())
                    continue;
            uint32 const lowGuid = bot->GetGUID().GetCounter();
            if (BotClaims::IsHomeGuildBot(lowGuid) || BotClaims::IsClaimed(lowGuid) || !sRandomPlayerbotMgr.IsRandomBot(bot))
                continue;
            victims.push_back(bot->GetGUID());
        }
        for (ObjectGuid const& guid : victims)
        {
            sRandomPlayerbotMgr.LogoutPlayerBot(guid);
            ++freed;
        }
#else
        (void)wanted;
#endif
        return freed;
    }

    // Conecta a los bots de la hermandad que estén desconectados (hasta
    // KeepOnlineBatch por pasada). M11: antes se recorrían sólo los primeros
    // `batch` desconectados y un prefijo inútil (nivel por encima de la etapa,
    // login ya reservado) consumía la pasada entera sin probar al resto; y
    // MakeRoom desconectaba bots ajenos aunque ningún miembro fuera a entrar.
    // Ahora se filtra primero, los no fijados van por turnos y sólo se hace
    // hueco para una demanda real.
    uint32 KeepOnline(Player* owner, Home& home, uint64_t now)
    {
        if (!cfg.keepOnline || now < home.nextWakeMs)
            return 0;
        home.nextWakeMs = now + TimeMs::SecsToMs(cfg.keepOnlineIntervalSecs);

        uint32 woken = 0;
#ifdef HOME_GUILD_WITH_PLAYERBOTS
        // Candidatos admisibles: desconectado, bot aleatorio, sin login ya en
        // curso y con un nivel que la etapa activa admite. A diferencia de
        // queue-bots/party-here, aquí el GUID ya está fijo (es un miembro
        // concreto del roster): el nivel real, no el rango pedido, es lo que
        // puede superar la etapa. StagePass no lo corrige después porque
        // BotClaims lo marca como reclamado.
        // Los no fijados se recorren desde la posición del roster en la que se
        // quedó la pasada anterior, para que todos acaben entrando.
        uint32 const stageCap = BotPopulationCoordinator::StageCap();
        size_t const rosterSize = home.bots.size();
        size_t const start = rosterSize ? home.wakeCursor % rosterSize : 0;
        std::vector<ObjectGuid> pinned;
        std::vector<ObjectGuid> others;
        std::vector<size_t> othersAt;   // posición en el roster de cada uno de 'others'
        uint32 offline = 0;
        for (size_t step = 0; step < rosterSize; ++step)
        {
            size_t const at = (start + step) % rosterSize;
            ObjectGuid const& guid = home.bots[at];
            if (ObjectAccessor::FindPlayer(guid))
                continue;
            ++offline;
            uint32 const lowGuid = guid.GetCounter();
            if (!IsRandomBotGuid(lowGuid) || BotPopulationCoordinator::IsPending(lowGuid, now))
                continue;
            if (sCharacterCache->GetCharacterLevelByGuid(guid) > stageCap)
                continue;
            auto info = home.members.find(lowGuid);
            if (info != home.members.end() && info->second.pinned)
                pinned.push_back(guid);
            else
            {
                others.push_back(guid);
                othersAt.push_back(at);
            }
        }
        if (pinned.empty() && others.empty())
            return 0;   // nada que conectar: tampoco se desconecta a nadie

        // Los fijados (".hermandad fijar") primero; después el resto en turno.
        std::vector<ObjectGuid> candidates = std::move(pinned);
        candidates.insert(candidates.end(), others.begin(), others.end());

        uint32 const requested = std::min<uint32>(cfg.keepOnlineBatch, static_cast<uint32>(candidates.size()));
        uint32 const globalCap = sConfigMgr->GetOption<uint32>("AiPlayerbot.MaxRandomBots", 200);
        uint32 const cap = cfg.maxOnlineBots ? std::min(cfg.maxOnlineBots, globalCap) : globalCap;
        // Foto cacheada del coordinador: mismo escaneo que los Reserve de abajo.
        uint32 const online = BotPopulationCoordinator::OnlineCount(now);
        uint32 freed = 0;
        if (online + requested > cap)
            freed = MakeRoom(online + requested - cap);
        uint32 const afterRoom = online > freed ? online - freed : 0;
        uint32 const capacity = cap > afterRoom ? cap - afterRoom : 0;
        uint32 const batch = std::min(requested, capacity);

        if (!batch)
        {
            LOG_DEBUG("module", "[home-guild] Sin hueco bajo AiPlayerbot.MaxRandomBots ({}): "
                                "no se conectan bots de hermandad en esta pasada.", cap);
            return 0;
        }

        uint32 const ownerLevel = owner->GetLevel();
        uint32 const minLevel = ownerLevel > cfg.levelBelow ? ownerLevel - cfg.levelBelow : 1;
        uint32 const maxLevel = std::min<uint32>(ownerLevel + cfg.levelAbove,
            sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        auto const faction = owner->GetTeamId() == TEAM_ALLIANCE ? BotPopulationCoordinator::Faction::Alliance
                                                                 : BotPopulationCoordinator::Faction::Horde;

        // El límite es de logins ACEPTADOS, no de candidatos examinados: un
        // rechazo por GUID (ya en línea, carrera con otro módulo) deja probar
        // al siguiente. Un rechazo por presupuesto afecta a todos por igual:
        // se para ahí.
        size_t examined = 0;
        size_t othersExamined = 0;
        size_t const pinnedCount = candidates.size() - others.size();
        for (ObjectGuid const& guid : candidates)
        {
            if (woken >= batch)
                break;
            ++examined;
            if (examined > pinnedCount)
                ++othersExamined;

            BotPopulationCoordinator::Reservation reservation = BotPopulationCoordinator::Reserve(
                guid.GetCounter(), "home-guild", faction, minLevel, maxLevel, now);
            if (!reservation)
            {
                if (reservation.reason == BotPopulationCoordinator::RejectReason::DuplicateGuid
                    || reservation.reason == BotPopulationCoordinator::RejectReason::AlreadyOnline)
                    continue;
                break;
            }
            sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
            ++woken;
        }
        if (othersExamined)
            home.wakeCursor = othersAt[othersExamined - 1] + 1;
        if (woken)
            LOG_INFO("module", "[home-guild] Conectando {} bots de la hermandad ({} desconectados, {} admisibles, {} examinados).",
                     woken, offline, candidates.size(), examined);
        else
            LOG_DEBUG("module", "[home-guild] Ningún bot de la hermandad admitido por el coordinador ({} admisibles, {} examinados).",
                      candidates.size(), examined);
#else
        (void)owner;
        (void)home;
#endif
        return woken;
    }

    // ¿Se puede renivelar a este bot AHORA? El mismo núcleo que usan los demás
    // módulos antes de mover a un bot (BotEligibility: vivo, cargado, sin
    // combate, vuelo, teletransporte, grupo, cola de BG ni LFG, fuera de
    // mazmorras y campos), más lo propio de la hermandad (M04): nadie lo tiene
    // reservado (un bot de casa prestado a quest-mates o a un evento lo
    // administra otro módulo) y lleva ReLevelMinWorldSeconds asentado en el
    // mundo (playerbots aún lo mueve al conectar).
#ifdef HOME_GUILD_WITH_PLAYERBOTS
    bool CanReLevel(Player* bot, uint64_t now)
    {
        if (!BotEligibility::IsAvailable(bot))
            return false;
        uint32 const low = bot->GetGUID().GetCounter();
        if (BotClaims::IsClaimed(low))
            return false;
        return BotWorldAge::IsMature(low, bot->GetSession(), now, cfg.reLevelMinWorldSecs);
    }
#endif

    // Sube de nivel a UN bot que se haya quedado atrás (uno por pasada: es caro).
    void ReLevelOne(Player* player, Home& home)
    {
        if (!cfg.reLevel)
            return;
#ifdef HOME_GUILD_WITH_PLAYERBOTS
        uint32 const level = player->GetLevel();
        if (level <= cfg.reLevelBehind)
            return;
        uint32 const floorLevel = level - cfg.reLevelBehind;
        uint32 const jitter = cfg.reLevelTargetJitter ? cfg.reLevelTargetJitter : cfg.levelBelow;

        // Techo del objetivo: el nivel del dueño, la etapa activa de
        // world-bots y el máximo del servidor. Suelo: floorLevel, que ya está
        // por encima del bot (sólo se renivela a quien está por debajo); sin
        // él, con ReLevelTargetJitter > ReLevelBehind el sorteo podía caer por
        // debajo del nivel actual y "subir" bajando (M04).
        uint32 const ceiling = std::min<uint32>({ level, BotPopulationCoordinator::StageCap(),
            sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL) });
        if (ceiling < floorLevel)
            return;   // la etapa no deja subir a nadie hasta el suelo

        uint64_t const now = TimeMs::NowMs();
        uint32 done = 0;
        for (ObjectGuid const& guid : home.bots)
        {
            if (done >= cfg.reLevelPerPass)
                return;

            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot || bot->GetLevel() >= floorLevel || !CanReLevel(bot, now))
                continue;

            uint32 target = level > jitter ? level - urand(0, jitter) : level;
            target = std::clamp<uint32>(target, floorLevel, ceiling);
            if (target <= bot->GetLevel())
                continue;

            // Comprobar -> reservar -> revalidar -> actuar: la reserva cubre la
            // fábrica y se suelta al salir del bloque, pase lo que pase.
            BotClaims::Lease claim(guid.GetCounter(), "home-guild");
            if (!claim || !claim.IsNew() || !BotEligibility::IsAvailable(bot))
                continue;

            uint8 const before = bot->GetLevel();
            uint32 const t0 = getMSTime();
            PlayerbotFactory factory(bot, target);
            factory.Randomize(true); // conserva equipo/skills y evita un reroll completo
            SlowTick::WarnIfSlow("home-guild", "ReLevel (fabrica)", t0);
            LOG_INFO("module", "[home-guild] {} sube de nivel {} a {} para seguir a {} ({} ms).",
                     bot->GetName(), before, bot->GetLevel(), player->GetName(), GetMSTimeDiffToNow(t0));
            ++done;
        }
#else
        (void)player; (void)home;
#endif
    }

    // Mantiene el equipo de los bots de casa conectados a la fase del owner,
    // aunque no estén en su grupo, para que estén listos para una banda sin
    // pasar antes por una mazmorra (mejora 7). El presupuesto es el mismo que
    // comparten queue-bots / party-here (BotGear::ProcessOne en OnUpdate).
    void GearRoster(Player* owner, Home& home, uint64_t now)
    {
        if (cfg.gear.mode == BotGear::MODE_OFF || now < home.nextGearMs)
            return;
        home.nextGearMs = now + TimeMs::SecsToMs(cfg.gearIntervalSecs);

        uint32 const target = BotGear::TargetItemLevel(owner, cfg.gear);
        if (!target)
            return;
        for (ObjectGuid const& guid : home.bots)
        {
            Player* bot = ObjectAccessor::FindPlayer(guid);
            WorldSession* session = bot ? bot->GetSession() : nullptr;
            if (!bot || !session || !bot->IsInWorld() || session->PlayerLoading())
                continue;
            if (bot->IsInCombat() || bot->IsBeingTeleported() || bot->GetGroup() || bot->InBattleground())
                continue;   // en grupo, ya se ocupa queue-bots / party-here
            if (BotClaims::IsClaimed(guid.GetCounter()))
                continue;   // prestado a otro módulo (quest-mates, un evento...): no es nuestro ahora

            // Prioridad de fondo: no pisa el trabajo de un grupo o un evento.
            // Al consumirse (M02) se cancela si el bot ya no es de la
            // hermandad, lo ha reservado alguien, ha entrado en un grupo o el
            // dueño se ha ido; si no, se recalcula con el dueño de ahora.
            BotGear::Schedule(guid, target, cfg.gear, "home-guild", BotGear::PRIORITY_BACKGROUND,
                [ownerGuid = owner->GetGUID()](Player* member, uint32& ilvl)
                {
                    uint32 const low = member->GetGUID().GetCounter();
                    if (!BotClaims::IsHomeGuildBot(low) || BotClaims::IsClaimed(low) || member->GetGroup())
                        return BotGear::Decision::Cancel;
                    Player* current = ObjectAccessor::FindPlayer(ownerGuid);
                    if (!current)
                        return BotGear::Decision::Cancel;
                    ilvl = BotGear::TargetItemLevel(current, cfg.gear);
                    return ilvl ? BotGear::Decision::Apply : BotGear::Decision::Cancel;
                });
        }
    }
}

class mod_home_guild_world : public WorldScript
{
public:
    mod_home_guild_world() : WorldScript("mod_home_guild_world",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        static bool wasEnabled = false;

        cfg.enabled       = sConfigMgr->GetOption<bool>("HomeGuild.Enable", true);
        cfg.legacyName    = sConfigMgr->GetOption<std::string>("HomeGuild.LegacyName", "Companeros de {name}");
        cfg.legacyMotd    = sConfigMgr->GetOption<std::string>("HomeGuild.LegacyMotd", "Bienvenido a casa.");
        cfg.cleanupLegacy = sConfigMgr->GetOption<bool>("HomeGuild.CleanupLegacyAutoGuilds", true);
        cfg.members       = sConfigMgr->GetOption<uint32>("HomeGuild.Members", 15);
        cfg.levelBelow    = sConfigMgr->GetOption<uint32>("HomeGuild.LevelBelow", 3);
        cfg.levelAbove    = sConfigMgr->GetOption<uint32>("HomeGuild.LevelAbove", 2);
        cfg.keepOnline    = sConfigMgr->GetOption<bool>("HomeGuild.KeepOnline", true);
        cfg.reLevel       = sConfigMgr->GetOption<bool>("HomeGuild.ReLevel", true);
        cfg.reLevelBehind = sConfigMgr->GetOption<uint32>("HomeGuild.ReLevelBehind", 4);
        cfg.announce      = sConfigMgr->GetOption<bool>("HomeGuild.Announce", true);
        cfg.announceWhenGuildless = sConfigMgr->GetOption<bool>("HomeGuild.AnnounceWhenGuildless", true);
        cfg.autoAdopt     = sConfigMgr->GetOption<bool>("HomeGuild.AutoAdopt", true);
        cfg.inactiveReclaimDays = sConfigMgr->GetOption<uint32>("HomeGuild.InactiveOwnerReclaimDays", 30);
        cfg.careIntervalSecs  = std::max<uint32>(sConfigMgr->GetOption<uint32>("HomeGuild.CareIntervalSeconds", 30), 1);
        cfg.recruitBackoffSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("HomeGuild.RecruitBackoffSeconds", 300), 1);
        cfg.keepOnlineIntervalSecs = std::max<uint32>(sConfigMgr->GetOption<uint32>("HomeGuild.KeepOnlineIntervalSeconds", 15), 1);
        cfg.keepOnlineBatch   = std::max<uint32>(sConfigMgr->GetOption<uint32>("HomeGuild.KeepOnlineBatch", 5), 1);
        cfg.reLevelPerPass    = std::max<uint32>(sConfigMgr->GetOption<uint32>("HomeGuild.ReLevelPerPass", 1), 1);
        cfg.recruitFromOfflinePool = sConfigMgr->GetOption<bool>("HomeGuild.RecruitFromOfflinePool", true);
        cfg.maxOnlineBots     = sConfigMgr->GetOption<uint32>("HomeGuild.MaxOnlineBots", 0);
        cfg.allianceRaces     = sConfigMgr->GetOption<std::string>("HomeGuild.AllianceRaces", "1,3,4,7,11");
        cfg.hordeRaces        = sConfigMgr->GetOption<std::string>("HomeGuild.HordeRaces", "2,5,6,8,10");
        cfg.legacyNumberMax   = sConfigMgr->GetOption<uint32>("HomeGuild.LegacyNumberMax", 100);
        cfg.reLevelTargetJitter = sConfigMgr->GetOption<uint32>("HomeGuild.ReLevelTargetJitter", 0);
        cfg.reLevelMinWorldSecs = sConfigMgr->GetOption<uint32>("HomeGuild.ReLevelMinWorldSeconds", 60);
        cfg.tanks             = sConfigMgr->GetOption<uint32>("HomeGuild.Tanks", 0);
        cfg.healers           = sConfigMgr->GetOption<uint32>("HomeGuild.Healers", 0);
        cfg.gear              = BotGear::LoadSettings("HomeGuild");
        cfg.gearIntervalSecs  = std::max<uint32>(sConfigMgr->GetOption<uint32>("HomeGuild.GearIntervalSeconds", 120), 15);
        BotPopulationCoordinator::LoadSettings();

        if (cfg.members > 200)
            cfg.members = 200;

#ifndef HOME_GUILD_WITH_PLAYERBOTS
        if (cfg.enabled)
        {
            LOG_INFO("module", "[home-guild] Compilado sin mod-playerbots: no hay bots, el modulo no hace nada.");
            cfg.enabled = false;
        }
#endif

        g_enabledHot.store(cfg.enabled, std::memory_order_relaxed);       // espejos para hooks de hilo de mapa (T7)
        g_autoAdoptHot.store(cfg.autoAdopt, std::memory_order_relaxed);

        if (reload)
        {
            // Re-habilitado en caliente (estaba apagado al arrancar, o se apago
            // y se encendio): cargar/limpiar como en OnStartup (T6).
            if (cfg.enabled && !wasEnabled)
            {
                LoadManagedGuilds();
                CleanupLegacyGuilds();
                ReclaimInactiveGuilds();
            }
            // Deshabilitado en caliente: soltar el estado y vaciar el conjunto
            // global de bots de hermandad, que si no quedaba poblado para
            // siempre (CS-1.4).
            else if (!cfg.enabled && wasEnabled)
            {
                g_homes.clear();
                g_managedGuilds.clear();
                BotClaims::SetHomeGuildBots({});
                LOG_INFO("module", "[home-guild] Desactivado en caliente: estado y marcas de hermandad liberados.");
            }
        }

        wasEnabled = cfg.enabled;
    }

    void OnStartup() override
    {
        if (!cfg.enabled)
            return;
        LoadManagedGuilds();
        CleanupLegacyGuilds();
        ReclaimInactiveGuilds();
        g_nextReclaimMs = TimeMs::NowMs() + TimeMs::SecsToMs(6 * 3600);
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        uint32 const t0 = getMSTime();
        DoUpdate();
        SlowTick::WarnIfSlow("home-guild", "OnUpdate", t0);
    }

private:
    // ya se publicó la estadística con enabled=false de este apagado.
    bool _offPublished = false;

    void DoUpdate()
    {
        if (!cfg.enabled)
        {
            // una última estadística con enabled=false y el buzón del
            // panel cerrado mientras siga apagado.
            if (!_offPublished)
            {
                _offPublished = true;
                BotOperations::GuildStats stats;
                stats.updatedAtMs = TimeMs::NowMs();
                BotOperations::PublishGuildStats(std::move(stats));
            }
            BotOperations::SetTargetOffline(BotOperations::Target::HomeGuild, TimeMs::NowMs(),
                                            "home-guild esta desactivado.");
            return;
        }
        _offPublished = false;

        uint64_t const now = TimeMs::NowMs();

        // Presupuesto compartido de reequipo (BotGear.h), como en queue/party.
        BotGear::ProcessOne();

        if (now >= g_nextReclaimMs)
        {
            g_nextReclaimMs = now + TimeMs::SecsToMs(6 * 3600);   // cada 6 h
            ReclaimInactiveGuilds();
        }

        std::deque<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lock(g_pendingLock);
            events.swap(g_pending);
        }
        SlowTick::WarnIfDeep("home-guild", "g_pending", events.size());
        for (PendingEvent const& event : events)
        {
            switch (event.kind)
            {
                case EVENT_GUILD_CREATED:
                {
                    Guild* guild = sGuildMgr->GetGuildById(event.guildId);
                    if (!guild || guild->GetLeaderGUID() != event.player)
                    {
                        LOG_WARN("module", "[home-guild] La guild id {} ya no pertenece a su fundador: no se vincula.",
                                 event.guildId);
                        break;
                    }
                    CharacterDatabase.Execute(   // async: g_managedGuilds ya queda actualizado en memoria (B10)
                        "INSERT INTO mod_home_guild (guild_id, owner_account, owner_guid) VALUES ({}, {}, {}) "
                        "ON DUPLICATE KEY UPDATE owner_account = VALUES(owner_account), owner_guid = VALUES(owner_guid)",
                        event.guildId, event.accountId, event.player.GetCounter());
                    g_managedGuilds[event.guildId] = { event.accountId, event.player.GetCounter() };
                    LOG_INFO("module", "[home-guild] La hermandad de creación manual id {} queda vinculada a {}.",
                             event.guildId, event.player.GetCounter());
                    OnLogin(event.player);
                    break;
                }
                case EVENT_GUILD_DISBANDED:
                    CharacterDatabase.Execute("DELETE FROM mod_home_guild WHERE guild_id = {}", event.guildId);
                    CharacterDatabase.Execute("DELETE FROM mod_home_guild_member WHERE guild_id = {}", event.guildId);
                    g_managedGuilds.erase(event.guildId);
                    for (auto it = g_homes.begin(); it != g_homes.end();)
                    {
                        if (it->second.guildId == event.guildId)
                            it = g_homes.erase(it);
                        else
                            ++it;
                    }
                    PublishHomeBots();
                    break;
                case EVENT_LOGIN:
                    OnLogin(event.player);
                    break;
                case EVENT_CMD_STATUS:
                    CmdStatus(event.player);
                    break;
                case EVENT_CMD_ACTIVATE:
                    CmdActivate(event.player);
                    break;
                case EVENT_CMD_DEACTIVATE:
                    CmdDeactivate(event.player);
                    break;
                case EVENT_CMD_PIN:
                    CmdMark(event.player, event.name, MARK_PIN);
                    break;
                case EVENT_CMD_UNPIN:
                    CmdMark(event.player, event.name, MARK_UNPIN);
                    break;
                case EVENT_CMD_EXCLUDE:
                    CmdMark(event.player, event.name, MARK_EXCLUDE);
                    break;
                case EVENT_CMD_RENEW:
                {
                    auto it = g_homes.find(event.player);
                    if (it != g_homes.end())
                    {
                        it->second.nextCareMs = 0;   // Care ya en la siguiente pasada
                        if (Player* p = ObjectAccessor::FindPlayer(event.player))
                            if (p->GetSession())
                                ChatHandler(p->GetSession()).SendSysMessage("Se renueva tu hermandad: reclutar y nivelar en la proxima pasada.");
                    }
                    break;
                }
            }
        }

        // Acciones del panel (mod-bot-operations, via BotOperations.h):
        // "adelanta el cuidado" es EVENT_CMD_RENEW pero para todas las
        // hermandades con dueño conectado, no solo una.
        for (BotOperations::ActionRequest const& request : BotOperations::TakeRequests(BotOperations::Target::HomeGuild, now))
        {
            if (request.type == BotOperations::ActionType::HomeGuildPass)
            {
                for (auto& pair : g_homes)
                    pair.second.nextCareMs = 0;
                BotOperations::ReportOutcome(request.id, true, "Proximo cuidado de hermandades adelantado.", now);
            }
            else
                BotOperations::ReportOutcome(request.id, false, "Accion no reconocida por home-guild.", now);
        }

        for (auto it = g_homes.begin(); it != g_homes.end();)
        {
            Player* player = ObjectAccessor::FindPlayer(it->first);
            if (!IsHuman(player))
            {
                it = g_homes.erase(it);
                PublishHomeBots();
                continue;
            }

            Home& home = it->second;
            if (now >= home.nextCareMs)
            {
                home.nextCareMs = now + TimeMs::SecsToMs(cfg.careIntervalSecs);
                if (!Care(player, home))
                {
                    it = g_homes.erase(it);
                    PublishHomeBots();
                    continue;
                }
            }
            ++it;
        }

        // Estadisticas para el panel (pestaña "Colas y grupos"): solo este
        // OnUpdate es dueño de g_homes/g_managedGuilds.
        {
            BotOperations::GuildStats stats;
            stats.enabled = cfg.enabled;
            stats.homeGuilds = static_cast<uint32_t>(g_managedGuilds.size());
            for (auto const& pair : g_homes)
                stats.botsManaged += static_cast<uint32_t>(pair.second.bots.size());
            stats.updatedAtMs = now;
            BotOperations::PublishGuildStats(std::move(stats));
        }
    }

private:
    void OnLogin(ObjectGuid guid)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!IsHuman(player))
            return;

        Guild* guild = player->GetGuild();

        if (!guild)
        {
            if (cfg.announce && cfg.announceWhenGuildless)
                ChatHandler(player->GetSession()).SendSysMessage(
                    "No tienes hermandad. mod-home-guild no crea ninguna automáticamente: "
                    "funda una en el juego si quieres una hermandad de casa.");
            return;
        }

        auto const managed = g_managedGuilds.find(guild->GetId());
        if (managed == g_managedGuilds.end())
        {
            LOG_DEBUG("module", "[home-guild] '{}' no fue creada bajo el módulo: no se toca.", guild->GetName());
            return;
        }

        uint32 const playerGuid = player->GetGUID().GetCounter();
        if (guild->GetLeaderGUID().GetCounter() != managed->second.ownerGuid)
        {
            LOG_INFO("module", "[home-guild] '{}' cambió de líder: deja de estar bajo gestión.", guild->GetName());
            CharacterDatabase.Execute("DELETE FROM mod_home_guild WHERE guild_id = {}", guild->GetId());
            g_managedGuilds.erase(managed);
            return;
        }

        if (managed->second.ownerGuid != playerGuid ||
            managed->second.ownerAccount != player->GetSession()->GetAccountId() ||
            guild->GetLeaderGUID() != player->GetGUID())
        {
            LOG_DEBUG("module", "[home-guild] {} es miembro de '{}', pero no su fundador y líder: no se toca.",
                      player->GetName(), guild->GetName());
            return;
        }

        Home& home = g_homes[guid];
        home.guildId = guild->GetId();
        home.bots = LoadBotMembers(guild->GetId());
        home.nextCareMs = 0;
        home.nextWakeMs = 0;
        PublishHomeBots();

        uint32 online = 0;
        for (ObjectGuid const& bot : home.bots)
            if (ObjectAccessor::FindPlayer(bot))
                ++online;

        if (cfg.announce)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Tu hermandad '{}': {} companeros, {} conectados ahora. Escribe .grupo para salir con ellos.",
                guild->GetName(), static_cast<uint32>(home.bots.size()), online);
    }

    // ".hermandad": el jugador debe ser el fundador-lider de una hermandad.
    // Devuelve la guild y su cuenta si es asi; si no, avisa y devuelve nullptr.
    Guild* OwnerGuildOf(Player* player)
    {
        Guild* guild = player ? player->GetGuild() : nullptr;
        if (!guild)
        {
            ChatHandler(player->GetSession()).SendSysMessage("No tienes hermandad. Funda una en el juego primero.");
            return nullptr;
        }
        if (guild->GetLeaderGUID() != player->GetGUID())
        {
            ChatHandler(player->GetSession()).SendSysMessage("Solo el lider fundador de la hermandad puede usar esto.");
            return nullptr;
        }
        return guild;
    }

    void CmdStatus(ObjectGuid guid)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!IsHuman(player))
            return;
        ChatHandler h(player->GetSession());
        Guild* guild = player->GetGuild();
        if (!guild || !g_managedGuilds.count(guild->GetId()))
        {
            h.PSendSysMessage("Tu hermandad no es una hermandad de casa. Usa .hermandad activar si eres su fundador"
                              "{}.", cfg.autoAdopt ? " (o funda una nueva)" : "");
            return;
        }
        std::vector<ObjectGuid> const bots = LoadBotMembers(guild->GetId());
        std::unordered_map<uint32, MemberInfo> roster;
        LoadRoster(guild->GetId(), roster);
        uint32 online = 0, tanks = 0, healers = 0;
        for (ObjectGuid const& b : bots)
        {
            if (ObjectAccessor::FindPlayer(b))
                ++online;
            auto info = roster.find(b.GetCounter());
            if (info != roster.end())
            {
                if (info->second.role == 1) ++tanks;
                else if (info->second.role == 2) ++healers;
            }
        }
        h.PSendSysMessage("Hermandad de casa '{}': {} companeros bot, {} conectados ({} tanques, {} sanadores conocidos).",
                          guild->GetName(), static_cast<uint32>(bots.size()), online, tanks, healers);
        for (ObjectGuid const& b : bots)
            if (Player* bot = ObjectAccessor::FindPlayer(b))
            {
                auto info = roster.find(b.GetCounter());
                h.PSendSysMessage("  {} (nivel {}, {}){}", bot->GetName(), bot->GetLevel(),
                                  RoleName(info != roster.end() ? info->second.role : 0),
                                  (info != roster.end() && info->second.pinned) ? " [fijado]" : "");
            }
    }

    enum MarkKind : uint8 { MARK_PIN, MARK_UNPIN, MARK_EXCLUDE };

    // ".hermandad fijar/soltar/excluir <nombre>" (CS-4.2).
    void CmdMark(ObjectGuid guid, std::string const& botName, MarkKind mark)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!IsHuman(player))
            return;
        Guild* guild = OwnerGuildOf(player);
        if (!guild)
            return;
        ChatHandler h(player->GetSession());
        if (!g_managedGuilds.count(guild->GetId()))
        {
            h.SendSysMessage("Tu hermandad no es una hermandad de casa.");
            return;
        }
        if (botName.empty())
        {
            h.SendSysMessage("Falta el nombre del companero.");
            return;
        }

        // Buscar el bot del roster por nombre (conectado, o por la tabla de personajes).
        uint32 low = 0;
        for (ObjectGuid const& b : LoadBotMembers(guild->GetId()))
            if (Player* bot = ObjectAccessor::FindPlayer(b))
            {
                if (bot->GetName().size() == botName.size() &&
                    std::equal(botName.begin(), botName.end(), bot->GetName().begin(),
                               [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); }))
                {
                    low = b.GetCounter();
                    break;
                }
            }
        if (!low)
        {
            std::string safe = botName;
            CharacterDatabase.EscapeString(safe);
            QueryResult r = CharacterDatabase.Query(
                "SELECT c.guid FROM characters c JOIN guild_member gm ON gm.guid = c.guid "
                "WHERE gm.guildid = {} AND LOWER(c.name) = LOWER('{}')", guild->GetId(), safe);
            if (r)
                low = (*r)[0].Get<uint32>();
        }
        if (!low)
        {
            h.PSendSysMessage("No hay ningun companero '{}' en tu hermandad.", botName);
            return;
        }

        char const* verb = "";
        if (mark == MARK_PIN)
        {
            CharacterDatabase.Execute(
                "INSERT INTO mod_home_guild_member (guild_id, bot_guid, pinned, excluded) VALUES ({}, {}, 1, 0) "
                "ON DUPLICATE KEY UPDATE pinned = 1, excluded = 0", guild->GetId(), low);
            verb = "fijado: se conecta primero y no se le echa";
        }
        else if (mark == MARK_UNPIN)
        {
            CharacterDatabase.Execute(
                "UPDATE mod_home_guild_member SET pinned = 0, excluded = 0 WHERE guild_id = {} AND bot_guid = {}",
                guild->GetId(), low);
            verb = "sin marca";
        }
        else // MARK_EXCLUDE
        {
            CharacterDatabase.Execute(
                "INSERT INTO mod_home_guild_member (guild_id, bot_guid, pinned, excluded) VALUES ({}, {}, 0, 1) "
                "ON DUPLICATE KEY UPDATE pinned = 0, excluded = 1", guild->GetId(), low);
            verb = "excluido: se saca de la hermandad y no se le vuelve a reclutar";
        }

        // La escritura es async; Care recarga el roster y aplica el efecto en la
        // siguiente pasada (nextCareMs = 0 la adelanta).
        if (auto it = g_homes.find(guid); it != g_homes.end())
            it->second.nextCareMs = 0;
        h.PSendSysMessage("Companero {}.", verb);
    }

    void CmdActivate(ObjectGuid guid)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!IsHuman(player))
            return;
        Guild* guild = OwnerGuildOf(player);
        if (!guild)
            return;
        ChatHandler h(player->GetSession());
        if (g_managedGuilds.count(guild->GetId()))
        {
            h.SendSysMessage("Tu hermandad ya es una hermandad de casa.");
            return;
        }
        uint32 const accountId = player->GetSession()->GetAccountId();
        CharacterDatabase.Execute(
            "INSERT INTO mod_home_guild (guild_id, owner_account, owner_guid) VALUES ({}, {}, {}) "
            "ON DUPLICATE KEY UPDATE owner_account = VALUES(owner_account), owner_guid = VALUES(owner_guid)",
            guild->GetId(), accountId, guid.GetCounter());
        g_managedGuilds[guild->GetId()] = { accountId, guid.GetCounter() };
        LOG_INFO("module", "[home-guild] '{}' activada como hermandad de casa por {}.", guild->GetName(), player->GetName());
        h.SendSysMessage("Hermandad de casa activada: se poblara con companeros de tu nivel.");
        OnLogin(guid);
    }

    void CmdDeactivate(ObjectGuid guid)
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!IsHuman(player))
            return;
        Guild* guild = OwnerGuildOf(player);
        if (!guild)
            return;
        ChatHandler h(player->GetSession());
        if (!g_managedGuilds.count(guild->GetId()))
        {
            h.SendSysMessage("Tu hermandad no es una hermandad de casa.");
            return;
        }

        uint32 freed = 0;
        for (ObjectGuid const& bot : LoadBotMembers(guild->GetId()))
        {
            guild->DeleteMember(bot, false, true, false);
            BotClaims::RemoveHomeGuildBot(bot.GetCounter());
            ++freed;
        }
        CharacterDatabase.Execute("DELETE FROM mod_home_guild WHERE guild_id = {}", guild->GetId());
        // Las filas de mod_home_guild_member NO se borran: si vuelves a activar,
        // se conservan las marcas de fijado/excluido y los roles conocidos.
        g_managedGuilds.erase(guild->GetId());
        g_homes.erase(guid);
        PublishHomeBots();
        LOG_INFO("module", "[home-guild] '{}' desactivada por {}: {} bots liberados.", guild->GetName(), player->GetName(), freed);
        h.PSendSysMessage("Hermandad de casa desactivada: {} companeros bot expulsados. La hermandad sigue existiendo.", freed);
    }

    // Devuelve false si el jugador ya no está en su hermandad (se disolvió, o
    // se fue): el que llama borra la entrada.
    bool Care(Player* player, Home& home)
    {
        Guild* guild = sGuildMgr->GetGuildById(home.guildId);
        auto const managed = g_managedGuilds.find(home.guildId);
        if (!guild || managed == g_managedGuilds.end() || player->GetGuildId() != home.guildId)
            return false;
        if (guild->GetLeaderGUID().GetCounter() != managed->second.ownerGuid)
        {
            LOG_INFO("module", "[home-guild] '{}' cambió de líder: deja de estar bajo gestión.", guild->GetName());
            CharacterDatabase.Execute("DELETE FROM mod_home_guild WHERE guild_id = {}", home.guildId);
            g_managedGuilds.erase(managed);
            return false;
        }
        if (managed->second.ownerGuid != player->GetGUID().GetCounter() || guild->GetLeaderGUID() != player->GetGUID())
            return false;

        // Miembros que ya no están (expulsados a mano): se refresca de la tabla.
        home.bots = LoadBotMembers(home.guildId);
        LoadRoster(home.guildId, home.members);

        uint64_t const now = TimeMs::NowMs();

        // Bots excluidos por el owner (".hermandad excluir"): se sacan de la
        // hermandad y del pool, y Recruit ya no los vuelve a meter.
        for (auto it = home.bots.begin(); it != home.bots.end();)
        {
            auto info = home.members.find(it->GetCounter());
            if (info != home.members.end() && info->second.excluded)
            {
                guild->DeleteMember(*it, false, true, false);
                BotClaims::RemoveHomeGuildBot(it->GetCounter());
                it = home.bots.erase(it);   // la fila con excluded=1 se queda: Recruit no lo re-mete
            }
            else
                ++it;
        }

        // Rol de los bots del roster que estén conectados y aún no lo tengamos
        // apuntado (se conoce al verlos con sus talentos puestos).
        for (ObjectGuid const& guid : home.bots)
        {
            uint32 const low = guid.GetCounter();
            MemberInfo& info = home.members[low];
            if (info.role != 0)
                continue;
            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot || !bot->IsInWorld() || (bot->GetSession() && bot->GetSession()->PlayerLoading()))
                continue;
            info.role = BotRole(bot);
            CharacterDatabase.Execute(
                "INSERT INTO mod_home_guild_member (guild_id, bot_guid, role) VALUES ({}, {}, {}) "
                "ON DUPLICATE KEY UPDATE role = VALUES(role)",
                home.guildId, low, info.role);
        }

        if (home.bots.size() < cfg.members)
        {
            if (!Recruit(player, guild, home))
                // Sin candidatos del tramo: la búsqueda es cara (recorre
                // characters LEFT JOIN guild_member) y repetirla cada 30 s
                // indefinidamente mientras no aparezcan bots nuevos del nivel
                // que toca no cambia el resultado. Se espacia a 5 min.
                home.nextCareMs = now + TimeMs::SecsToMs(cfg.recruitBackoffSecs);
        }

        KeepOnline(player, home, now);
        ReLevelOne(player, home);
        GearRoster(player, home, now);

        // Incondicional (B2): home.bots ya viene refrescado de la tabla, asi que
        // si el owner expulso un bot a mano su GUID hay que quitarlo del
        // conjunto global -antes solo se re-publicaba tras un Recruit con exito.
        PublishHomeBots();
        return true;
    }
};

class mod_home_guild_player : public PlayerScript
{
public:
    mod_home_guild_player() : PlayerScript("mod_home_guild_player", { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_enabledHot.load(std::memory_order_relaxed) || !IsHuman(player))
            return;
        Push({ EVENT_LOGIN, player->GetGUID(), 0, player->GetSession()->GetAccountId() });
    }

    // La próxima sesión de este GUID vuelve a esperar ReLevelMinWorldSeconds
    // aunque su WorldSession reutilice la dirección de la anterior (M08).
    void OnPlayerLogout(Player* player) override
    {
        if (player)
            BotWorldAge::Forget(player->GetGUID().GetCounter());
    }
};

class mod_home_guild_guild : public GuildScript
{
public:
    mod_home_guild_guild() : GuildScript("mod_home_guild_guild", { GUILDHOOK_ON_CREATE, GUILDHOOK_ON_DISBAND }) { }

    void OnCreate(Guild* guild, Player* leader, std::string const& /*name*/) override
    {
        // Con AutoAdopt=0 fundar una guild NO la hace "de casa": el jugador la
        // activa con ".hermandad activar" (B5).
        if (!g_enabledHot.load(std::memory_order_relaxed) || !g_autoAdoptHot.load(std::memory_order_relaxed)
            || !guild || !IsHuman(leader))
            return;
        Push({ EVENT_GUILD_CREATED, leader->GetGUID(), guild->GetId(), leader->GetSession()->GetAccountId() });
    }

    void OnDisband(Guild* guild) override
    {
        if (g_enabledHot.load(std::memory_order_relaxed) && guild)
            Push({ EVENT_GUILD_DISBANDED, ObjectGuid::Empty, guild->GetId(), 0 });
    }
};

// ".hermandad": el handler corre en el hilo del mapa, asi que solo encola un
// PendingEvent y el trabajo lo hace OnUpdate del mundo (modules/README.md, regla 5).
class mod_home_guild_command : public CommandScript
{
public:
    mod_home_guild_command() : CommandScript("mod_home_guild_command") { }

    Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        using namespace Acore::ChatCommands;
        static ChatCommandTable hermandadTable =
        {
            { "estado",      HandleStatus,     SEC_PLAYER, Console::No },
            { "activar",     HandleActivate,   SEC_PLAYER, Console::No },
            { "desactivar",  HandleDeactivate, SEC_PLAYER, Console::No },
            { "renovar",     HandleRenew,      SEC_PLAYER, Console::No },
            { "fijar",       HandlePin,        SEC_PLAYER, Console::No },
            { "soltar",      HandleUnpin,      SEC_PLAYER, Console::No },
            { "excluir",     HandleExclude,    SEC_PLAYER, Console::No },
            { "",            HandleStatus,     SEC_PLAYER, Console::No },
        };
        static ChatCommandTable commandTable =
        {
            { "hermandad", hermandadTable },
        };
        return commandTable;
    }

    static bool Enqueue(ChatHandler* handler, EventKind kind)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return true;
        if (!g_enabledHot.load(std::memory_order_relaxed))
        {
            handler->SendSysMessage("mod-home-guild esta desactivado.");
            return true;
        }
        Push({ kind, player->GetGUID(), player->GetGuildId(), player->GetSession()->GetAccountId() });
        return true;
    }

    static bool EnqueueNamed(ChatHandler* handler, EventKind kind, std::string const& name)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return true;
        if (!g_enabledHot.load(std::memory_order_relaxed))
        {
            handler->SendSysMessage("mod-home-guild esta desactivado.");
            return true;
        }
        if (name.empty())
        {
            handler->SendSysMessage("Falta el nombre del companero.");
            return true;
        }
        Push({ kind, player->GetGUID(), player->GetGuildId(), player->GetSession()->GetAccountId(), name });
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)     { return Enqueue(handler, EVENT_CMD_STATUS); }
    static bool HandleActivate(ChatHandler* handler)   { return Enqueue(handler, EVENT_CMD_ACTIVATE); }
    static bool HandleDeactivate(ChatHandler* handler) { return Enqueue(handler, EVENT_CMD_DEACTIVATE); }
    static bool HandleRenew(ChatHandler* handler)      { return Enqueue(handler, EVENT_CMD_RENEW); }
    static bool HandlePin(ChatHandler* handler, Optional<std::string> name)
        { return EnqueueNamed(handler, EVENT_CMD_PIN, name ? *name : std::string()); }
    static bool HandleUnpin(ChatHandler* handler, Optional<std::string> name)
        { return EnqueueNamed(handler, EVENT_CMD_UNPIN, name ? *name : std::string()); }
    static bool HandleExclude(ChatHandler* handler, Optional<std::string> name)
        { return EnqueueNamed(handler, EVENT_CMD_EXCLUDE, name ? *name : std::string()); }
};

void AddSC_mod_home_guild()
{
    new mod_home_guild_world();
    new mod_home_guild_player();
    new mod_home_guild_guild();
    new mod_home_guild_command();
}
