// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-standby — apagar el worldserver cuando no hay jugadores humanos.
 *
 * EL PROBLEMA
 * El servidor es de una sola persona (single player con bots). Con 200-400 bots
 * dentro consume ~6 GB y ~270 % de CPU las 24 horas, aunque no haya nadie
 * jugando. La VM no se puede apagar (authserver, MySQL y el panel siguen en
 * pie y el reino tiene que seguir seleccionable), pero el worldserver sí: es el
 * proceso que se lleva toda la CPU y casi toda la RAM.
 *
 * LA SOLUCIÓN
 * Este módulo cuenta las sesiones humanas (WorldSession::IsHeadless() == false) en
 * cada tick del mundo. Cuando no queda ninguna durante Standby.IdleMinutes,
 * pide un apagado limpio con codigo de salida 0 (SHUTDOWN_EXIT_CODE). La unidad
 * systemd (Restart=on-failure) NO revive un proceso que salio con 0, asi que el
 * worldserver se queda dormido; systemd mantiene abierto el puerto 8085
 * (Network.UseSocketActivation=1) y la siguiente conexion de un cliente lo
 * vuelve a arrancar. Un crash (codigo 1) o un ".server restart" (codigo 2) si
 * son "fallo" para systemd, que entonces relanza el proceso.
 *
 * SEGURIDAD
 *  - No actua en los primeros Standby.MinUptimeMinutes tras arrancar: cubre el
 *    cuelgue conocido del primer arranque con muchas cuentas de bots y da
 *    margen a que el jugador que desperto el servidor termine de entrar.
 *  - Con Standby.RequireSocketActivation = 1 (por defecto) solo actua si el
 *    proceso lo lanzo systemd por activacion de socket (variable de entorno
 *    LISTEN_FDS presente). En un arranque manual, o con el viejo lanzador por
 *    screen, no hace nada: apagarlo ahi dejaria el mundo caido sin nadie que
 *    lo despierte.
 *  - Si aparece un humano durante la cuenta atras, cancela el apagado (solo el
 *    que armo este modulo por inactividad; nunca el de ".standby ahora" ni uno
 *    ajeno, p. ej. un ".server restart" o el de systemctl).
 *  - ".standby mantener <min>" lo suspende un rato para una sesion larga.
 *
 * DE QUIEN ES EL APAGADO EN CURSO (M03, 24/09/2026)
 * g_own dice si el apagado en curso lo pidio este modulo y por que: ninguno
 * (no hay, o es ajeno), por inactividad o por ".standby ahora". Lo fijan los
 * hooks OnShutdownInitiate/OnShutdownCancel, que el core llama en CADA
 * ShutdownServ/ShutdownCancel: si otro (GM, consola, systemctl) pide un apagado
 * encima del nuestro, la marca pasa a "ajeno" y ya no lo cancelamos por una
 * marca antigua. Reglas:
 *  - Llega un humano: se cancela solo el de inactividad.
 *  - ".standby ahora": no pisa un apagado ajeno; si el en curso es el de
 *    inactividad lo convierte en manual (mismo codigo de salida).
 *  - Camino automatico: no pide nada si ya hay un apagado en curso.
 *  - Standby.Enable = 0 durante la cuenta atras: se cancela el de inactividad,
 *    se respeta el manual y se reinicia el contador de vacio.
 *
 * DONDE SE HACE EL TRABAJO
 * Todo en WorldScript::OnUpdate (hilo del mundo), como pide modules/README.md.
 * Los comandos (chat y consola) y ".reload config" tambien corren en el hilo
 * del mundo con los mapas parados (ver modules/README.md, regla 5), asi que
 * los atomicos de abajo son solo por prudencia. Sin dependencia de ningun otro
 * modulo.
 *
 * Comando: .standby [estado | ahora | mantener <min> | reanudar]
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Log.h"
#include "ModLocale.h"
#include "standby_locale.h"
#include "Optional.h"
#include "ScriptMgr.h"
#include "TimeMs.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>

using namespace Acore::ChatCommands;

namespace
{
    struct Config
    {
        bool     enable                 = true;
        uint32   idleMinutes            = 15;
        uint32   warnSeconds            = 60;
        uint32   minUptimeMinutes       = 10;
        uint32   checkSeconds           = 30;
        bool     requireSocketActivation = true;
    };

    // Doble buffer inmutable bajo candado (mismo patron que mod-update-notice).
    // ".reload config" corre en el hilo del mundo, asi que hoy no compite con
    // OnUpdate; el candado es barato y protege si el core cambiara.
    std::mutex                    g_cfgLock;
    std::shared_ptr<Config const> g_cfg = std::make_shared<Config const>();

    std::shared_ptr<Config const> Cfg()
    {
        std::lock_guard<std::mutex> lock(g_cfgLock);
        return g_cfg;
    }

    // De quien es el apagado en curso (ver cabecera). None tambien cuando hay
    // uno en curso pero es ajeno.
    enum class Own : uint8
    {
        None,
        Idle,    // lo armo OnUpdate por inactividad
        Manual,  // ".standby ahora"
    };

    char const* OwnName(Own o)
    {
        switch (o)
        {
            case Own::Idle:   return "inactividad";
            case Own::Manual: return "manual";
            default:          return "ajeno";
        }
    }

    // --- Estado ---
    // OnUpdate, los comandos y los hooks de apagado corren todos en el hilo
    // del mundo; los escalares son atomicos solo por prudencia. g_nextCheckMs
    // y g_loggedDisabled solo los toca OnUpdate (y OnStartup, que corre antes).
    std::atomic<bool>     g_socketActivated{ false };
    std::atomic<uint64_t> g_emptySinceMs{ 0 };   // 0 = hay (o habia) un humano
    std::atomic<Own>      g_own{ Own::None };    // dueno del apagado en curso
    std::atomic<uint64_t> g_suppressUntilMs{ 0 };   // ".standby mantener"
    std::atomic<uint32>   g_lastHumanCount{ 0 };    // ultimo recuento (para el comando)
    uint64_t g_nextCheckMs    = 0;
    bool     g_loggedDisabled = false;
    // Solo distinto de None mientras RequestShutdown llama a ShutdownServ: asi
    // OnShutdownInitiate sabe si el apagado que se inicia es nuestro.
    Own      g_issuing        = Own::None;

    // Pide el apagado con la marca de quien lo pide. Si el core lo ignora
    // (IsStopped) no salta OnShutdownInitiate y g_own no cambia.
    void RequestShutdown(Own kind, uint32 warnSeconds, std::string const& reason)
    {
        g_issuing = kind;
        sWorld->ShutdownServ(warnSeconds, 0, SHUTDOWN_EXIT_CODE, reason);
        g_issuing = Own::None;
    }

    // Cancela el apagado solo si sigue siendo nuestro y del tipo indicado.
    bool CancelOwn(std::initializer_list<Own> kinds)
    {
        Own const own = g_own.load(std::memory_order_relaxed);
        if (own == Own::None || !sWorld->IsShuttingDown())
            return false;
        if (std::find(kinds.begin(), kinds.end(), own) == kinds.end())
            return false;
        sWorld->ShutdownCancel();   // OnShutdownCancel pone g_own = None
        g_own.store(Own::None, std::memory_order_relaxed);
        return true;
    }

    // Recorre el mapa de sesiones: SOLO se llama desde OnUpdate (hilo del
    // mundo), donde el core no esta a la vez modificando _sessions. El comando
    // ".standby estado" NO lo llama — lee g_lastHumanCount, que rellena OnUpdate.
    uint32 CountHumanSessions()
    {
        uint32 humans = 0;
        for (auto const& itr : sWorldSessionMgr->GetAllSessions())
            if (itr.second && !itr.second->IsHeadless())
                ++humans;
        return humans;
    }

    // Minutos que faltan para dormir (0 = ya toca / apagado en curso), o -1 si
    // ahora mismo hay un humano.
    int64_t MinutesUntilSleep(Config const& c)
    {
        uint64_t const emptySince = g_emptySinceMs.load(std::memory_order_relaxed);
        if (emptySince == 0)
            return -1;
        uint64_t const idleMs   = TimeMs::NowMs() - emptySince;
        uint64_t const targetMs = TimeMs::MinsToMs(c.idleMinutes);
        if (idleMs >= targetMs)
            return 0;
        return static_cast<int64_t>((targetMs - idleMs) / 60000) + 1;
    }
}

class mod_standby_world : public WorldScript
{
public:
    mod_standby_world() : WorldScript("mod_standby_world",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE,
          WORLDHOOK_ON_SHUTDOWN_INITIATE, WORLDHOOK_ON_SHUTDOWN_CANCEL }) { }

    // El core los llama en cada ShutdownServ/ShutdownCancel, pida quien pida.
    // Un apagado iniciado fuera de RequestShutdown es ajeno aunque sustituya
    // al nuestro: desde ese momento ya no es nuestro para cancelarlo.
    void OnShutdownInitiate(ShutdownExitCode /*code*/, ShutdownMask /*mask*/) override
    {
        Own const prev = g_own.exchange(g_issuing, std::memory_order_relaxed);
        if (g_issuing == Own::None && prev != Own::None)
            LOG_INFO("server.worldserver",
                "[standby] otro apagado sustituye al del modo en espera ({}): ya no se cancelara al entrar alguien.",
                OwnName(prev));
    }

    void OnShutdownCancel() override
    {
        g_own.store(Own::None, std::memory_order_relaxed);
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        auto next = std::make_shared<Config>();
        next->enable                  = sConfigMgr->GetOption<bool>("Standby.Enable", true);
        next->idleMinutes             = sConfigMgr->GetOption<uint32>("Standby.IdleMinutes", 15);
        next->warnSeconds             = sConfigMgr->GetOption<uint32>("Standby.WarnSeconds", 60);
        next->minUptimeMinutes        = sConfigMgr->GetOption<uint32>("Standby.MinUptimeMinutes", 10);
        next->checkSeconds            = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("Standby.CheckSeconds", 30));
        next->requireSocketActivation = sConfigMgr->GetOption<bool>("Standby.RequireSocketActivation", true);
        if (!next->idleMinutes)
            next->idleMinutes = 1;

        std::lock_guard<std::mutex> lock(g_cfgLock);
        g_cfg = std::move(next);
    }

    void OnStartup() override
    {
        // systemd pone LISTEN_FDS (y LISTEN_PID) cuando arranca el proceso por
        // activacion de socket. El core solo hereda el socket si ademas
        // Network.UseSocketActivation = 1 (ver AsyncAcceptor), pero para decidir
        // si dormir nos basta con saber que fue systemd quien nos lanzo asi.
        bool const activated = (std::getenv("LISTEN_FDS") != nullptr);
        g_socketActivated.store(activated, std::memory_order_relaxed);

        auto const c = Cfg();
        LOG_INFO("server.worldserver",
            "[standby] modulo cargado: enable={} idle={}min warn={}s minUptime={}min socketActivation={} (heredado={})",
            c->enable, c->idleMinutes, c->warnSeconds, c->minUptimeMinutes, c->requireSocketActivation, activated);
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        auto const c = Cfg();

        // Red de seguridad: los hooks ya limpian la marca, pero si el apagado
        // desaparecio por otra via no debe quedar una marca antigua.
        if (g_own.load(std::memory_order_relaxed) != Own::None && !sWorld->IsShuttingDown())
            g_own.store(Own::None, std::memory_order_relaxed);

        if (!c->enable)
        {
            // Desactivado en caliente: el apagado por inactividad deja de tener
            // sentido; el manual es una orden explicita y se respeta. El
            // contador de vacio se reinicia para que al reactivar no se duerma
            // de golpe con un tiempo acumulado mientras estaba apagado.
            if (CancelOwn({ Own::Idle }))
                LOG_INFO("server.worldserver",
                    "[standby] Standby.Enable=0: apagado por inactividad CANCELADO.");
            g_emptySinceMs.store(0, std::memory_order_relaxed);
            return;
        }

        uint64_t const now = TimeMs::NowMs();
        Own const own = g_own.load(std::memory_order_relaxed);

        // Con nuestro apagado por inactividad en curso se vigila cada tick
        // (para cancelarlo a tiempo si entra alguien); si no, cada
        // Standby.CheckSeconds.
        if (own != Own::Idle && now < g_nextCheckMs)
            return;
        g_nextCheckMs = now + TimeMs::SecsToMs(c->checkSeconds);

        if (c->requireSocketActivation && !g_socketActivated.load(std::memory_order_relaxed))
        {
            if (!g_loggedDisabled)
            {
                LOG_INFO("server.worldserver",
                    "[standby] inactivo: el proceso no lo lanzo systemd por activacion de socket "
                    "(Standby.RequireSocketActivation=1). No se apagara por inactividad.");
                g_loggedDisabled = true;
            }
            return;
        }

        uint32 const humans = CountHumanSessions();
        g_lastHumanCount.store(humans, std::memory_order_relaxed);

        if (humans > 0)
        {
            g_emptySinceMs.store(0, std::memory_order_relaxed);
            // Solo el de inactividad: el manual lo pidio alguien conectado y
            // uno ajeno no es nuestro.
            if (CancelOwn({ Own::Idle }))
                LOG_INFO("server.worldserver",
                    "[standby] apagado por inactividad CANCELADO: {} jugador(es) conectado(s).", humans);
            return;
        }

        // 0 humanos: se empieza a contar YA (aunque el margen de arranque no
        // haya pasado), para que el tiempo hasta dormir sea IdleMinutes y no
        // MinUptimeMinutes + IdleMinutes.
        uint64_t emptySince = g_emptySinceMs.load(std::memory_order_relaxed);
        if (emptySince == 0)
        {
            emptySince = now;
            g_emptySinceMs.store(now, std::memory_order_relaxed);
            LOG_INFO("server.worldserver",
                "[standby] sin jugadores humanos. Apagado por inactividad en {} min si no vuelve nadie.", c->idleMinutes);
        }

        // Ya hay un apagado en curso (nuestro o ajeno): no se pisa. Un
        // ".server restart" ajeno lleva otro codigo de salida y otra intencion.
        if (sWorld->IsShuttingDown())
            return;

        if (now < g_suppressUntilMs.load(std::memory_order_relaxed))
            return;

        // Margen tras arrancar: no dormir durante el asentamiento del primer
        // arranque ni mientras el jugador que desperto el servidor esta entrando.
        if (now < TimeMs::MinsToMs(c->minUptimeMinutes))
            return;

        if (now - emptySince < TimeMs::MinsToMs(c->idleMinutes))
            return;

        // Sin SHUTDOWN_MASK_RESTART: el proceso sale con codigo 0 y systemd no
        // lo revive. Sin SHUTDOWN_MASK_IDLE: ya sabemos que no hay humanos y
        // queremos que baje si o si; a los bots que queden (deberian haber
        // salido ya, AiPlayerbot.DisabledWithoutRealPlayerLogoutDelay < idle)
        // el apagado normal les guarda el personaje igual.
        LOG_INFO("server.worldserver",
            "[standby] {} min sin jugadores humanos: apagando el worldserver en {} s (modo en espera).",
            c->idleMinutes, c->warnSeconds);
        RequestShutdown(Own::Idle, c->warnSeconds, "inactividad");
    }
};

class mod_standby_command : public CommandScript
{
public:
    mod_standby_command() : CommandScript("mod_standby_command") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable standbyTable =
        {
            { "estado",   HandleStatus,  SEC_GAMEMASTER,     Console::Yes },
            { "ahora",    HandleNow,     SEC_ADMINISTRATOR,  Console::Yes },
            { "mantener", HandleKeep,    SEC_ADMINISTRATOR,  Console::Yes },
            { "reanudar", HandleResume,  SEC_ADMINISTRATOR,  Console::Yes },
            { "",         HandleStatus,  SEC_GAMEMASTER,     Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "standby", standbyTable },
        };
        return commandTable;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        auto const c = Cfg();
        bool const activated = g_socketActivated.load(std::memory_order_relaxed);
        handler->PSendSysMessage(ModLocale::L(handler, "Modo en espera: {}"), c->enable ? ModLocale::L(handler, "activado") : ModLocale::L(handler, "DESACTIVADO"));
        handler->PSendSysMessage(ModLocale::L(handler, "  Activacion de socket heredada: {}{}"),
            activated ? ModLocale::L(handler, "si") : ModLocale::L(handler, "no"),
            (c->requireSocketActivation && !activated) ? ModLocale::L(handler, " -> el modulo NO apagara el servidor") : "");
        handler->PSendSysMessage(ModLocale::L(handler, "  Ventana de inactividad: {} min | aviso: {} s | margen tras arranque: {} min"),
            c->idleMinutes, c->warnSeconds, c->minUptimeMinutes);

        uint32 const humans = g_lastHumanCount.load(std::memory_order_relaxed);
        handler->PSendSysMessage(ModLocale::L(handler, "  Sesiones humanas (ultimo recuento): {}"), humans);

        if (sWorld->IsShuttingDown())
        {
            Own const own = g_own.load(std::memory_order_relaxed);
            handler->PSendSysMessage(ModLocale::L(handler, "  Apagado EN CURSO: quedan {} s ({}{})"),
                sWorld->GetShutDownTimeLeft(),
                own == Own::None ? "" : ModLocale::L(handler, "modo en espera, "), ModLocale::L(handler, OwnName(own)));
        }
        else if (humans > 0)
            handler->SendSysMessage(ModLocale::L(handler, "  No se dormira: hay jugadores conectados."));
        else
        {
            int64_t const mins = MinutesUntilSleep(*c);
            if (mins < 0)
                handler->SendSysMessage(ModLocale::L(handler, "  Estado indeterminado (aun no se ha hecho la primera comprobacion)."));
            else
                handler->PSendSysMessage(ModLocale::L(handler, "  Se dormira en ~{} min si no entra nadie."), mins);
        }

        uint64_t const suppressUntil = g_suppressUntilMs.load(std::memory_order_relaxed);
        uint64_t const now = TimeMs::NowMs();
        if (suppressUntil > now)
            handler->PSendSysMessage(ModLocale::L(handler, "  Suspendido por '.standby mantener': {} min restantes."),
                static_cast<uint32>((suppressUntil - now) / 60000) + 1);
        return true;
    }

    static bool HandleNow(ChatHandler* handler)
    {
        auto const c = Cfg();
        if (sWorld->IsShuttingDown())
        {
            switch (g_own.load(std::memory_order_relaxed))
            {
                case Own::Idle:
                    // Mismo apagado y mismo codigo de salida: solo cambia la
                    // intencion, para que entrar alguien ya no lo cancele.
                    g_own.store(Own::Manual, std::memory_order_relaxed);
                    handler->PSendSysMessage(ModLocale::L(handler, "El apagado por inactividad en curso pasa a ser manual: quedan {} s."),
                        sWorld->GetShutDownTimeLeft());
                    LOG_INFO("server.worldserver", "[standby] {} convierte en manual el apagado por inactividad.",
                        handler->GetNameLink());
                    break;
                case Own::Manual:
                    handler->SendSysMessage(ModLocale::L(handler, "Ya hay un apagado manual del modo en espera en curso."));
                    break;
                default:
                    handler->SendSysMessage(ModLocale::L(handler, "Ya hay un apagado en curso que no es del modo en espera; no se toca."));
                    break;
            }
            return true;
        }
        handler->PSendSysMessage(ModLocale::L(handler, "Modo en espera: apagando el worldserver en {} s."), c->warnSeconds);
        LOG_INFO("server.worldserver", "[standby] apagado manual solicitado por {}.", handler->GetNameLink());
        RequestShutdown(Own::Manual, c->warnSeconds, "modo en espera (manual)");
        return true;
    }

    static bool HandleKeep(ChatHandler* handler, Optional<uint32> minutes)
    {
        uint32 const mins = minutes.value_or(60);
        if (!mins || mins > 1440)
        {
            handler->SendSysMessage(ModLocale::L(handler, "Uso: .standby mantener <minutos> (1-1440)."));
            return true;
        }
        g_suppressUntilMs.store(TimeMs::NowMs() + TimeMs::MinsToMs(mins), std::memory_order_relaxed);
        // Orden explicita y posterior: cancela nuestro apagado, sea cual sea
        // su motivo. Uno ajeno no se toca.
        if (CancelOwn({ Own::Idle, Own::Manual }))
            handler->SendSysMessage(ModLocale::L(handler, "Apagado del modo en espera cancelado."));
        else if (sWorld->IsShuttingDown())
            handler->SendSysMessage(ModLocale::L(handler, "Hay un apagado en curso que no es del modo en espera; no se cancela."));
        handler->PSendSysMessage(ModLocale::L(handler, "Modo en espera suspendido {} min. '.standby reanudar' lo vuelve a activar."), mins);
        LOG_INFO("server.worldserver", "[standby] suspendido {} min por {}.", mins, handler->GetNameLink());
        return true;
    }

    static bool HandleResume(ChatHandler* handler)
    {
        g_suppressUntilMs.store(0, std::memory_order_relaxed);
        handler->SendSysMessage(ModLocale::L(handler, "Modo en espera reanudado."));
        return true;
    }
};

void AddSC_mod_standby()
{
    ModLocale::Register(StandbyLocale::kEntries);
    new mod_standby_world();
    new mod_standby_command();
}
