// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
/*
 * mod-adaptive-ai — cabecera interna compartida por los tres .cpp del módulo.
 *
 * QUÉ ES
 * Una capa de decisión que aprende, encima de la IA de mod-playerbots. La idea
 * (CHANGELOG.md anexo A1): Adaptive decide QUÉ debería hacer el bot en cada
 * punto de decisión (interrumpir, cargar, control, defensiva, nada...) y
 * playerbots decide CÓMO (qué hechizo, rango, visión, cooldown, GCD). Cada
 * decisión se puntúa con lo que pasa después (daño, interrupción lograda,
 * control, muerte, victoria) y una tabla Q por (clase propia, clase enemiga,
 * estado, acción) va aprendiendo. Los bots luchan entre ellos en arenas
 * instanciadas en segundo plano para entrenar, y cada cierto tiempo se
 * comprueba que la tabla candidata gana a la última validada antes de que la
 * usen los bots que se cruzan con el jugador.
 *
 * CÓMO SE ENGANCHA A PLAYERBOTS (fase 0, verificado en la VM el 02/09/2026)
 * Las estrategias de playerbots se registran en listas estáticas privadas
 * (AiObjectContext::sharedStrategyContexts), así que un módulo no puede
 * añadir una Strategy ni un Multiplier sin tocar su código. Lo que sí es
 * público y estable: PlayerbotAI::DoSpecificAction(nombre) ejecuta una acción
 * por su nombre pasando por el motor (listeners, isUseful, isPossible,
 * Execute), y PlayerbotAI::CanCastSpell(nombre, objetivo) dice si un hechizo
 * está disponible. El cerebro corre desde el hook OnPlayerUpdate del core
 * (mismo hilo del mapa que actualiza la IA del bot en OnPlayerAfterUpdate) y
 * las recompensas salen de hooks del core (OnDamage, OnAuraApply, OnUnitDeath,
 * OnPlayerSpellCast, OnPlayerPVPKill, OnBattlegroundEnd). Ni una línea de
 * playerbots ni del core se toca. Sin playerbots, nada de esto se compila.
 *
 * HILOS
 * OnPlayerUpdate y los eventos de daño/auras llegan desde los hilos de mapa;
 * el planificador de arenas y los comandos, desde el hilo del mundo;
 * OnBattlegroundEnd puede llegar desde cualquiera de los dos. Todo lo
 * compartido lleva su mutex. Las escrituras a la base de datos son asíncronas.
 */

#ifndef MOD_ADAPTIVE_AI_H
#define MOD_ADAPTIVE_AI_H

#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <atomic>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;
class Unit;
class Battleground;
class SpellInfo;

#if defined(__has_include)
#  if __has_include("Playerbots.h") && __has_include("PlayerbotAI.h")
#    define ADAPTIVE_WITH_PLAYERBOTS 1
#  endif
#endif

namespace AdaptiveAI
{
    // ─── Configuración (mod_adaptive_ai.conf) ───────────────────────────────
    struct Difficulty
    {
        float miss   = 0.0f;   // probabilidad de no reaccionar en un punto de decisión
        float second = 0.0f;   // probabilidad de escoger la segunda mejor acción
    };

    // Un miembro de un equipo: clase (0 = cualquiera de las habilitadas) y
    // especialización (-1 = cualquiera). "warrior", "mage.frost", "*".
    struct ClassSpec
    {
        uint8 cls = 0;
        int8  spec = -1;
        std::string Name() const;
    };

    // Composición de una partida: equipo A contra equipo B, del mismo tamaño
    // (1c1, 2c2, 3c3, 5c5). "warrior:mage", "warrior+priest:mage+rogue", "*+*:*+*".
    struct CompSpec
    {
        std::vector<ClassSpec> a, b;
        uint8  Size() const { return uint8(a.size()); }
        std::string Name() const;              // "warrior+priest:mage+rogue"
        std::string TypeName() const;          // "2c2"
    };

    struct Config
    {
        bool   enabled          = true;
        bool   learn            = true;
        bool   learnOnlyArena   = false;
        bool   realEnable       = true;    // fuente 1: combates reales contra jugadores/bots
        // Qué modelo sale al mundo: "validada" (el de siempre), "mejor" (por
        // clase, el peldaño con más rating medido, que puede ser la serie) o
        // "espejo" (el peldaño más cercano al rating del jugador).
        std::string realMode    = "mejor";
        uint32 mirrorSpread     = 1;       // escalones arriba o abajo del centro, al azar por bot: variedad de rivales
        uint32 mirrorHours      = 6;       // cada cuánto se resortea el desvío de un bot

        bool   arenaEnable      = true;
        uint32 arenaSimultaneous = 20;
        uint32 arenaTeamSimultaneous = 1;  // arenas de equipo (2c2, 3c3, 5c5) a la vez en el automático: el grueso va a 1c1, una de equipo rotando
        uint32 arenaBotsMax     = 40;      // bots a la vez en arenas (los equipos comen muchos de nivel 80)
        std::map<uint8, uint32> arenaWithPlayer; // con un jugador conectado: partidas a la vez por tamaño (1c1:1,2c2:1...)
        uint32 bgWithPlayer     = 0;       // con un jugador conectado: campos automáticos a la vez
        bool   bracketsWithPlayer = true;  // franjas de nivel de playerbots: automáticas con jugador dentro, fijas entrenando solo
        uint32 arenaPauseSecs   = 5;
        std::vector<CompSpec> arenaPairs;      // composiciones (AdaptiveAI.Arena.Pares)
        std::vector<uint8> arenaTypes;         // tamaños que entrena el automático: 1, 2, 3, 5
        std::vector<uint8> classes;            // clases que entrenan y deciden (vacío = todas)
        std::map<uint8, uint32> level80Targets; // AdaptiveAI.Bots.Nivel80: clase -> bots de nivel 80 conectados que se quieren
        std::map<uint8, std::string> arenaSpecs; // AdaptiveAI.Arena.Specs: clase -> spec premade PvP de playerbots ("arms pvp")
        std::set<std::pair<uint8, std::string>> excludedActions;   // AdaptiveAI.Acciones.Excluir: (clase, acción) que no se ofrecen

        // Loadout: doble spec (0 = PvE, 1 = PvP) y equipo por propósito
        bool   loadoutEnable    = true;
        std::map<uint32, uint32> loadoutPvpIlvl;  // rating mínimo -> nivel de objeto PvP (0:232,1600:251,1800:264,2200:270)
        uint32 loadoutPveNormal = 187, loadoutPveHeroic = 200, loadoutPveRaid10 = 219, loadoutPveRaid25 = 232, loadoutPveWorld = 0;
        uint32 loadoutPerPass   = 5;           // bots a los que se les da la doble spec por pasada (30 s)
        std::vector<uint8> loadoutCapLevels = { 60, 70, 80 };   // niveles tope de etapa: ahí es normal ir con equipo PvP por el mundo; por debajo, PvE
        std::map<std::pair<uint8, uint8>, uint8> matchupTargets; // AdaptiveAI.Objetivos: (a, b) -> % que se espera que a gane a b
        bool   objectivesStop   = true;        // dejar de lanzar un emparejamiento cuando alcanza su objetivo
        uint32 objectiveMinMatches = 200;      // partidas por orientación para darlo por alcanzado
        uint32 objectiveMargin  = 5;           // puntos por debajo del objetivo que aún cuentan
        bool   arenaYield       = true;    // ceder cuando hay humanos en cola PvP
        uint32 arenaMap         = 559;     // 0 = al azar entre las arenas
        uint32 arenaPrepSecs    = 15;
        uint32 arenaLevel       = 80;
        uint32 arenaGearScore   = 200;     // 0 = no tocar el equipo
        uint32 arenaGearTolerance = 15;
        uint32 arenaTimeoutSecs = 90;      // sin llegar los dos bots, se aborta
        uint32 arenaMaxSecs     = 600;     // duración máxima de un combate
        std::string arenaMode   = "mixto"; // entrenar | contraste | mixto
        uint32 arenaReferencePct = 10;     // % de partidas automáticas serie contra serie (línea base viva)

        // Campos de batalla (fase 6, primer corte: objetivos de playerbots,
        // decisiones de combate y recompensas de objetivo del módulo)
        bool   bgEnable         = false;
        std::vector<uint8> bgTypes;        // BattlegroundTypeId: WS, AB, EY, AV, SA, IC
        uint32 bgEveryMinutes   = 30;      // un campo automático cada tantos minutos
        uint32 bgSimultaneous   = 1;
        uint32 bgPlayersPerTeam = 0;       // 0 = el mínimo del campo
        uint32 bgLevel          = 80;
        uint32 bgMaxMinutes     = 30;
        uint32 bgTimeoutSecs    = 150;     // sin llegar todos, se aborta
        std::string bgMode      = "contraste";
        std::string exportFile  = "adaptive_entrenado.sql";

        uint32 calibrateMinutes = 60;
        uint32 calibrateMatches = 5000;
        float  calibrateMargin  = 0.60f;
        uint32 calibrateMinNew  = 200;     // decisiones nuevas mínimas para calibrar
        uint32 calibrateSkipSaturated = 5; // % por debajo (o por encima de 100 menos eso) al que una pareja no tiene señal y el examen no la juega; 0 = jugarlas todas
        uint32 roundMaxExams    = 6;       // exámenes tras los que la ronda se cierra aunque no hayan aprobado todas (0 = esperar a todas)
        bool   calibratePerClass = true;   // aprobado por clase: las clases que ganan su calibración pasan a la validada
        uint32 calibratePerClassMin = 100; // partidas por clase y lado (sumando los últimos VentanaCiclos exámenes) para decidir
        bool   calibrateCutWhenJudged = true;  // el examen se corta cuando todas las clases que se juzgan ya tienen partidas de sobra
        uint32 calibrateCutMatches = 0;        // partidas por lado para ese corte; 0 = usar calibratePerClassMin
        std::vector<uint8> calibrateTypes = { 1 };   // tamaños que entran en el examen; solo 1c1 por defecto (en un 5c5 el resultado no se puede atribuir a una clase)
        uint32 calibrateMaxMinutes = 60;   // el examen dura esto y concluye con lo que haya (0 = hasta completar Combates)
        bool   calibrateClock = true;      // el examen va por el reloj del sistema (ventanas fijas desde la medianoche local), no por el tiempo desde el arranque
        bool   calibrateExclusive = true;  // mientras dura el examen, el automático no lanza: todas las arenas miden
        float  calibrateClassGain = 0.05f; // mejora mínima de la candidata sobre la validada (misma clase) para aprobar
        uint32 calibrateMaxCycles = 6;     // calibraciones sin aprobar tras las que, si no mejora, vuelve a lo anterior (0 = nunca)
        uint32 calibratePerPair = 4;       // partidas a la vez POR PAREJA durante el examen: con el aforo entero, las primeras parejas de la lista se lo comian y a las ultimas no llegaba el examen
        uint32 calibrateWindow  = 2;       // examenes que cuentan para el aprobado por clase (ventana movil): sumar sin limite congela la nota
        // Mismo rival (05/09/2026): la nota "con la validada" de una clase sale
        // de partidas validada contra validada (tercer brazo del examen), y no
        // de las partidas donde su rival llevaba la candidata. Sin esto el
        // aprobado media "mi mejora mas la de mis rivales".
        bool   calibrateSameRival = true;
        float  calibrateZ       = 1.64f;   // el aprobado exige ademas z * error tipico de la diferencia (0 = solo MejoraPorClase)
        // La serie como vara (05/09/2026, REVISION parte 2 N2+N6): la candidata
        // sin explorar se mide en la escalera como un peldano mas
        // (Arena.EscaleraCandidata % de las partidas, modo "sonda") y en cada
        // examen se calcula, por clase, si su peldano supera al mejor actual.
        // Con VaraSerie = 0 solo se traza (sombra); con 1, ese es el aprobado.
        uint32 arenaProbePct    = 5;
        bool   calibrateLadderRule = false;
        // Sin aprender (validada, generacion, mundo) solo se elige entre acciones
        // con al menos estas visitas (contando la fila general); si ninguna
        // llega, none. El 52-58 % de los estados tenia la mejor accion con UNA
        // visita (05/09/2026, N4).
        uint32 greedyMinVisits  = 5;
        // La accion que playerbots rechaza aprendia "mejor Q - 1" y quedaba como
        // segunda mejor del estado sin haberse ejecutado nunca (N4). Apagado:
        // solo cuenta en failedHere y en el premio de accion fallida.
        bool   rejectLearns     = false;
        // Las clases aprobadas siguen aprendiendo en 1c1 (N5); cuando les toca el
        // lado no adaptativo llevan su validada este % de las veces (el resto,
        // de serie), para que las suspensas entrenen contra lo que se examina.
        bool   approvedKeepLearning = true;
        uint32 approvedWithModelPct = 50;
        // En el examen, las aprobadas y las que ya llegaron al tope siguen
        // jugando como las demas (peticion del usuario, 05/09/2026): al sacarlas
        // del reparto, las que quedan jugaban menos contra ellas.
        bool   calibrateAllPlay     = true;
        uint32 arenaGenerationsPct = 15;   // % de partidas automáticas contra una generación anterior
        uint32 arenaLadderPct   = 15;      // % de partidas automáticas que miden un peldaño contra playerbots de serie (la escalera)
        uint32 ladderMinMatches = 30;      // partidas mínimas de un peldaño en una clase para que entre en la escalera
        uint32 ladderWindow     = 200;     // últimas partidas por peldaño y clase que se miran
        uint32 ladderSince      = 0;       // Unix: medir solo desde este cambio de comportamiento (0 = historial)
        uint32 generationsKeep  = 12;      // generaciones que se guardan (las más recientes): son los peldaños de la escalera
        bool   generalize       = true;    // fila "contra cualquier rival" (clase enemiga 0) como punto de partida

        float  epsilon          = 0.05f;
        float  alpha            = 0.10f;   // paso mínimo; con alphaDecay el paso es max(alpha, 1/(visitas+1))
        bool   alphaDecay       = true;
        float  gamma            = 0.90f;
        float  explorationBonus = 3.0f;    // optimismo: bonus/sqrt(visitas); sin probar = mejor Q del estado + bonus
        float  returnWeight     = 0.5f;    // al acabar la partida, retorno Monte Carlo a todas sus decisiones (× alpha)
        bool   interruptOnlyCasting = true;
        float  kiteDistance     = 20.0f;   // acción "kitear": alejarse hasta esta distancia
        uint32 kiteMs           = 3000;    // ventana de la acción "kitear"
        uint32 decisionIntervalMs = 1500;
        uint32 decisionMinGapMs = 400;
        uint32 tickMs           = 200;
        // Casteo propio (05/09/2026): mientras el bot lanza un hechizo con tiempo
        // de casteo no se decide (salvo interrumpir al rival). Una orden de
        // hechizo por DoSpecificAction se salta la espera de playerbots y el core
        // interrumpe el casteo en curso: los lanzadores perdian el final de sus
        // hechizos. Ver BrainTick.
        bool   respectOwnCast   = true;
        // La forma (05/09/2026): no se ofrece un hechizo que la forma actual no
        // permite (CanCastSpell de playerbots devuelve true con
        // SPELL_FAILED_NOT_SHAPESHIFT), y el druida feral no lleva en su catalogo
        // el dano de forma de lanzador (Colera, Fuego estelar, Fuego lunar...).
        bool   respectForm      = true;

        uint8  defaultDifficulty = 3;
        Difficulty difficulty[7];          // índice 1..6
        std::vector<float> brackets;       // umbrales de rating

        bool   logDecisions     = false;
        uint32 auditBot         = 0;       // GUID de un bot: traza contable selectiva, 0 desactiva
        uint32 auditClassMask   = 0;       // selección automática de sesiones activas por clase
        uint32 auditSessions    = 3;       // cuota por clase y proceso; solo candidata 1c1 que aprende
        bool   logAuras         = false;   // diagnostico: cada control/snare cobrado, con hechizo y objetivo
        std::string warlockPvpPet = "felhunter";   // mascota que lleva el brujo en PvP (imp, voidwalker, succubus, felhunter, felguard)
        bool   logMatches       = true;
        bool   tracesGM         = true;    // trazas como mensajes de sistema a los GM con .gm on
        uint32 matchRetentionDays = 30;    // borrar partidas de mas de N dias (0 = nunca)

        // Recompensas
        float  rDamage          = 10.0f;   // por vida entera de daño hecho / recibido
        float  rInterruptOk     = 5.0f;
        float  rInterruptBad    = -2.0f;
        float  rCcDone          = 3.0f;
        float  rCcTaken         = -3.0f;   // simétrico de ControlHecho: con -2 cada intercambio de control creaba +1 de la nada, y los controles crecen con la duración (0,6 por lado en partidas de menos de 30 s, 4,0 en las de 90+)
        float  rSnareDone       = 1.0f;
        float  rSnareTaken      = -1.0f;   // simétrico de SnareHecho: aplicar una ralentización dejó de ser gratis
        float  rKill            = 30.0f;
        float  rDeath           = -30.0f;
        float  rWin             = 20.0f;
        float  rLoss            = -20.0f;
        float  rWastedDefensive = -1.0f;
        float  rFailedAction    = -0.2f;
        float  rTime            = -0.05f;
        float  rDistance        = 0.3f;    // por segundo: lanzador a distancia (+) o pegado (−); cuerpo a cuerpo al revés
        // Tope del premio de SUCESOS acumulado en un combate: daño, control,
        // snares, interrupciones, defensiva desperdiciada, acción fallida y el
        // goteo de tiempo y posición, todos juntos. Fuera del tope quedan los
        // TERMINALES (Kill, Muerte, Victoria, Derrota, y en equipo KillEquipo y
        // MuerteAliado) y los objetivos de campo de batalla, que son la
        // puntuación del campo y no el "cómo".
        //
        // Todos los sucesos crecen con la duración y ninguno estaba acotado, así
        // que en las partidas largas mandaban ellos y no el resultado: el
        // 04/09/2026 el que pierde cobraba −62,2 de media en partidas de menos
        // de 30 s y −18,6 en las de más de 150. Con el tope, el ±50 de ganar o
        // perder manda siempre y los sucesos sólo ordenan dentro. 0 = sin tope.
        float  rShapingCap      = 40.0f;
        float  rTeamKill        = 10.0f;   // un aliado mata (asistencia)
        float  rAllyDeath       = -10.0f;  // muere un aliado
        float  rObjective       = 15.0f;   // objetivo de campo de batalla propio (bandera, base, torre...)
        float  rObjectiveTeam   = 5.0f;    // objetivo de un compañero
        float  personalityWeight = 0.5f;
    };

    extern Config cfg;
    void LoadConfig();

    // ─── Acciones (catálogo por clase) ──────────────────────────────────────
    enum ActionKind : uint8
    {
        KIND_NONE = 0,     // dejar hacer a playerbots
        KIND_OFFENSIVE,
        KIND_DEFENSIVE,
        KIND_CONTROL,
        KIND_INTERRUPT,
        KIND_MOBILITY,
        KIND_RESOURCE,
        KIND_TEAM          // elegir objetivo con el equipo (asistir, foco sanador, proteger)
    };

    struct ActionDef
    {
        char const* name;      // nombre de la acción en playerbots
        ActionKind  kind;
        bool        selfTarget;
        bool        isSpell;   // se comprueba con CanCastSpell
        bool        key = false; // herramienta clave de la clase (la que decide el kiteo: Nova, Tendón...); el estado dice si está lista
    };

    std::vector<ActionDef> const& ActionsFor(uint8 cls);   // catálogo de la clase + acciones de equipo
    bool   ClassEnabled(uint8 cls);                        // AdaptiveAI.Clases
    bool   IsCasterClass(uint8 cls);
    int8   CombatRole(Player* bot);                        // por spec y forma: 1 distancia, -1 cuerpo a cuerpo, 0 neutro
    char const* KindName(ActionKind kind);
    uint8  ParseClass(std::string const& text, int8& spec); // "*" -> 0 (cualquiera)
    char const* ClassName(uint8 cls);
    bool   ParseTeam(std::string const& text, std::vector<ClassSpec>& out);   // "warrior+priest"
    bool   ParseComps(std::string const& text, std::vector<CompSpec>& out);   // "a:b, a+b:c+d"
    uint8  ParseBgType(std::string const& text);           // "WS" -> BATTLEGROUND_WS, 0 si no
    char const* BgName(uint8 bgType);                      // "WS"

    // ─── Estado discretizado ────────────────────────────────────────────────
    // 864 estados de 1c1 (fase 1). En partidas de equipo se añade el contexto
    // de equipo (aliado bajo, me enfocan, sanador enemigo vivo): 8 variantes
    // más, así que las claves de 1c1 no cambian y la tabla vieja sigue valiendo.
    constexpr uint16 STATE_SOLO  = 3 * 3 * 3 * 2 * 2 * 2 * 2 * 2;   // 864
    // Contextos: equipo (9) x situación (8: control clave listo, movilidad
    // lista, enemigo ralentizado). Las claves de antes (situación 0) siguen
    // valiendo como punto de partida.
    constexpr uint16 STATE_COUNT = STATE_SOLO * 9 * 8;              // 62208

    struct TeamInfo
    {
        bool   inTeam = false;
        uint8  allyLow = 0;        // algún aliado vivo por debajo del 35 %
        uint8  focused = 0;        // dos o más enemigos me atacan
        uint8  enemyHealer = 0;    // queda un sanador enemigo vivo
        uint8  Ctx() const { return inTeam ? uint8(1 + allyLow + 2 * focused + 4 * enemyHealer) : 0; }
    };

    struct StateInfo
    {
        uint8 selfHp = 2, enemyHp = 2, dist = 2;
        uint8 enemyCasting = 0, selfControlled = 0, enemyControlled = 0, lowResource = 0, selfSnared = 0;
        uint8 team = 0;            // TeamInfo::Ctx()
        // Situación (03/09/2026): lo que decide si huir o cerrar distancia compensa
        uint8 keyReady = 0;        // la herramienta clave de la clase está lista (ActionDef::key castable)
        uint8 mobilityReady = 0;   // un hechizo de movilidad está listo (blink, carga, interceptar, pisotón...)
        uint8 enemySnared = 0;     // el enemigo va ralentizado (no me alcanza a velocidad normal)
        uint16 Key() const;
        std::string Describe() const;
    };

    struct Brain;
    StateInfo ComputeState(Player* bot, Unit* enemy, Brain const* brain);
    bool   IsHealerSpec(uint8 cls, uint8 spec);

    // ─── Tabla Q ────────────────────────────────────────────────────────────
    struct QEntry
    {
        float  q = 0.0f;
        uint32 visits = 0;
        bool   dirty = false;
    };

    class QTable
    {
    public:
        explicit QTable(uint32 version) : _version(version) {}

        uint32 Version() const { return _version; }
        float  Get(uint8 cls, uint8 enemyCls, uint16 state, uint8 action);
        QEntry Entry(uint8 cls, uint8 enemyCls, uint16 state, uint8 action);   // copia; sin entrada = (0, 0)
        std::vector<QEntry> Row(uint8 cls, uint8 enemyCls, uint16 state, uint8 actionCount);   // todas las acciones de un estado, con un solo bloqueo
        // generalFallback: una acción sin visitas contra este rival vale lo aprendido contra cualquiera (clase enemiga 0)
        float  MaxQ(uint8 cls, uint8 enemyCls, uint16 state, uint8 actionCount, bool generalFallback = false);
        void   Update(uint8 cls, uint8 enemyCls, uint16 state, uint8 action, float target, float alpha, bool countVisit = true);
        void   Load();                       // desde adaptive_q
        // REPLACE de lo cambiado (asíncrono). maxRows > 0: solo ese tramo, el
        // resto queda para la siguiente llamada (05/09/2026: formatear miles de
        // filas de golpe cada minuto costaba 100-145 ms de tick).
        void   SaveDirty(uint32 maxRows = 0);
        size_t DirtyRows();                  // filas pendientes de guardar
        void   CopyFrom(QTable& other);      // clona todo (nueva candidata)
        void   CopyClassFrom(QTable& other, uint8 cls);   // sustituye las filas de una clase por las de other (aprobado por clase)
        size_t Size();
        size_t ClassRows(uint8 cls);         // filas de una clase: 0 = revertir a esta tabla dejaria la clase a cero
        // Una tabla SIN filas de una clase no es "playerbots de serie" para esa
        // clase si se deja decidir: sin aprender no hay bonus ni ruido y la
        // eleccion la hace la personalidad del bot (05/09/2026, REVISION parte 2
        // N1). Quien decide con una tabla tiene que preguntar esto primero.
        bool   HasClass(uint8 cls) { return ClassRows(cls) > 0; }
        std::vector<std::pair<uint64, QEntry>> Snapshot();   // copia (clave = cls<<40 | enemigo<<32 | estado<<8 | accion)
        uint32 DirtySince() const { return _updatesSinceSave; }
        uint32 Updates() const { return _updates; }

    private:
        static uint64 Key(uint8 cls, uint8 enemyCls, uint16 state, uint8 action)
        {
            return (uint64(cls) << 40) | (uint64(enemyCls) << 32) | (uint64(state) << 8) | uint64(action);
        }
        uint32 _version;
        std::unordered_map<uint64, QEntry> _map;
        std::mutex _lock;
        uint32 _updates = 0;
        uint32 _updatesSinceSave = 0;
        std::vector<uint64> _dirty;          // claves con dirty = true, en orden de cambio: el guardado por tramos las va vaciando
        uint32 _classRows[16] = {};          // filas por clase, al dia con el mapa: ClassRows() en O(1) (se consulta en cada lanzamiento)
    };

    // Modelos: candidata (la que entrena) y validada (la que ven los jugadores).
    std::shared_ptr<QTable> Candidate();
    std::shared_ptr<QTable> Validated();
    void   LoadModels();                                   // al arrancar
    void   PromoteCandidate(float winrate, uint32 matches, std::string const& detail = ""); // candidata -> validada, nueva candidata
    // Aprobado por clase: la validada nueva es la validada de ahora con las filas
    // de esas clases copiadas de la candidata; la candidata sigue entrenando.
    void   PromoteClasses(std::vector<uint8> const& classes, std::string const& detail);
    bool   RevertClass(uint8 cls, std::string& detail, bool force = false);   // la candidata recupera las filas de la validada (force: vaciar aunque la validada tampoco tenga) para esa clase
    void   ArenaResetClassHistory(uint8 cls);             // borra su ventana de examenes y sus ciclos
    void   RecordCalibration(float winrate, uint32 matches, bool promoted, std::string const& detail = "");
    bool   UseModel(uint32 version, std::string& error);   // volver a una versión anterior
    // Generaciones: versiones que fueron validadas (adaptive_model.generacion = 1),
    // rivales de la candidata en una parte de las partidas para que no se estanque.
    std::vector<uint32> Generations();                     // versiones disponibles (sin la v1 vacía ni la candidata)
    std::shared_ptr<QTable> GenerationTable(uint32 version); // cargada y cacheada
    std::shared_ptr<QTable> TableOfVersion(uint32 version);  // candidata, validada o generacion; nullptr si no existe o esta vacia
    struct ModelInfo { uint32 version; std::string created; bool validated; bool generation; float winrate; uint32 matches; std::string note; };

    // ─── La escalera ─────────────────────────────────────────────────────────
    // Un peldaño es un modelo concreto midiéndose, por clase, contra playerbots
    // de serie. La serie es el peldaño 0 y el ancla: rating 1500 por
    // definición, y todo lo demás se coloca alrededor con la fórmula de Elo
    // (1500 + 400·log10(w/(1-w))). No se ordena por número de versión: la v4
    // puede ser peor que la v3, y como la validada es un mosaico de clases
    // aprobadas, cada clase tiene su propia escalera.
    struct LadderRung
    {
        uint32 version = 0;        // 0 = playerbots de serie
        uint32 matches = 0;
        float  winrate = 0.5f;     // contra la serie
        float  rating  = 1500.0f;
    };
    // Rating del jugador (adaptive_jugador): se mueve con sus muertes y sus
    // bajas contra bots, con la misma fórmula de Elo que los bots. Es el centro
    // de la escalera en el modo espejo.
    float  PlayerRating(uint32 guidLow);
    void   PlayerRatingUpdate(Player* human, float opponentRating, float score);

    std::vector<LadderRung> LadderFor(uint8 cls);   // de más fuerte a más flojo; siempre incluye el peldaño 0
    float  LadderRating(uint8 cls, uint32 version); // 1500 si no hay medida
    std::string LadderStatus();
    std::vector<ModelInfo> ListModels();
    void   SaveAllDirty(uint32 maxRowsPerTable = 0);   // 0 = todo lo pendiente
    // Diagnóstico del tick: avisa en el log si un paso del módulo en el hilo del
    // mundo ha tardado 200 ms o más (05/09/2026: ticks de 1,7-2,2 s sin culpable).
    void   WarnIfSlow(char const* step, uint32 startMs);
    void   PurgeOldData();                                 // partidas viejas y tablas Q de versiones descartadas

    // ─── Perfil por bot ─────────────────────────────────────────────────────
    struct BotProfile
    {
        uint32 guid = 0;
        std::string name;
        uint8  cls = 0;
        uint8  spec = 0;
        float  rating = 1500.0f;
        uint32 wins = 0, losses = 0, draws = 0;
        uint8  difficulty = 3;
        float  aggression = 0.5f, risk = 0.5f, defense = 0.5f, priority = 0.5f, mobility = 0.5f;
        uint32 xp = 0;
        uint32 model = 0;
        int8   rungOffset = 0;     // escalones por encima (-) o por debajo (+) del centro de la escalera; estable por bot
        uint32 rungAt = 0;         // cuándo se sorteó (unix): se resortea cada Real.Espejo.Horas
        int8   specPve = -1;       // índice de spec premade de playerbots (spec 0 del bot)
        int8   specPvp = -1;       // ídem para la spec 1
        std::string purpose = "pve";
        uint32 ilvl = 0;           // último nivel de objeto al que se le equipó
        bool   dirty = false;
    };

    BotProfile& GetProfile(Player* bot);           // crea si no existe
    BotProfile* FindProfile(uint32 guidLow);
    void   SaveProfile(BotProfile& profile);
    void   EloUpdate(BotProfile& a, BotProfile& b, float scoreA);
    uint8  BracketOf(float rating);
    char const* BracketName(uint8 bracket);
    char const* DifficultyName(uint8 difficulty);

    // ─── Cerebro (una sesión de combate de un bot) ──────────────────────────
    enum BrainMode : uint8
    {
        BRAIN_NONE = 0,       // no decide (línea base: playerbots de serie)
        BRAIN_CANDIDATE,
        BRAIN_VALIDATED,
        BRAIN_VERSION         // una generación anterior (tabla de otra versión, sin aprender)
    };
    char const* ModeName(BrainMode mode);

    constexpr size_t HISTORY_MAX = 200;

    struct Decision
    {
        uint32 timeMs = 0;
        uint16 state = 0;
        uint8  action = 0;
        float  q = 0.0f;
        float  reward = 0.0f;   // lo que se cobró después (se rellena en la siguiente decisión)
        std::string explain;
    };

    struct Brain
    {
        ObjectGuid bot;
        ObjectGuid enemy;
        uint8  myClass = 0;
        uint8  enemyClass = 0;
        int8   role = 0;            // CombatRole(): 1 a distancia, -1 cuerpo a cuerpo, 0 neutro
        BrainMode mode = BRAIN_NONE;
        bool   learn = false;
        float  epsilon = 0.0f;
        Difficulty difficulty;
        BotProfile* profile = nullptr;
        std::shared_ptr<QTable> table;        // con la que decide
        std::shared_ptr<QTable> learnTable;   // en la que aprende (siempre la candidata)
        uint32 matchId = 0;         // 0 = combate real
        uint32 startMs = 0;
        int8   side = -1;           // lado en la partida (0/1), -1 en combate real
        bool   teamMatch = false;   // 2c2 o más, o campo de batalla
        std::vector<ObjectGuid> allies;    // compañeros (sin uno mismo)
        std::vector<ObjectGuid> enemies;   // rivales
        uint32 lastEnemySearchMs = 0;

        uint32 nextTickMs = 0;
        uint32 lastDecisionMs = 0;
        uint16 lastState = 0;
        int16  lastAction = -1;
        float  pendingReward = 0.0f;
        float  shaping = 0.0f;      // goteo (tiempo + posición) ya cobrado en este combate, para el tope rShapingCap
        bool   dead = false;
        // EndBrain ya cerró esta sesión. Puede pasar DENTRO de Decide(): en este
        // core un instantáneo se resuelve en la misma llamada (Spell::prepare →
        // cast), y si mata, la arena termina y llega hasta EndBrain antes de que
        // DoSpecificAction vuelva. Decide lo mira al volver para no reabrir nada.
        bool   ended = false;
        bool   audit = false;             // se fija al empezar, evita sesiones parciales tras reload
        uint32 auditEvents = 0;           // tope por sesión; una traza truncada no valida nada

        // Acción "kitear": ventana en la que el cerebro sigue alejando al bot
        uint32 kiteUntilMs = 0;
        uint32 kiteLastMoveMs = 0;
        // Control y snares ya cobrados: (hechizo << 32 | guid) -> ms hasta el
        // que no se vuelve a pagar. Sin esto, un DoT o un snare que se
        // refresca cobraba en cada reaplicación.
        std::unordered_map<uint64, uint32> ccPaidUntilMs;

        // Acciones que playerbots rechazó en un estado durante esta sesión
        // (clave estado<<8 | acción): a la segunda dejan de ofrecerse
        std::unordered_map<uint32, uint8> failedHere;

        // Lo que este bot puede hacer de verdad: bit i del catálogo de su clase
        // (ComputeUsable). Se calcula una vez por combate, en el hilo del mapa.
        uint64 usable = 0;
        bool   usableReady = false;
        // El SpellInfo de cada accion del catalogo (nullptr si no es hechizo o no
        // lo conoce), resuelto una vez por combate junto con `usable`: la
        // comprobacion de forma por decision es entonces un CheckShapeshift.
        std::vector<SpellInfo const*> spells;

        // Estadísticas de la sesión
        uint32 decisions = 0, interruptsOk = 0, interruptsBad = 0, ccDone = 0, ccTaken = 0;
        uint32 damageDealt = 0, damageTaken = 0, failedActions = 0;
        float  totalReward = 0.0f;
        std::deque<Decision> history;   // toda la partida (tope HISTORY_MAX): el retorno final se reparte a todas

        // Reentrante: BrainTick lo tiene cogido mientras ejecuta la acción, y la
        // acción dispara hooks del core (hechizo lanzado, daño) que vuelven a
        // entrar aquí en el mismo hilo. Con un mutex normal se colgaba el mapa.
        std::recursive_mutex lock;
    };

    std::shared_ptr<Brain> FindBrain(ObjectGuid guid);
    std::shared_ptr<Brain> StartBrain(Player* bot, Unit* enemy, BrainMode mode, bool learn, float epsilon, uint32 matchId,
                                      std::shared_ptr<QTable> tableOverride = nullptr, bool auditEligible = false);   // BRAIN_VERSION: la tabla de la generación
    void   SetBrainTeam(std::shared_ptr<Brain> const& brain, int8 side, std::vector<ObjectGuid> const& allies, std::vector<ObjectGuid> const& enemies);
    void   Reward(ObjectGuid guid, float amount);          // suma a la decisión en curso, SIN tope (terminales: Kill, Muerte, objetivos)
    void   RewardShaped(ObjectGuid guid, float amount);    // ídem, pero contra el tope de sucesos (rShapingCap)
    void   EndBrain(ObjectGuid guid, float terminalReward);
    void   BrainTick(Player* bot);
    uint32 ActiveBrains();

    // Últimas decisiones por bot, para ".adaptive explicar" aunque haya acabado
    std::vector<Decision> LastDecisions(uint32 guidLow);

    // ─── Arena de entrenamiento ─────────────────────────────────────────────
    enum MatchSource : uint8 { SOURCE_AUTO = 0, SOURCE_MANUAL, SOURCE_CALIBRATION };
    char const* SourceName(MatchSource source);

    struct SideStats
    {
        uint32 decisions = 0, interruptsOk = 0, interruptsBad = 0, ccDone = 0, ccTaken = 0;
        uint32 damageDealt = 0, damageTaken = 0, objectives = 0;
        float  reward = 0.0f;
    };

    // Un bot dentro de una partida
    struct Fighter
    {
        ObjectGuid guid;
        std::string name;
        uint8  cls = 0, spec = 0;
        BrainMode mode = BRAIN_NONE;
        uint32 version = 0;        // BRAIN_VERSION: generación con la que decide
        bool   learn = false;
        float  ratingBefore = 1500.0f;
        bool   arrived = false;
        bool   invited = false;    // IncreaseInvitedCount pendiente de compensar
        bool   claimed = false;
        uint32 lastObjectives = 0; // puntuación de objetivos ya cobrada (campos)
        SideStats stats;
    };

    struct Match
    {
        uint32 id = 0;
        uint32 instanceId = 0;
        uint32 bgTypeId = 0;
        uint32 mapId = 0;
        bool   isBg = false;       // campo de batalla (no arena)
        uint8  size = 1;           // bots por lado
        MatchSource source = SOURCE_AUTO;
        uint32 seriesId = 0;
        std::string rule;          // entrenar | contraste | referencia | calibracion
        std::vector<Fighter> side[2];
        TeamId team[2] = {TEAM_ALLIANCE, TEAM_HORDE};
        BrainMode sideMode[2] = {BRAIN_NONE, BRAIN_NONE};   // el PAPEL de cada lado en la regla; lo que juega cada bot va en Fighter::mode
        int8   calibArm = -1;      // examen: 0/1 = lado que lleva la candidata, 2 = validada contra validada (tercer brazo)
        ObjectGuid group[2];       // grupo creado para el lado (2c2 o más)
        uint32 createdMs = 0, startedMs = 0, endedMs = 0, lastScoreMs = 0;
        bool   started = false, ended = false, leaveIssued = false;
        int8   winner = -1;        // 0/1, -1 empate, -2 abortado
        std::string TypeName() const;   // "1c1", "3c3", "bg:WS"
    };

    struct ArenaStatus
    {
        uint32 running = 0, waiting = 0, finishedToday = 0, bgRunning = 0;
        std::vector<std::string> lines;
    };

    void   ArenaInit();
    void   ArenaShutdown();   // concluye el examen en marcha si ya lleva media ventana: que un reinicio no lo tire
    void   ArenaTick(uint32 diff);                 // hilo del mundo, desde WorldScript::OnUpdate
    uint32 ArenaLaunchSeries(CompSpec const& comp, uint32 count, uint32 simultaneous, std::string const& mode, std::string& error);
    bool   BgLaunch(uint8 bgType, uint32 perTeam, std::string const& mode, std::string& error);
    uint32 ArenaStopAll();
    void   ArenaSetAuto(bool on);
    bool   ArenaAutoEnabled();
    ArenaStatus ArenaGetStatus();
    bool   CalibrationStart(std::string& error);
    bool   CalibrationRunning();
    std::string CalibrationStatus();

    // Hooks de campo de batalla (los llama el script de mod_adaptive_ai_arena.cpp)
    void   OnBgSetup(Battleground* bg);
    void   OnBgAddPlayer(Battleground* bg, Player* player);
    void   OnBgStart(Battleground* bg);
    void   OnBgEnd(Battleground* bg, TeamId winner);
    void   OnBgUpdate(Battleground* bg, uint32 diff);
    void   OnBgRemovePlayer(Battleground* bg, Player* player);
    void   OnBgDestroy(Battleground* bg);
    bool   IsTrainingBot(ObjectGuid guid);        // está en una partida de entrenamiento

    // ─── Loadout: doble spec y equipo por propósito ──────────────────────────
    bool   EnsureSpec(Player* bot);              // (sin doble spec posible) pone la spec PvP sobre la única; true si cambió
    bool   EnsureDualSpec(Player* bot);          // spec 0 PvE completa, spec 1 PvP completa; true si tocó algo
    bool   ApplyPurpose(Player* bot, bool pvp, uint32 ilvl, bool forceGear);   // activa la spec y (re)equipa; true si cambió de spec
    void   QueuePurpose(ObjectGuid bot, bool pvp, uint32 ilvl, bool forceGear); // lo mismo, desde otro hilo: lo hace el mundo
    uint32 PvpIlvlFor(float rating);
    void   MaintainLoadout(uint32 diff);         // hilo del mundo: cola y pasada de doble spec
    std::string LoadoutStatus();
    std::string UsableStatus();                  // acciones descartadas por "no la tiene"
    int    TargetFor(uint8 cls, uint8 enemyCls); // % objetivo de cls contra enemyCls, -1 si no hay
    std::string ObjectivesStatus();              // emparejamientos alcanzados / en curso
    std::string ApprovalStatus();                // ronda, clases aprobadas y suspensas, generaciones
    bool   ClassApproved(uint8 cls);             // aprobada en la ronda actual: sus arenas van a las suspensas

    // ─── Bots de nivel 80: mantener N conectados por clase (sube bots con la fábrica de playerbots)
    void   MaintainLevel80();                   // hilo del mundo, cada 30 s
    std::string Level80Status();

    // ─── Franjas de nivel de playerbots: reparto automático solo con jugador ──
    void   SyncLevelBrackets();                 // hilo del mundo, cada 30 s
    std::string LevelBracketsStatus();

    // ─── Exportar e importar lo aprendido (modelos, tabla Q, perfiles) ────────
    bool   ExportTraining(std::string const& path, std::string& summary);
    bool   ImportTraining(std::string const& path, std::string& summary);

    // ─── Trazas: al log y, si AdaptiveAI.Trazas.GM, a los GM conectados ───────
    void   Trace(std::string const& text);
    void   SetTraces(bool on);
    bool   TracesOn();

    // ─── Utilidades comunes ─────────────────────────────────────────────────
    bool   IsBot(Player* player);
    Player* PlayerOf(Unit* unit);                 // jugador dueño (mascota, elemental) o él mismo
    void   OwnWarlockPet(Player* bot, bool take); // garantiza la mascota de PvP y apaga las estrategias de mascota de playerbots
    std::string Now();                            // 'YYYY-MM-DD HH:MM:SS'
}

#endif // MOD_ADAPTIVE_AI_H
