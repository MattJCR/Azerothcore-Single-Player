// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-update-notice — que el GM se entere al entrar de que hay versiones nuevas.
 *
 * EL PROBLEMA
 * El servidor vive de repositorios de terceros fijados en versions.lock, más
 * los addons de cliente de addons.lock y web-panel/addons/catalog.json. La
 * tarea semanal (lib/check-updates.sh, con lib/versions.sh y
 * lib/addon-versions.sh) mira si hay commits nuevos en cualquiera de los dos
 * y manda un correo dentro del juego, pero el correo hay que ir a leerlo al
 * buzón, y el aviso se olvida.
 *
 * LA SOLUCIÓN
 * Un fichero de texto con el informe combinado (módulos y addons), que
 * escriben esas herramientas (a mano o desde la tarea semanal), y este
 * módulo, que lo lee cuando entra una cuenta con nivel de GM y se lo enseña
 * como mensajes del sistema unos segundos después de aparecer en el mundo.
 * El módulo no habla con GitHub ni con la red: sólo lee un fichero.
 *
 * Comando ".actualizaciones" para volver a verlo.
 *
 * DÓNDE SE HACE EL TRABAJO
 * OnPlayerLogin apunta; OnUpdate lee el fichero y manda los mensajes.
 * Sin dependencia con ningún módulo.
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Log.h"
#include "ModLocale.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "update_notice_locale.h"
#include "ScriptMgr.h"
#include "TimeMs.h"
#include "WorldSession.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    struct Config
    {
        bool        enabled      = true;
        std::string file         = "updates-pending.txt";
        std::string versionsLock = "versions.lock"; // para avisar si el informe es anterior al ultimo --freeze
        uint32      minSecurity  = 2;
        uint32      delaySecs    = 8;
        uint32      maxLines     = 15;
        uint32      maxDaysStale = 10;
        bool        oncePerSession = false;   // no repetir el aviso por relog en el mismo arranque
        bool        commandOnly    = false;   // sin popup de login; solo .actualizaciones
        bool        sayIfNoneOnLogin = false; // decir "todo al dia" tambien al entrar
        bool        severity       = true;    // ordenar y colorear por [seguridad]/[incompatibilidad]/[funcional]/[informativo]
    };

    // Doble buffer inmutable (T7): se sustituye entera bajo candado, como hace
    // mod-server-help con su Data. ".reload config" y ".actualizaciones" corren
    // los dos en el hilo del mundo (modules/README.md, regla 5; M05,
    // 24/09/2026), así que hoy no compiten; se conserva por prudencia.
    std::mutex                    g_cfgLock;
    std::shared_ptr<Config const> g_cfg = std::make_shared<Config const>();

    std::shared_ptr<Config const> Cfg()
    {
        std::lock_guard<std::mutex> lock(g_cfgLock);
        return g_cfg;
    }

    struct Pending
    {
        ObjectGuid player;
        uint64     dueMs = 0;
    };

    std::mutex          g_pendingLock;
    std::deque<Pending> g_pending;
    std::set<uint32>    g_shownAccounts;   // bajo g_pendingLock (OncePerSession)

    struct Report
    {
        std::vector<std::string> lines;    // todas las lineas utiles, sin aplicar MaxLines (M10)
        time_t                   mtime = 0;
        bool                     exists = false;   // stat() encontro el fichero
        bool                     ok     = false;   // se abrio y se leyo entero
    };

    // Lee el informe. std::ifstream sincrono: se cachea por (ruta, mtime, size)
    // para no reabrirlo en cada login de GM ni en cada ".actualizaciones"
    // (hallazgos-vigilancia-05-09). Se llama desde el hilo del mundo (OnUpdate)
    // y desde el hilo de mapa (comando), asi que la cache va bajo un mutex
    // corto que no reentra en core/playerbots. Devuelve una COPIA.
    //
    // la cache no aplicaba MaxLines en la lectura, asi que subir/bajar
    // UpdateNotice.MaxLines o cambiar de fichero manteniendo tamaño/fecha
    // devolvia el contenido truncado/ajeno cacheado. Ahora se cachea el
    // contenido completo (MaxLines se aplica en Show(), en cada llamada) y la
    // clave de la cache incluye la ruta. Ademas 'ok' distingue una lectura
    // fallida (ifstream no abre pese a existir el fichero) de un informe
    // vacio valido: una lectura fallida no queda marcada como valida ni se
    // reutiliza en la siguiente llamada.
    Report ReadReport(std::string const& file)
    {
        static std::mutex cacheLock;
        static Report     cached;
        static std::string cachedFile;
        static time_t      cachedMtime = -1;
        static off_t       cachedSize  = -1;

        std::lock_guard<std::mutex> lock(cacheLock);

        struct stat st{};
        if (::stat(file.c_str(), &st) != 0)
        {
            cached = Report{};
            cachedFile.clear();
            cachedMtime = -1;
            cachedSize  = -1;
            return cached;
        }

        if (file == cachedFile && st.st_mtime == cachedMtime && st.st_size == cachedSize && cached.ok)
            return cached;

        cachedFile  = file;
        cachedMtime = st.st_mtime;
        cachedSize  = st.st_size;

        cached = Report{};
        cached.exists = true;
        cached.mtime  = st.st_mtime;

        std::ifstream in(file);
        if (!in.is_open())
            return cached;   // exists=true, ok=false: no se cachea como valida

        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            // "#" como comentario aunque venga con sangría (B-B7).
            size_t const firstNonSpace = line.find_first_not_of(" \t");
            if (firstNonSpace == std::string::npos || line[firstNonSpace] == '#')
                continue;
            cached.lines.push_back(line);
        }
        cached.ok = true;
        return cached;
    }

    std::string DateOf(time_t t)
    {
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
        return buf;
    }

    // Severidad de una línea del informe según un prefijo opcional entre
    // corchetes ([seguridad], [incompatibilidad]/[incompat], [funcional],
    // [informativo]/[info]). Sin prefijo -> funcional. Devuelve el peso (0 = más
    // grave) y el color, y deja en 'text' la línea sin el prefijo (CS-4.5).
    int SeverityOf(std::string& text, char const*& color)
    {
        int weight = 2;             // funcional por defecto
        color = "ffcc00";
        if (!text.empty() && text.front() == '[')
        {
            size_t const close = text.find(']');
            if (close != std::string::npos)
            {
                std::string tag = text.substr(1, close - 1);
                for (char& ch : tag) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                bool matched = true;
                if (tag == "seguridad" || tag == "security")            { weight = 0; color = "ff4040"; }
                else if (tag.rfind("incompat", 0) == 0)                 { weight = 1; color = "ff8800"; }
                else if (tag == "funcional" || tag == "functional")     { weight = 2; color = "ffcc00"; }
                else if (tag == "informativo" || tag == "info")         { weight = 3; color = "aaaaaa"; }
                else matched = false;
                if (matched)
                {
                    size_t start = close + 1;
                    while (start < text.size() && (text[start] == ' ' || text[start] == ':'))
                        ++start;
                    text = text.substr(start);
                }
            }
        }
        return weight;
    }

    // Manda el informe al jugador. Devuelve cuántas líneas se mandaron.
    uint32 Show(Player* player, bool sayIfNone)
    {
        if (!player || !player->GetSession())
            return 0;

        ChatHandler handler(player->GetSession());
        auto const c = Cfg();
        Report const report = ReadReport(c->file);

        // un fallo de lectura (el fichero existe pero ifstream no abre,
        // p.ej. permisos) no es "todo al dia": se avisa aparte y no se
        // confunde con un informe vacio valido.
        if (report.exists && !report.ok)
        {
            handler.SendSysMessage(ModLocale::L(handler, "|cffff6060[Actualizaciones]|r No se pudo leer el informe (revisa permisos del fichero)."));
            return 0;
        }

        if (report.lines.empty())
        {
            if (sayIfNone)
                handler.SendSysMessage(ModLocale::L(handler, "|cff00ff00[Actualizaciones]|r Todo al dia: ningun repositorio tiene commits nuevos sobre versions.lock."));
            return 0;
        }

        // MaxLines se aplica aqui, no en la cache (M10): cachear el contenido
        // completo deja que ".reload config" cambie el corte sin esperar a que
        // el fichero cambie de tamaño/fecha.
        uint32 const maxLines = c->maxLines ? c->maxLines : 1;
        uint32 const omitted = report.lines.size() > maxLines
            ? static_cast<uint32>(report.lines.size() - maxLines) : 0;

        handler.SendSysMessage(ModLocale::L(handler, "|cffffcc00[Actualizaciones]|r Hay versiones nuevas rio arriba (tools/revisar-actualizaciones.sh):"));

        // Severidad: separa el prefijo, ordena de más grave a menos y colorea.
        struct Row { std::string text; int weight; char const* color; };
        std::vector<Row> rows;
        rows.reserve(std::min<size_t>(report.lines.size(), maxLines));
        for (std::string line : report.lines)
        {
            if (rows.size() >= maxLines)
                break;
            char const* color = "ffcc00";
            int const weight = c->severity ? SeverityOf(line, color) : 2;
            rows.push_back({ std::move(line), weight, color });
        }
        if (c->severity)
            std::stable_sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) { return a.weight < b.weight; });
        for (Row const& row : rows)
            handler.PSendSysMessage("|cff{}  {}|r", row.color, row.text);

        if (omitted)
            handler.PSendSysMessage(ModLocale::L(handler, "|cffffcc00  ... y {} lineas mas (sube UpdateNotice.MaxLines para verlas).|r"), omitted);

        if (report.mtime)
        {
            uint32 const ageDays = static_cast<uint32>((std::time(nullptr) - report.mtime) / 86400);
            if (c->maxDaysStale && ageDays >= c->maxDaysStale)
                handler.PSendSysMessage(ModLocale::L(handler, "|cffff6060  Informe del {} ({} dias): la revision semanal puede haber fallado.|r"),
                                        DateOf(report.mtime), ageDays);
            else
                handler.PSendSysMessage(ModLocale::L(handler, "|cffffcc00  Informe del {}.|r"), DateOf(report.mtime));

            // Si versions.lock se ha tocado (un --freeze) despues del informe, lo
            // que este lista puede estar ya fijado: conviene rehacerlo.
            struct stat lockSt{};
            if (!c->versionsLock.empty() && ::stat(c->versionsLock.c_str(), &lockSt) == 0
                && lockSt.st_mtime > report.mtime + 60)
                handler.PSendSysMessage(ModLocale::L(handler, "|cffff6060  versions.lock es posterior al informe ({}): puede estar desfasado, "
                                        "vuelve a ejecutar tools/revisar-actualizaciones.sh.|r"), DateOf(lockSt.st_mtime));
        }

        handler.SendSysMessage(ModLocale::L(handler, "|cffffcc00  El servidor NO se ha tocado. Actualizar: ./install.sh --only 3, --from 4, --freeze.|r"));
        return static_cast<uint32>(report.lines.size());
    }
}

class mod_update_notice_world : public WorldScript
{
public:
    mod_update_notice_world() : WorldScript("mod_update_notice_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        auto next = std::make_shared<Config>();
        next->enabled     = sConfigMgr->GetOption<bool>("UpdateNotice.Enable", true);
        next->file        = sConfigMgr->GetOption<std::string>("UpdateNotice.File", "updates-pending.txt");
        next->versionsLock = sConfigMgr->GetOption<std::string>("UpdateNotice.VersionsLockFile", "versions.lock");
        next->severity    = sConfigMgr->GetOption<bool>("UpdateNotice.Severity", true);
        next->minSecurity = std::min<uint32>(sConfigMgr->GetOption<uint32>("UpdateNotice.MinSecurity", 2), SEC_ADMINISTRATOR);
        next->delaySecs   = sConfigMgr->GetOption<uint32>("UpdateNotice.DelaySeconds", 8);
        next->maxLines    = sConfigMgr->GetOption<uint32>("UpdateNotice.MaxLines", 15);
        next->maxDaysStale = sConfigMgr->GetOption<uint32>("UpdateNotice.MaxDaysStale", 10);
        next->oncePerSession = sConfigMgr->GetOption<bool>("UpdateNotice.OncePerSession", false);
        next->commandOnly = sConfigMgr->GetOption<bool>("UpdateNotice.CommandOnly", false);
        next->sayIfNoneOnLogin = sConfigMgr->GetOption<bool>("UpdateNotice.SayIfNoneOnLogin", false);
        if (!next->maxLines)
            next->maxLines = 1;

        std::lock_guard<std::mutex> lock(g_cfgLock);
        g_cfg = std::move(next);
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        auto const c = Cfg();
        if (!c->enabled)
            return;

        uint64_t const now = TimeMs::NowMs();
        std::vector<Pending> due;
        {
            std::lock_guard<std::mutex> lock(g_pendingLock);
            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                if (now >= it->dueMs)
                {
                    due.push_back(*it);
                    it = g_pending.erase(it);
                }
                else
                    ++it;
            }
        }

        for (Pending const& pending : due)
        {
            Player* player = ObjectAccessor::FindPlayer(pending.player);
            if (!player || !player->IsInWorld())
                continue;

            if (c->oncePerSession)
            {
                uint32 const acc = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
                std::lock_guard<std::mutex> lock(g_pendingLock);
                if (!g_shownAccounts.insert(acc).second)
                    continue;   // ya visto en este arranque
            }

            uint32 const shown = Show(player, c->sayIfNoneOnLogin);
            if (shown)
                LOG_INFO("module", "[update-notice] Aviso de actualizaciones mostrado a {} ({} lineas).", player->GetName(), shown);
        }
    }
};

class mod_update_notice_player : public PlayerScript
{
public:
    mod_update_notice_player() : PlayerScript("mod_update_notice_player", { PLAYERHOOK_ON_LOGIN }) { }

    void OnPlayerLogin(Player* player) override
    {
        auto const c = Cfg();
        if (!c->enabled || c->commandOnly || !player)
            return;   // CommandOnly: sin popup de login, ".actualizaciones" sigue

        WorldSession* session = player->GetSession();
        if (!session || session->IsHeadless() || static_cast<uint32>(session->GetSecurity()) < c->minSecurity)
            return;

        // Sin informe no hay nada que enseñar: no encolar (el caso normal).
        struct stat st{};
        if (::stat(c->file.c_str(), &st) != 0 || st.st_size == 0)
            return;

        ObjectGuid const guid = player->GetGUID();
        std::lock_guard<std::mutex> lock(g_pendingLock);
        // Un GM que hace relog varias veces en pocos segundos no debe acumular
        // varios avisos.
        for (Pending const& p : g_pending)
            if (p.player == guid)
                return;
        g_pending.push_back({ guid, TimeMs::NowMs() + TimeMs::SecsToMs(c->delaySecs) });
    }
};

class mod_update_notice_command : public CommandScript
{
public:
    mod_update_notice_command() : CommandScript("mod_update_notice_command") { }

    // Registrado con el nivel más bajo posible (SEC_PLAYER): la única puerta
    // es cfg.minSecurity dentro de HandleShow, la misma que usa el aviso de
    // login. Antes el comando exigía SEC_GAMEMASTER fijo sin volver a mirar
    // la config, así que un UpdateNotice.MinSecurity distinto de 2 hacía que
    // el aviso de login y el comando no coincidieran en quién podía verlo.
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable =
        {
            { "actualizaciones", HandleShow, SEC_PLAYER, Console::No },
        };
        return commandTable;
    }

    static bool HandleShow(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
            return true;
        auto const c = Cfg();
        if (!c->enabled)
        {
            handler->SendSysMessage(ModLocale::L(handler, "mod-update-notice esta desactivado."));
            return true;
        }
        if (static_cast<uint32>(handler->GetSession()->GetSecurity()) < c->minSecurity)
        {
            // return false haria que el nucleo imprima la ayuda/sintaxis del
            // comando, revelando que existe a un jugador raso. Mensaje limpio.
            handler->SendSysMessage(ModLocale::L(handler, "No tienes permiso para ver los avisos de actualizacion."));
            return true;
        }
        Show(player, true);
        return true;
    }
};

void AddSC_mod_update_notice()
{
    ModLocale::Register(UpdateNoticeLocale::kEntries);
    new mod_update_notice_world();
    new mod_update_notice_player();
    new mod_update_notice_command();
}
