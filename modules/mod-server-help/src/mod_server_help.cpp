// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-server-help — la pestaña "Solicitud de ayuda" del cliente como base de
 * conocimiento del servidor: todos los comandos que ESTA cuenta puede usar,
 * con su ayuda, su nivel de permiso y su categoría, más artículos propios.
 *
 * EL PROBLEMA
 * La pestaña "Ayuda básica" (KnowledgeBaseFrame) del cliente 3.3.5a consulta
 * por HTTP los servidores de soporte de Blizzard (support.wow-europe.com/kb/),
 * así que en un servidor privado sólo muestra "no disponible". No existe
 * ningún opcode de juego para ella: no hay nada que implementar del protocolo
 * original. Y el jugador no tiene forma de saber qué comandos existen ni
 * cuáles puede usar (.grupo, .dc, .tokenturnin, .transmog claim...).
 *
 * LA SOLUCIÓN
 * Un comando ".ayuda" que el addon del cliente (cliente/Interface/AddOns/
 * ServerHelp) invoca por el canal de addon que el core ya procesa
 * (AddonChannelCommandHandler, prefijo "AzerothCore": Chat.cpp). El módulo
 * recorre el árbol de comandos con la API pública del core y con la sesión
 * REAL del jugador, así que el filtro de permisos es el mismo que aplica el
 * core al ejecutar: lo que no puede usar no sale del servidor.
 *
 * CÓMO SE OBTIENE CADA DATO SIN TOCAR NADA PRIVADO DEL CORE
 *   - Qué comandos ve la sesión: Acore::ChatCommands::GetAutoCompletionsFor
 *     (devuelve los hijos visibles de una ruta, ya filtrados por permisos).
 *   - Ayuda e "invocable sí/no": Acore::ChatCommands::SendCommandHelpFor con
 *     un ChatHandler derivado que captura SendSysMessage y GetAcoreString
 *     (los dos son virtuales; CliHandler hace lo mismo).
 *   - Nivel exacto de cada comando: el hook
 *     AllCommandScript::OnBeforeIsInvokerVisible(nombre, permisos, quién), que
 *     el core llama con el nombre completo y el RequiredLevel cada vez que
 *     evalúa la visibilidad de un nodo. Aquí sólo se apunta y se devuelve true.
 *
 * DÓNDE SE HACE EL TRABAJO
 * En el propio comando (hilo principal, al procesar CMSG_MESSAGECHAT). Los
 * datos de SQL se cargan al arrancar y con ".ayuda recargar" en una instantánea
 * inmutable (shared_ptr) que se sustituye entera. Sin dependencia con ningún
 * otro módulo: si mañana se añade uno con comandos nuevos, aparecen solos.
 *
 * FORMATO DE LÍNEAS PARA EL ADDON (campos separados por tabulador)
 *   ayuda version              V  hash  reino  entradas  nivel
 *   ayuda indice               C id padre nombre        (categorías usadas)
 *                              E tipo clave cat nivel titulo hot   (A artículo / C comando)
 *                              Z total
 *   ayuda buscar <texto>       R tipo clave ... Z n
 *   ayuda articulo <id>        T titulo · P nivel · G categoria · S sintaxis ·
 *   ayuda comando <ruta>       D linea (varias; "D+" continúa la anterior) ·
 *                              X ejemplo · K keywords · Z
 *   error                      ! mensaje   (y el core responde "f")
 * Tecleado en el chat o en la consola, ".ayuda" responde en texto legible.
 */

#include "AllCommandScript.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Language.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // ─────────────────────────────────────────────────────────────────────────
    // Configuración
    // ─────────────────────────────────────────────────────────────────────────
    struct Config
    {
        bool   enabled           = true;
        uint32 lineBytes         = 230;   // tope por línea: el core limita el mensaje de addon a 255
        uint32 maxSearchResults  = 60;
        uint32 cacheSeconds      = 300;   // vida de la instantánea por cuenta
        uint32 indexCooldown     = 2;     // segundos entre dos "indice" de la misma cuenta
        uint32 maxDepth          = 8;     // profundidad máxima del árbol de comandos
        uint32 catPlayer         = 2;     // categorías por defecto cuando ninguna regla casa
        uint32 catModerator      = 14;
        uint32 catGameMaster     = 13;
        uint32 catAdmin          = 15;
        uint32 catConsole        = 0;     // 0 = usar catAdmin para la consola
        bool   logRequests       = false;
        uint32 snapshotMaxAge    = 3600;  // segundos que se guarda una instantánea sin uso
        uint32 searchMaxBytes    = 64;    // recorte de la consulta de ".ayuda buscar"
        uint32 commandMaxBytes   = 128;   // recorte de la ruta de ".ayuda comando"
        uint32 cutKey            = 90;    // recorte de campos del índice para el addon
        uint32 cutTitle          = 100;
        uint32 cutCategory       = 80;
        std::string addonPrefix  = "AzerothCore"; // prefijo del canal de addon del core
        uint32 rateWindowSecs    = 20;   // ventana del límite de peticiones por cuenta
        uint32 rateMaxPerWindow  = 20;   // peticiones de .ayuda permitidas por ventana (0 = sin límite)
        bool   exportEnable      = true; // ".ayuda export" puede escribir filas auto en server_help_command
    };

    Config cfg;

    // ─────────────────────────────────────────────────────────────────────────
    // Utilidades de texto
    // ─────────────────────────────────────────────────────────────────────────
    std::string Lower(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    // Minúsculas y sin acentos (á é í ó ú ü ñ, en UTF-8), para buscar.
    std::string Fold(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == 0xC3 && i + 1 < s.size())
            {
                unsigned char d = static_cast<unsigned char>(s[i + 1]);
                char mapped = 0;
                switch (d)
                {
                    case 0xA1: case 0x81: mapped = 'a'; break;
                    case 0xA9: case 0x89: mapped = 'e'; break;
                    case 0xAD: case 0x8D: mapped = 'i'; break;
                    case 0xB3: case 0x93: mapped = 'o'; break;
                    case 0xBA: case 0x9A: case 0xBC: case 0x9C: mapped = 'u'; break;
                    case 0xB1: case 0x91: mapped = 'n'; break;
                    default: break;
                }
                if (mapped)
                {
                    out.push_back(mapped);
                    ++i;
                    continue;
                }
            }
            if (c >= 'A' && c <= 'Z')
                out.push_back(static_cast<char>(c - 'A' + 'a'));
            else
                out.push_back(static_cast<char>(c));
        }
        return out;
    }

    std::string Trim(std::string_view s)
    {
        std::size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
            ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
            --e;
        return std::string(s.substr(b, e - b));
    }

    std::vector<std::string> SplitLines(std::string_view s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s)
        {
            if (c == '\r')
                continue;
            if (c == '\n')
            {
                out.push_back(cur);
                cur.clear();
            }
            else
                cur.push_back(c);
        }
        out.push_back(cur);
        while (!out.empty() && Trim(out.back()).empty())
            out.pop_back();
        while (!out.empty() && Trim(out.front()).empty())
            out.erase(out.begin());
        return out;
    }

    // Tabuladores y saltos dentro de un campo romperían el formato de líneas.
    std::string Clean(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            if (c == '\t' || c == '\n' || c == '\r')
                c = ' ';
        return out;
    }

    // Parte una cadena en trozos de como mucho maxBytes sin romper un carácter UTF-8.
    std::vector<std::string> Chunk(std::string_view s, std::size_t maxBytes)
    {
        std::vector<std::string> out;
        if (maxBytes < 8)
            maxBytes = 8;
        std::size_t pos = 0;
        while (pos < s.size())
        {
            std::size_t len = std::min(maxBytes, s.size() - pos);
            if (pos + len < s.size())
            {
                // Retrocede hasta el inicio de un carácter (byte que no sea 10xxxxxx).
                while (len > 1 && (static_cast<unsigned char>(s[pos + len]) & 0xC0) == 0x80)
                    --len;
            }
            out.emplace_back(s.substr(pos, len));
            pos += len;
        }
        if (out.empty())
            out.emplace_back();
        return out;
    }

    // Recorta a maxBytes sin partir un carácter UTF-8 (para títulos en líneas de índice).
    std::string Cut(std::string_view s, std::size_t maxBytes)
    {
        if (s.size() <= maxBytes)
            return std::string(s);
        return Chunk(s, maxBytes).front();
    }

    std::vector<std::string> Tokens(std::string_view s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s)
        {
            if (c == ' ')
            {
                if (!cur.empty())
                {
                    out.push_back(cur);
                    cur.clear();
                }
            }
            else
                cur.push_back(c);
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    // Etiqueta del nivel de permiso.
    std::string LevelName(uint32 level, bool spanish)
    {
        if (level >= rbac::RBAC_PERM_COMMAND_RBAC)
            return (spanish ? "Permiso RBAC " : "RBAC permission ") + std::to_string(level);
        switch (level)
        {
            case SEC_PLAYER:        return spanish ? "Jugador" : "Player";
            case SEC_MODERATOR:     return spanish ? "Moderador" : "Moderator";
            case SEC_GAMEMASTER:    return spanish ? "Game Master" : "Game Master";
            case SEC_ADMINISTRATOR: return spanish ? "Administrador" : "Administrator";
            default:                return spanish ? "Consola" : "Console";
        }
    }

    bool IsSpanish(ChatHandler const& handler)
    {
        WorldSession const* session = const_cast<ChatHandler&>(handler).GetSession();
        if (!session)
            return true;
        // El idioma del CLIENTE, no el de los DBC del servidor: GetSessionDbcLocale() devuelve
        // el de los DBC cargados (enUS en esta instalación) sea cual sea el cliente, así que
        // nunca detectaba español y, con las columnas *_en rellenas, un cliente esES veía inglés.
        LocaleConstant loc = static_cast<LocaleConstant>(session->GetSessionDbLocaleIndex());
        return loc == LOCALE_esES || loc == LOCALE_esMX;
    }

    uint64_t Fnv1a(std::string_view s, uint64_t h)
    {
        for (unsigned char c : s)
        {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Nivel de cada comando, apuntado desde el hook del core
    // ─────────────────────────────────────────────────────────────────────────
    struct Perm
    {
        uint32 level   = 0;
        bool   console = false;
    };

    std::mutex g_permLock;
    std::unordered_map<std::string, Perm> g_perms;   // clave: ruta en minúsculas

    bool FindPerm(std::string const& lowerPath, Perm& out)
    {
        std::lock_guard<std::mutex> lock(g_permLock);
        auto it = g_perms.find(lowerPath);
        if (it == g_perms.end())
            return false;
        out = it->second;
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Datos de SQL (instantánea inmutable)
    // ─────────────────────────────────────────────────────────────────────────
    struct Category
    {
        uint32      id = 0;
        uint32      parent = 0;
        std::string name;
        std::string nameEn;
        int32       sort = 0;
        uint32      minSecurity = 0;
        bool        enabled = true;
    };

    struct Article
    {
        uint32      id = 0;
        uint32      category = 0;
        std::string title;
        std::string titleEn;
        std::string body;
        std::string bodyEn;
        std::string keywords;
        std::string commandPath;   // vacío = artículo libre
        uint32      minSecurity = 0;
        int32       sort = 0;
        bool        enabled = true;
        bool        hot = false;
    };

    struct CommandDoc
    {
        uint32      category = 0;
        std::string title;
        std::string titleEn;
        std::string description;
        std::string descriptionEn;
        std::string syntax;
        std::string examples;
        std::string keywords;
        bool        enabled = true;
        bool        autoRow = false;   // fila generada por ".ayuda export"
    };

    struct Rule
    {
        std::vector<std::string> tokens;   // prefijo en minúsculas, por tokens
        uint32 category = 0;
    };

    struct Data
    {
        std::map<uint32, Category>                  categories;
        std::vector<Article>                        articles;
        std::unordered_map<std::string, CommandDoc> docs;    // clave: ruta en minúsculas
        std::vector<Rule>                           rules;
        uint32                                      loadedAt = 0;
    };

    std::mutex                  g_dataLock;
    std::shared_ptr<Data const> g_data = std::make_shared<Data>();

    std::shared_ptr<Data const> GetData()
    {
        std::lock_guard<std::mutex> lock(g_dataLock);
        return g_data;
    }

    void LoadData()
    {
        auto data = std::make_shared<Data>();
        data->loadedAt = static_cast<uint32>(GameTime::GetGameTime().count());

        if (QueryResult result = WorldDatabase.Query("SELECT id, parent_id, name, name_en, sort, min_security, enabled FROM server_help_category"))
        {
            do
            {
                Field* f = result->Fetch();
                Category c;
                c.id          = f[0].Get<uint32>();
                c.parent      = f[1].Get<uint32>();
                c.name        = f[2].Get<std::string>();
                c.nameEn      = f[3].Get<std::string>();
                c.sort        = f[4].Get<int32>();
                c.minSecurity = f[5].Get<uint8>();
                c.enabled     = f[6].Get<uint8>() != 0;
                data->categories[c.id] = c;
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query("SELECT id, category_id, title, body, keywords, command_path, min_security, sort, enabled, is_hot, title_en, body_en FROM server_help_article"))
        {
            do
            {
                Field* f = result->Fetch();
                Article a;
                a.id          = f[0].Get<uint32>();
                a.category    = f[1].Get<uint32>();
                a.title       = f[2].Get<std::string>();
                a.body        = f[3].Get<std::string>();
                a.keywords    = f[4].Get<std::string>();
                a.commandPath = Lower(Trim(f[5].Get<std::string>()));
                a.minSecurity = f[6].Get<uint8>();
                a.sort        = f[7].Get<int32>();
                a.enabled     = f[8].Get<uint8>() != 0;
                a.hot         = f[9].Get<uint8>() != 0;
                a.titleEn     = f[10].Get<std::string>();
                a.bodyEn      = f[11].Get<std::string>();
                data->articles.push_back(std::move(a));
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query("SELECT command_path, category_id, title, description, syntax, examples, keywords, enabled, title_en, description_en, `auto` FROM server_help_command"))
        {
            do
            {
                Field* f = result->Fetch();
                CommandDoc d;
                std::string path = Lower(Trim(f[0].Get<std::string>()));
                d.category       = f[1].Get<uint32>();
                d.title          = f[2].Get<std::string>();
                d.description    = f[3].Get<std::string>();
                d.syntax         = f[4].Get<std::string>();
                d.examples       = f[5].Get<std::string>();
                d.keywords       = f[6].Get<std::string>();
                d.enabled        = f[7].Get<uint8>() != 0;
                d.titleEn        = f[8].Get<std::string>();
                d.descriptionEn  = f[9].Get<std::string>();
                d.autoRow        = f[10].Get<uint8>() != 0;
                data->docs[path] = std::move(d);
            } while (result->NextRow());
        }

        if (QueryResult result = WorldDatabase.Query("SELECT prefix, category_id FROM server_help_rule ORDER BY sort, id"))
        {
            do
            {
                Field* f = result->Fetch();
                Rule r;
                r.tokens   = Tokens(Lower(Trim(f[0].Get<std::string>())));
                r.category = f[1].Get<uint32>();
                if (!r.tokens.empty())
                    data->rules.push_back(std::move(r));
            } while (result->NextRow());
        }

        {
            std::lock_guard<std::mutex> lock(g_dataLock);
            g_data = data;
        }

        LOG_INFO("module", "[server-help] {} categorias, {} articulos, {} fichas de comando, {} reglas de categoria.",
            data->categories.size(), data->articles.size(), data->docs.size(), data->rules.size());
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Captura de la ayuda del core sin que llegue al chat
    // ─────────────────────────────────────────────────────────────────────────
    class CaptureHandler : public ChatHandler
    {
    public:
        explicit CaptureHandler(WorldSession* session) : ChatHandler(session) { }

        // Centinela: "\x01<id>\x02". Así sabemos qué acore_string se emitió
        // (195 = ayuda genérica, 196 = sin ayuda, 8 = lista de subcomandos).
        std::string GetAcoreString(uint32 entry) const override
        {
            return "\x01" + std::to_string(entry) + "\x02";
        }

        using ChatHandler::SendSysMessage;
        void SendSysMessage(std::string_view str, bool /*escapeCharacters*/) override
        {
            for (std::string const& line : SplitLines(str))
                lines.push_back(line);
        }

        // Con sesión nula (consola) el ChatHandler base callaría los mensajes.
        bool HasSession() const override { return true; }

        std::vector<std::string> lines;
    };

    // Un centinela solo en la línea → id; si no, 0.
    uint32 SentinelId(std::string const& line)
    {
        if (line.size() < 3 || line.front() != '\x01')
            return 0;
        std::size_t end = line.find('\x02');
        if (end == std::string::npos)
            return 0;
        // Puede venir con argumentos formateados detrás (no ocurre con el core,
        // pero por si acaso sólo se mira el principio).
        uint32 id = 0;
        for (std::size_t i = 1; i < end; ++i)
        {
            if (line[i] < '0' || line[i] > '9')
                return 0;
            id = id * 10 + static_cast<uint32>(line[i] - '0');
        }
        return id;
    }

    std::string ResolveSentinels(std::string const& line, WorldSession* session)
    {
        std::string out;
        std::size_t pos = 0;
        while (pos < line.size())
        {
            if (line[pos] == '\x01')
            {
                std::size_t end = line.find('\x02', pos);
                if (end != std::string::npos)
                {
                    uint32 id = 0;
                    bool ok = end > pos + 1;
                    for (std::size_t i = pos + 1; i < end && ok; ++i)
                    {
                        if (line[i] < '0' || line[i] > '9')
                            ok = false;
                        else
                            id = id * 10 + static_cast<uint32>(line[i] - '0');
                    }
                    if (ok)
                    {
                        out += session ? session->GetAcoreString(id) : sObjectMgr->GetAcoreStringForDBCLocale(id);
                        pos = end + 1;
                        continue;
                    }
                }
            }
            out.push_back(line[pos]);
            ++pos;
        }
        return out;
    }

    struct CoreHelp
    {
        bool        exists = false;      // el core respondió algo para esa ruta
        bool        invokable = false;   // la sesión puede EJECUTARLO (no sólo ver hijos)
        std::string syntax;              // línea "Syntax: ..." de la tabla command, si la hay
        std::vector<std::string> description;
    };

    CoreHelp ReadCoreHelp(WorldSession* session, std::string const& path)
    {
        CoreHelp out;
        CaptureHandler capture(session);
        Acore::ChatCommands::SendCommandHelpFor(capture, path);
        if (capture.lines.empty())
            return out;

        uint32 const first = SentinelId(capture.lines[0]);
        if (first == LANG_CMD_INVALID || first == LANG_SUBCMD_INVALID)
            return out;   // no existe o no es visible para esta sesión

        out.exists = true;
        // Sólo contenedor: "Help for 'x'" seguido directamente de la lista de subcomandos.
        if (first == LANG_CMD_HELP_GENERIC && capture.lines.size() >= 2 && SentinelId(capture.lines[1]) == LANG_SUBCMDS_LIST)
            return out;

        out.invokable = true;
        for (std::string const& raw : capture.lines)
        {
            uint32 const id = SentinelId(raw);
            if (id == LANG_SUBCMDS_LIST)
                break;                      // de aquí en adelante, la lista de subcomandos
            if (id == LANG_CMD_HELP_GENERIC || id == LANG_CMD_NO_HELP_AVAILABLE)
                continue;
            std::string line = Trim(ResolveSentinels(raw, session));
            if (out.syntax.empty() && out.description.empty())
            {
                std::string lower = Lower(line);
                if (lower.rfind("syntax:", 0) == 0)
                {
                    out.syntax = Trim(line.substr(7));
                    continue;
                }
                if (lower.rfind("sintaxis:", 0) == 0)
                {
                    out.syntax = Trim(line.substr(9));
                    continue;
                }
            }
            out.description.push_back(line);
        }
        while (!out.description.empty() && out.description.front().empty())
            out.description.erase(out.description.begin());
        while (!out.description.empty() && out.description.back().empty())
            out.description.pop_back();
        return out;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Instantánea por cuenta: lo que ESTA sesión puede ver
    // ─────────────────────────────────────────────────────────────────────────
    struct Entry
    {
        char        kind = 'C';       // 'A' artículo, 'C' comando
        std::string key;              // id del artículo o ruta del comando
        std::string title;            // título de la ficha/artículo (vacío = sólo la ruta)
        uint32      category = 0;
        uint32      level = 0;
        bool        hot = false;
        int32       catSort = 0;
        int32       sort = 0;
        std::string haystack;         // texto plegado para buscar
        // detalle
        std::string syntax;
        std::vector<std::string> description;
        std::string examples;
        std::string keywords;
        std::string body;             // artículos
    };

    struct Snapshot
    {
        std::vector<Entry>  entries;
        std::set<uint32>    categories;   // ids usados (con sus ancestros)
        uint64_t            hash = 0;
        uint32              builtAt = 0;
        uint32              dataLoadedAt = 0;
        uint32              security = 0;
        bool                console = false;
    };

    // Permisos RBAC efectivos de la sesion. Dos cuentas del mismo nivel de
    // seguridad pueden ver arboles de comandos distintos si el servidor
    // concede o retira permisos por cuenta o por grupos enlazados; entonces no
    // deben compartir instantanea (MEJORAS §8.3 / CS-3.3). Consola y sesiones
    // sin RBAC cargado -> conjunto vacio: comparten entre si por nivel, como
    // una cuenta recien creada.
    //
    // se guarda y compara el conjunto de permisos completo (std::set ya
    // ordena e identifica sin ambiguedad), no un hash de 64 bits calculado
    // concatenando decimales sin separador -eso hacia que {1,234} y {12,34}
    // produjeran la misma huella ("1234") y compartieran catalogo pese a
    // representar permisos distintos.
    rbac::RBACPermissionContainer RbacPermissions(WorldSession* session)
    {
        if (!session)
            return {};
        rbac::RBACData* rbacData = session->GetRBACData();
        if (!rbacData)
            return {};
        return rbacData->GetPermissions();
    }

    struct SnapshotKey
    {
        uint32 security = 0;
        bool spanish = true;
        rbac::RBACPermissionContainer rbac;

        bool operator<(SnapshotKey const& other) const
        {
            if (security != other.security) return security < other.security;
            if (spanish != other.spanish)   return spanish < other.spanish;
            return rbac < other.rbac;
        }
    };

    std::mutex g_snapLock;
    std::map<SnapshotKey, std::shared_ptr<Snapshot>> g_snapshots;
    std::unordered_map<uint32, uint32> g_lastIndexAt; // el cooldown sigue siendo por cuenta

    // Ventana deslizante por cuenta para todos los subcomandos de .ayuda
    // (reconstrucciones de índice, búsquedas y envíos repetidos desde el addon).
    struct RateState { uint32 windowStart = 0; uint32 count = 0; };
    std::mutex g_rateLock;
    std::unordered_map<uint32, RateState> g_rate;

    void WalkTree(ChatHandler const& handler, std::string const& path, uint32 depth,
                  std::vector<std::string>& out, std::set<std::string>& seen)
    {
        if (depth > cfg.maxDepth)
            return;
        std::vector<std::string> children = Acore::ChatCommands::GetAutoCompletionsFor(handler, path);
        for (std::string const& child : children)
        {
            // Sólo rutas que amplían la actual: evita bucles si el core devolviera
            // posibilidades por coincidencia parcial.
            if (!path.empty() && (child.size() <= path.size() || Lower(child.substr(0, path.size())) != Lower(path)))
                continue;
            if (!seen.insert(Lower(child)).second)
                continue;
            out.push_back(child);
            WalkTree(handler, child, depth + 1, out, seen);
        }
    }

    bool CategoryVisible(Data const& data, uint32 id, uint32 security, bool console)
    {
        auto it = data.categories.find(id);
        if (it == data.categories.end() || !it->second.enabled)
            return false;
        return console || it->second.minSecurity <= security;
    }

    int32 CategorySort(Data const& data, uint32 id)
    {
        auto it = data.categories.find(id);
        return it == data.categories.end() ? 0 : it->second.sort;
    }

    std::string CategoryName(Data const& data, uint32 id, bool spanish)
    {
        auto it = data.categories.find(id);
        if (it == data.categories.end())
            return spanish ? "Sin categoria" : "Uncategorized";
        if (!spanish && !it->second.nameEn.empty())
            return it->second.nameEn;
        return it->second.name;
    }

    uint32 DefaultCategoryForLevel(uint32 level)
    {
        if (level >= SEC_CONSOLE && cfg.catConsole)
            return cfg.catConsole;   // la consola tiene su propia categoría si se configura
        if (level >= rbac::RBAC_PERM_COMMAND_RBAC || level >= SEC_ADMINISTRATOR)
            return cfg.catAdmin;
        if (level == SEC_GAMEMASTER)
            return cfg.catGameMaster;
        if (level == SEC_MODERATOR)
            return cfg.catModerator;
        return cfg.catPlayer;
    }

    uint32 CategoryByRule(Data const& data, std::vector<std::string> const& pathTokens)
    {
        std::size_t best = 0;
        uint32 category = 0;
        for (Rule const& rule : data.rules)
        {
            if (rule.tokens.size() > pathTokens.size() || rule.tokens.size() <= best)
                continue;
            bool match = true;
            for (std::size_t i = 0; i < rule.tokens.size() && match; ++i)
                match = (rule.tokens[i] == pathTokens[i]);
            if (match)
            {
                best = rule.tokens.size();
                category = rule.category;
            }
        }
        return category;
    }

    std::shared_ptr<Snapshot> BuildSnapshot(ChatHandler& handler)
    {
        auto data = GetData();
        auto snap = std::make_shared<Snapshot>();
        WorldSession* session = handler.GetSession();
        snap->console      = (session == nullptr);
        snap->security     = session ? static_cast<uint32>(session->GetSecurity()) : SEC_CONSOLE;
        snap->builtAt      = static_cast<uint32>(GameTime::GetGameTime().count());
        snap->dataLoadedAt = data->loadedAt;
        bool const spanish = IsSpanish(handler);

        // 1. Comandos: el árbol que el core deja ver a esta sesión.
        std::vector<std::string> paths;
        std::set<std::string> seenPaths;
        WalkTree(handler, "", 0, paths, seenPaths);

        std::set<std::string> invokable;   // rutas en minúsculas que la sesión puede ejecutar
        for (std::string const& path : paths)
        {
            CoreHelp help = ReadCoreHelp(session, path);
            if (!help.exists || !help.invokable)
                continue;

            std::string const lower = Lower(path);
            invokable.insert(lower);

            Entry e;
            e.kind = 'C';
            e.key  = path;

            Perm perm;
            if (FindPerm(lower, perm))
                e.level = perm.level;
            else
                e.level = snap->security;   // no debería pasar: el hook se llama al evaluar la visibilidad

            e.syntax      = help.syntax;
            e.description = help.description;

            auto doc = data->docs.find(lower);
            bool const hasDoc = (doc != data->docs.end() && doc->second.enabled);
            if (hasDoc)
            {
                e.title    = (!spanish && !doc->second.titleEn.empty()) ? doc->second.titleEn : doc->second.title;
                e.keywords = doc->second.keywords;
                e.examples = doc->second.examples;
                if (!doc->second.syntax.empty())
                    e.syntax = doc->second.syntax;
                std::string const& desc = (!spanish && !doc->second.descriptionEn.empty())
                    ? doc->second.descriptionEn : doc->second.description;
                if (!desc.empty())
                    e.description = SplitLines(desc);
                e.category = doc->second.category;
            }
            if (!e.category || !CategoryVisible(*data, e.category, snap->security, snap->console))
                e.category = CategoryByRule(*data, Tokens(lower));
            if (!e.category || !CategoryVisible(*data, e.category, snap->security, snap->console))
                e.category = DefaultCategoryForLevel(e.level);
            e.catSort = CategorySort(*data, e.category);

            std::string hay = "." + lower + " " + Fold(e.title) + " " + Fold(e.keywords) + " " + Fold(CategoryName(*data, e.category, spanish));
            for (std::string const& line : e.description)
            {
                hay.push_back(' ');
                hay += Fold(line);
            }
            e.haystack = std::move(hay);
            snap->entries.push_back(std::move(e));
        }

        // 2. Artículos.
        for (Article const& a : data->articles)
        {
            if (!a.enabled)
                continue;
            if (!snap->console && a.minSecurity > snap->security)
                continue;
            if (!CategoryVisible(*data, a.category, snap->security, snap->console))
                continue;
            if (!a.commandPath.empty() && !invokable.count(a.commandPath))
                continue;   // ligado a un comando que esta sesión no puede usar

            Entry e;
            e.kind     = 'A';
            e.key      = std::to_string(a.id);
            e.title    = (!spanish && !a.titleEn.empty()) ? a.titleEn : a.title;
            e.category = a.category;
            e.level    = a.minSecurity;
            e.hot      = a.hot;
            e.sort     = a.sort;
            e.catSort  = CategorySort(*data, a.category);
            e.keywords = a.keywords;
            e.body     = (!spanish && !a.bodyEn.empty()) ? a.bodyEn : a.body;
            e.haystack = Fold(a.title) + " " + Fold(a.keywords) + " " + Fold(a.body) + " " + Fold(CategoryName(*data, a.category, spanish));
            if (!a.commandPath.empty())
                e.haystack += " ." + a.commandPath;
            snap->entries.push_back(std::move(e));
        }

        // 3. Orden: categoría, artículos antes que comandos, sort, título/ruta.
        std::sort(snap->entries.begin(), snap->entries.end(), [](Entry const& x, Entry const& y)
        {
            if (x.catSort != y.catSort) return x.catSort < y.catSort;
            if (x.category != y.category) return x.category < y.category;
            if (x.kind != y.kind) return x.kind == 'A';
            if (x.sort != y.sort) return x.sort < y.sort;
            return Lower(x.kind == 'A' ? x.title : x.key) < Lower(y.kind == 'A' ? y.title : y.key);
        });

        // 4. Categorías usadas, con sus ancestros, y el hash del índice.
        uint64_t h = 1469598103934665603ULL;
        h = Fnv1a(std::to_string(data->loadedAt), h);
        h = Fnv1a(std::to_string(snap->security), h);
        h = Fnv1a(spanish ? "es" : "en", h);
        for (Entry const& e : snap->entries)
        {
            uint32 cat = e.category;
            for (uint32 guard = 0; cat && guard < 16; ++guard)
            {
                snap->categories.insert(cat);
                auto it = data->categories.find(cat);
                cat = (it == data->categories.end()) ? 0 : it->second.parent;
            }
            h = Fnv1a(std::string(1, e.kind), h);
            h = Fnv1a(e.key, h);
            h = Fnv1a(e.title, h);
            h = Fnv1a(std::to_string(e.category), h);
            h = Fnv1a(std::to_string(e.level), h);
        }
        snap->hash = h;
        return snap;
    }

    std::shared_ptr<Snapshot> GetSnapshot(ChatHandler& handler, bool force)
    {
        WorldSession* session = handler.GetSession();
        uint32 const now      = static_cast<uint32>(GameTime::GetGameTime().count());
        uint32 const security = session ? static_cast<uint32>(session->GetSecurity()) : SEC_CONSOLE;
        SnapshotKey const key { security, IsSpanish(handler), RbacPermissions(session) };
        auto data = GetData();

        {
            std::lock_guard<std::mutex> lock(g_snapLock);
            // Una entrada por (nivel, idioma, huella RBAC); se retiran las
            // antiguas para reconstruir tras el TTL.
            for (auto it = g_snapshots.begin(); it != g_snapshots.end();)
            {
                if (now - it->second->builtAt > cfg.snapshotMaxAge)
                    it = g_snapshots.erase(it);
                else
                    ++it;
            }
            // El cooldown de índice por cuenta: misma poda por TTL.
            for (auto it = g_lastIndexAt.begin(); it != g_lastIndexAt.end();)
            {
                if (now - it->second > cfg.snapshotMaxAge)
                    it = g_lastIndexAt.erase(it);
                else
                    ++it;
            }
            auto it = g_snapshots.find(key);
            if (!force && it != g_snapshots.end())
            {
                Snapshot const& s = *it->second;
                if (s.security == security && s.dataLoadedAt == data->loadedAt && now - s.builtAt <= cfg.cacheSeconds)
                    return it->second;
            }
        }

        auto snap = BuildSnapshot(handler);
        {
            std::lock_guard<std::mutex> lock(g_snapLock);
            g_snapshots[key] = snap;
        }
        return snap;
    }

    bool IndexOnCooldown(ChatHandler& handler, uint32 now)
    {
        WorldSession* session = handler.GetSession();
        uint32 const account = session ? session->GetAccountId() : 0;
        std::lock_guard<std::mutex> lock(g_snapLock);
        uint32& last = g_lastIndexAt[account];
        if (last && now - last < cfg.indexCooldown)
            return true;
        last = now;
        return false;
    }

    // Ventana deslizante por cuenta, común a todos los subcomandos de .ayuda.
    // Cuenta la petición ANTES de servirla: si devuelve true, no se sirve.
    bool RateLimited(uint32 account, uint32 now)
    {
        if (!cfg.rateMaxPerWindow || !account)   // 0 = sin límite; la consola no cuenta
            return false;
        std::lock_guard<std::mutex> lock(g_rateLock);
        RateState& s = g_rate[account];
        if (!s.windowStart || now - s.windowStart >= cfg.rateWindowSecs)
        {
            s.windowStart = now;
            s.count = 1;
            return false;
        }
        if (s.count >= cfg.rateMaxPerWindow)
            return true;
        ++s.count;
        return false;
    }

    // Comprueba el límite y, si se ha pasado, responde. Devuelve true si se ha
    // rechazado; el handler hace entonces `return handler->IsHumanReadable()`:
    // para un humano ya se envió un mensaje limpio (true = comando atendido),
    // para el addon un error (false = el core lo enruta como fallo, como el
    // cooldown del índice).
    bool Throttled(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        if (!RateLimited(session ? session->GetAccountId() : 0,
                         static_cast<uint32>(GameTime::GetGameTime().count())))
            return false;
        if (handler->IsHumanReadable())
            handler->SendSysMessage(IsSpanish(*handler) ? "Demasiadas peticiones de ayuda: espera unos segundos."
                                                        : "Too many help requests: wait a few seconds.");
        else
            handler->SendErrorMessage("!\trate", false);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Salida
    // ─────────────────────────────────────────────────────────────────────────
    void Emit(ChatHandler* handler, std::string const& line)
    {
        for (std::string const& part : Chunk(line, cfg.lineBytes))
            handler->SendSysMessage(part);
    }

    // Una línea del formato del addon; los campos largos se trocean con "D+".
    void EmitField(ChatHandler* handler, char tag, std::string const& value)
    {
        std::string const head = std::string(1, tag) + "\t";
        std::size_t const room = cfg.lineBytes > head.size() + 8 ? cfg.lineBytes - head.size() - 1 : 8;
        std::vector<std::string> parts = Chunk(Clean(value), room);
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i == 0)
                handler->SendSysMessage(head + parts[i]);
            else
                handler->SendSysMessage(std::string(1, tag) + "+\t" + parts[i]);
        }
    }

    std::string Hex(uint64_t v)
    {
        static char const* digits = "0123456789abcdef";
        std::string out(16, '0');
        for (int i = 15; i >= 0; --i)
        {
            out[i] = digits[v & 0xF];
            v >>= 4;
        }
        return out;
    }

    Entry const* FindEntry(Snapshot const& snap, char kind, std::string const& key)
    {
        std::string const lowerKey = Lower(key);
        for (Entry const& e : snap.entries)
            if (e.kind == kind && Lower(e.key) == lowerKey)
                return &e;
        return nullptr;
    }

    void SendDetail(ChatHandler* handler, Entry const& e, Data const& data)
    {
        bool const spanish = IsSpanish(*handler);
        bool const human   = handler->IsHumanReadable();
        std::string const level = LevelName(e.level, spanish);
        std::string const cat   = CategoryName(data, e.category, spanish);

        if (human)
        {
            if (e.kind == 'C')
            {
                handler->PSendSysMessage("|cffffd100.{}|r{}", e.key, e.title.empty() ? "" : " - " + e.title);
                handler->PSendSysMessage("  {}: {}   {}: {}", spanish ? "Permiso" : "Permission", level, spanish ? "Categoria" : "Category", cat);
                if (!e.syntax.empty())
                    handler->PSendSysMessage("  {}: {}", spanish ? "Uso" : "Usage", e.syntax);
                for (std::string const& line : e.description)
                    handler->PSendSysMessage("  {}", line);
                if (!e.examples.empty())
                {
                    handler->SendSysMessage(spanish ? "  Ejemplos:" : "  Examples:");
                    for (std::string const& line : SplitLines(e.examples))
                        handler->PSendSysMessage("    {}", line);
                }
            }
            else
            {
                handler->PSendSysMessage("|cffffd100{}|r  [{}]", e.title, cat);
                for (std::string const& line : SplitLines(e.body))
                    handler->PSendSysMessage("  {}", line);
            }
            return;
        }

        EmitField(handler, 'T', e.title);
        EmitField(handler, 'P', level);
        EmitField(handler, 'G', cat);
        if (e.kind == 'C')
        {
            EmitField(handler, 'S', e.syntax);
            for (std::string const& line : e.description)
                EmitField(handler, 'D', line);
            for (std::string const& line : SplitLines(e.examples))
                EmitField(handler, 'X', line);
        }
        else
        {
            for (std::string const& line : SplitLines(e.body))
                EmitField(handler, 'D', line);
        }
        EmitField(handler, 'K', e.keywords);
        handler->SendSysMessage("Z");
    }

    bool NotFound(ChatHandler* handler)
    {
        // Mismo texto exista o no: no se revela nada de lo que no se puede usar.
        bool const spanish = IsSpanish(*handler);
        if (handler->IsHumanReadable())
            handler->SendErrorMessage(spanish ? "No existe o no tienes permiso para usarlo." : "It does not exist or you may not use it.");
        else
            handler->SendErrorMessage(std::string("!\t") + (spanish ? "No existe o no tienes permiso para usarlo." : "It does not exist or you may not use it."), false);
        return false;
    }

    bool Disabled(ChatHandler* handler)
    {
        if (handler->IsHumanReadable())
            handler->SendErrorMessage("mod-server-help esta desactivado.");
        else
            handler->SendErrorMessage("!\tmod-server-help esta desactivado.", false);
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Scripts
// ─────────────────────────────────────────────────────────────────────────────
class mod_server_help_world : public WorldScript
{
public:
    mod_server_help_world() : WorldScript("mod_server_help_world", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        cfg.enabled          = sConfigMgr->GetOption<bool>("ServerHelp.Enable", true);
        cfg.lineBytes        = sConfigMgr->GetOption<uint32>("ServerHelp.LineBytes", 230);
        cfg.maxSearchResults = sConfigMgr->GetOption<uint32>("ServerHelp.MaxSearchResults", 60);
        cfg.cacheSeconds     = sConfigMgr->GetOption<uint32>("ServerHelp.CacheSeconds", 300);
        cfg.indexCooldown    = sConfigMgr->GetOption<uint32>("ServerHelp.IndexCooldownSeconds", 2);
        cfg.maxDepth         = sConfigMgr->GetOption<uint32>("ServerHelp.MaxDepth", 8);
        cfg.catPlayer        = sConfigMgr->GetOption<uint32>("ServerHelp.DefaultCategory.Player", 2);
        cfg.catModerator     = sConfigMgr->GetOption<uint32>("ServerHelp.DefaultCategory.Moderator", 14);
        cfg.catGameMaster    = sConfigMgr->GetOption<uint32>("ServerHelp.DefaultCategory.GameMaster", 13);
        cfg.catAdmin         = sConfigMgr->GetOption<uint32>("ServerHelp.DefaultCategory.Administrator", 15);
        cfg.catConsole       = sConfigMgr->GetOption<uint32>("ServerHelp.DefaultCategory.Console", 0);
        cfg.logRequests      = sConfigMgr->GetOption<bool>("ServerHelp.LogRequests", false);
        cfg.snapshotMaxAge   = std::max<uint32>(sConfigMgr->GetOption<uint32>("ServerHelp.SnapshotMaxAge", 3600), 60);
        cfg.searchMaxBytes   = std::max<uint32>(sConfigMgr->GetOption<uint32>("ServerHelp.SearchQueryMaxBytes", 64), 8);
        cfg.commandMaxBytes  = std::max<uint32>(sConfigMgr->GetOption<uint32>("ServerHelp.CommandQueryMaxBytes", 128), 8);
        cfg.cutKey           = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("ServerHelp.IndexCutBytes.Key", 90), 16, 100);
        cfg.cutTitle         = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("ServerHelp.IndexCutBytes.Title", 100), 16, 100);
        cfg.cutCategory      = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("ServerHelp.IndexCutBytes.Category", 80), 16, 100);
        cfg.addonPrefix      = sConfigMgr->GetOption<std::string>("ServerHelp.AddonPrefix", "AzerothCore");
        if (cfg.addonPrefix.empty())
            cfg.addonPrefix = "AzerothCore";
        cfg.rateWindowSecs   = std::max<uint32>(sConfigMgr->GetOption<uint32>("ServerHelp.RateWindowSeconds", 20), 1);
        cfg.rateMaxPerWindow = sConfigMgr->GetOption<uint32>("ServerHelp.RateMaxPerWindow", 20);
        cfg.exportEnable     = sConfigMgr->GetOption<bool>("ServerHelp.Export", true);
        // Suelo 230: las líneas C/E del índice se trocean con Emit() SIN marcador
        // de continuación (a diferencia de EmitField), y con los campos ya
        // recortados a ≤100 bytes cada uno una línea E llega a ~222 bytes. Por
        // debajo de eso el addon recibiría una línea rota + un fragmento que no
        // sabe parsear (§8.2 2.1 / CS-3.1). El cap de arriba (255 menos el
        // marco del canal de addon) es mayor, así que el rango queda [230, cap].
        if (cfg.lineBytes < 230) cfg.lineBytes = 230;
        // 255 (tope del mensaje de addon del core) menos "<prefijo>\tm0000".
        uint32 const lineCap = 255 > cfg.addonPrefix.size() + 6 ? 255 - static_cast<uint32>(cfg.addonPrefix.size()) - 6 : 230;
        if (cfg.lineBytes > lineCap) cfg.lineBytes = std::max<uint32>(lineCap, 230);
        if (!cfg.maxSearchResults) cfg.maxSearchResults = 20;

        // Un ".reload config" que reactive el modulo (estaba apagado al
        // arrancar, o se apago y encendio) dejaba g_data vacio hasta que
        // alguien hiciera ".ayuda recargar" (T6). OnStartup ya carga en el
        // arranque normal.
        if (reload && cfg.enabled && GetData()->loadedAt == 0)
            LoadData();
    }

    void OnStartup() override
    {
        if (!cfg.enabled)
        {
            LOG_INFO("module", "[server-help] Desactivado (ServerHelp.Enable = 0).");
            return;
        }
        LoadData();
    }
};

// Apunta el nivel de cada comando cuando el core evalúa su visibilidad.
class mod_server_help_perms : public AllCommandScript
{
public:
    mod_server_help_perms() : AllCommandScript("mod_server_help_perms", { ALLCOMMANDHOOK_ON_BEFORE_IS_INVOKER_VISIBLE }) { }

    bool OnBeforeIsInvokerVisible(std::string name, Acore::Impl::ChatCommands::CommandPermissions permissions, ChatHandler const& /*who*/) override
    {
        // El core evalúa la visibilidad de cada nodo desde los 4 hilos de mapa,
        // así que este candado global está en un camino relativamente caliente
        // (§8.2 2.11). Sección crítica mínima —dos asignaciones en un mapa— y NO
        // reentra en el core ni en playerbots, a diferencia de
        // [cerrojos-reentrantes-con-hooks]. Un shared_mutex no ayudaría: el hook
        // siempre escribe.
        std::lock_guard<std::mutex> lock(g_permLock);
        Perm& p   = g_perms[Lower(name)];
        p.level   = permissions.RequiredLevel;
        p.console = (permissions.AllowConsole == Console::Yes);
        return true;   // no se altera la decisión del core
    }
};

class mod_server_help_command : public CommandScript
{
public:
    mod_server_help_command() : CommandScript("mod_server_help_command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable ayudaTable =
        {
            { "version",  HandleVersion, SEC_PLAYER,        Console::Yes },
            { "indice",   HandleIndex,   SEC_PLAYER,        Console::Yes },
            { "buscar",   HandleSearch,  SEC_PLAYER,        Console::Yes },
            { "articulo", HandleArticle, SEC_PLAYER,        Console::Yes },
            { "comando",  HandleCommand, SEC_PLAYER,        Console::Yes },
            { "cobertura",HandleCoverage,SEC_ADMINISTRATOR, Console::Yes },
            { "export",   HandleExport,  SEC_ADMINISTRATOR, Console::Yes },
            { "recargar", HandleReload,  SEC_ADMINISTRATOR, Console::Yes },
            { "",         HandleRoot,    SEC_PLAYER,        Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "ayuda", ayudaTable },
        };
        return commandTable;
    }

    static void Log(ChatHandler* handler, char const* what, std::string_view arg)
    {
        if (!cfg.logRequests)
            return;
        WorldSession* s = handler->GetSession();
        // Clean() quita \t\n\r: un salto de linea en el argumento (posible desde
        // consola en ".ayuda comando") inyectaria una linea falsa en Server.log.
        LOG_INFO("module", "[server-help] {} pide '{}' '{}' (cuenta {}, nivel {})",
            s && s->GetPlayer() ? s->GetPlayer()->GetName() : "consola", what, Clean(arg),
            s ? s->GetAccountId() : 0, s ? static_cast<uint32>(s->GetSecurity()) : 4u);
    }

    static bool HandleRoot(ChatHandler* handler)
    {
        if (!cfg.enabled)
            return Disabled(handler);   // ".ayuda" a secas también respeta el interruptor (§8.2 2.8)
        bool const spanish = IsSpanish(*handler);
        if (spanish)
        {
            handler->SendSysMessage("|cffffd100.ayuda|r - base de conocimiento del servidor (la usa el addon ServerHelp).");
            handler->SendSysMessage("  .ayuda buscar <texto>   busca en comandos y articulos que puedes usar");
            handler->SendSysMessage("  .ayuda comando <ruta>   ficha de un comando (p. ej. .ayuda comando grupo banda)");
            handler->SendSysMessage("  .ayuda articulo <id>    un articulo");
            handler->SendSysMessage("  .ayuda indice           todo lo visible para tu cuenta");
        }
        else
        {
            handler->SendSysMessage("|cffffd100.ayuda|r - server knowledge base (used by the ServerHelp addon).");
            handler->SendSysMessage("  .ayuda buscar <text>    search commands and articles you may use");
            handler->SendSysMessage("  .ayuda comando <path>   command sheet (e.g. .ayuda comando grupo banda)");
            handler->SendSysMessage("  .ayuda articulo <id>    an article");
            handler->SendSysMessage("  .ayuda indice           everything visible to your account");
        }
        return true;
    }

    static bool HandleVersion(ChatHandler* handler)
    {
        if (!cfg.enabled)
            return Disabled(handler);
        Log(handler, "version", "");
        auto snap = GetSnapshot(*handler, false);
        if (handler->IsHumanReadable())
            handler->PSendSysMessage("[server-help] indice {} · {} entradas · nivel {}", Hex(snap->hash), snap->entries.size(), snap->security);
        else
            handler->SendSysMessage(Acore::StringFormat("V\t{}\t{}\t{}\t{}", Hex(snap->hash), Clean(sWorld->GetRealmName()), snap->entries.size(), snap->security));
        return true;
    }

    static bool HandleIndex(ChatHandler* handler)
    {
        if (!cfg.enabled)
            return Disabled(handler);
        if (Throttled(handler))
            return handler->IsHumanReadable();   // humano: mensaje limpio (true); addon: fallo (false)
        // Cooldown ANTES de construir la instantánea (§8.3 #9): reconstruirla
        // es lo caro, y el addon la pide en ráfaga al abrir la ventana.
        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());
        if (!handler->IsHumanReadable() && IndexOnCooldown(*handler, now))
        {
            handler->SendErrorMessage("!\tcooldown", false);
            return false;
        }
        Log(handler, "indice", "");
        auto snap = GetSnapshot(*handler, false);
        auto data = GetData();
        bool const spanish = IsSpanish(*handler);

        if (handler->IsHumanReadable())
        {
            uint32 lastCat = 0xFFFFFFFF;
            for (Entry const& e : snap->entries)
            {
                if (e.category != lastCat)
                {
                    lastCat = e.category;
                    handler->PSendSysMessage("|cff00ff00== {} ==|r", CategoryName(*data, e.category, spanish));
                }
                if (e.kind == 'C')
                    handler->PSendSysMessage("  .{}{}  |cff808080[{}]|r", e.key, e.title.empty() ? "" : " - " + e.title, LevelName(e.level, spanish));
                else
                    handler->PSendSysMessage("  #{} {}", e.key, e.title);
            }
            handler->PSendSysMessage("{} entradas.", snap->entries.size());
            return true;
        }

        // Categorías: en orden de sort, padres antes que hijos (por id de padre 0 primero).
        std::vector<Category const*> cats;
        for (uint32 id : snap->categories)
        {
            auto it = data->categories.find(id);
            if (it != data->categories.end())
                cats.push_back(&it->second);
        }
        std::sort(cats.begin(), cats.end(), [](Category const* a, Category const* b)
        {
            if (a->parent != b->parent) return a->parent < b->parent;
            if (a->sort != b->sort) return a->sort < b->sort;
            return a->id < b->id;
        });
        for (Category const* c : cats)
            Emit(handler, Acore::StringFormat("C\t{}\t{}\t{}", c->id, c->parent, Cut(Clean((!spanish && !c->nameEn.empty()) ? c->nameEn : c->name), cfg.cutCategory)));

        for (Entry const& e : snap->entries)
            Emit(handler, Acore::StringFormat("E\t{}\t{}\t{}\t{}\t{}\t{}", e.kind, Cut(Clean(e.key), cfg.cutKey), e.category, e.level, Cut(Clean(e.title), cfg.cutTitle), e.hot ? 1 : 0));

        handler->SendSysMessage(Acore::StringFormat("Z\t{}", snap->entries.size()));
        return true;
    }

    static bool HandleSearch(ChatHandler* handler, Tail text)
    {
        if (!cfg.enabled)
            return Disabled(handler);
        if (Throttled(handler))
            return handler->IsHumanReadable();   // humano: mensaje limpio (true); addon: fallo (false)
        std::string query = Fold(Trim(text));
        for (char& c : query)
            if (static_cast<unsigned char>(c) < 0x20)
                c = ' ';
        if (query.size() > cfg.searchMaxBytes)
        {
            query.resize(cfg.searchMaxBytes);
            // Fold deja en ASCII los acentos, pero un carácter multibyte no
            // mapeado (emoji, CJK) puede partirse en el byte 64: se recorta a la
            // frontera de carácter UTF-8 anterior (§8.2 2.13).
            while (!query.empty() && (static_cast<unsigned char>(query.back()) & 0xC0) == 0x80)
                query.pop_back();
            if (!query.empty() && (static_cast<unsigned char>(query.back()) & 0x80))
                query.pop_back();
        }
        Log(handler, "buscar", query);
        if (query.empty())
            return HandleRoot(handler);

        auto snap = GetSnapshot(*handler, false);
        auto data = GetData();
        bool const spanish = IsSpanish(*handler);
        std::vector<std::string> const words = Tokens(query);

        struct Hit { int rank; Entry const* e; };
        std::vector<Hit> hits;
        for (Entry const& e : snap->entries)
        {
            bool all = true;
            for (std::string const& w : words)
                if (e.haystack.find(w) == std::string::npos) { all = false; break; }
            if (!all)
                continue;
            std::string const primary = (e.kind == 'C') ? "." + Lower(e.key) : Fold(e.title);
            int rank = 2;
            if (primary.rfind(query, 0) == 0 || primary.rfind("." + query, 0) == 0)
                rank = 0;
            else if (primary.find(query) != std::string::npos || Fold(e.title).find(query) != std::string::npos)
                rank = 1;
            hits.push_back({ rank, &e });
        }
        std::stable_sort(hits.begin(), hits.end(), [](Hit const& a, Hit const& b) { return a.rank < b.rank; });
        if (hits.size() > cfg.maxSearchResults)
            hits.resize(cfg.maxSearchResults);

        if (handler->IsHumanReadable())
        {
            if (hits.empty())
                handler->SendSysMessage(spanish ? "Sin resultados." : "No results.");
            for (Hit const& h : hits)
            {
                if (h.e->kind == 'C')
                    handler->PSendSysMessage("  .{}{}  |cff808080[{} · {}]|r", h.e->key, h.e->title.empty() ? "" : " - " + h.e->title, CategoryName(*data, h.e->category, spanish), LevelName(h.e->level, spanish));
                else
                    handler->PSendSysMessage("  #{} {}  |cff808080[{}]|r", h.e->key, h.e->title, CategoryName(*data, h.e->category, spanish));
            }
            return true;
        }

        for (Hit const& h : hits)
            Emit(handler, Acore::StringFormat("R\t{}\t{}", h.e->kind, Clean(h.e->key)));
        handler->SendSysMessage(Acore::StringFormat("Z\t{}", hits.size()));
        return true;
    }

    static bool HandleArticle(ChatHandler* handler, uint32 id)
    {
        if (!cfg.enabled)
            return Disabled(handler);
        if (Throttled(handler))
            return handler->IsHumanReadable();   // humano: mensaje limpio (true); addon: fallo (false)
        Log(handler, "articulo", std::to_string(id));
        auto snap = GetSnapshot(*handler, false);
        Entry const* e = FindEntry(*snap, 'A', std::to_string(id));
        if (!e)
            return NotFound(handler);
        SendDetail(handler, *e, *GetData());
        return true;
    }

    static bool HandleCommand(ChatHandler* handler, Tail path)
    {
        if (!cfg.enabled)
            return Disabled(handler);
        if (Throttled(handler))
            return handler->IsHumanReadable();   // humano: mensaje limpio (true); addon: fallo (false)
        std::string clean = Trim(path);
        while (!clean.empty() && clean.front() == '.')
            clean.erase(clean.begin());
        // Acota la entrada: HandleSearch recorta y HandleArticle recibe un
        // uint32, pero aqui el tail llegaba sin limite ni de longitud ni de
        // tokens a ReadCoreHelp/SendCommandHelpFor y a FindEntry (bucle lineal).
        if (clean.size() > cfg.commandMaxBytes)
            clean.resize(cfg.commandMaxBytes);
        // Quita cualquier carácter de control (un '\n' desde consola sobrevivía a
        // Tokens y llegaba a ReadCoreHelp y al log — CS-3.1).
        for (char& c : clean)
            if (static_cast<unsigned char>(c) < 0x20)
                c = ' ';
        // Colapsa espacios repetidos y limita la profundidad a cfg.maxDepth.
        std::string norm;
        uint32 depth = 0;
        for (std::string const& t : Tokens(clean))
        {
            if (++depth > cfg.maxDepth)
                break;
            if (!norm.empty())
                norm.push_back(' ');
            norm += t;
        }
        Log(handler, "comando", norm);
        if (norm.empty())
            return NotFound(handler);

        auto snap = GetSnapshot(*handler, false);
        Entry const* e = FindEntry(*snap, 'C', norm);
        if (!e)
        {
            // Segunda oportunidad con el core, por si la instantánea es anterior a un cambio.
            CoreHelp help = ReadCoreHelp(handler->GetSession(), norm);
            if (!help.exists || !help.invokable)
                return NotFound(handler);
            snap = GetSnapshot(*handler, true);
            e = FindEntry(*snap, 'C', norm);
            if (!e)
                return NotFound(handler);
        }
        SendDetail(handler, *e, *GetData());
        return true;
    }

    // ".ayuda cobertura" — cuántos comandos del árbol tienen ficha propia.
    static bool HandleCoverage(ChatHandler* handler)
    {
        if (!cfg.enabled)
            return Disabled(handler);
        auto data = GetData();
        auto snap = GetSnapshot(*handler, true);
        uint32 total = 0, withDoc = 0, withRule = 0, bare = 0;
        std::vector<std::string> uncovered;
        for (Entry const& e : snap->entries)
        {
            if (e.kind != 'C')
                continue;
            ++total;
            std::string const lower = Lower(e.key);
            auto doc = data->docs.find(lower);
            if (doc != data->docs.end() && doc->second.enabled && !doc->second.autoRow)
            {
                ++withDoc;
                continue;
            }
            if (CategoryByRule(*data, Tokens(lower)))
            {
                ++withRule;
                if (uncovered.size() < 40)
                    uncovered.push_back(lower);
            }
            else
            {
                ++bare;
                if (uncovered.size() < 40)
                    uncovered.push_back(lower + " (sin regla)");
            }
        }
        handler->PSendSysMessage("[server-help] Cobertura: {} comandos visibles, {} con ficha propia, "
                                 "{} solo por regla de categoria, {} sin nada.", total, withDoc, withRule, bare);
        if (!uncovered.empty())
        {
            handler->SendSysMessage("Sin ficha propia (hasta 40):");
            for (std::string const& s : uncovered)
                handler->PSendSysMessage("  {}", s);
        }
        return true;
    }

    // ".ayuda export" — vuelca los comandos descubiertos que no tienen ficha
    // propia como filas auto=1 en server_help_command, para que el panel web
    // (que lee esa tabla) vea el árbol completo, no solo las fichas curadas.
    static bool HandleExport(ChatHandler* handler)
    {
        if (!cfg.enabled)
            return Disabled(handler);
        if (!cfg.exportEnable)
        {
            handler->SendSysMessage("[server-help] ServerHelp.Export = 0: exportacion desactivada.");
            return true;
        }

        auto data = GetData();
        auto snap = GetSnapshot(*handler, true);

        // proteger TODA fila de origen manual (fichas curadas), este
        // habilitada o no. Antes sólo se protegían las habilitadas
        // (doc->second.enabled && !doc->second.autoRow); una ficha curada
        // deshabilitada no cumplía esa condición, así que el UPSERT de abajo
        // la reescribía con `auto` = 1 y la exportación SIGUIENTE la borraba
        // con el DELETE de aquí abajo, perdiendo el título/descripción
        // curados para siempre.
        //
        // DELETE + INSERTs van en una única transacción: si algo falla a
        // media exportación, MySQL no deja el catálogo automático parcialmente
        // reemplazado. El `IF(\`auto\` = 1, ...)` en el UPSERT es cinturón y
        // tirantes: aunque la caché en memoria (data->docs) estuviera desfasada
        // respecto a la base, nunca se toca una fila que ya sea manual en la
        // base real. Y en vez de dar el número de filas escritas por
        // supuesto, se confirma con una relectura real tras el commit.
        WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
        trans->Append("DELETE FROM server_help_command WHERE `auto` = 1");

        // `snap->entries` puede repetir el mismo comando en minúsculas más de
        // una vez (p.ej. un alias del árbol del core): sin deduplicar,
        // `attempted` contaba dos INSERT para la misma fila (la segunda sólo
        // reafirma la primera vía ON DUPLICATE KEY), inflando el recuento.
        uint32 attempted = 0;
        std::set<std::string> intentados;
        for (Entry const& e : snap->entries)
        {
            if (e.kind != 'C')
                continue;
            std::string const lower = Lower(e.key);
            auto doc = data->docs.find(lower);
            if (doc != data->docs.end() && !doc->second.autoRow)
                continue;   // ficha curada: protegida siempre, esté habilitada o no
            if (!intentados.insert(lower).second)
                continue;   // ya intentada en esta misma exportación

            std::string path = lower;
            std::string syntax = e.syntax;
            WorldDatabase.EscapeString(path);
            WorldDatabase.EscapeString(syntax);
            trans->Append(
                "INSERT INTO server_help_command (command_path, category_id, title, syntax, min_security, enabled, `auto`) "
                "VALUES ('{}', {}, '{}', '{}', {}, 1, 1) "
                "ON DUPLICATE KEY UPDATE "
                "category_id  = IF(`auto` = 1, VALUES(category_id), category_id), "
                "syntax       = IF(`auto` = 1, VALUES(syntax), syntax), "
                "min_security = IF(`auto` = 1, VALUES(min_security), min_security), "
                "`auto`       = IF(`auto` = 1, 1, `auto`)",
                path, e.category, path, syntax, e.level);
            ++attempted;
        }

        WorldDatabase.DirectCommitTransaction(trans);

        // Confirmación real: cuántas de las rutas intentadas tienen AHORA
        // alguna fila, sea `auto`=1 (recién escrita) o `auto`=0 (ya era una
        // ficha curada que el UPSERT respetó vía el IF de arriba). Comparar
        // contra `WHERE auto=1` a secas daba una discrepancia en falso: una
        // ruta protegida por el IF nunca cuenta como auto=1 aunque la
        // transacción haya ido perfecta, así que ese recuento por sí solo no
        // distingue "protegida" de "la transacción falló a medias". Contando
        // presencia (cualquier auto) sí lo distingue: si de verdad se
        // abortara a medias, las rutas nuevas (que no existian antes)
        // faltarían del todo.
        uint32 written = 0;
        if (!intentados.empty())
        {
            std::string inList;
            inList.reserve(intentados.size() * 24);
            for (std::string path : intentados)
            {
                WorldDatabase.EscapeString(path);
                if (!inList.empty())
                    inList += ",";
                inList += "'" + path + "'";
            }
            std::set<std::string> presentes;
            if (QueryResult result = WorldDatabase.Query(Acore::StringFormat(
                    "SELECT command_path FROM server_help_command WHERE command_path IN ({})", inList)))
            {
                do
                {
                    presentes.insert(result->Fetch()[0].Get<std::string>());
                } while (result->NextRow());
            }
            written = static_cast<uint32>(presentes.size());
        }

        if (written != attempted)
            handler->PSendSysMessage("[server-help] Aviso: se intentaron {} fichas automaticas y quedaron {} con "
                                     "fila tras confirmar (ninguna ficha curada se ha tocado: la diferencia es de "
                                     "comandos sin fila propia, revisa server_help_command si quieres saber cuales).",
                                     attempted, written);
        else
            handler->PSendSysMessage("[server-help] {} fichas automaticas escritas en server_help_command. "
                                     "'.ayuda recargar' para aplicarlas aqui; el panel web las ve al momento.", written);
        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadData();
        {
            std::lock_guard<std::mutex> lock(g_snapLock);
            g_snapshots.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_rateLock);
            g_rate.clear();   // recargar libera también los cupos de peticiones
        }
        {
            // Si un comando cambio de nivel o se retiro, la entrada vieja
            // sobrevivia hasta el reinicio. Se repuebla sola al evaluar
            // visibilidad (OnBeforeIsInvokerVisible).
            std::lock_guard<std::mutex> lock(g_permLock);
            g_perms.clear();
        }
        auto data = GetData();
        handler->PSendSysMessage("[server-help] Recargado: {} categorias, {} articulos, {} fichas, {} reglas.",
            data->categories.size(), data->articles.size(), data->docs.size(), data->rules.size());
        return true;
    }
};

void AddSC_mod_server_help()
{
    new mod_server_help_world();
    new mod_server_help_perms();
    new mod_server_help_command();
}
