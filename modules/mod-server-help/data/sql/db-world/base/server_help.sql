-- mod-server-help: base de conocimiento del servidor (acore_world).
--
-- Cuatro tablas. Idempotente: se puede relanzar (fase 5 del instalador o el
-- actualizador del core al arrancar). Lo que se conserva al reaplicar:
--   server_help_category : `sort` y `enabled` de las filas de semilla
--   server_help_article  : `sort` y `enabled` (ids 1-99 son de semilla; los
--                          tuyos, a partir de 100)
--   server_help_command  : `enabled` (la ficha y su permiso se actualizan)
--   server_help_rule     : las reglas con id >= 1000 (las de semilla, 1-999,
--                          se sustituyen enteras)
-- Recargar sin reiniciar: ".ayuda recargar" (administrador).
--
-- COMO SE DECIDE LA CATEGORIA DE UN COMANDO
--   1. Si tiene ficha en server_help_command con category_id, esa.
--   2. Si no, la regla de server_help_rule cuyo prefijo (por tokens) mas
--      largo case con la ruta: "teleport add" gana a "teleport".
--   3. Si no, la categoria por defecto de su nivel (ServerHelp.DefaultCategory.*).
--   Si la categoria elegida no es visible para el jugador (min_security),
--   se pasa al siguiente paso: un jugador nunca ve el nombre de una
--   categoria de GM por culpa de un comando suyo.
--
-- Los comandos NO se listan aqui: los descubre el modulo en el arbol real del
-- core (los de cualquier modulo incluidos). Aqui solo van categorias,
-- articulos, fichas en espanol y reglas.

CREATE TABLE IF NOT EXISTS `server_help_category` (
  `id` int unsigned NOT NULL,
  `parent_id` int unsigned NOT NULL DEFAULT 0 COMMENT '0 = categoria raiz',
  `name` varchar(64) NOT NULL,
  `name_en` varchar(64) NOT NULL DEFAULT '',
  `sort` int NOT NULL DEFAULT 0,
  `min_security` tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 jugador, 1 moderador, 2 GM, 3 administrador',
  `enabled` tinyint unsigned NOT NULL DEFAULT 1,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `server_help_article` (
  `id` int unsigned NOT NULL AUTO_INCREMENT,
  `category_id` int unsigned NOT NULL,
  `title` varchar(120) NOT NULL,
  `body` text NOT NULL,
  `keywords` varchar(255) NOT NULL DEFAULT '',
  `command_path` varchar(90) NOT NULL DEFAULT '' COMMENT 'si no esta vacio, solo se ve si el jugador puede usar ese comando',
  `min_security` tinyint unsigned NOT NULL DEFAULT 0,
  `sort` int NOT NULL DEFAULT 0,
  `enabled` tinyint unsigned NOT NULL DEFAULT 1,
  `is_hot` tinyint unsigned NOT NULL DEFAULT 0 COMMENT 'icono de tema destacado en la lista',
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_server_help_article_category` (`category_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `server_help_command` (
  `command_path` varchar(90) NOT NULL COMMENT 'ruta sin punto, p. ej. "grupo banda"',
  `category_id` int unsigned NOT NULL DEFAULT 0 COMMENT '0 = decidir por regla/nivel',
  `title` varchar(120) NOT NULL DEFAULT '',
  `description` text,
  `syntax` varchar(255) NOT NULL DEFAULT '' COMMENT 'vacio = la de la tabla command del core',
  `examples` text,
  `keywords` varchar(255) NOT NULL DEFAULT '',
  `min_security` tinyint unsigned NULL DEFAULT NULL COMMENT 'permiso exacto para consumidores externos; NULL = heredar categoria',
  `enabled` tinyint unsigned NOT NULL DEFAULT 1,
  `auto` tinyint unsigned NOT NULL DEFAULT 0 COMMENT 'fila generada por ".ayuda export": ese comando se descubrio del arbol, sin ficha propia',
  `title_en` varchar(120) NOT NULL DEFAULT '',
  `description_en` text,
  PRIMARY KEY (`command_path`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- CREATE TABLE IF NOT EXISTS no amplía instalaciones anteriores. Se usa una
-- sentencia preparada porque MariaDB acepta ADD COLUMN IF NOT EXISTS, pero
-- MySQL 8.0 no: esta forma es idempotente en ambos motores.
SET @server_help_has_min_security := (
  SELECT COUNT(*) FROM `information_schema`.`COLUMNS`
  WHERE `TABLE_SCHEMA` = DATABASE()
    AND `TABLE_NAME` = 'server_help_command'
    AND `COLUMN_NAME` = 'min_security'
);
SET @server_help_add_min_security := IF(
  @server_help_has_min_security = 0,
  'ALTER TABLE `server_help_command` ADD COLUMN `min_security` tinyint unsigned NULL DEFAULT NULL COMMENT ''permiso exacto para consumidores externos; NULL = heredar categoria'' AFTER `keywords`',
  'SELECT 1'
);
PREPARE server_help_migration FROM @server_help_add_min_security;
EXECUTE server_help_migration;
DEALLOCATE PREPARE server_help_migration;

-- Columnas nuevas de la Tanda 4. Mismo patron PREPARE que arriba (el aplicador
-- de SQL parte en ';' y no entiende DELIMITER/CREATE PROCEDURE), idempotente en
-- MySQL 8 y MariaDB.
SET @c := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'server_help_command' AND COLUMN_NAME = 'auto');
SET @s := IF(@c = 0, 'ALTER TABLE `server_help_command` ADD COLUMN `auto` tinyint unsigned NOT NULL DEFAULT 0 AFTER `enabled`', 'SELECT 1');
PREPARE m FROM @s; EXECUTE m; DEALLOCATE PREPARE m;

SET @c := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'server_help_command' AND COLUMN_NAME = 'title_en');
SET @s := IF(@c = 0, 'ALTER TABLE `server_help_command` ADD COLUMN `title_en` varchar(120) NOT NULL DEFAULT '''' AFTER `auto`', 'SELECT 1');
PREPARE m FROM @s; EXECUTE m; DEALLOCATE PREPARE m;

SET @c := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'server_help_command' AND COLUMN_NAME = 'description_en');
SET @s := IF(@c = 0, 'ALTER TABLE `server_help_command` ADD COLUMN `description_en` text AFTER `title_en`', 'SELECT 1');
PREPARE m FROM @s; EXECUTE m; DEALLOCATE PREPARE m;

SET @c := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'server_help_article' AND COLUMN_NAME = 'title_en');
SET @s := IF(@c = 0, 'ALTER TABLE `server_help_article` ADD COLUMN `title_en` varchar(120) NOT NULL DEFAULT '''' AFTER `title`', 'SELECT 1');
PREPARE m FROM @s; EXECUTE m; DEALLOCATE PREPARE m;

SET @c := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'server_help_article' AND COLUMN_NAME = 'body_en');
SET @s := IF(@c = 0, 'ALTER TABLE `server_help_article` ADD COLUMN `body_en` text AFTER `body`', 'SELECT 1');
PREPARE m FROM @s; EXECUTE m; DEALLOCATE PREPARE m;

CREATE TABLE IF NOT EXISTS `server_help_rule` (
  `id` int unsigned NOT NULL,
  `prefix` varchar(90) NOT NULL COMMENT 'prefijo de ruta, por tokens: "teleport" casa con "teleport add"',
  `category_id` int unsigned NOT NULL,
  `sort` int NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ───────────────────────────────────────────────────────────────────────────
-- Categorias
-- ───────────────────────────────────────────────────────────────────────────
-- NOTA: los ON DUPLICATE KEY UPDATE usan VALUES(col), no el alias de fila
-- "AS new ... new.col" (MySQL 8.0.19+), que MariaDB no soporta. AzerothCore
-- admite ambos motores oficialmente.
INSERT INTO `server_help_category` (`id`,`parent_id`,`name`,`name_en`,`sort`,`min_security`,`enabled`) VALUES
(1, 0,'General',                'General',            10,0,1),
(2, 0,'Comandos de jugador',    'Player commands',    20,0,1),
(3, 0,'Personaje',              'Character',          30,0,1),
(4, 0,'Social',                 'Social',             40,0,1),
(5, 0,'Chat',                   'Chat',               50,0,1),
(6, 0,'Grupo',                  'Group',              60,0,1),
(7, 0,'Hermandad',              'Guild',              70,0,1),
(8, 0,'Eventos',                'Events',             80,0,1),
(9, 0,'Teletransportes',        'Teleports',          90,0,1),
(10,0,'Sistemas del servidor',  'Server systems',    100,0,1),
(11,0,'VIP',                    'VIP',               110,0,0),
(12,0,'Profesiones',            'Professions',       120,0,1),
(13,0,'Game Master',            'Game Master',       130,2,1),
(14,0,'Moderación',             'Moderation',        140,1,1),
(15,0,'Administración',         'Administration',    150,3,1)
ON DUPLICATE KEY UPDATE `parent_id`=VALUES(`parent_id`), `name`=VALUES(`name`), `name_en`=VALUES(`name_en`), `min_security`=VALUES(`min_security`);

-- ───────────────────────────────────────────────────────────────────────────
-- Reglas de categoria por prefijo (semilla: ids 1-999)
-- ───────────────────────────────────────────────────────────────────────────
DELETE FROM `server_help_rule` WHERE `id` < 1000;
INSERT INTO `server_help_rule` (`id`,`prefix`,`category_id`,`sort`) VALUES
-- jugador / general
(1,  'ayuda',            1, 0),
(2,  'help',             1, 0),
(3,  'commands',         1, 0),
(4,  'account',          2, 0),
(5,  'server info',      1, 0),
(6,  'server motd',      1, 0),
(7,  'save',             3, 0),
(8,  'dismount',         3, 0),
(9,  'gear',             3, 0),
(10, 'spect',            2, 0),
(11, 'aoeloot',          2, 0),
(12, 'transmog',         3, 0),
(13, 'whispers',         4, 0),
-- grupo y hermandad
(20, 'grupo',            6, 0),
(21, 'group',            6, 0),
(22, 'groupsummon',      6, 0),
(23, 'lfg',              6, 0),
(24, 'dc',               6, 0),
(25, 'tokenturnin',      6, 0),
(26, 'guild',            7, 0),
(27, 'hermandad',        7, 0),
(28, 'queuebots',        6, 0),
-- eventos
(30, 'event',            8, 0),
(31, 'wpvp',             8, 0),
(32, 'bf',               8, 0),
-- teletransportes
(40, 'teleport',         9, 0),
(41, 'go',               9, 0),
(42, 'appear',           9, 0),
(43, 'summon',           9, 0),
(44, 'recall',           9, 0),
(45, 'cometome',         9, 0),
(46, 'unstuck',          9, 0),
-- sistemas del servidor
(50, 'ahbot',           10, 0),
(51, 'ab',              10, 0),
(52, 'ip',              10, 0),
(53, 'playerbots',      10, 0),
(54, 'actualizaciones', 10, 0),
(55, 'server',          10, 0),
(56, 'instance',        10, 0),
(57, 'bots',            10, 0),
-- profesiones
(60, 'setskill',        12, 0),
(61, 'maxskill',        12, 0),
-- chat
(70, 'announce',         5, 0),
(71, 'gmannounce',       5, 0),
(72, 'gmnameannounce',   5, 0),
(73, 'nameannounce',     5, 0),
(74, 'notify',           5, 0),
(75, 'gmnotify',         5, 0),
(76, 'autobroadcast',    5, 0),
(77, 'chatfilter',       5, 0),
-- moderacion
(80, 'ban',             14, 0),
(81, 'baninfo',         14, 0),
(82, 'banlist',         14, 0),
(83, 'unban',           14, 0),
(84, 'kick',            14, 0),
(85, 'mute',            14, 0),
(86, 'unmute',          14, 0),
(87, 'mutehistory',     14, 0),
(88, 'ticket',          14, 0),
(89, 'pinfo',           14, 0),
(90, 'commentator',     14, 0),
(91, 'freeze',          14, 0),
(92, 'unfreeze',        14, 0),
-- administracion
(100,'account set',     15, 0),
(101,'account create',  15, 0),
(102,'account delete',  15, 0),
(103,'reload',          15, 0),
(104,'reset',           15, 0),
(105,'debug',           15, 0),
(106,'dev',             15, 0),
(107,'rbac',            15, 0),
(108,'mmap',            15, 0),
(109,'wp',              15, 0),
(110,'wpgps',           15, 0),
(111,'pool',            15, 0),
(112,'pooltools',       15, 0),
(113,'packetlog',       15, 0),
(114,'pdump',           15, 0),
(115,'cache',           15, 0),
(116,'string',          15, 0),
(117,'wchange',         15, 0),
(118,'server shutdown', 15, 0),
(119,'server restart',  15, 0),
(120,'server exit',     15, 0),
(121,'server idleshutdown', 15, 0),
(122,'server idlerestart',  15, 0),
(123,'server set',      15, 0),
(124,'skirmish',        15, 0),
(125,'flusharenapoints',15, 0);
-- Lo demas de nivel GM (npc, gobject, modify, additem, cast, learn, lookup,
-- list, quest, respawn, send...) cae en "Game Master" por su nivel, sin regla.

-- ───────────────────────────────────────────────────────────────────────────
-- Fichas de comandos en espanol (se actualizan enteras; `enabled` se conserva)
-- ───────────────────────────────────────────────────────────────────────────
INSERT INTO `server_help_command` (`command_path`,`category_id`,`title`,`description`,`syntax`,`examples`,`keywords`,`enabled`) VALUES
('ayuda', 1, 'Ayuda del servidor',
 'Base de conocimiento del servidor: la usa la pestaña "Solicitud de ayuda" del cliente (addon ServerHelp), pero también se puede consultar desde el chat.\nSólo enseña lo que tu cuenta puede usar. Los textos salen en español o inglés según el idioma del cliente.',
 '.ayuda [buscar <texto> | comando <ruta> | articulo <id> | indice]',
 '.ayuda buscar grupo\n.ayuda comando grupo banda', 'ayuda kb conocimiento comandos', 1),
('ayuda buscar', 1, 'Buscar en la ayuda',
 'Busca el texto en los nombres de comando, títulos, descripciones, palabras clave y categorías de todo lo que tu cuenta puede ver.',
 '.ayuda buscar <texto>', '.ayuda buscar teletransporte\n.ayuda buscar mazmorra', 'buscar', 1),
('ayuda comando', 1, 'Ficha de un comando',
 'Muestra la ficha de un comando: uso, descripción, permiso y ejemplos. La ruta va sin el punto inicial.',
 '.ayuda comando <ruta>', '.ayuda comando grupo\n.ayuda comando dc on', 'ficha comando', 1),

('grupo', 6, 'Grupo de bots donde estás',
 'Forma un grupo de cinco con bots de tu facción y de tu nivel (ninguno por encima: te robaría experiencia), con tanque y sanador de verdad, y los trae a tu lado en unos segundos. Sirve para misiones de élite y para entrar a una mazmorra andando, sin buscador.\nLos bots de tu hermandad tienen preferencia. En cuanto pisáis una instancia, mod-queue-bots toma el mando: activa el tanque bot (.dc) si lo hay y canjea los tokens tras cada jefe.\nMientras un bot sea tu compañero, también lleva tus misiones activas que pueda coger (mazmorra y banda incluidas), para que recoja su propia copia del botín de misión en grupo; se avisa por chat y se le retira si la abandonas, la entregas o deja de acompañarte.\nSin argumentos forma el grupo de cinco. Con un número, ese número de bots. Si todavía no hay bots despiertos, la petición se queda pendiente y se completa sola en cuanto lleguen (hasta tres minutos; ".grupo estado" la muestra y ".grupo fuera" la cancela); el tanque y el sanador se esperan un rato antes de cubrir su plaza con otro rol. Si un compañero se cae de un grupo manual, se repone (no si lo echas tú); si te desconectas y vuelves pronto, se recompone el grupo.',
 '.grupo [n | mazmorra | banda [10|25|40] | fuera [nombre] | cambia <nombre> | quieto [nombre] | sigue [nombre] | estado]',
 '.grupo\n.grupo 2\n.grupo banda 10\n.grupo quieto\n.grupo fuera', 'grupo bots party companeros mazmorra elite quieto sigue reponer pendiente cambiar', 1),
('grupo mazmorra', 6, 'Grupo de cinco para mazmorra',
 'Igual que ".grupo" sin argumentos: tanque, sanador y daño, menos el rol que tú ocupes.',
 '.grupo mazmorra', '.grupo mazmorra', 'grupo mazmorra cinco', 1),
('grupo quieto', 6, 'Plantar a los compañeros',
 'Los compañeros del grupo se quedan donde están en vez de seguirte (playerbots: "-follow,+stay"). Con un nombre, solo ese. Se deshace con ".grupo sigue".',
 '.grupo quieto [nombre]', '.grupo quieto\n.grupo quieto Tanquebot', 'grupo quieto quieta parar seguir stay bot', 1),
('grupo sigue', 6, 'Que vuelvan a seguirte',
 'Deshace ".grupo quieto": los compañeros vuelven a seguirte. Con un nombre, solo ese.',
 '.grupo sigue [nombre]', '.grupo sigue\n.grupo sigue Tanquebot', 'grupo sigue seguir follow bot', 1),
('grupo banda', 6, 'Una banda, sin cola',
 'Forma una banda de 10, 25 o 40 con bots de tu nivel: 2/3, 3/6 o 5/12 tanques y sanadores. Sin número, según tu dificultad de banda actual. Los bots se teletransportan a tu lado y se les ajusta el equipo a tu fase de progresión.',
 '.grupo banda [10|25|40]', '.grupo banda 10\n.grupo banda 25', 'banda raid 10 25 40', 1),
('grupo fuera', 6, 'Despedir a los bots del grupo',
 'Sin nombre, todos los bots que trajo ".grupo" se van, el grupo se deshace y se cancela lo que los traería de vuelta: la petición pendiente, la reposición y la recomposición tras desconectar. Con un nombre, solo ese compañero se despide y el grupo pasa a ser uno más pequeño: no se repone ni vuelve ese bot. Si está en combate o de viaje, se va en cuanto termine (".grupo estado" lo muestra). Los que hayas invitado tú a mano no se tocan. Para cambiar a uno por otro, ".grupo cambia".',
 '.grupo fuera [nombre]', '.grupo fuera\n.grupo fuera Tanquebot', 'fuera salir deshacer grupo companero nombre', 1),
('grupo cambia', 6, 'Cambiar a un compañero por otro',
 'Ese compañero se va y se busca otro bot para su plaza, sin bajar el tamaño del grupo; el que se fue no vuelve en un rato. Si está en combate o de viaje, el cambio se hace en cuanto termine.',
 '.grupo cambia <nombre>', '.grupo cambia Tanquebot', 'cambia cambiar sustituir reemplazar companero bot', 1),
('grupo estado', 6, 'Quién está en el grupo y por qué',
 'Lista los bots del grupo con su rol y el motivo por el que vinieron (comando, misión de grupo), los que se van al terminar el combate y la petición de compañeros pendiente, si la hay.',
 '.grupo estado', '.grupo estado', 'estado grupo pendiente', 1),

('hermandad', 7, 'Tu hermandad de casa',
 'mod-home-guild: si fundas una hermandad por el flujo normal del juego, se convierte en tu "hermandad de casa" y se rellena con hasta 15 bots de tu facción y tu nivel que se conectan contigo, suben de nivel contigo y van primero en las colas. El roster busca tanques y sanadores de sobra, y su equipo se mantiene a tu fase. El módulo nunca crea ni adopta hermandades por su cuenta.\nSin argumentos muestra el estado (roster, roles y conectados).',
 '.hermandad [estado | activar | desactivar | renovar | fijar <nombre> | soltar <nombre> | excluir <nombre>]',
 '.hermandad estado\n.hermandad renovar\n.hermandad fijar Tanquebot\n.hermandad excluir Malbot', 'hermandad gremio guild bots companeros casa home fijar excluir renovar roster', 1),
('hermandad renovar', 7, 'Reclutar y nivelar ya',
 'Fuerza una pasada de cuidado de la hermandad: reclutar hasta el objetivo y subir de nivel a los rezagados, sin esperar al ciclo normal.',
 '.hermandad renovar', '.hermandad renovar', 'hermandad renovar reclutar nivelar', 1),
('hermandad fijar', 7, 'Fijar un compañero',
 'Marca un bot del roster como fijado: se conecta el primero, se muestra el primero y no se le echa. Se deshace con ".hermandad soltar".',
 '.hermandad fijar <nombre>', '.hermandad fijar Tanquebot', 'hermandad fijar pin favorito bot', 1),
('hermandad soltar', 7, 'Quitar la marca de un compañero',
 'Deshace ".hermandad fijar" o ".hermandad excluir" sobre ese bot.',
 '.hermandad soltar <nombre>', '.hermandad soltar Tanquebot', 'hermandad soltar quitar marca pin', 1),
('hermandad excluir', 7, 'Excluir un compañero',
 'Saca un bot de la hermandad y evita que se le vuelva a reclutar. Se deshace con ".hermandad soltar".',
 '.hermandad excluir <nombre>', '.hermandad excluir Malbot', 'hermandad excluir quitar bot no reclutar', 1),
('hermandad estado', 7, 'Roster de la hermandad de casa',
 'Lista los bots de tu hermandad de casa con su nivel y si están conectados ahora.',
 '.hermandad estado', '.hermandad estado', 'estado roster hermandad bots', 1),
('hermandad activar', 7, 'Convertir tu hermandad en hermandad de casa',
 'Marca tu hermandad actual como hermandad de casa y empieza a poblarla con bots. Debes ser su fundador y líder. Útil cuando HomeGuild.AutoAdopt está en 0 (fundar una hermandad no la adopta automáticamente).',
 '.hermandad activar', '.hermandad activar', 'activar adoptar hermandad casa', 1),
('hermandad desactivar', 7, 'Vaciar de bots la hermandad de casa',
 'Expulsa a todos los bots de la hermandad y deja de poblarla. La hermandad sigue existiendo con sus miembros humanos; se puede volver a activar con ".hermandad activar".',
 '.hermandad desactivar', '.hermandad desactivar', 'desactivar vaciar quitar bots hermandad', 1),

('dc', 6, 'El tanque bot lleva la mazmorra',
 'mod-dungeon-clear: un tanque bot recorre la mazmorra de punta a punta (ruta entre jefes, pulls, botín, descanso, resurrección). Tú no puedes ser el tanque: apúntate de daño o sanador.\nCon el grupo del buscador dentro de la mazmorra, mod-queue-bots lo activa solo; estos comandos son para llevarlo a mano.',
 '.dc on|off|pause|skip|pull|status|bosses|go|wing|config|spectate', '.dc on\n.dc status\n.dc go devorador\n.dc off', 'dungeon clear mazmorra tanque bot automatico', 1),
('dc on', 6, 'Activar el tanque bot', 'Arranca el recorrido automático. Hace falta un tanque bot en el grupo y estar dentro de la instancia. En Roca Negra (Cumbre y Profundidades) se puede indicar el ala: lbrs, ubrs, brd-db o brd-uc.', '.dc on [ala]', '.dc on\n.dc on ubrs', 'activar dc', 1),
('dc off', 6, 'Parar el tanque bot', 'Detiene el recorrido: los bots vuelven a seguirte a ti.', '.dc off', '.dc off', 'parar dc', 1),
('dc pause', 6, 'Pausar o reanudar', 'Pausa el avance (y lo reanuda si ya estaba en pausa). Útil antes de un jefe.', '.dc pause', '.dc pause', 'pausa dc', 1),
('dc skip', 6, 'Saltar el paso actual', 'Si el tanque se atasca en un evento con guion, salta al siguiente objetivo.', '.dc skip', '.dc skip', 'saltar dc', 1),
('dc status', 6, 'Estado del recorrido', 'Dice si está activo, en qué paso va y a quién espera.', '.dc status', '.dc status', 'estado dc', 1),
('dc bosses', 6, 'Jefes de la mazmorra', 'Lista los jefes de la instancia y cuáles quedan.', '.dc bosses', '.dc bosses', 'jefes dc', 1),

('tokenturnin', 6, 'Canje de tokens de los bots',
 'mod-token-turnin: convierte los tokens de tier que ganan los bots de tu grupo en la pieza de su especialización. mod-queue-bots lo lanza solo 90 segundos después de matar un jefe; estos comandos son para hacerlo a mano.',
 '.tokenturnin check|redeem', '.tokenturnin check\n.tokenturnin redeem', 'token tier canje bots', 1),
('tokenturnin check', 6, 'Ver qué tokens tienen los bots', 'Lista los tokens canjeables que llevan los bots del grupo, sin canjear nada.', '.tokenturnin check', '.tokenturnin check', 'token ver', 1),
('tokenturnin redeem', 6, 'Canjear los tokens', 'Canjea los tokens de los bots del grupo por su pieza de tier.', '.tokenturnin redeem', '.tokenturnin redeem', 'token canjear', 1),

('transmog', 3, 'Transfiguración',
 'mod-transmog. Sin argumentos activa o desactiva ver las transfiguraciones. El NPC de transfiguración (Warpweaver) está en las once capitales junto al Dungeon Master.',
 '.transmog [claim [all] | sync | portable | interface | disclaimer]', '.transmog claim\n.transmog', 'transmog transfiguracion aspecto', 1),
('transmog claim', 3, 'Añadir aspectos a tu colección',
 'Añade a la colección de transfiguración el aspecto del objeto que tengas en la bolsa (o de todos con "all"), sin tener que ir al NPC.',
 '.transmog claim [all]', '.transmog claim\n.transmog claim all', 'coleccion aspecto claim', 1),
('aoeloot', 2, 'Botín en área',
 'Al abrir un cadáver se recoge también el botín de los cadáveres cercanos. Si estás en grupo, los objetos blancos de misión de cada miembro elegible se recogen en su propia bolsa sin quitar las copias ajenas. Aquí activas o desactivas el saqueo en área para tu personaje.',
 '.aoeloot on|off', '.aoeloot on', 'botin area loot', 1),

('wpvp', 8, 'Guerra de mundo (bots)',
 'mod-world-bots: escaramuzas entre facciones y duelos en 23 puntos calientes (Costasur, Molino Tarren, El Cruce, Astranaar...), sólo con bots libres y sólo en zonas con jugador. Por defecto salta sola cada ~4 minutos de media; estos comandos la gobiernan.',
 '.wpvp [estado | lista | iniciar <nombre> | parar [id] | recargar]', '.wpvp lista\n.wpvp iniciar Southshore\n.wpvp parar', 'guerra mundo pvp escaramuza duelo', 1),
('wpvp estado', 8, 'Eventos de guerra activos', 'Qué escaramuzas y duelos hay en marcha, con su id, zona y tiempo restante.', '.wpvp estado', '.wpvp estado', 'estado guerra', 1),
('wpvp lista', 8, 'Puntos calientes', 'Lista los puntos calientes de la tabla world_bots_pvp_hotspot con su nombre interno (el que usa "iniciar"), nivel y estado: on / off, o "auto" / "auto-espera" para los de 50-60 que se encienden solos cuando hay población de ese nivel.', '.wpvp lista', '.wpvp lista', 'lista puntos calientes auto', 1),
('wpvp iniciar', 8, 'Forzar una escaramuza', 'Arranca ya el evento del punto caliente indicado (nombre interno de ".wpvp lista"), aunque no toque por azar.', '.wpvp iniciar <nombre>', '.wpvp iniciar Southshore\n.wpvp iniciar StormwindDuel', 'iniciar forzar guerra', 1),
('wpvp parar', 8, 'Terminar un evento', 'Termina el evento indicado (o todos si no se da id): los bots recuperan sus estrategias y vuelven a donde estaban.', '.wpvp parar [id]', '.wpvp parar\n.wpvp parar 3', 'parar guerra', 1),
('wpvp recargar', 8, 'Recargar los puntos calientes', 'Vuelve a leer la tabla world_bots_pvp_hotspot sin reiniciar.', '.wpvp recargar', '.wpvp recargar', 'recargar hotspots', 1),
('actualizaciones', 10, 'Versiones nuevas río arriba',
 'mod-update-notice: vuelve a mostrar el aviso de repositorios (core y módulos) con commits nuevos sobre versions.lock, ordenados por severidad ([seguridad], [incompatibilidad], [funcional], [informativo]). Lo escribe tools/revisar-actualizaciones.sh; el servidor no se toca solo. Avisa si el informe quedó anterior a un ./install.sh --freeze.',
 '.actualizaciones', '.actualizaciones', 'actualizaciones versiones update severidad', 1),

('ip', 10, 'Progresión individual',
 'mod-individual-progression: consulta y administra la fase Vanilla → TBC → WotLK, teletransportes, reputaciones y attunements de personajes y bots.\nOjo: ".ip set" desde la consola del servidor no es seguro; úsalo desde el juego.',
 '.ip <get|set|tele|setbot|setrep|pvp|attune> ...', '.ip get Lightcore\n.ip set Lightcore 5', 'progresion fase vanilla tbc wotlk attune reputacion', 1),
('ahbot', 10, 'Bot de la casa de subastas',
 'mod-ah-bot-plus: la subasta tarda horas en llenarse sola; "update" la repuebla al momento (repetir varias veces al principio), "reload" recarga el .conf y "empty" la vacía (sólo lo del bot).',
 '.ahbot update|reload|empty', '.ahbot update', 'subasta ahbot casa de subastas', 1),

('teleport', 9, 'Teletransporte por nombre',
 'Te lleva a una ubicación de la lista de teletransportes del servidor (tabla game_tele). Sin argumento no hace nada: hay que dar el nombre.',
 '.teleport <ubicacion>', '.teleport dalaran\n.teleport stormwind', 'tele teletransporte viajar', 1),
('teleport add', 9, 'Guardar tu posición como destino', 'Añade el sitio donde estás a la lista de ".teleport" con ese nombre.', '.teleport add <nombre>', '.teleport add micasa', 'anadir destino tele', 1),
('go', 9, 'Ir a un sitio, criatura u objeto',
 'Familia de teletransportes de GM: a coordenadas, a una criatura por entrada o guid, a un objeto, a un cementerio, a una zona...',
 '.go <xyz|creature|object|zonexy|graveyard|ticket|...> ...', '.go xyz -8842 626 94.3 0\n.go creature id 500000 1', 'go ir coordenadas criatura', 1),
('gm', 13, 'Modo Game Master',
 'Activa o desactiva la bandera de GM. La visibilidad, el vuelo, la insignia del chat y el modo espectador se controlan con subcomandos separados.',
 '.gm <on|off|visible|fly|chat|spectator|ingame|list> ...', '.gm on\n.gm visible off\n.gm chat on', 'gm modo maestro juego invisible vuelo insignia', 1),
('additem', 13, 'Dar un objeto', 'Crea en tu bolsa (o en la del jugador seleccionado) N unidades del objeto. Con ".lookup item <nombre>" se saca el id.', '.additem <id|[enlace]> [cantidad]', '.additem 2589 20', 'objeto item dar', 1),
('levelup', 13, 'Subir niveles', 'Sube (o baja, con negativo) N niveles al jugador seleccionado o a ti.', '.levelup [personaje] [niveles]', '.levelup 9\n.levelup Lightcore 1', 'nivel subir', 1),
('learn', 13, 'Aprender un hechizo', 'Enseña el hechizo al jugador seleccionado o a ti. "all" y sus variantes aprenden familias enteras.', '.learn <id> | .learn all ...', '.learn 1459', 'hechizo aprender', 1),
('revive', 13, 'Revivir', 'Revive al jugador seleccionado (o a ti) donde está.', '.revive [personaje]', '.revive', 'revivir resucitar', 1),
('npc add', 13, 'Crear un NPC aquí', 'Aparece la criatura con esa entrada donde estás, de forma permanente (se guarda en la BD).', '.npc add <entrada>', '.npc add 190010', 'npc crear spawn', 1),
('npc delete', 13, 'Borrar el NPC seleccionado', 'Elimina de la BD la criatura seleccionada.', '.npc delete', '.npc delete', 'npc borrar', 1),
('lookup item', 13, 'Buscar un objeto por nombre', 'Lista los objetos cuyo nombre contenga el texto, con su id.', '.lookup item <texto>', '.lookup item bolsa', 'buscar objeto id', 1),
('lookup creature', 13, 'Buscar una criatura por nombre', 'Lista las criaturas cuyo nombre contenga el texto, con su entrada.', '.lookup creature <texto>', '.lookup creature warpweaver', 'buscar criatura entrada', 1),
('instance unbind', 10, 'Soltar instancias guardadas', 'Quita las vinculaciones de instancia del jugador seleccionado ("all" o un mapa).', '.instance unbind <all|mapa> [dificultad]', '.instance unbind all', 'instancia vinculacion reset', 1),
('reload config', 15, 'Recargar el worldserver.conf', 'Vuelve a leer worldserver.conf y los .conf de los módulos que lo soporten, sin reiniciar. Muchas opciones de módulos sólo se leen al arrancar.', '.reload config', '.reload config', 'recargar configuracion', 1),
('announce', 5, 'Anuncio a todo el servidor', 'Manda el texto a todos los jugadores como mensaje del sistema.', '.announce <texto>', '.announce El servidor se reinicia en 5 minutos', 'anuncio todos', 1),
('server info', 1, 'Información del servidor', 'Versión del core, jugadores conectados, tiempo en marcha y carga.', '.server info', '.server info', 'servidor info version uptime', 1),
('server shutdown', 15, 'Apagar el servidor', 'Apaga el worldserver en N segundos avisando a los jugadores. Con systemd, el servicio no vuelve a arrancar solo: usa "sudo systemctl restart ac-worldserver" desde la VM para reiniciar con aviso.', '.server shutdown <segundos> [codigo]', '.server shutdown 60', 'apagar servidor', 1),
('server restart', 15, 'Reiniciar el servidor', 'Reinicia el worldserver en N segundos avisando a los jugadores (con el servicio systemd, que lo vuelve a levantar).', '.server restart <segundos>', '.server restart 60', 'reiniciar servidor', 1),
('account', 2, 'Tu cuenta', 'Muestra el nivel de acceso de tu cuenta (y el correo si tienes permiso). Con subcomandos, gestiona la cuenta.', '.account', '.account', 'cuenta acceso nivel', 1),
('account password', 2, 'Cambiar tu contraseña', 'Cambia la contraseña de tu cuenta. Hay que repetir la nueva dos veces.', '.account password <actual> <nueva> <nueva>', '.account password admin Nueva123 Nueva123', 'contrasena password cambiar', 1),
('help', 1, 'Ayuda del core', 'La ayuda clásica del core, en el chat: sin argumento lista los comandos disponibles; con uno, su ayuda.', '.help [comando]', '.help\n.help teleport', 'help ayuda core', 1),
('commands', 1, 'Lista de comandos', 'Lista los comandos de primer nivel que tu cuenta puede usar.', '.commands', '.commands', 'comandos lista', 1),
('save', 3, 'Guardar el personaje', 'Fuerza el guardado de tu personaje en la base de datos ahora mismo.', '.save', '.save', 'guardar personaje', 1),
('dismount', 3, 'Desmontar', 'Te baja de la montura.', '.dismount', '.dismount', 'desmontar montura', 1),
('gear', 3, 'Estadísticas de equipo', 'Muestra la puntuación de equipo (gear score) del personaje seleccionado o del tuyo.', '.gear stats', '.gear stats', 'gear equipo puntuacion', 1),
('ticket', 14, 'Consultas de los jugadores', 'Gestión de los tickets que abren los jugadores desde "Solicitud de ayuda": listar, asignar, responder, cerrar.', '.ticket <list|onlinelist|assign|comment|complete|close|delete|response|viewname|viewid|...>', '.ticket list\n.ticket viewid 3\n.ticket response append 3 Ya esta arreglado', 'ticket consulta soporte gm', 1)
ON DUPLICATE KEY UPDATE `category_id`=VALUES(`category_id`), `title`=VALUES(`title`), `description`=VALUES(`description`), `syntax`=VALUES(`syntax`), `examples`=VALUES(`examples`), `keywords`=VALUES(`keywords`);

-- La categoría es temática; el permiso es propio de cada comando. El panel
-- web no dispone de la sesión del juego con la que el core hace su filtro y
-- necesita este dato explícito (el módulo lo ignora: su filtro es la sesión).
UPDATE `server_help_command` SET `min_security`=2 WHERE `command_path` IN
('wpvp','wpvp estado','wpvp lista','wpvp iniciar','wpvp parar','wpvp recargar',
 'actualizaciones','ip','ahbot','teleport','instance unbind','announce','ticket');
UPDATE `server_help_command` SET `min_security`=3 WHERE `command_path` IN
('teleport add','npc add','npc delete');
UPDATE `server_help_command` SET `min_security`=1 WHERE `command_path` IN
('go','gm','lookup item','lookup creature');
-- El primer bloque de INSERT no trae min_security en su lista de columnas
-- (§8.2 2.4): lo que no cae en un UPDATE de arriba son comandos de jugador, se
-- fija 0 explícito para que la columna quede consistente con los demás bloques
-- y el panel no dependa de coaccionar NULL.
UPDATE `server_help_command` SET `min_security`=0 WHERE `min_security` IS NULL;

-- Fichas adicionales: comandos de jugador y subcomandos que antes sólo se
-- intuían desde una ficha de familia.
INSERT INTO `server_help_command`
(`command_path`,`category_id`,`title`,`description`,`syntax`,`examples`,`keywords`,`min_security`,`enabled`) VALUES
('ayuda version',1,'Versión del catálogo','Muestra la huella del índice, el número de entradas y el nivel con el que se ha construido. Útil para comprobar que el addon y el servidor están sincronizados.','.ayuda version','.ayuda version','ayuda version diagnostico indice',0,1),
('ayuda indice',1,'Índice completo de ayuda','Lista todas las entradas que el core permite ver a tu sesión, ordenadas por categoría.','.ayuda indice','.ayuda indice','ayuda indice todos comandos',0,1),
('ayuda articulo',1,'Abrir un artículo','Abre un artículo de la base de conocimiento por su identificador. El buscador y el índice enseñan ese número.','.ayuda articulo <id>','.ayuda articulo 2','ayuda articulo guia',0,1),
('ayuda recargar',15,'Recargar la base de conocimiento','Vuelve a leer categorías, artículos, fichas y reglas desde acore_world sin reiniciar el servidor.','.ayuda recargar','.ayuda recargar','ayuda recargar sql cache',3,1),
('ayuda cobertura',15,'Cobertura de fichas','Cuántos comandos del árbol visible tienen ficha propia, cuántos caen solo por regla de categoría y cuántos por nivel; lista hasta 40 sin ficha. Útil para saber qué documentar.','.ayuda cobertura','.ayuda cobertura','ayuda cobertura fichas comandos sin documentar',3,1),
('ayuda export',15,'Exportar el árbol al panel','Vuelca los comandos descubiertos que no tienen ficha propia como filas auto=1 en server_help_command, para que el panel web vea el árbol completo. No toca las fichas curadas. Ejecútalo tras añadir módulos y luego ".ayuda recargar".','.ayuda export','.ayuda export','ayuda export panel web catalogo comandos auto',3,1),

('dc pull',6,'Cambiar el modo de pull','Alterna cómo reúne enemigos el tanque bot durante el recorrido. Se aplica a la ejecución actual.','.dc pull [modo]','.dc pull','dungeon clear pull atraer enemigos',0,1),
('dc go',6,'Ir directamente a un jefe','Cambia la ruta del tanque bot para dirigirse al jefe indicado por nombre. Consulta primero ".dc bosses".','.dc go <jefe>','.dc go devorador','dungeon clear jefe ruta ir',0,1),
('dc config',6,'Configuración efectiva del recorrido','Muestra los valores DungeonClear efectivos, incluidas las anulaciones del addon y los valores heroicos.','.dc config','.dc config','dungeon clear configuracion valores',0,1),
('dc spectate',6,'Cámara de espectador','Activa la cámara libre. Con "follow" hace que la cámara siga a un bot; no exige pertenecer al grupo.','.dc spectate [follow [nombre]]','.dc spectate\n.dc spectate follow Tanquebot','dungeon clear camara espectador seguir',0,1),
('dc wing',6,'Ala de la mazmorra','En las mazmorras partidas en alas (Cumbre de Roca Negra: lbrs / ubrs; Profundidades: brd-db / brd-uc) enseña el ala que recorre el tanque bot o la cambia. No activa ni para el recorrido.','.dc wing [ala]','.dc wing\n.dc wing ubrs','dungeon clear ala roca negra cumbre profundidades',0,1),

('queuebots',6,'Bots para tu cola',
 'mod-queue-bots: cuando te pones en una cola (buscador de mazmorras, buscador de bandas, campo de batalla, arena, 1c1) se rellena sola con bots de tu tramo de nivel y de los roles que falten. En campos de batalla y arenas, si un bot se cae a mitad de partida se repone. Estos comandos son para ver cómo va y forzarlo; sin argumentos muestra el estado.',
 '.queuebots [estado | traer | salir]','.queuebots\n.queuebots traer\n.queuebots salir','cola queue bots mazmorra banda campo batalla arena rellenar faltan',0,1),
('queuebots estado',6,'Estado del rellenado de tu cola','Cuántos bots hay apuntados a tu cola actual, de qué roles (tanque, sanador, daño) y cuántos faltan por entrar.','.queuebots estado','.queuebots estado','cola estado bots faltan roles',0,1),
('queuebots traer',6,'Forzar otra pasada de rellenado','Pide que se revise tu cola en la próxima pasada sin esperar al intervalo normal, por si faltan bots.','.queuebots traer','.queuebots traer','cola forzar traer rellenar bots pasada',0,1),
('queuebots salir',6,'Sacar los bots de tu cola','Saca de tu cola actual todos los bots que metió el módulo; tú sigues en la cola. Útil si prefieres esperar a gente de verdad.','.queuebots salir','.queuebots salir','cola salir quitar bots opt-out esperar gente',0,1),

('q1v1',8,'Cola de arena 1c1','Familia de comandos del módulo de arena 1c1. El personaje debe cumplir los requisitos configurados por el módulo.','.q1v1 <rated|unrated|stats>','.q1v1 unrated','arena 1c1 cola duelo',0,1),
('q1v1 rated',8,'Apuntarse a 1c1 puntuada','Entra o sale de la cola de arena 1c1 puntuada.','.q1v1 rated','.q1v1 rated','arena 1c1 puntuada rating cola',0,1),
('q1v1 unrated',8,'Apuntarse a 1c1 no puntuada','Entra o sale de la cola de refriega 1c1, sin afectar a la puntuación.','.q1v1 unrated','.q1v1 unrated','arena 1c1 refriega no puntuada cola',0,1),
('q1v1 stats',8,'Estadísticas de 1c1','Muestra las estadísticas y la puntuación del personaje en la modalidad 1c1.','.q1v1 stats','.q1v1 stats','arena 1c1 estadisticas rating',0,1),

('worldboss',8,'Jefes de mundo instanciados','Familia de comandos de mod-instanced-worldbosses. Los bloqueos son individuales por personaje.','.worldboss locks','.worldboss locks','jefe mundo instancia bloqueo',0,1),
('worldboss locks',8,'Bloqueos de jefes de mundo','Lista los jefes de mundo derrotados por el personaje y cuándo caduca cada bloqueo; limpia los que ya hayan vencido.','.worldboss locks','.worldboss locks','jefe mundo bloqueo reinicio',0,1),

('playerbots',10,'Administrar Playerbots','Familia de comandos directos del módulo Playerbots. Para dar órdenes cotidianas a los bots resulta más cómodo el addon MultiBot.','.playerbots <bot|account> ...','.playerbots bot self','playerbots bots cuenta multibot',0,1),
('playerbots bot',10,'Orden directa a Playerbots','Envía una orden al gestor de bots. Admite las órdenes que entiende la versión instalada de mod-playerbots.','.playerbots bot <orden>','.playerbots bot self','playerbots bot orden ia',0,1),
('playerbots account setkey',10,'Establecer clave de vinculación','Define la clave que permite vincular otra cuenta para usar sus personajes como bots. No reutilices la contraseña de la cuenta.','.playerbots account setKey <clave>','.playerbots account setKey MiClaveBots','playerbots cuenta clave vincular',0,1),
('playerbots account link',10,'Vincular una cuenta de bots','Vincula otra cuenta mediante la clave de seguridad configurada en ella.','.playerbots account link <cuenta> <clave>','.playerbots account link SECUNDARIA MiClaveBots','playerbots cuenta vincular link',0,1),
('playerbots account linkedaccounts',10,'Ver cuentas vinculadas','Lista las cuentas cuyos personajes puedes incorporar como bots.','.playerbots account linkedAccounts','.playerbots account linkedAccounts','playerbots cuentas vinculadas lista',0,1),
('playerbots account unlink',10,'Desvincular una cuenta','Elimina una vinculación de cuentas de Playerbots.','.playerbots account unlink <cuenta>','.playerbots account unlink SECUNDARIA','playerbots cuenta desvincular unlink',0,1),

('autobalance',10,'Escalado automático de instancias','Consulta cómo mod-autobalance está escalando la instancia. ".ab" es un alias completo de ".autobalance".','.autobalance <getoffset|mapstat|creaturestat|setoffset>','.autobalance mapstat\n.ab getoffset','autobalance ab dificultad instancia escala',0,1),
('autobalance getoffset',10,'Ver ajuste de dificultad','Muestra el desplazamiento global aplicado al número de jugadores efectivo. También funciona como ".ab getoffset".','.autobalance getoffset','.autobalance getoffset','autobalance dificultad offset',0,1),
('autobalance mapstat',10,'Estadísticas de escalado de la instancia','Dentro de una instancia, muestra jugadores efectivos, nivel, salud y daño escalados. También funciona como ".ab mapstat".','.autobalance mapstat','.autobalance mapstat','autobalance mapa estadisticas escala',0,1),
('autobalance creaturestat',10,'Escalado de la criatura seleccionada','Muestra los valores originales y escalados de la criatura seleccionada dentro de una instancia. También funciona como ".ab creaturestat".','.autobalance creaturestat','.autobalance creaturestat','autobalance criatura estadisticas escala',0,1),
('autobalance setoffset',13,'Cambiar ajuste de dificultad','Cambia temporalmente el desplazamiento global del número de jugadores efectivo. Afecta al balance de las instancias. También funciona como ".ab setoffset".','.autobalance setoffset <numero>','.autobalance setoffset 1','autobalance dificultad offset cambiar',2,1),

('aoeloot on',2,'Activar botín en área','Activa el saqueo de cadáveres cercanos para el personaje.','.aoeloot on','.aoeloot on','botin area activar loot',0,1),
('aoeloot off',2,'Desactivar botín en área','Desactiva el saqueo de cadáveres cercanos para el personaje.','.aoeloot off','.aoeloot off','botin area desactivar loot',0,1),

('transmog sync',3,'Sincronizar la colección','Fuerza el envío de la colección de apariencias al addon de transfiguración.','.transmog sync','.transmog sync','transmog sincronizar coleccion addon',0,1),
('transmog portable',3,'Transfiguración portátil','Abre o configura la interfaz portátil de transfiguración si está habilitada en el servidor.','.transmog portable','.transmog portable','transmog portatil interfaz',0,1),
('transmog interface',3,'Preferencia de interfaz','Activa o desactiva la interfaz de addon del sistema de transfiguración.','.transmog interface','.transmog interface','transmog interfaz addon opcion',0,1),
('transmog disclaimer',3,'Aviso de transfiguración','Muestra o cambia la preferencia del aviso informativo de transfiguración.','.transmog disclaimer','.transmog disclaimer','transmog aviso disclaimer',0,1),
('transmog add',14,'Añadir una apariencia','Añade a una colección la apariencia del objeto indicado; actúa sobre el jugador seleccionado cuando corresponde.','.transmog add <objeto>','.transmog add 2589','transmog gm coleccion añadir',1,1),
('transmog add set',14,'Añadir un conjunto de apariencias','Añade a la colección un conjunto completo de objetos.','.transmog add set <id>','.transmog add set 1','transmog gm coleccion conjunto',1,1),
('transmog check',13,'Comprobar una transfiguración','Inspecciona la información de transfiguración del objetivo o del objeto indicado.','.transmog check ...','.transmog check','transmog comprobar diagnostico',2,1),
('transmog reload',15,'Recargar Transmog','Recarga la configuración de mod-transmog sin reiniciar el servidor.','.transmog reload','.transmog reload','transmog recargar configuracion',3,1),

('account 2fa',2,'Doble factor de la cuenta','Configura o elimina la autenticación de doble factor de tu propia cuenta. El servidor debe tener configurado el mismo secreto maestro en authserver y worldserver.','.account 2fa <setup|remove>','.account 2fa setup','cuenta 2fa totp seguridad',0,1),
('account 2fa setup',2,'Activar doble factor','Inicia la configuración TOTP de la cuenta y muestra los datos necesarios para el autenticador.','.account 2fa setup','.account 2fa setup','cuenta 2fa totp activar',0,1),
('account 2fa remove',2,'Quitar doble factor','Desactiva TOTP después de comprobar un código válido.','.account 2fa remove <codigo>','.account 2fa remove 123456','cuenta 2fa totp quitar',0,1),
('account lock ip',2,'Bloquear la cuenta a esta IP','Activa o desactiva que la cuenta sólo pueda conectarse desde la dirección IP actual.','.account lock ip <on|off>','.account lock ip on','cuenta bloquear ip seguridad',0,1),
('account lock country',2,'Bloquear la cuenta al país','Activa o desactiva la restricción de acceso al país detectado para la cuenta. Requiere la base GeoIP del servidor.','.account lock country <on|off>','.account lock country on','cuenta bloquear pais geoip seguridad',0,1),
('server motd',1,'Mensaje del día','Muestra el mensaje del día configurado por el servidor.','.server motd','.server motd','servidor motd mensaje dia',0,1),
('spect',8,'Espectador de arenas','Familia de comandos para observar arenas sin participar.','.spect <spectate|watch|leave|reset|version> ...','.spect watch Jugador','arena espectador observar',0,1),
('spect spectate',8,'Observar al objetivo','Empieza a observar al jugador seleccionado dentro de una arena.','.spect spectate','.spect spectate','arena espectador objetivo',0,1),
('spect watch',8,'Observar por nombre','Empieza a observar en arena al personaje indicado.','.spect watch <personaje>','.spect watch Lightcore','arena espectador nombre',0,1),
('spect leave',8,'Salir del modo espectador','Abandona la arena observada y restaura el estado del personaje.','.spect leave','.spect leave','arena espectador salir',0,1),
('spect reset',8,'Restablecer la cámara','Restablece el seguimiento y las opciones de la cámara de espectador.','.spect reset','.spect reset','arena espectador camara reset',0,1),
('spect version',8,'Versión del espectador','Muestra la versión del protocolo o addon de espectador.','.spect version','.spect version','arena espectador version',0,1)
ON DUPLICATE KEY UPDATE `category_id`=VALUES(`category_id`), `title`=VALUES(`title`), `description`=VALUES(`description`), `syntax`=VALUES(`syntax`), `examples`=VALUES(`examples`), `keywords`=VALUES(`keywords`), `min_security`=VALUES(`min_security`);

-- Operación y diagnóstico de los módulos instalados.
INSERT INTO `server_help_command`
(`command_path`,`category_id`,`title`,`description`,`syntax`,`examples`,`keywords`,`min_security`,`enabled`) VALUES
('bots',10,'Diagnóstico transversal de bots','Familia de diagnóstico del coordinador compartido de población bot. Muestra ocupación, reservas, claims, presupuestos y rechazos recientes.','.bots estado','.bots estado','bots estado poblacion coordinador diagnostico claims presupuesto',2,1),
('bots estado',10,'Estado de la población bot','Muestra bots online y logins pendientes, capacidad global, uso por módulo, facción y tramo de nivel, claims activos y los cinco rechazos más recientes.','.bots estado','.bots estado','bots estado online pendientes claims modulo faccion nivel rechazos limite',2,1),

('wbots',10,'Poblado del mundo (bots)','mod-world-bots: estado del poblado de zonas. Sin argumentos muestra la etapa activa, los contadores desde el arranque y, por cada zona con jugador, su objetivo de bots. ".wbots aqui" fuerza el relleno de la zona en la que estás.','.wbots [estado | aqui | etapa]','.wbots\n.wbots aqui\n.wbots etapa','world bots poblado zona estado etapa contadores relleno aqui',2,1),
('wbots estado',10,'Estado del poblado por zona','Etapa activa, contadores desde el arranque (pasadas, rellenados de zona, bots reubicados y despertados) y, por cada zona con jugador en seguimiento, su objetivo de bots.','.wbots estado','.wbots estado','world bots poblado zona estado contadores objetivo',2,1),
('wbots aqui',10,'Rellenar tu zona ahora','Fuerza una pasada de poblado de la zona en la que estás, sin esperar al ciclo normal. Reelige el objetivo de bots de la zona.','.wbots aqui','.wbots aqui','world bots poblado zona forzar relleno aqui ahora',2,1),
('wbots samaritano',6,'Ayuda de bots en apuros','Modo experimental: si está activado en el servidor (WorldBots.Samaritan), cuando estás en combate con poca vida, uno o dos bots libres cercanos acuden a limpiar lo que te rodea y luego vuelven a lo suyo. No se agrupan contigo ni tocan tu botín. ".wbots samaritano off" lo rechaza hasta el reinicio; "on" lo vuelve a permitir.','.wbots samaritano [on|off]','.wbots samaritano off\n.wbots samaritano on','world bots samaritano ayuda combate apuros rescate opt-out',0,1),
('wbots etapa',10,'Etapa de World Bots','Informa de la etapa de inicialización, movimiento o reposición en la que se encuentra mod-world-bots (tope de nivel y mapas de la progresión más alta conectada).','.wbots etapa','.wbots etapa','world bots etapa estado diagnostico progresion',2,1),

('adaptive',10,'Adaptive AI','Estado general del decisor, aprendizaje, arenas, modelos, perfiles y carga de entrenamiento de los bots.','.adaptive [subcomando]','.adaptive estado','adaptive ai bots aprendizaje entrenamiento',2,1),
('adaptive estado',10,'Estado de Adaptive AI','Muestra decisor, aprendizaje, combates, arenas, modelos, cerebros activos, calibración, escalera y franjas de nivel.','.adaptive estado','.adaptive estado','adaptive estado diagnostico',2,1),
('adaptive on',10,'Activar el decisor adaptativo','Activa Adaptive AI para los bots; las partidas de entrenamiento no se detienen al desactivarlo.','.adaptive on','.adaptive on','adaptive activar decisor',2,1),
('adaptive off',10,'Desactivar el decisor adaptativo','Devuelve los bots al comportamiento estándar de Playerbots; las arenas continúan registrando resultados.','.adaptive off','.adaptive off','adaptive desactivar decisor',2,1),
('adaptive aprender',10,'Controlar el aprendizaje','Activa o desactiva que los nuevos combates actualicen el modelo candidato.','.adaptive aprender <on|off>','.adaptive aprender on','adaptive aprendizaje activar',2,1),
('adaptive arena',10,'Estado de las arenas de entrenamiento','Muestra las partidas de entrenamiento o contraste en curso y las pendientes.','.adaptive arena [estado]','.adaptive arena','adaptive arena entrenamiento estado',2,1),
('adaptive arena lanzar',10,'Lanzar una serie de arenas','Crea una serie entre composiciones de clase o especialización. Los dos equipos deben tener tamaño 1, 2, 3 o 5.','.adaptive arena lanzar <equipoA> <equipoB> [cantidad] [simultaneas] [entrenar|contraste|mixto|referencia]','.adaptive arena lanzar warrior+priest mage+rogue 4 2 contraste','adaptive arena lanzar equipos contraste',2,1),
('adaptive arena parar',10,'Parar series de arena','Borra las series pendientes y detiene las partidas activas; el entrenamiento automático conserva su estado.','.adaptive arena parar','.adaptive arena parar','adaptive arena parar',2,1),
('adaptive arena auto',10,'Entrenamiento automático','Consulta, activa o desactiva el lanzamiento automático de arenas de entrenamiento.','.adaptive arena auto [on|off]','.adaptive arena auto off','adaptive arena automatico',2,1),
('adaptive arena estado',10,'Seguimiento de arenas','Muestra partidas en curso, pendientes, campos de batalla y progreso de calibración.','.adaptive arena estado','.adaptive arena estado','adaptive arena estado',2,1),
('adaptive bg',10,'Estado del entrenamiento en campos','Muestra el mismo resumen de entrenamiento que ".adaptive arena estado".','.adaptive bg [estado]','.adaptive bg','adaptive battleground campo batalla estado',2,1),
('adaptive bg lanzar',10,'Lanzar un campo de entrenamiento','Lanza un campo WS, AB, EY, AV, SA o IC con el número de bots y modo indicados.','.adaptive bg lanzar <WS|AB|EY|AV|SA|IC> [bots por equipo] [entrenar|contraste|mixto|referencia]','.adaptive bg lanzar WS 5 contraste','adaptive bg campo batalla lanzar',2,1),
('adaptive bg estado',10,'Seguimiento de campos','Muestra campos de batalla de entrenamiento en curso junto al resumen de arenas.','.adaptive bg estado','.adaptive bg estado','adaptive bg estado',2,1),
('adaptive calibrar',10,'Calibrar el modelo candidato','Lanza combates de la candidata contra la versión validada, sin aprender, para medir si puede aprobarse.','.adaptive calibrar','.adaptive calibrar','adaptive calibracion modelo',2,1),
('adaptive exportar',15,'Exportar el modelo','Exporta las tablas entrenadas a un fichero del directorio del worldserver.','.adaptive exportar [fichero]','.adaptive exportar adaptive_entrenado.sql','adaptive modelo exportar sql',3,1),
('adaptive importar',15,'Importar un modelo','Importa un modelo Adaptive AI desde un fichero accesible para el worldserver. Revisa el origen antes de usarlo.','.adaptive importar [fichero]','.adaptive importar adaptive_entrenado.sql','adaptive modelo importar sql',3,1),
('adaptive modelo',10,'Modelos disponibles','Lista las versiones de modelo guardadas, su estado y sus resultados.','.adaptive modelo [lista]','.adaptive modelo','adaptive modelo versiones',2,1),
('adaptive modelo lista',10,'Listar modelos','Lista versiones validadas y candidatas con sus combates y tasa de éxito.','.adaptive modelo lista','.adaptive modelo lista','adaptive modelo lista versiones',2,1),
('adaptive modelo usar',15,'Elegir modelo validado','Hace que los bots que se crucen con jugadores usen la versión indicada. La candidata sigue entrenando.','.adaptive modelo usar <version>','.adaptive modelo usar 3','adaptive modelo usar validar version',3,1),
('adaptive bot',10,'Perfil de un bot','Muestra rating, victorias, personalidad, especializaciones y equipo; permite fijar dificultad de 1 a 6.','.adaptive bot <nombre> [dificultad <1-6>]','.adaptive bot Examplebot dificultad 4','adaptive bot perfil dificultad rating',2,1),
('adaptive explicar',10,'Explicar decisiones de un bot','Muestra las decisiones recientes, acciones elegidas, valores Q y recompensa de un bot.','.adaptive explicar <nombre>','.adaptive explicar Examplebot','adaptive bot explicar decisiones',2,1),
('adaptive revertir',10,'Revertir una clase','Revierte el modelo candidato de una clase; "forzar" omite las protecciones normales.','.adaptive revertir <clase> [forzar]','.adaptive revertir mage','adaptive revertir clase modelo',2,1),
('adaptive guardar',10,'Guardar Adaptive AI','Vuelca inmediatamente tablas y perfiles pendientes a acore_playerbots.','.adaptive guardar','.adaptive guardar','adaptive guardar tablas perfiles',2,1),
('adaptive trazas',10,'Trazas para GM','Consulta, activa o desactiva las trazas de decisiones enviadas a personajes con modo GM.','.adaptive trazas [on|off]','.adaptive trazas on','adaptive trazas debug gm',2,1),
('adaptive escalera',10,'Escalera de dificultad','Muestra los peldaños y el reparto del modo de dificultad usado para encuentros con jugadores.','.adaptive escalera','.adaptive escalera','adaptive escalera dificultad rating',2,1),

('dm',10,'Dungeon Master','Herramientas GM del módulo de mazmorras procedurales y Roguelike.','.dm <status|list|clearcooldown|end|reload>','.dm status','dungeon master roguelike gm',2,1),
('dm status',10,'Estado de Dungeon Master','Muestra si el módulo está activo, sesiones, banda de nivel, dificultades, temas y mazmorras.','.dm status','.dm status','dungeon master estado sesiones',2,1),
('dm list',10,'Sesiones de Dungeon Master','Lista las sesiones procedurales o Roguelike activas con sus datos.','.dm list','.dm list','dungeon master sesiones lista',2,1),
('dm clearcooldown',13,'Limpiar reutilización de Dungeon Master','Elimina la reutilización de Dungeon Master para todo el grupo del objetivo seleccionado.','.dm clearcooldown','.dm clearcooldown','dungeon master cooldown grupo',2,1),
('dm end',15,'Terminar una sesión de Dungeon Master','Fuerza el final de una sesión; sin id intenta usar la sesión propia.','.dm end [id]','.dm end 3','dungeon master terminar sesion',3,1),
('dm reload',15,'Recargar Dungeon Master','Recarga en caliente la configuración del módulo.','.dm reload','.dm reload','dungeon master recargar configuracion',3,1),

('ip get',10,'Consultar progresión','Muestra la fase de progresión individual del objetivo, o la propia si no se indica.','.ip get [personaje]','.ip get Lightcore','progresion individual consultar fase',2,1),
('ip set',10,'Cambiar progresión','Fija la fase de progresión del personaje. Usa sólo valores válidos del módulo.','.ip set <personaje> <fase>','.ip set Lightcore 5','progresion individual cambiar fase',2,1),
('ip tele',9,'Teletransportar a una instancia de progresión','Lleva al personaje a una ubicación especial aceptada por el módulo: naxx40, onyxia40, naxx u onyxia.','.ip tele [personaje] <naxx40|onyxia40|naxx|onyxia>','.ip tele Lightcore naxx40','progresion teletransporte naxx onyxia',2,1),
('ip setbot',10,'Ajustar progresión de bots','Sincroniza o ajusta la progresión de los bots relacionados con el jugador que ejecuta el comando. Requiere sesión de juego.','.ip setbot','.ip setbot','progresion bots sincronizar',2,1),
('ip setrep',10,'Ajustar reputaciones por progresión','Ajusta las reputaciones del grupo según la progresión. La versión instalada puede mantener esta operación desactivada.','.ip setrep','.ip setrep','progresion reputacion grupo',2,1),
('ip pvp',10,'Revisar progresión JcJ','Consulta o actualiza el estado JcJ de progresión del objetivo.','.ip pvp [personaje]','.ip pvp Lightcore','progresion pvp rango',2,1),
('ip attune',10,'Conceder attunement','Aplica al grupo el attunement reconocido por el módulo para Onyxia o Templo Oscuro.','.ip attune <onyxia40|onyxia|bt|blacktemple>','.ip attune onyxia','progresion attune acceso raid onyxia black temple',2,1),

('ahbot update',10,'Actualizar la casa de subastas','Ejecuta inmediatamente un ciclo de publicación y compra del bot de subastas. Puede repetirse durante el llenado inicial.','.ahbot update','.ahbot update','ahbot subasta actualizar llenar',2,1),
('ahbot reload',10,'Recargar AHBot','Recarga la configuración y vuelve a preparar candidatos, proporciones y reglas avanzadas.','.ahbot reload','.ahbot reload','ahbot subasta recargar configuracion',2,1),
('ahbot empty',10,'Vaciar subastas del bot','Retira las subastas creadas por AuctionHouseBot y limpia sus objetos caducados. No borra las de jugadores.','.ahbot empty','.ahbot empty','ahbot subasta vaciar borrar',2,1),
('ahbot help',10,'Ayuda de AHBot','Muestra en el chat el resumen de comandos que ofrece el módulo.','.ahbot help','.ahbot help','ahbot ayuda comandos',2,1),

('wareffort',8,'Esfuerzo de Guerra de Ahn Qiraj','Herramientas de consulta del progreso de materiales entregados por ambas facciones.','.wareffort scores','.wareffort scores','ahn qiraj war effort esfuerzo guerra',1,1),
('wareffort scores',8,'Materiales del Esfuerzo de Guerra','Muestra los materiales reunidos por Alianza y Horda y si se han completado los objetivos.','.wareffort scores','.wareffort scores','ahn qiraj materiales puntuacion progreso',1,1),

('playerbots gtask',10,'Tareas de hermandad de Playerbots','Ejecuta órdenes administrativas del gestor de tareas de hermandad de Playerbots.','.playerbots gtask <orden>','.playerbots gtask','playerbots guild task hermandad',2,1),
('playerbots pmon',10,'Monitor de rendimiento de Playerbots','Consulta o reinicia métricas internas de rendimiento de la IA.','.playerbots pmon <tick|stack|reset|...>','.playerbots pmon tick','playerbots rendimiento monitor debug',2,1),
('playerbots rndbot',10,'Administrar bots aleatorios','Ejecuta una orden de consola del gestor de RandomPlayerbots.','.playerbots rndbot <orden>','.playerbots rndbot stats','playerbots random bot administrar',2,1),
('playerbots debug bg',10,'Diagnóstico de campos de batalla','Ejecuta el diagnóstico de Playerbots relacionado con campos de batalla.','.playerbots debug bg ...','.playerbots debug bg','playerbots debug battleground',2,1),

('dc test',13,'Pruebas automáticas de Dungeon Clear','Arnés técnico GM: crea un grupo aleatorio de cinco bots, lo equipa y ejecuta una mazmorra sin meter al GM en el grupo. Cada prueba conserva su semilla.','.dc test <start|status|stop|list|gear|watch|plan> ...','.dc test list','dungeon clear test prueba automatica',2,1),
('dc test start',13,'Iniciar una prueba de mazmorra','Crea y lanza una prueba reproducible. Admite modo heroico, nivel, semilla, nivel de objeto y calidad.','.dc test start <mazmorra> [heroic] [level=N] [seed=N] [ilvl=N|none] [quality=rare|epic|...]','.dc test start ragefire level=20 seed=42','dungeon clear test iniciar semilla equipo',2,1),
('dc test status',13,'Estado de una prueba','Muestra fase, grupo, objetivo y resultado provisional de la prueba activa.','.dc test status [id]','.dc test status','dungeon clear test estado',2,1),
('dc test stop',13,'Detener una prueba','Cancela y limpia una prueba automática activa.','.dc test stop [id]','.dc test stop','dungeon clear test parar',2,1),
('dc test list',13,'Mazmorras de prueba','Lista los identificadores de mazmorra aceptados por ".dc test start".','.dc test list','.dc test list','dungeon clear test lista mazmorras',2,1),
('dc test gear',13,'Equipo de una prueba','Muestra o inspecciona la tirada de equipo de los bots de prueba.','.dc test gear [id]','.dc test gear','dungeon clear test equipo ilvl',2,1),
('dc test watch',13,'Observar una prueba','Conecta la cámara del GM a una prueba automática. Necesita una sesión dentro del juego.','.dc test watch [id]','.dc test watch','dungeon clear test observar camara',2,1),
('dc test plan',13,'Plan de pruebas repetidas','Ejecuta la misma mazmorra varias veces para obtener una tasa de éxito reproducible.','.dc test plan <start|status|stop|edit|pause|resume> ...','.dc test plan status','dungeon clear test plan repeticion',2,1),
('dc test plan start',13,'Iniciar un plan de pruebas','Programa varias ejecuciones de una mazmorra con los parámetros indicados.','.dc test plan start <mazmorra> <repeticiones> [opciones]','.dc test plan start ragefire 10','dungeon clear test plan iniciar',2,1),
('dc test plan status',13,'Estado del plan de pruebas','Muestra ejecuciones completadas, éxitos, fallos y la prueba actual.','.dc test plan status','.dc test plan status','dungeon clear test plan estado',2,1),
('dc test plan stop',13,'Detener el plan de pruebas','Cancela el plan y la ejecución técnica asociada.','.dc test plan stop','.dc test plan stop','dungeon clear test plan parar',2,1),
('dc test plan edit',13,'Reajustar un plan en marcha','Cambia el conjunto de mazmorras o la concurrencia de un plan vivo sin reiniciarlo; las ejecuciones en curso no se abortan.','.dc test plan edit <idPlan> [pool=...] [concurrent=N]','.dc test plan edit 1 concurrent=2','dungeon clear test plan editar reajustar',2,1),
('dc test plan pause',13,'Pausar un plan de pruebas','Deja de lanzar ejecuciones nuevas; las que ya están en marcha terminan. Sin selector, o con "all", afecta a todos los planes.','.dc test plan pause [idPlan|all]','.dc test plan pause','dungeon clear test plan pausar',2,1),
('dc test plan resume',13,'Reanudar un plan de pruebas','Vuelve a lanzar ejecuciones en un plan pausado.','.dc test plan resume [idPlan|all]','.dc test plan resume','dungeon clear test plan reanudar',2,1),
('dc dungeonqueuefill',13,'Diagnóstico del llenado RDF','Herramientas GM del llenado instantáneo de la cola del buscador de mazmorras.','.dc dungeonqueuefill <status|cancel|test> ...','.dc dungeonqueuefill status','dungeon clear rdf queue fill test',2,1),
('dc dungeonqueuefill status',13,'Estado del llenado RDF','Muestra el estado del proceso de llenado de la cola de mazmorras.','.dc dungeonqueuefill status','.dc dungeonqueuefill status','dungeon clear rdf cola estado',2,1),
('dc dungeonqueuefill cancel',13,'Cancelar el llenado RDF','Cancela el proceso técnico de llenado de cola activo.','.dc dungeonqueuefill cancel','.dc dungeonqueuefill cancel','dungeon clear rdf cola cancelar',2,1),
('dc dungeonqueuefill test',13,'Probar el llenado RDF','Lanza la comprobación técnica del llenado automático de la cola.','.dc dungeonqueuefill test ...','.dc dungeonqueuefill test','dungeon clear rdf cola prueba',2,1),
('dc bgqueuefill',13,'Diagnóstico del llenado de campos de batalla','Herramientas GM del llenado instantáneo de campos de batalla de mod-dungeon-clear. Está apagado por defecto (DungeonClear.BgQueueFill.Enable) y en este servidor las colas las rellena mod-queue-bots.','.dc bgqueuefill <status|cancel|test> ...','.dc bgqueuefill status','dungeon clear campo batalla cola bg llenado',2,1),
('dc bgqueuefill status',13,'Estado del llenado de campos de batalla','Dice si está activo, los llenados en marcha con su fase y bots por bando, los jugadores en espera y los bots liberados.','.dc bgqueuefill status','.dc bgqueuefill status','dungeon clear campo batalla cola estado',2,1),
('dc bgqueuefill cancel',13,'Cancelar un llenado de campo de batalla','Libera el llenado de un jugador. Conserva su sitio en la cola real o su partida.','.dc bgqueuefill cancel <jugador>','.dc bgqueuefill cancel Matt','dungeon clear campo batalla cola cancelar',2,1),
('dc bgqueuefill test',13,'Probar el llenado de campos de batalla','Abre un llenado para un jugador que ya espera en la cola de un campo de batalla, sin que vuelva a apuntarse.','.dc bgqueuefill test <jugador>','.dc bgqueuefill test Matt','dungeon clear campo batalla cola prueba',2,1)
ON DUPLICATE KEY UPDATE `category_id`=VALUES(`category_id`), `title`=VALUES(`title`), `description`=VALUES(`description`), `syntax`=VALUES(`syntax`), `examples`=VALUES(`examples`), `keywords`=VALUES(`keywords`), `min_security`=VALUES(`min_security`);

-- Familias operativas de uso frecuente del core. El resto del árbol continúa
-- disponible mediante `.help`/`.commands`; aquí se documentan las operaciones
-- que resultan útiles en la administración cotidiana del reino.
INSERT INTO `server_help_command`
(`command_path`,`category_id`,`title`,`description`,`syntax`,`examples`,`keywords`,`min_security`,`enabled`) VALUES
('gm on',13,'Activar la bandera GM','Activa las facultades de GM del personaje. No cambia por sí sola visibilidad, vuelo ni distintivo de chat.','.gm on','.gm on','gm activar modo',1,1),
('gm off',13,'Desactivar la bandera GM','Desactiva la bandera GM del personaje.','.gm off','.gm off','gm desactivar modo',1,1),
('gm ingame',13,'GM conectados','Lista los personajes GM que están conectados y visibles como tales.','.gm ingame','.gm ingame','gm conectados lista',0,1),
('gm chat',13,'Distintivo GM en el chat','Consulta, activa o desactiva la insignia GM en los mensajes del personaje.','.gm chat [on|off]','.gm chat on','gm chat insignia',2,1),
('gm fly',13,'Vuelo GM','Consulta, activa o desactiva el vuelo libre del personaje GM.','.gm fly [on|off]','.gm fly on','gm volar fly',2,1),
('gm visible',13,'Visibilidad GM','Consulta o cambia si otros jugadores pueden ver al personaje GM. "off" lo hace invisible.','.gm visible [on|off]','.gm visible off','gm invisible visibilidad',2,1),
('gm spectator',13,'Compatibilidad de espectador GM','Permite a un GM seguir a miembros de la facción contraria; puede requerir cambiar de zona.','.gm spectator <on|off>','.gm spectator on','gm espectador faccion',2,1),
('gm list',15,'Cuentas GM','Lista todas las cuentas GM y sus niveles de seguridad.','.gm list','.gm list','gm cuentas lista seguridad',3,1),

('gps',9,'Coordenadas actuales','Muestra mapa, zona, coordenadas y orientación del objetivo o de tu personaje.','.gps [personaje]','.gps','coordenadas posicion mapa zona',1,1),
('appear',9,'Ir hasta un jugador','Teletransporta al GM hasta el personaje indicado o seleccionado.','.appear <personaje>','.appear Lightcore','teletransportar aparecer jugador',1,1),
('summon',9,'Traer a un jugador','Teletransporta al personaje indicado o seleccionado hasta el GM.','.summon <personaje>','.summon Lightcore','teletransportar invocar traer jugador',2,1),
('go xyz',9,'Ir a coordenadas','Teletransporta al GM a unas coordenadas del mapa indicado.','.go xyz <x> <y> [z [mapa [orientacion]]]','.go xyz -8842 626 94.3 0','go coordenadas xyz mapa',1,1),
('go creature id',9,'Ir a una criatura por entrada','Busca una aparición de la entrada indicada y lleva al GM hasta ella; el número final elige coincidencia.','.go creature id <entrada> [n]','.go creature id 500000 1','go criatura entrada npc',1,1),
('go gameobject id',9,'Ir a un objeto del mundo','Busca una aparición del gameobject indicado y lleva al GM hasta ella.','.go gameobject id <entrada> [n]','.go gameobject id 180055 1','go gameobject objeto entrada',1,1),
('go zonexy',9,'Ir a coordenadas de zona','Teletransporta usando porcentajes X/Y dentro de una zona.','.go zonexy <x> <y> [zona]','.go zonexy 50 50','go zona coordenadas porcentaje',1,1),
('go graveyard',9,'Ir a un cementerio','Lleva al GM al cementerio con el identificador indicado.','.go graveyard <id>','.go graveyard 10','go cementerio graveyard',1,1),
('go quest',9,'Ir a una misión','Lleva al GM a una criatura u objeto relacionado con la misión indicada.','.go quest <id>','.go quest 12345','go mision quest',1,1),
('teleport name',9,'Teletransportar a otro jugador','Envía al personaje indicado a una ubicación guardada.','.teleport name <personaje> <ubicacion>','.teleport name Lightcore dalaran','teleport jugador ubicacion',2,1),
('teleport group',9,'Teletransportar a un grupo','Envía al grupo del jugador indicado a una ubicación guardada.','.teleport group <personaje> <ubicacion>','.teleport group Lightcore dalaran','teleport grupo ubicacion',2,1),
('teleport del',9,'Borrar una ubicación','Elimina un destino guardado de la tabla de teletransportes.','.teleport del <nombre>','.teleport del micasa','teleport borrar destino',3,1),

('modify',13,'Modificar un personaje','Familia de cambios directos sobre el jugador seleccionado: recursos, dinero, velocidad, fase, escala, reputación y puntos. Revisa el subcomando antes de aplicarlo.','.modify <campo> <valor> ...','.modify money 10000','modificar personaje recursos dinero velocidad',2,1),
('modify hp',13,'Modificar salud','Cambia la salud actual y máxima del jugador seleccionado.','.modify hp <actual> [maxima]','.modify hp 1000 1000','modificar salud hp',2,1),
('modify mana',13,'Modificar maná','Cambia el maná actual y máximo del jugador seleccionado.','.modify mana <actual> [maximo]','.modify mana 1000 1000','modificar mana',2,1),
('modify money',13,'Modificar dinero','Suma o resta cobre al jugador seleccionado. Diez mil cobres equivalen a una moneda de oro.','.modify money <cobre>','.modify money 10000','modificar dinero oro cobre',2,1),
('modify speed',13,'Modificar velocidad','Familia para cambiar velocidades de carrera, vuelo, nado y marcha del objetivo.','.modify speed <all|walk|swim|backwalk|fly> <factor>','.modify speed all 1.5','modificar velocidad correr volar nadar',2,1),
('modify phase',13,'Modificar fase','Cambia la máscara de fase del personaje seleccionado. Un valor incorrecto puede ocultar contenido.','.modify phase <mascara>','.modify phase 1','modificar fase phase',2,1),
('modify scale',13,'Modificar escala','Cambia temporalmente el tamaño visual del objetivo.','.modify scale <factor>','.modify scale 1.2','modificar escala tamaño',2,1),
('modify talentpoints',13,'Modificar puntos de talento','Fija los puntos de talento libres del personaje seleccionado.','.modify talentpoints <cantidad>','.modify talentpoints 1','modificar talentos puntos',2,1),

('event',8,'Eventos del mundo','Consulta y controla manualmente los eventos definidos en game_event.','.event <activelist|info|start|stop> ...','.event activelist','evento mundo calendario',2,1),
('event activelist',8,'Eventos activos','Lista los eventos del mundo que están activos en este momento.','.event activelist','.event activelist','evento activos lista',2,1),
('event info',8,'Información de un evento','Muestra estado, fechas y datos del evento indicado.','.event info <id>','.event info 1','evento informacion id',2,1),
('event start',8,'Iniciar un evento','Activa manualmente el evento del mundo indicado.','.event start <id>','.event start 1','evento iniciar activar',2,1),
('event stop',8,'Detener un evento','Desactiva manualmente el evento del mundo indicado.','.event stop <id>','.event stop 1','evento parar desactivar',2,1),

('quest',13,'Administrar misiones','Familia de comandos para añadir, completar, retirar, recompensar o consultar misiones del jugador seleccionado.','.quest <add|complete|remove|reward|status> <id>','.quest status 12345','mision quest administrar',2,1),
('quest add',13,'Añadir una misión','Añade al registro del jugador seleccionado la misión indicada, si puede aceptarla.','.quest add <id>','.quest add 12345','mision añadir aceptar',2,1),
('quest complete',13,'Completar objetivos de misión','Marca como completados los objetivos de la misión indicada; no entrega automáticamente la recompensa.','.quest complete <id>','.quest complete 12345','mision completar objetivos',2,1),
('quest remove',13,'Retirar una misión','Elimina la misión indicada del registro del jugador seleccionado.','.quest remove <id>','.quest remove 12345','mision quitar borrar',2,1),
('quest reward',13,'Recompensar una misión','Entrega la recompensa de una misión completada cuando el comando puede resolverla.','.quest reward <id>','.quest reward 12345','mision recompensa entregar',2,1),
('quest status',13,'Estado de una misión','Muestra el estado de la misión indicada para el jugador seleccionado.','.quest status <id>','.quest status 12345','mision estado consultar',2,1),

('kick',14,'Expulsar un jugador','Desconecta al personaje indicado, guardando el motivo en el registro de comandos. También está disponible como formulario en Moderación.','.kick <personaje> [motivo]','.kick Lightcore AFK','moderacion expulsar kick',2,1),
('mute',14,'Silenciar una cuenta','Impide hablar durante la duración indicada al personaje o cuenta resueltos por el core. También está disponible como formulario en Moderación.','.mute <personaje> <duracion> <motivo>','.mute Lightcore 1h spam','moderacion silenciar mute',2,1),
('unmute',14,'Quitar un silencio','Elimina el silencio de la cuenta asociada al personaje. También está disponible como formulario en Moderación.','.unmute <personaje>','.unmute Lightcore','moderacion quitar silencio',2,1),
('mutehistory',14,'Historial de silencios','Muestra el historial de silencios del objetivo indicado.','.mutehistory <personaje|cuenta>','.mutehistory Lightcore','moderacion historial silencio',2,1),
('ban',14,'Aplicar un baneo','Familia para bloquear una cuenta, personaje o IP durante un periodo. Confirma siempre tipo, duración y motivo.','.ban <account|character|ip> <objetivo> <duracion> <motivo>','.ban account CUENTA 7d trampas','moderacion ban baneo',2,1),
('baninfo',14,'Información de un baneo','Consulta los bloqueos asociados a una cuenta, personaje o IP.','.baninfo <account|character|ip> <objetivo>','.baninfo account CUENTA','moderacion ban informacion',2,1),
('banlist',14,'Listar baneos','Lista baneos del tipo indicado que coincidan con el filtro.','.banlist <account|character|ip> [filtro]','.banlist account CUENTA','moderacion ban lista',2,1),
('unban',15,'Retirar un baneo','Familia administrativa para retirar un bloqueo de cuenta, personaje o IP.','.unban <account|character|ip> <objetivo>','.unban account CUENTA','moderacion unban desbanear',3,1),
('pinfo',14,'Información de un jugador','Muestra cuenta, personaje, IP, nivel, tiempo jugado y sanciones del objetivo según tus permisos.','.pinfo [personaje]','.pinfo Lightcore','moderacion jugador informacion cuenta ip',2,1),
('freeze',14,'Congelar un jugador','Inmoviliza al personaje indicado hasta que se use ".unfreeze".','.freeze <personaje>','.freeze Lightcore','moderacion congelar inmovilizar',2,1),
('unfreeze',14,'Descongelar un jugador','Retira la inmovilización aplicada con ".freeze".','.unfreeze <personaje>','.unfreeze Lightcore','moderacion descongelar',2,1),

('send',13,'Enviar correo del servidor','Familia para enviar correo, objetos u oro a un personaje; algunas variantes también admiten mensajes directos.','.send <items|mail|money|message> ...','.send money Lightcore "Premio" "Buen trabajo" 10000','correo enviar objetos oro',2,1),
('send items',13,'Enviar objetos por correo','Envía uno o varios objetos al personaje, con asunto y texto. Usa pares entrada:cantidad.','.send items <personaje> "asunto" "texto" <entrada:cantidad>...','.send items Lightcore "Regalo" "Disfrútalo" 6948:1','correo objetos items enviar',2,1),
('send mail',13,'Enviar un correo','Envía un mensaje de correo del sistema sin adjuntos.','.send mail <personaje> "asunto" "texto"','.send mail Lightcore "Aviso" "Mensaje"','correo mensaje enviar',2,1),
('send money',13,'Enviar oro por correo','Envía cobre adjunto a un correo del sistema. Diez mil cobres equivalen a una moneda de oro.','.send money <personaje> "asunto" "texto" <cobre>','.send money Lightcore "Premio" "Buen trabajo" 10000','correo oro dinero cobre enviar',2,1),
('send message',15,'Mensaje directo del servidor','Envía un mensaje de sistema al personaje indicado.','.send message <personaje> <texto>','.send message Lightcore Reinicio en cinco minutos','mensaje directo jugador servidor',3,1),

('reset',15,'Restablecer un personaje','Familia administrativa para restablecer nivel, estadísticas, hechizos, talentos, equipo u otros datos. Algunas operaciones son irreversibles.','.reset <level|stats|spells|talents|items|...> [personaje]','.reset talents Lightcore','reset restablecer personaje',3,1),
('reset level',15,'Restablecer nivel','Devuelve el nivel del personaje al inicial definido por el servidor y recalcula sus estadísticas.','.reset level [personaje]','.reset level Lightcore','reset nivel personaje',3,1),
('reset stats',15,'Recalcular estadísticas','Restablece y recalcula las estadísticas base del personaje.','.reset stats [personaje]','.reset stats Lightcore','reset estadisticas personaje',3,1),
('reset spells',15,'Restablecer hechizos','Elimina y vuelve a aprender los hechizos que correspondan al personaje.','.reset spells [personaje]','.reset spells Lightcore','reset hechizos personaje',3,1),
('reset talents',15,'Restablecer talentos','Reinicia los talentos del personaje indicado.','.reset talents [personaje]','.reset talents Lightcore','reset talentos personaje',3,1)
ON DUPLICATE KEY UPDATE `category_id`=VALUES(`category_id`), `title`=VALUES(`title`), `description`=VALUES(`description`), `syntax`=VALUES(`syntax`), `examples`=VALUES(`examples`), `keywords`=VALUES(`keywords`), `min_security`=VALUES(`min_security`);

-- Herramientas de profesiones para GM. Los jugadores las ven sólo como guía:
-- el permiso exacto de las fichas impide exponer estos mandatos administrativos.
INSERT INTO `server_help_command`
(`command_path`,`category_id`,`title`,`description`,`syntax`,`examples`,`keywords`,`min_security`,`enabled`) VALUES
('setskill',12,'Ajustar una habilidad','Cambia el valor y, opcionalmente, el máximo de una habilidad del personaje seleccionado. Úsalo para corregir un personaje; no enseña recetas ni sustituye al instructor.','.setskill <habilidad> <valor> [máximo]','.setskill Mining 225 225','profesion habilidad mineria herboristeria setskill ajustar',2,1),
('maxskill',12,'Maximizar habilidades','Lleva al máximo permitido todas las habilidades del personaje seleccionado. Es una herramienta de GM y altera también habilidades que no son profesiones.','.maxskill','.maxskill','profesion habilidad maxskill maximizar gm',2,1)
ON DUPLICATE KEY UPDATE `category_id`=VALUES(`category_id`), `title`=VALUES(`title`), `description`=VALUES(`description`), `syntax`=VALUES(`syntax`), `examples`=VALUES(`examples`), `keywords`=VALUES(`keywords`), `min_security`=VALUES(`min_security`);

-- ───────────────────────────────────────────────────────────────────────────
-- Articulos (semilla: ids 1-99; los tuyos desde 100)
-- ───────────────────────────────────────────────────────────────────────────
INSERT INTO `server_help_article` (`id`,`category_id`,`title`,`body`,`keywords`,`command_path`,`min_security`,`sort`,`enabled`,`is_hot`) VALUES
(1, 1, 'Cómo funciona este servidor',
 'Es un servidor de World of Warcraft 3.3.5a (AzerothCore) pensado para jugar solo o con muy poca gente, con bots que hacen de compañeros:\n\n• Progresión por personaje: empiezas en Vanilla y vas desbloqueando TBC y WotLK (mod-individual-progression).\n• Cientos de bots pueblan el mundo, hacen misiones, mazmorras y campos de batalla. Los de tu zona y los de tu nivel se traen a tu lado (mod-world-bots).\n• Cualquier cola del juego se llena con bots: buscador de mazmorras, bandas, campos de batalla, arenas (mod-queue-bots).\n• Puedes fundar una hermandad propia y llenarla con bots que se conectan contigo (mod-home-guild), y formar grupo donde estés con ".grupo".\n• Cualquier raza puede ser cualquier clase (ARAC), transfiguración, banco de materiales y más servicios en las once capitales.\n\nEsta pestaña de ayuda enseña todos los comandos que tu cuenta puede usar. Escribe una palabra en el buscador (por ejemplo "grupo" o "mazmorra") o elige una categoría.',
 'servidor bienvenida introduccion como funciona bots', '', 0, 0, 1, 1),
(2, 6, 'Jugar en grupo con bots',
 'Tres formas de tener grupo, de menos a más control:\n\n1. Buscador de mazmorras / bandas / campos de batalla: apúntate como siempre. La cola se rellena con bots de tu tramo de nivel y de los roles que falten (mod-queue-bots). En una mazmorra del buscador, si hay tanque bot y tú no eres el tanque, el tanque lleva la mazmorra solo (mod-dungeon-clear, ".dc").\n\n2. ".grupo": donde estés, un grupo de cinco con tanque y sanador de verdad que aparece a tu lado. ".grupo banda 10|25|40" para una banda. ".grupo fuera" para despedirlos. Al aceptar una misión que sugiere varios jugadores, se te unen solos uno o dos y se van al entregarla.\n\n3. Tus propios personajes como bots: ".playerbots bot add <nombre>" mete en tu grupo un personaje de tu cuenta manejado por la IA.\n\nEl equipo de los bots que entran en tu grupo se ajusta a tu media y al tope de tu fase de progresión, para que no vayan mejor vestidos que el contenido.',
 'grupo bots mazmorra banda cola buscador tanque sanador', '', 0, 10, 1, 0),
(3, 4, 'Tu hermandad',
 'El servidor no te crea ni adopta una hermandad automáticamente. Cuando fundas una mediante el sistema normal del juego, queda marcada como tu hermandad de casa mientras sigas siendo su líder (con HomeGuild.AutoAdopt = 0 hay que marcarla a mano con ".hermandad activar"). Una hermandad anterior o donde sólo eres miembro no se modifica.\n\nLa hermandad de casa recibe hasta 15 bots de tu facción y nivel. Se conectan contigo, suben de nivel cuando se quedan atrás y son los primeros a los que llaman ".grupo" y el buscador: el mismo tanque cada semana. Comentan en el chat de hermandad.\n\n".hermandad estado" muestra el roster; ".hermandad desactivar" expulsa a los bots sin disolver la hermandad. Si la disuelves, cedes el liderazgo o dejas de entrar durante muchos días, el módulo deja de administrarla y sus bots vuelven al pool.',
 'hermandad gremio guild bots companeros', 'hermandad', 0, 20, 1, 0),
(4, 3, 'Razas y clases: cualquier combinación',
 'Este servidor permite cualquier raza con cualquier clase (mod-arac): un tauren paladín, un humano chamán, un draenei pícaro... Para que la pantalla de creación de personaje las ofrezca hace falta el parche de cliente "Patch-Arac.MPQ" en la carpeta Data de tu WoW (está en la carpeta cliente/ del instalador).\n\nLas combinaciones nuevas tienen instructor de clase en su zona de inicio, su primer pueblo y su capital: son los "Instructor de <clase>" genéricos, amistosos con las dos facciones. Sólo responden a personajes de su clase.',
 'raza clase arac combinacion instructor', '', 0, 30, 1, 0),
(5, 3, 'Recompensas al subir de nivel',
 'Al alcanzar ciertos niveles recibes cosas que quitan fricción jugando solo (mod-congrats-on-level):\n\n• 10: 1 de oro y cuatro bolsas de 10 huecos (el brujo, además, una bolsa de almas).\n• 20: 5 de oro y Equitación de aprendiz.\n• 40: 25 de oro, cuatro bolsas de 14, Equitación oficial y la doble especialización.\n• 60: 75 de oro, cuatro bolsas de 16 y vuelo normal.\n• 70: 150 de oro y vuelo épico.\n• 77: Vuelo en clima frío (sin él no se vuela en Rasganorte).\n• 80: 50 de oro y cuatro bolsas de 20.\n\nSi no cabe en la bolsa, llega por correo. No es retroactivo: un personaje que ya pasó el nivel no lo recibe.',
 'recompensa nivel bolsas equitacion vuelo oro', '', 0, 40, 1, 0),
(6, 3, 'Progresión Vanilla → TBC → WotLK',
 'Cada personaje avanza por fases (mod-individual-progression): empiezas con el contenido de Vanilla (Núcleo de Magma, Guarida de Alanegra, Ahn Qiraj, Naxxramas de 40) y al completarlo se abre Terrallende, y después Rasganorte. Las estadísticas de los objetos, los talentos y los bots respetan la fase en la que estás.\n\nLas bandas de 40 están ajustadas para poder hacerse con un grupo pequeño y bots. El Esfuerzo de Guerra de Ahn Qiraj lo completa un solo jugador.',
 'progresion fase vanilla tbc wotlk raid', '', 0, 50, 1, 0),
(7, 10, 'Servicios en las capitales',
 'En las once capitales, alrededor del NPC del Dungeon Master, están:\n\n• Warpweaver: transfiguración (también ".transmog claim" desde la bolsa).\n• Ling: banco de materiales de profesión.\n• Swirl: cambio de rasgo racial, por oro.\n• Maestro de arena 1c1: refriega contra un bot (nivel 80).\n\nY el Dungeon Master, para mazmorras procedurales y el modo Roguelike.',
 'npc servicios capital transmog banco materiales racial arena', '', 0, 60, 1, 0),
(8, 1, 'Pedir ayuda y avisos del servidor',
 'Para hablar con el administrador, usa "Hablar con un MJ" o "Informar de problema" en esta misma ventana: abre una consulta (ticket) que el GM ve al conectarse.\n\nSi te quedas atascado, "Personaje atascado" usa primero la piedra de hogar y, si no puede, te empuja fuera del sitio.\n\nEl servidor se reinicia todos los días a las 00:00 (avisa a las 23:55) y los domingos a las 05:00 se reinicia la máquina entera.',
 'ticket consulta gm atascado reinicio horario', '', 0, 70, 1, 0),
(9, 13, 'Guía rápida del GM en este servidor',
 'Lo que más se usa, con la cuenta de administrador:\n\n• ".gm on" para el modo GM.\n• ".go creature id 500000 <n>" te lleva al Dungeon Master de la capital n (1 Cima del Trueno ... 11 Ventormenta).\n• ".ahbot update" varias veces al principio para llenar la subasta.\n• ".wpvp iniciar Southshore" fuerza una escaramuza de bots; ".wpvp estado" y ".wpvp parar".\n• ".actualizaciones" repite el aviso de versiones nuevas.\n• ".reload config" recarga el worldserver.conf; los módulos suelen necesitar reinicio.\n• ".account set gmlevel <cuenta> <0-3> -1" cambia el nivel de una cuenta (reconectar para que se note en esta ayuda).\n\nEl log del servidor está en env/dist/bin/Server.log; cada módulo propio escribe sus líneas con su nombre entre corchetes ([world-bots], [queue-bots], [party-here]...).',
 'gm guia rapida administrador comandos', '', 2, 0, 1, 1),
(10, 8, 'La guerra de mundo',
 'Cada minuto se tira un dado (25 %): si sale, se elige un punto caliente de una zona con jugador (Costasur, Molino Tarren, El Cruce, Astranaar, Refugio Roca del Sol...) y un grupo de bots de una facción marcha sobre él mientras la otra lo defiende. Se anuncia a los de la zona ("La Horda marcha sobre Costasur") y dura entre 5 y 18 minutos. El bando que aguanta más bots vivos junto al objetivo lo va controlando; si mantiene el control lo bastante, captura el punto y la escaramuza termina antes de tiempo con anuncio de ganador. A las puertas de Ventormenta y Orgrimmar, en vez de guerra, hay duelos.\n\nLos puntos calientes de 50-60 (Quebradas Abrasadas, Montaña Roca Negra, Capilla de la Esperanza de la Luz) se activan solos cuando hay bots de ese nivel en ambas facciones. Los bots de la guerra llevan equipo acorde a tu fase de progresión.\n\nSólo participan bots libres (nunca los de tu grupo, tu hermandad o una cola) y nadie aparece a menos de 160 yardas de un jugador.',
 'guerra mundo pvp escaramuza duelo costasur', '', 0, 80, 1, 0),
(11, 12, 'Profesiones y banco de materiales',
 'Puedes aprender dos profesiones primarias, como alquimia, herrería, encantamiento, ingeniería, herboristería, inscripción, joyería, minería, peletería o sastrería. Cocina, pesca y primeros auxilios son secundarias y no ocupan uno de esos dos huecos. Se aprenden y se suben hablando con los instructores y usando sus recetas, igual que en el juego original.\n\nEn las capitales hay un NPC llamado Ling: es el banco de materiales. Guarda reactivos de profesión por separado para no ocupar espacio en tu banco normal. La profesión de cada personaje y sus recetas siguen siendo individuales; el banco sólo almacena los materiales. La sede de hermandad puede comprar a su propio Ling, que comparte ese mismo banco.\n\nLa progresión de contenido también se aplica a recetas, reactivos y objetos fabricados: al desbloquear una fase tendrás acceso a su contenido correspondiente.\n\nCon la experiencia por profesiones activada, las actividades naranjas dan un 1 % de la XP necesaria para subir tu nivel actual, las amarillas un 0,5 %, las verdes un 0,25 % y las grises nada, antes de los modificadores de desafíos. También cuenta la basura de pesca y no es necesario ganar un punto de habilidad. Al alcanzar el máximo del rango aprendido, deja de dar XP hasta entrenar el siguiente rango; a 450/450 tampoco da XP.\n\nIncluye las actividades configurables de todas las profesiones, fundición, desencantamiento y ganzúa. Prospección y molienda no dan XP. Los bots siguen las mismas reglas; esta XP no aporta experiencia de hermandad ni recibe su bonificación. Se respetan XP bloqueada, nivel máximo, límites de progresión y desafíos: Sólo misiones impide esta XP y XP lenta la reduce. No hay reducción adicional por usar materiales iniciales con un personaje de nivel alto.',
 'profesion profesiones receta recetas craftear fabricar recolectar mineria herboristeria desuello pesca cocina primeros auxilios ling banco materiales reactivos', '', 0, 0, 1, 1),
(12, 2, 'Botín de misión en grupo',
 'Al matar una criatura en grupo, cada miembro cercano y elegible puede recoger una copia de los objetos blancos que aparecen como botín de misión. Funciona también en mazmorras y con bots del grupo. Cada persona saquea su propia copia; no aparece siempre el objeto ni se entrega automáticamente.\n\nCon el botín en área activado, al abrir un cadáver recoges también tus copias de los cadáveres cercanos, siempre que quepan enteras en las bolsas. Las copias de los demás permanecen disponibles. Quien se una o llegue después de morir la criatura no obtiene una copia nueva. En solitario, los materiales normales y los objetos de otras calidades rigen por las reglas habituales.\n\nUn bot que traes con ".grupo" también lleva tus misiones (mientras siga en tu grupo) para poder recoger su propia copia igual que un jugador; se avisa por chat cuando esto pasa.',
 'botin misión misiones grupo copia individual area aoeloot saqueo bots companero sincronizar', '', 0, 15, 1, 0),
(13, 12, 'La sede de hermandad como centro de profesiones',
 'La hermandad se compra una sede en la Isla de los MJ (el líder, 1.000 oro, hablando con Talamortis en las capitales). Se entra con la Piedra de la sede (10 segundos, 30 minutos de espera, sin compartir la de tu piedra de hogar), con ".gh teleport" o con el vendedor. Quien tiene rango de compra pide mejoras al mayordomo Xrispins; las hay de oficio:

• Instructores de profesión primaria y secundaria (50 oro cada uno). Enseñan lo mismo que sus homólogos del juego: los rangos altos de cada oficio se entrenan en Shattrath o Dalaran, a los que la sede tiene portal según tu etapa. El de peletería sólo aparece al pasar TBC tier 2.
• Fragua y yunque (50 oro), necesarios para fundir y forjar.
• Ling, el banquero de materiales (100 oro): guarda los materiales de profesión de cada personaje. Es el mismo banco que el de las capitales, es tuyo y no de la hermandad, y sigue ahí si se vende la sede. Un invitado de tu grupo usa su propio banco.

Recorrido típico: abres un cofre de tesoro, depositas el botín en Ling (\"Depositar todos los materiales\"), retiras lo que necesites, funde en la fragua y fabrica. Si un servicio no te sale en el menú del mayordomo, mira que tu rango de hermandad permita comprar mejoras.',
 'sede hermandad guildhouse mayordomo ling banquero materiales profesiones instructores fragua yunque piedra', '', 0, 10, 1, 0)
ON DUPLICATE KEY UPDATE `category_id`=VALUES(`category_id`), `title`=VALUES(`title`), `body`=VALUES(`body`), `keywords`=VALUES(`keywords`), `command_path`=VALUES(`command_path`), `min_security`=VALUES(`min_security`), `is_hot`=VALUES(`is_hot`);

-- ───────────────────────────────────────────────────────────────────────────
-- Traducción al inglés de las fichas y los artículos de semilla. Son datos, no
-- código: el módulo elige `title_en` / `description_en` / `body_en` cuando el
-- cliente no es esES/esMX (si la columna está vacía, cae al español) y el panel
-- web los sirve en inglés cuando está en ese idioma. Sólo UPDATE por clave:
-- idempotente, sin crear filas y sin tocar `enabled` ni lo que haya añadido el
-- administrador (artículos con id >= 100). El nombre de categoría ya era
-- bilingüe (`name_en`). Sintaxis y ejemplos no se traducen: son los comandos.
-- ───────────────────────────────────────────────────────────────────────────
UPDATE `server_help_command` SET `title_en`='Server help', `description_en`='The server knowledge base: it is used by the "Help request" tab of the client (ServerHelp addon), but it can also be queried from chat.\nIt only shows what your account can use. Texts come out in Spanish or English depending on the client language.' WHERE `command_path`='ayuda';
UPDATE `server_help_command` SET `title_en`='Search the help', `description_en`='Searches the text in command names, titles, descriptions, keywords and categories of everything your account can see.' WHERE `command_path`='ayuda buscar';
UPDATE `server_help_command` SET `title_en`='Entry for a command', `description_en`='Shows the entry for a command: usage, description, permission and examples. The path goes without the leading dot.' WHERE `command_path`='ayuda comando';

UPDATE `server_help_command` SET `title_en`='Bot group where you are', `description_en`='Forms a group of five with bots of your faction and level (none above you: it would steal your experience), with a real tank and healer, and brings them to your side in a few seconds. Useful for elite quests and for walking into a dungeon without the finder.\nBots from your guild have priority. As soon as you step into an instance, mod-queue-bots takes over: it enables the bot tank (.dc) if there is one and redeems the tokens after each boss.\nWhile a bot is your mate, it also carries your active quests that it can pick up (dungeon and raid included), so it collects its own copy of group quest loot; you are told in chat and it is removed if you abandon the quest, hand it in or it stops accompanying you.\nWith no arguments it forms the group of five. With a number, that number of bots. If there are no awake bots yet, the request stays pending and completes by itself as soon as they arrive (up to three minutes; ".grupo estado" shows it and ".grupo fuera" cancels it); the tank and healer are waited for a while before their slot is filled with another role. If a mate drops out of a manual group, it is replaced (not if you dismiss them); if you disconnect and return soon, the group is rebuilt.' WHERE `command_path`='grupo';
UPDATE `server_help_command` SET `title_en`='Five-player dungeon group', `description_en`='Same as ".grupo" with no arguments: tank, healer and damage, minus the role you fill.' WHERE `command_path`='grupo mazmorra';
UPDATE `server_help_command` SET `title_en`='Make your mates stay put', `description_en`='The group mates stay where they are instead of following you (playerbots: "-follow,+stay"). With a name, only that one. Undone with ".grupo sigue".' WHERE `command_path`='grupo quieto';
UPDATE `server_help_command` SET `title_en`='Have them follow you again', `description_en`='Undoes ".grupo quieto": the mates follow you again. With a name, only that one.' WHERE `command_path`='grupo sigue';
UPDATE `server_help_command` SET `title_en`='A raid, no queue', `description_en`='Forms a raid of 10, 25 or 40 with bots of your level: 2/3, 3/6 or 5/12 tanks and healers. With no number, according to your current raid difficulty. The bots teleport to your side and their gear is adjusted to your progression phase.' WHERE `command_path`='grupo banda';
UPDATE `server_help_command` SET `title_en`='Dismiss the group bots', `description_en`='With no name, every bot that ".grupo" brought leaves, the group is disbanded and whatever would bring them back is cancelled: the pending request, the replenishment and the rebuild after disconnecting. With a name, only that mate is dismissed and the group becomes a smaller one: that bot is neither replaced nor returns. If it is in combat or travelling, it leaves as soon as it finishes (".grupo estado" shows it). Those you invited by hand are not touched. To swap one for another, ".grupo cambia".' WHERE `command_path`='grupo fuera';
UPDATE `server_help_command` SET `title_en`='Swap one mate for another', `description_en`='That mate leaves and another bot is sought for its slot, without lowering the group size; the one who left does not return for a while. If it is in combat or travelling, the swap happens as soon as it finishes.' WHERE `command_path`='grupo cambia';
UPDATE `server_help_command` SET `title_en`='Who is in the group and why', `description_en`='Lists the group bots with their role and the reason they came (command, group quest), those who leave when combat ends and the pending mates request, if there is one.' WHERE `command_path`='grupo estado';

UPDATE `server_help_command` SET `title_en`='Your home guild', `description_en`='mod-home-guild: if you found a guild through the normal game flow, it becomes your "home guild" and is filled with up to 15 bots of your faction and level that log in with you, level up with you and go first in queues. The roster looks for plenty of tanks and healers, and its gear is kept at your phase. The module never creates or adopts guilds on its own.\nWith no arguments it shows the status (roster, roles and who is online).' WHERE `command_path`='hermandad';
UPDATE `server_help_command` SET `title_en`='Recruit and level now', `description_en`='Forces a guild care pass: recruit up to the target and level up the stragglers, without waiting for the normal cycle.' WHERE `command_path`='hermandad renovar';
UPDATE `server_help_command` SET `title_en`='Pin a mate', `description_en`='Marks a roster bot as pinned: it logs in first, is shown first and is not kicked out. Undone with ".hermandad soltar".' WHERE `command_path`='hermandad fijar';
UPDATE `server_help_command` SET `title_en`='Remove a mate''s mark', `description_en`='Undoes ".hermandad fijar" or ".hermandad excluir" on that bot.' WHERE `command_path`='hermandad soltar';
UPDATE `server_help_command` SET `title_en`='Exclude a mate', `description_en`='Removes a bot from the guild and prevents it from being recruited again. Undone with ".hermandad soltar".' WHERE `command_path`='hermandad excluir';
UPDATE `server_help_command` SET `title_en`='Home guild roster', `description_en`='Lists the bots of your home guild with their level and whether they are online right now.' WHERE `command_path`='hermandad estado';
UPDATE `server_help_command` SET `title_en`='Turn your guild into a home guild', `description_en`='Marks your current guild as a home guild and starts populating it with bots. You must be its founder and leader. Useful when HomeGuild.AutoAdopt is 0 (founding a guild does not adopt it automatically).' WHERE `command_path`='hermandad activar';
UPDATE `server_help_command` SET `title_en`='Empty the home guild of bots', `description_en`='Kicks every bot from the guild and stops populating it. The guild keeps existing with its human members; it can be enabled again with ".hermandad activar".' WHERE `command_path`='hermandad desactivar';

UPDATE `server_help_command` SET `title_en`='The bot tank runs the dungeon', `description_en`='mod-dungeon-clear: a bot tank walks the dungeon from end to end (route between bosses, pulls, loot, rest, resurrection). You cannot be the tank: sign up as damage or healer.\nWith the finder group inside the dungeon, mod-queue-bots enables it on its own; these commands are for running it by hand.' WHERE `command_path`='dc';
UPDATE `server_help_command` SET `title_en`='Enable the bot tank', `description_en`='Starts the automatic run. A bot tank in the group and being inside the instance are required. In Blackrock (Spire and Depths) the wing can be given: lbrs, ubrs, brd-db or brd-uc.' WHERE `command_path`='dc on';
UPDATE `server_help_command` SET `title_en`='Stop the bot tank', `description_en`='Stops the run: the bots go back to following you.' WHERE `command_path`='dc off';
UPDATE `server_help_command` SET `title_en`='Pause or resume', `description_en`='Pauses the advance (and resumes it if already paused). Useful before a boss.' WHERE `command_path`='dc pause';
UPDATE `server_help_command` SET `title_en`='Skip the current step', `description_en`='If the tank gets stuck on a scripted event, skips to the next objective.' WHERE `command_path`='dc skip';
UPDATE `server_help_command` SET `title_en`='Run status', `description_en`='Says whether it is active, which step it is on and who it is waiting for.' WHERE `command_path`='dc status';
UPDATE `server_help_command` SET `title_en`='Dungeon bosses', `description_en`='Lists the instance bosses and which ones are left.' WHERE `command_path`='dc bosses';

UPDATE `server_help_command` SET `title_en`='Bot token hand-in', `description_en`='mod-token-turnin: converts the tier tokens that your group''s bots earn into the piece for their specialisation. mod-queue-bots launches it by itself 90 seconds after a boss is killed; these commands are for doing it by hand.' WHERE `command_path`='tokenturnin';
UPDATE `server_help_command` SET `title_en`='See which tokens the bots have', `description_en`='Lists the redeemable tokens the group bots carry, without redeeming anything.' WHERE `command_path`='tokenturnin check';
UPDATE `server_help_command` SET `title_en`='Redeem the tokens', `description_en`='Redeems the group bots'' tokens for their tier piece.' WHERE `command_path`='tokenturnin redeem';

UPDATE `server_help_command` SET `title_en`='Transmogrification', `description_en`='mod-transmog. With no arguments it toggles seeing transmogrifications. The transmogrification NPC (Warpweaver) is in the eleven capitals next to the Dungeon Master.' WHERE `command_path`='transmog';
UPDATE `server_help_command` SET `title_en`='Add appearances to your collection', `description_en`='Adds the appearance of the item in your bag (or of all of them with "all") to the transmogrification collection, without having to go to the NPC.' WHERE `command_path`='transmog claim';
UPDATE `server_help_command` SET `title_en`='Area loot', `description_en`='When you open a corpse, the loot of nearby corpses is also collected. If you are in a group, the white quest items of each eligible member are collected into their own bag without taking away the other copies. Here you turn area looting on or off for your character.' WHERE `command_path`='aoeloot';

UPDATE `server_help_command` SET `title_en`='World war (bots)', `description_en`='mod-world-bots: faction skirmishes and duels at 23 hotspots (Southshore, Tarren Mill, the Crossroads, Astranaar...), only with free bots and only in zones with a player. By default it triggers by itself about every ~4 minutes on average; these commands govern it.' WHERE `command_path`='wpvp';
UPDATE `server_help_command` SET `title_en`='Active war events', `description_en`='Which skirmishes and duels are in progress, with their id, zone and remaining time.' WHERE `command_path`='wpvp estado';
UPDATE `server_help_command` SET `title_en`='Hotspots', `description_en`='Lists the hotspots of the world_bots_pvp_hotspot table with their internal name (the one "iniciar" uses), level and state: on / off, or "auto" / "auto-espera" for the 50-60 ones that switch on by themselves when there is a population of that level.' WHERE `command_path`='wpvp lista';
UPDATE `server_help_command` SET `title_en`='Force a skirmish', `description_en`='Starts the event of the given hotspot right now (internal name from ".wpvp lista"), even if it is not its turn by chance.' WHERE `command_path`='wpvp iniciar';
UPDATE `server_help_command` SET `title_en`='End an event', `description_en`='Ends the given event (or all of them if no id is given): the bots recover their strategies and go back to where they were.' WHERE `command_path`='wpvp parar';
UPDATE `server_help_command` SET `title_en`='Reload the hotspots', `description_en`='Reads the world_bots_pvp_hotspot table again without restarting.' WHERE `command_path`='wpvp recargar';
UPDATE `server_help_command` SET `title_en`='New upstream versions', `description_en`='mod-update-notice: shows again the notice of repositories (core and modules) with new commits over versions.lock, sorted by severity ([security], [incompatibility], [functional], [informative]). It is written by tools/revisar-actualizaciones.sh; the server is not touched by itself. It warns if the report predates an ./install.sh --freeze.' WHERE `command_path`='actualizaciones';

UPDATE `server_help_command` SET `title_en`='Individual progression', `description_en`='mod-individual-progression: queries and manages the Vanilla → TBC → WotLK phase, teleports, reputations and attunements of characters and bots.\nNote: ".ip set" from the server console is not safe; use it from in game.' WHERE `command_path`='ip';
UPDATE `server_help_command` SET `title_en`='Auction house bot', `description_en`='mod-ah-bot-plus: the auction house takes hours to fill by itself; "update" repopulates it instantly (repeat several times at the start), "reload" reloads the .conf and "empty" empties it (only the bot''s items).' WHERE `command_path`='ahbot';

UPDATE `server_help_command` SET `title_en`='Teleport by name', `description_en`='Takes you to a location from the server teleport list (game_tele table). With no argument it does nothing: the name must be given.' WHERE `command_path`='teleport';
UPDATE `server_help_command` SET `title_en`='Save your position as a destination', `description_en`='Adds the place where you are to the ".teleport" list under that name.' WHERE `command_path`='teleport add';
UPDATE `server_help_command` SET `title_en`='Go to a place, creature or object', `description_en`='GM teleport family: to coordinates, to a creature by entry or guid, to an object, to a graveyard, to a zone...' WHERE `command_path`='go';
UPDATE `server_help_command` SET `title_en`='Game Master mode', `description_en`='Turns the GM flag on or off. Visibility, flight, the chat badge and spectator mode are controlled with separate subcommands.' WHERE `command_path`='gm';
UPDATE `server_help_command` SET `title_en`='Give an item', `description_en`='Creates N units of the item in your bag (or in the selected player''s). ".lookup item <name>" gives you the id.' WHERE `command_path`='additem';
UPDATE `server_help_command` SET `title_en`='Raise levels', `description_en`='Raises (or lowers, with a negative number) N levels to the selected player or to you.' WHERE `command_path`='levelup';
UPDATE `server_help_command` SET `title_en`='Learn a spell', `description_en`='Teaches the spell to the selected player or to you. "all" and its variants learn whole families.' WHERE `command_path`='learn';
UPDATE `server_help_command` SET `title_en`='Revive', `description_en`='Revives the selected player (or you) where they are.' WHERE `command_path`='revive';
UPDATE `server_help_command` SET `title_en`='Create an NPC here', `description_en`='The creature with that entry appears where you are, permanently (saved in the DB).' WHERE `command_path`='npc add';
UPDATE `server_help_command` SET `title_en`='Delete the selected NPC', `description_en`='Removes the selected creature from the DB.' WHERE `command_path`='npc delete';
UPDATE `server_help_command` SET `title_en`='Find an item by name', `description_en`='Lists the items whose name contains the text, with their id.' WHERE `command_path`='lookup item';
UPDATE `server_help_command` SET `title_en`='Find a creature by name', `description_en`='Lists the creatures whose name contains the text, with their entry.' WHERE `command_path`='lookup creature';
UPDATE `server_help_command` SET `title_en`='Release saved instances', `description_en`='Removes the instance bindings of the selected player ("all" or one map).' WHERE `command_path`='instance unbind';
UPDATE `server_help_command` SET `title_en`='Reload worldserver.conf', `description_en`='Reads worldserver.conf and the .conf files of the modules that support it again, without restarting. Many module options are only read at startup.' WHERE `command_path`='reload config';
UPDATE `server_help_command` SET `title_en`='Announcement to the whole server', `description_en`='Sends the text to all players as a system message.' WHERE `command_path`='announce';
UPDATE `server_help_command` SET `title_en`='Server information', `description_en`='Core version, connected players, uptime and load.' WHERE `command_path`='server info';
UPDATE `server_help_command` SET `title_en`='Shut down the server', `description_en`='Shuts the worldserver down in N seconds, warning the players. With systemd, the service does not start again by itself: use "sudo systemctl restart ac-worldserver" from the VM to restart with a warning.' WHERE `command_path`='server shutdown';
UPDATE `server_help_command` SET `title_en`='Restart the server', `description_en`='Restarts the worldserver in N seconds, warning the players (with the systemd service, which brings it up again).' WHERE `command_path`='server restart';
UPDATE `server_help_command` SET `title_en`='Your account', `description_en`='Shows your account''s access level (and the email if you have permission). With subcommands, it manages the account.' WHERE `command_path`='account';
UPDATE `server_help_command` SET `title_en`='Change your password', `description_en`='Changes your account password. The new one must be repeated twice.' WHERE `command_path`='account password';
UPDATE `server_help_command` SET `title_en`='Core help', `description_en`='The classic core help, in chat: with no argument it lists the available commands; with one, its help.' WHERE `command_path`='help';
UPDATE `server_help_command` SET `title_en`='Command list', `description_en`='Lists the top-level commands your account can use.' WHERE `command_path`='commands';
UPDATE `server_help_command` SET `title_en`='Save the character', `description_en`='Forces your character to be saved to the database right now.' WHERE `command_path`='save';
UPDATE `server_help_command` SET `title_en`='Dismount', `description_en`='Gets you off your mount.' WHERE `command_path`='dismount';
UPDATE `server_help_command` SET `title_en`='Gear statistics', `description_en`='Shows the gear score of the selected character or of yours.' WHERE `command_path`='gear';
UPDATE `server_help_command` SET `title_en`='Player enquiries', `description_en`='Management of the tickets players open from "Help request": list, assign, reply, close.' WHERE `command_path`='ticket';

UPDATE `server_help_command` SET `title_en`='Catalogue version', `description_en`='Shows the index fingerprint, the number of entries and the level it was built with. Useful to check that the addon and the server are in sync.' WHERE `command_path`='ayuda version';
UPDATE `server_help_command` SET `title_en`='Full help index', `description_en`='Lists every entry the core lets your session see, sorted by category.' WHERE `command_path`='ayuda indice';
UPDATE `server_help_command` SET `title_en`='Open an article', `description_en`='Opens an article of the knowledge base by its identifier. The search and the index show that number.' WHERE `command_path`='ayuda articulo';
UPDATE `server_help_command` SET `title_en`='Reload the knowledge base', `description_en`='Reads categories, articles, entries and rules again from acore_world without restarting the server.' WHERE `command_path`='ayuda recargar';
UPDATE `server_help_command` SET `title_en`='Entry coverage', `description_en`='How many commands of the visible tree have their own entry, how many fall only by category rule and how many by level; lists up to 40 without an entry. Useful to know what to document.' WHERE `command_path`='ayuda cobertura';
UPDATE `server_help_command` SET `title_en`='Export the tree to the panel', `description_en`='Dumps the discovered commands that have no entry of their own as auto=1 rows in server_help_command, so the web panel sees the whole tree. It does not touch curated entries. Run it after adding modules and then ".ayuda recargar".' WHERE `command_path`='ayuda export';

UPDATE `server_help_command` SET `title_en`='Change the pull mode', `description_en`='Toggles how the bot tank gathers enemies during the run. It applies to the current run.' WHERE `command_path`='dc pull';
UPDATE `server_help_command` SET `title_en`='Go straight to a boss', `description_en`='Changes the bot tank''s route to head for the boss given by name. Check ".dc bosses" first.' WHERE `command_path`='dc go';
UPDATE `server_help_command` SET `title_en`='Effective run configuration', `description_en`='Shows the effective DungeonClear values, including the addon overrides and the heroic values.' WHERE `command_path`='dc config';
UPDATE `server_help_command` SET `title_en`='Spectator camera', `description_en`='Enables the free camera. With "follow" it makes the camera follow a bot; it does not require belonging to the group.' WHERE `command_path`='dc spectate';
UPDATE `server_help_command` SET `title_en`='Dungeon wing', `description_en`='In dungeons split into wings (Lower Blackrock Spire / Upper Blackrock Spire: lbrs / ubrs; Depths: brd-db / brd-uc) shows the wing the bot tank is running or changes it. It neither starts nor stops the run.' WHERE `command_path`='dc wing';

UPDATE `server_help_command` SET `title_en`='Bots for your queue', `description_en`='mod-queue-bots: when you join a queue (dungeon finder, raid finder, battleground, arena, 1v1) it fills by itself with bots of your level range and the roles that are missing. In battlegrounds and arenas, if a bot drops out mid-match it is replaced. These commands are for seeing how it is going and forcing it; with no arguments they show the status.' WHERE `command_path`='queuebots';
UPDATE `server_help_command` SET `title_en`='Status of your queue filling', `description_en`='How many bots are signed up for your current queue, with which roles (tank, healer, damage) and how many are still to join.' WHERE `command_path`='queuebots estado';
UPDATE `server_help_command` SET `title_en`='Force another filling pass', `description_en`='Asks for your queue to be checked on the next pass without waiting for the normal interval, in case bots are missing.' WHERE `command_path`='queuebots traer';
UPDATE `server_help_command` SET `title_en`='Remove the bots from your queue', `description_en`='Removes from your current queue all the bots the module added; you stay in the queue. Useful if you prefer to wait for real people.' WHERE `command_path`='queuebots salir';

UPDATE `server_help_command` SET `title_en`='1v1 arena queue', `description_en`='Command family of the 1v1 arena module. The character must meet the requirements configured by the module.' WHERE `command_path`='q1v1';
UPDATE `server_help_command` SET `title_en`='Join rated 1v1', `description_en`='Joins or leaves the rated 1v1 arena queue.' WHERE `command_path`='q1v1 rated';
UPDATE `server_help_command` SET `title_en`='Join unrated 1v1', `description_en`='Joins or leaves the 1v1 skirmish queue, without affecting the rating.' WHERE `command_path`='q1v1 unrated';
UPDATE `server_help_command` SET `title_en`='1v1 statistics', `description_en`='Shows the character''s statistics and rating in the 1v1 mode.' WHERE `command_path`='q1v1 stats';

UPDATE `server_help_command` SET `title_en`='Instanced world bosses', `description_en`='Command family of mod-instanced-worldbosses. Lockouts are per character.' WHERE `command_path`='worldboss';
UPDATE `server_help_command` SET `title_en`='World boss lockouts', `description_en`='Lists the world bosses the character has defeated and when each lockout expires; clears those that have already expired.' WHERE `command_path`='worldboss locks';

UPDATE `server_help_command` SET `title_en`='Manage Playerbots', `description_en`='Family of direct commands of the Playerbots module. For everyday orders to the bots the MultiBot addon is more convenient.' WHERE `command_path`='playerbots';
UPDATE `server_help_command` SET `title_en`='Direct order to Playerbots', `description_en`='Sends an order to the bot manager. It accepts the orders understood by the installed version of mod-playerbots.' WHERE `command_path`='playerbots bot';
UPDATE `server_help_command` SET `title_en`='Set the linking key', `description_en`='Defines the key that allows linking another account to use its characters as bots. Do not reuse the account password.' WHERE `command_path`='playerbots account setkey';
UPDATE `server_help_command` SET `title_en`='Link a bot account', `description_en`='Links another account using the security key configured on it.' WHERE `command_path`='playerbots account link';
UPDATE `server_help_command` SET `title_en`='See linked accounts', `description_en`='Lists the accounts whose characters you can bring in as bots.' WHERE `command_path`='playerbots account linkedaccounts';
UPDATE `server_help_command` SET `title_en`='Unlink an account', `description_en`='Removes a Playerbots account link.' WHERE `command_path`='playerbots account unlink';

UPDATE `server_help_command` SET `title_en`='Automatic instance scaling', `description_en`='Shows how mod-autobalance is scaling the instance. ".ab" is a full alias of ".autobalance".' WHERE `command_path`='autobalance';
UPDATE `server_help_command` SET `title_en`='See the difficulty offset', `description_en`='Shows the global offset applied to the effective player count. It also works as ".ab getoffset".' WHERE `command_path`='autobalance getoffset';
UPDATE `server_help_command` SET `title_en`='Instance scaling statistics', `description_en`='Inside an instance, shows effective players, level, and scaled health and damage. It also works as ".ab mapstat".' WHERE `command_path`='autobalance mapstat';
UPDATE `server_help_command` SET `title_en`='Scaling of the selected creature', `description_en`='Shows the original and scaled values of the selected creature inside an instance. It also works as ".ab creaturestat".' WHERE `command_path`='autobalance creaturestat';
UPDATE `server_help_command` SET `title_en`='Change the difficulty offset', `description_en`='Temporarily changes the global offset of the effective player count. It affects instance balance. It also works as ".ab setoffset".' WHERE `command_path`='autobalance setoffset';

UPDATE `server_help_command` SET `title_en`='Enable area loot', `description_en`='Enables looting nearby corpses for the character.' WHERE `command_path`='aoeloot on';
UPDATE `server_help_command` SET `title_en`='Disable area loot', `description_en`='Disables looting nearby corpses for the character.' WHERE `command_path`='aoeloot off';

UPDATE `server_help_command` SET `title_en`='Sync the collection', `description_en`='Forces the appearance collection to be sent to the transmogrification addon.' WHERE `command_path`='transmog sync';
UPDATE `server_help_command` SET `title_en`='Portable transmogrification', `description_en`='Opens or configures the portable transmogrification interface if it is enabled on the server.' WHERE `command_path`='transmog portable';
UPDATE `server_help_command` SET `title_en`='Interface preference', `description_en`='Turns the transmogrification system''s addon interface on or off.' WHERE `command_path`='transmog interface';
UPDATE `server_help_command` SET `title_en`='Transmogrification notice', `description_en`='Shows or changes the preference for the transmogrification informational notice.' WHERE `command_path`='transmog disclaimer';
UPDATE `server_help_command` SET `title_en`='Add an appearance', `description_en`='Adds the appearance of the given item to a collection; it acts on the selected player when applicable.' WHERE `command_path`='transmog add';
UPDATE `server_help_command` SET `title_en`='Add a set of appearances', `description_en`='Adds a whole set of items to the collection.' WHERE `command_path`='transmog add set';
UPDATE `server_help_command` SET `title_en`='Check a transmogrification', `description_en`='Inspects the transmogrification information of the target or of the given item.' WHERE `command_path`='transmog check';
UPDATE `server_help_command` SET `title_en`='Reload Transmog', `description_en`='Reloads the mod-transmog configuration without restarting the server.' WHERE `command_path`='transmog reload';

UPDATE `server_help_command` SET `title_en`='Account two-factor', `description_en`='Sets up or removes two-factor authentication on your own account. The server must have the same master secret configured in authserver and worldserver.' WHERE `command_path`='account 2fa';
UPDATE `server_help_command` SET `title_en`='Enable two-factor', `description_en`='Starts the TOTP setup of the account and shows the data needed for the authenticator.' WHERE `command_path`='account 2fa setup';
UPDATE `server_help_command` SET `title_en`='Remove two-factor', `description_en`='Disables TOTP after checking a valid code.' WHERE `command_path`='account 2fa remove';
UPDATE `server_help_command` SET `title_en`='Lock the account to this IP', `description_en`='Turns on or off whether the account can only connect from the current IP address.' WHERE `command_path`='account lock ip';
UPDATE `server_help_command` SET `title_en`='Lock the account to the country', `description_en`='Turns on or off the access restriction to the country detected for the account. It requires the server''s GeoIP database.' WHERE `command_path`='account lock country';
UPDATE `server_help_command` SET `title_en`='Message of the day', `description_en`='Shows the message of the day configured by the server.' WHERE `command_path`='server motd';
UPDATE `server_help_command` SET `title_en`='Arena spectator', `description_en`='Command family for watching arenas without taking part.' WHERE `command_path`='spect';
UPDATE `server_help_command` SET `title_en`='Watch the target', `description_en`='Starts watching the selected player inside an arena.' WHERE `command_path`='spect spectate';
UPDATE `server_help_command` SET `title_en`='Watch by name', `description_en`='Starts watching the given character in an arena.' WHERE `command_path`='spect watch';
UPDATE `server_help_command` SET `title_en`='Leave spectator mode', `description_en`='Leaves the watched arena and restores the character''s state.' WHERE `command_path`='spect leave';
UPDATE `server_help_command` SET `title_en`='Reset the camera', `description_en`='Resets the tracking and the options of the spectator camera.' WHERE `command_path`='spect reset';
UPDATE `server_help_command` SET `title_en`='Spectator version', `description_en`='Shows the version of the spectator protocol or addon.' WHERE `command_path`='spect version';

UPDATE `server_help_command` SET `title_en`='Cross-cutting bot diagnostics', `description_en`='Diagnostic family of the shared bot population coordinator. It shows occupancy, reservations, claims, budgets and recent rejections.' WHERE `command_path`='bots';
UPDATE `server_help_command` SET `title_en`='Bot population status', `description_en`='Shows online bots and pending logins, global capacity, usage per module, faction and level range, active claims and the five most recent rejections.' WHERE `command_path`='bots estado';

UPDATE `server_help_command` SET `title_en`='World population (bots)', `description_en`='mod-world-bots: zone population status. With no arguments it shows the active stage, the counters since startup and, for each zone with a player, its bot target. ".wbots aqui" forces the fill of the zone you are in.' WHERE `command_path`='wbots';
UPDATE `server_help_command` SET `title_en`='Population status by zone', `description_en`='Active stage, counters since startup (passes, zone fills, bots relocated and woken) and, for each tracked zone with a player, its bot target.' WHERE `command_path`='wbots estado';
UPDATE `server_help_command` SET `title_en`='Fill your zone now', `description_en`='Forces a population pass of the zone you are in, without waiting for the normal cycle. It re-picks the zone''s bot target.' WHERE `command_path`='wbots aqui';
UPDATE `server_help_command` SET `title_en`='Bots helping you in a pinch', `description_en`='Experimental mode: if enabled on the server (WorldBots.Samaritan), when you are in combat with low health, one or two nearby free bots come to clear what surrounds you and then go back to their business. They do not group with you or touch your loot. ".wbots samaritano off" turns it down until restart; "on" allows it again.' WHERE `command_path`='wbots samaritano';
UPDATE `server_help_command` SET `title_en`='World Bots stage', `description_en`='Reports the initialisation, movement or replenishment stage mod-world-bots is in (level cap and maps of the highest connected progression).' WHERE `command_path`='wbots etapa';

UPDATE `server_help_command` SET `title_en`='Adaptive AI', `description_en`='General status of the decision maker, learning, arenas, models, profiles and the training load of the bots.' WHERE `command_path`='adaptive';
UPDATE `server_help_command` SET `title_en`='Adaptive AI status', `description_en`='Shows the decision maker, learning, fights, arenas, models, active brains, calibration, ladder and level bands.' WHERE `command_path`='adaptive estado';
UPDATE `server_help_command` SET `title_en`='Enable the adaptive decision maker', `description_en`='Enables Adaptive AI for the bots; training matches do not stop when it is disabled.' WHERE `command_path`='adaptive on';
UPDATE `server_help_command` SET `title_en`='Disable the adaptive decision maker', `description_en`='Returns the bots to the standard Playerbots behaviour; the arenas keep recording results.' WHERE `command_path`='adaptive off';
UPDATE `server_help_command` SET `title_en`='Control learning', `description_en`='Turns on or off whether new fights update the candidate model.' WHERE `command_path`='adaptive aprender';
UPDATE `server_help_command` SET `title_en`='Training arena status', `description_en`='Shows the training or contrast matches in progress and the pending ones.' WHERE `command_path`='adaptive arena';
UPDATE `server_help_command` SET `title_en`='Launch an arena series', `description_en`='Creates a series between class or specialisation compositions. Both teams must have size 1, 2, 3 or 5.' WHERE `command_path`='adaptive arena lanzar';
UPDATE `server_help_command` SET `title_en`='Stop arena series', `description_en`='Deletes the pending series and stops the active matches; automatic training keeps its state.' WHERE `command_path`='adaptive arena parar';
UPDATE `server_help_command` SET `title_en`='Automatic training', `description_en`='Queries, enables or disables the automatic launching of training arenas.' WHERE `command_path`='adaptive arena auto';
UPDATE `server_help_command` SET `title_en`='Arena tracking', `description_en`='Shows matches in progress, pending ones, battlegrounds and calibration progress.' WHERE `command_path`='adaptive arena estado';
UPDATE `server_help_command` SET `title_en`='Battleground training status', `description_en`='Shows the same training summary as ".adaptive arena estado".' WHERE `command_path`='adaptive bg';
UPDATE `server_help_command` SET `title_en`='Launch a training battleground', `description_en`='Launches a WS, AB, EY, AV, SA or IC battleground with the given number of bots and mode.' WHERE `command_path`='adaptive bg lanzar';
UPDATE `server_help_command` SET `title_en`='Battleground tracking', `description_en`='Shows training battlegrounds in progress together with the arena summary.' WHERE `command_path`='adaptive bg estado';
UPDATE `server_help_command` SET `title_en`='Calibrate the candidate model', `description_en`='Launches fights of the candidate against the validated version, without learning, to measure whether it can be approved.' WHERE `command_path`='adaptive calibrar';
UPDATE `server_help_command` SET `title_en`='Export the model', `description_en`='Exports the trained tables to a file in the worldserver directory.' WHERE `command_path`='adaptive exportar';
UPDATE `server_help_command` SET `title_en`='Import a model', `description_en`='Imports an Adaptive AI model from a file accessible to the worldserver. Check the source before using it.' WHERE `command_path`='adaptive importar';
UPDATE `server_help_command` SET `title_en`='Available models', `description_en`='Lists the saved model versions, their state and their results.' WHERE `command_path`='adaptive modelo';
UPDATE `server_help_command` SET `title_en`='List models', `description_en`='Lists validated and candidate versions with their fights and success rate.' WHERE `command_path`='adaptive modelo lista';
UPDATE `server_help_command` SET `title_en`='Choose a validated model', `description_en`='Makes the bots that meet players use the given version. The candidate keeps training.' WHERE `command_path`='adaptive modelo usar';
UPDATE `server_help_command` SET `title_en`='A bot''s profile', `description_en`='Shows rating, wins, personality, specialisations and gear; it lets you set a difficulty from 1 to 6.' WHERE `command_path`='adaptive bot';
UPDATE `server_help_command` SET `title_en`='Explain a bot''s decisions', `description_en`='Shows the recent decisions, chosen actions, Q values and reward of a bot.' WHERE `command_path`='adaptive explicar';
UPDATE `server_help_command` SET `title_en`='Revert a class', `description_en`='Reverts the candidate model of a class; "forzar" skips the normal safeguards.' WHERE `command_path`='adaptive revertir';
UPDATE `server_help_command` SET `title_en`='Save Adaptive AI', `description_en`='Immediately dumps pending tables and profiles to acore_playerbots.' WHERE `command_path`='adaptive guardar';
UPDATE `server_help_command` SET `title_en`='Traces for GMs', `description_en`='Queries, enables or disables the decision traces sent to characters in GM mode.' WHERE `command_path`='adaptive trazas';
UPDATE `server_help_command` SET `title_en`='Difficulty ladder', `description_en`='Shows the rungs and the split of the difficulty mode used for encounters with players.' WHERE `command_path`='adaptive escalera';

UPDATE `server_help_command` SET `title_en`='Dungeon Master', `description_en`='GM tools of the procedural dungeon and Roguelike module.' WHERE `command_path`='dm';
UPDATE `server_help_command` SET `title_en`='Dungeon Master status', `description_en`='Shows whether the module is active, sessions, level band, difficulties, themes and dungeons.' WHERE `command_path`='dm status';
UPDATE `server_help_command` SET `title_en`='Dungeon Master sessions', `description_en`='Lists the active procedural or Roguelike sessions with their data.' WHERE `command_path`='dm list';
UPDATE `server_help_command` SET `title_en`='Clear the Dungeon Master cooldown', `description_en`='Removes the Dungeon Master cooldown for the whole group of the selected target.' WHERE `command_path`='dm clearcooldown';
UPDATE `server_help_command` SET `title_en`='End a Dungeon Master session', `description_en`='Forces a session to end; with no id it tries to use your own session.' WHERE `command_path`='dm end';
UPDATE `server_help_command` SET `title_en`='Reload Dungeon Master', `description_en`='Hot-reloads the module configuration.' WHERE `command_path`='dm reload';

UPDATE `server_help_command` SET `title_en`='Query progression', `description_en`='Shows the individual progression phase of the target, or your own if none is given.' WHERE `command_path`='ip get';
UPDATE `server_help_command` SET `title_en`='Change progression', `description_en`='Sets the character''s progression phase. Use only values valid for the module.' WHERE `command_path`='ip set';
UPDATE `server_help_command` SET `title_en`='Teleport to a progression instance', `description_en`='Takes the character to a special location accepted by the module: naxx40, onyxia40, naxx or onyxia.' WHERE `command_path`='ip tele';
UPDATE `server_help_command` SET `title_en`='Adjust bot progression', `description_en`='Syncs or adjusts the progression of the bots related to the player running the command. It requires a game session.' WHERE `command_path`='ip setbot';
UPDATE `server_help_command` SET `title_en`='Adjust reputations by progression', `description_en`='Adjusts the group''s reputations according to progression. The installed version may keep this operation disabled.' WHERE `command_path`='ip setrep';
UPDATE `server_help_command` SET `title_en`='Review PvP progression', `description_en`='Queries or updates the target''s PvP progression state.' WHERE `command_path`='ip pvp';
UPDATE `server_help_command` SET `title_en`='Grant attunement', `description_en`='Applies to the group the attunement recognised by the module for Onyxia or the Black Temple.' WHERE `command_path`='ip attune';

UPDATE `server_help_command` SET `title_en`='Update the auction house', `description_en`='Immediately runs a posting and buying cycle of the auction bot. It can be repeated during the initial fill.' WHERE `command_path`='ahbot update';
UPDATE `server_help_command` SET `title_en`='Reload AHBot', `description_en`='Reloads the configuration and prepares candidates, proportions and advanced rules again.' WHERE `command_path`='ahbot reload';
UPDATE `server_help_command` SET `title_en`='Empty the bot''s auctions', `description_en`='Removes the auctions created by AuctionHouseBot and cleans up their expired items. It does not delete players'' ones.' WHERE `command_path`='ahbot empty';
UPDATE `server_help_command` SET `title_en`='AHBot help', `description_en`='Shows in chat the summary of commands the module offers.' WHERE `command_path`='ahbot help';

UPDATE `server_help_command` SET `title_en`='Ahn''Qiraj War Effort', `description_en`='Tools to check the progress of materials handed in by both factions.' WHERE `command_path`='wareffort';
UPDATE `server_help_command` SET `title_en`='War Effort materials', `description_en`='Shows the materials gathered by Alliance and Horde and whether the goals have been completed.' WHERE `command_path`='wareffort scores';

UPDATE `server_help_command` SET `title_en`='Playerbots guild tasks', `description_en`='Runs administrative orders of the Playerbots guild task manager.' WHERE `command_path`='playerbots gtask';
UPDATE `server_help_command` SET `title_en`='Playerbots performance monitor', `description_en`='Queries or resets the AI''s internal performance metrics.' WHERE `command_path`='playerbots pmon';
UPDATE `server_help_command` SET `title_en`='Manage random bots', `description_en`='Runs a console order of the RandomPlayerbots manager.' WHERE `command_path`='playerbots rndbot';
UPDATE `server_help_command` SET `title_en`='Battleground diagnostics', `description_en`='Runs the Playerbots diagnostics related to battlegrounds.' WHERE `command_path`='playerbots debug bg';

UPDATE `server_help_command` SET `title_en`='Dungeon Clear automatic tests', `description_en`='GM technical harness: creates a random group of five bots, equips it and runs a dungeon without putting the GM in the group. Each test keeps its seed.' WHERE `command_path`='dc test';
UPDATE `server_help_command` SET `title_en`='Start a dungeon test', `description_en`='Creates and launches a reproducible test. It supports heroic mode, level, seed, item level and quality.' WHERE `command_path`='dc test start';
UPDATE `server_help_command` SET `title_en`='Test status', `description_en`='Shows phase, group, target and provisional result of the active test.' WHERE `command_path`='dc test status';
UPDATE `server_help_command` SET `title_en`='Stop a test', `description_en`='Cancels and cleans up an active automatic test.' WHERE `command_path`='dc test stop';
UPDATE `server_help_command` SET `title_en`='Test dungeons', `description_en`='Lists the dungeon identifiers accepted by ".dc test start".' WHERE `command_path`='dc test list';
UPDATE `server_help_command` SET `title_en`='A test''s gear', `description_en`='Shows or inspects the gear roll of the test bots.' WHERE `command_path`='dc test gear';
UPDATE `server_help_command` SET `title_en`='Watch a test', `description_en`='Connects the GM camera to an automatic test. It needs an in-game session.' WHERE `command_path`='dc test watch';
UPDATE `server_help_command` SET `title_en`='Repeated test plan', `description_en`='Runs the same dungeon several times to obtain a reproducible success rate.' WHERE `command_path`='dc test plan';
UPDATE `server_help_command` SET `title_en`='Start a test plan', `description_en`='Schedules several runs of a dungeon with the given parameters.' WHERE `command_path`='dc test plan start';
UPDATE `server_help_command` SET `title_en`='Test plan status', `description_en`='Shows completed runs, successes, failures and the current test.' WHERE `command_path`='dc test plan status';
UPDATE `server_help_command` SET `title_en`='Stop the test plan', `description_en`='Cancels the plan and the associated technical run.' WHERE `command_path`='dc test plan stop';
UPDATE `server_help_command` SET `title_en`='Readjust a running plan', `description_en`='Changes the set of dungeons or the concurrency of a live plan without restarting it; runs in progress are not aborted.' WHERE `command_path`='dc test plan edit';
UPDATE `server_help_command` SET `title_en`='Pause a test plan', `description_en`='Stops launching new runs; those already running finish. With no selector, or with "all", it affects all plans.' WHERE `command_path`='dc test plan pause';
UPDATE `server_help_command` SET `title_en`='Resume a test plan', `description_en`='Launches runs again in a paused plan.' WHERE `command_path`='dc test plan resume';
UPDATE `server_help_command` SET `title_en`='RDF fill diagnostics', `description_en`='GM tools for the instant filling of the dungeon finder queue.' WHERE `command_path`='dc dungeonqueuefill';
UPDATE `server_help_command` SET `title_en`='RDF fill status', `description_en`='Shows the state of the dungeon queue filling process.' WHERE `command_path`='dc dungeonqueuefill status';
UPDATE `server_help_command` SET `title_en`='Cancel the RDF fill', `description_en`='Cancels the active technical queue filling process.' WHERE `command_path`='dc dungeonqueuefill cancel';
UPDATE `server_help_command` SET `title_en`='Test the RDF fill', `description_en`='Launches the technical check of the automatic queue filling.' WHERE `command_path`='dc dungeonqueuefill test';
UPDATE `server_help_command` SET `title_en`='Battleground fill diagnostics', `description_en`='GM tools for the instant battleground filling of mod-dungeon-clear. It is off by default (DungeonClear.BgQueueFill.Enable) and on this server the queues are filled by mod-queue-bots.' WHERE `command_path`='dc bgqueuefill';
UPDATE `server_help_command` SET `title_en`='Battleground fill status', `description_en`='Says whether it is active, the fills in progress with their phase and bots per side, the waiting players and the released bots.' WHERE `command_path`='dc bgqueuefill status';
UPDATE `server_help_command` SET `title_en`='Cancel a battleground fill', `description_en`='Releases a player''s fill. It keeps their place in the real queue or their match.' WHERE `command_path`='dc bgqueuefill cancel';
UPDATE `server_help_command` SET `title_en`='Test the battleground fill', `description_en`='Opens a fill for a player already waiting in a battleground queue, without them having to sign up again.' WHERE `command_path`='dc bgqueuefill test';

UPDATE `server_help_command` SET `title_en`='Turn on the GM flag', `description_en`='Turns on the character''s GM powers. By itself it does not change visibility, flight or chat badge.' WHERE `command_path`='gm on';
UPDATE `server_help_command` SET `title_en`='Turn off the GM flag', `description_en`='Turns off the character''s GM flag.' WHERE `command_path`='gm off';
UPDATE `server_help_command` SET `title_en`='GMs online', `description_en`='Lists the GM characters that are online and visible as such.' WHERE `command_path`='gm ingame';
UPDATE `server_help_command` SET `title_en`='GM badge in chat', `description_en`='Queries, turns on or off the GM badge on the character''s messages.' WHERE `command_path`='gm chat';
UPDATE `server_help_command` SET `title_en`='GM flight', `description_en`='Queries, turns on or off free flight of the GM character.' WHERE `command_path`='gm fly';
UPDATE `server_help_command` SET `title_en`='GM visibility', `description_en`='Queries or changes whether other players can see the GM character. "off" makes it invisible.' WHERE `command_path`='gm visible';
UPDATE `server_help_command` SET `title_en`='GM spectator compatibility', `description_en`='Allows a GM to follow members of the opposite faction; it may require changing zone.' WHERE `command_path`='gm spectator';
UPDATE `server_help_command` SET `title_en`='GM accounts', `description_en`='Lists all GM accounts and their security levels.' WHERE `command_path`='gm list';

UPDATE `server_help_command` SET `title_en`='Current coordinates', `description_en`='Shows map, zone, coordinates and orientation of the target or of your character.' WHERE `command_path`='gps';
UPDATE `server_help_command` SET `title_en`='Go to a player', `description_en`='Teleports the GM to the given or selected character.' WHERE `command_path`='appear';
UPDATE `server_help_command` SET `title_en`='Bring a player', `description_en`='Teleports the given or selected character to the GM.' WHERE `command_path`='summon';
UPDATE `server_help_command` SET `title_en`='Go to coordinates', `description_en`='Teleports the GM to coordinates on the given map.' WHERE `command_path`='go xyz';
UPDATE `server_help_command` SET `title_en`='Go to a creature by entry', `description_en`='Finds a spawn of the given entry and takes the GM to it; the final number picks the match.' WHERE `command_path`='go creature id';
UPDATE `server_help_command` SET `title_en`='Go to a world object', `description_en`='Finds a spawn of the given gameobject and takes the GM to it.' WHERE `command_path`='go gameobject id';
UPDATE `server_help_command` SET `title_en`='Go to zone coordinates', `description_en`='Teleports using X/Y percentages within a zone.' WHERE `command_path`='go zonexy';
UPDATE `server_help_command` SET `title_en`='Go to a graveyard', `description_en`='Takes the GM to the graveyard with the given identifier.' WHERE `command_path`='go graveyard';
UPDATE `server_help_command` SET `title_en`='Go to a quest', `description_en`='Takes the GM to a creature or object related to the given quest.' WHERE `command_path`='go quest';
UPDATE `server_help_command` SET `title_en`='Teleport another player', `description_en`='Sends the given character to a saved location.' WHERE `command_path`='teleport name';
UPDATE `server_help_command` SET `title_en`='Teleport a group', `description_en`='Sends the group of the given player to a saved location.' WHERE `command_path`='teleport group';
UPDATE `server_help_command` SET `title_en`='Delete a location', `description_en`='Removes a saved destination from the teleport table.' WHERE `command_path`='teleport del';

UPDATE `server_help_command` SET `title_en`='Modify a character', `description_en`='Family of direct changes on the selected player: resources, money, speed, phase, scale, reputation and points. Check the subcommand before applying it.' WHERE `command_path`='modify';
UPDATE `server_help_command` SET `title_en`='Modify health', `description_en`='Changes the current and maximum health of the selected player.' WHERE `command_path`='modify hp';
UPDATE `server_help_command` SET `title_en`='Modify mana', `description_en`='Changes the current and maximum mana of the selected player.' WHERE `command_path`='modify mana';
UPDATE `server_help_command` SET `title_en`='Modify money', `description_en`='Adds or subtracts copper to the selected player. Ten thousand copper equal one gold coin.' WHERE `command_path`='modify money';
UPDATE `server_help_command` SET `title_en`='Modify speed', `description_en`='Family to change the run, flight, swim and walk speeds of the target.' WHERE `command_path`='modify speed';
UPDATE `server_help_command` SET `title_en`='Modify phase', `description_en`='Changes the phase mask of the selected character. A wrong value can hide content.' WHERE `command_path`='modify phase';
UPDATE `server_help_command` SET `title_en`='Modify scale', `description_en`='Temporarily changes the visual size of the target.' WHERE `command_path`='modify scale';
UPDATE `server_help_command` SET `title_en`='Modify talent points', `description_en`='Sets the free talent points of the selected character.' WHERE `command_path`='modify talentpoints';

UPDATE `server_help_command` SET `title_en`='World events', `description_en`='Queries and manually controls the events defined in game_event.' WHERE `command_path`='event';
UPDATE `server_help_command` SET `title_en`='Active events', `description_en`='Lists the world events that are active right now.' WHERE `command_path`='event activelist';
UPDATE `server_help_command` SET `title_en`='Event information', `description_en`='Shows the state, dates and data of the given event.' WHERE `command_path`='event info';
UPDATE `server_help_command` SET `title_en`='Start an event', `description_en`='Manually activates the given world event.' WHERE `command_path`='event start';
UPDATE `server_help_command` SET `title_en`='Stop an event', `description_en`='Manually deactivates the given world event.' WHERE `command_path`='event stop';

UPDATE `server_help_command` SET `title_en`='Manage quests', `description_en`='Command family to add, complete, remove, reward or query quests of the selected player.' WHERE `command_path`='quest';
UPDATE `server_help_command` SET `title_en`='Add a quest', `description_en`='Adds the given quest to the selected player''s log, if they can accept it.' WHERE `command_path`='quest add';
UPDATE `server_help_command` SET `title_en`='Complete quest objectives', `description_en`='Marks the objectives of the given quest as completed; it does not hand in the reward automatically.' WHERE `command_path`='quest complete';
UPDATE `server_help_command` SET `title_en`='Remove a quest', `description_en`='Removes the given quest from the selected player''s log.' WHERE `command_path`='quest remove';
UPDATE `server_help_command` SET `title_en`='Reward a quest', `description_en`='Hands in the reward of a completed quest when the command can resolve it.' WHERE `command_path`='quest reward';
UPDATE `server_help_command` SET `title_en`='Quest status', `description_en`='Shows the state of the given quest for the selected player.' WHERE `command_path`='quest status';

UPDATE `server_help_command` SET `title_en`='Kick a player', `description_en`='Disconnects the given character, saving the reason in the command log. It is also available as a form in Moderation.' WHERE `command_path`='kick';
UPDATE `server_help_command` SET `title_en`='Mute an account', `description_en`='Prevents talking for the given duration for the character or account resolved by the core. It is also available as a form in Moderation.' WHERE `command_path`='mute';
UPDATE `server_help_command` SET `title_en`='Remove a mute', `description_en`='Removes the mute from the account associated with the character. It is also available as a form in Moderation.' WHERE `command_path`='unmute';
UPDATE `server_help_command` SET `title_en`='Mute history', `description_en`='Shows the mute history of the given target.' WHERE `command_path`='mutehistory';
UPDATE `server_help_command` SET `title_en`='Apply a ban', `description_en`='Family to block an account, character or IP for a period. Always confirm type, duration and reason.' WHERE `command_path`='ban';
UPDATE `server_help_command` SET `title_en`='Ban information', `description_en`='Queries the blocks associated with an account, character or IP.' WHERE `command_path`='baninfo';
UPDATE `server_help_command` SET `title_en`='List bans', `description_en`='Lists bans of the given type that match the filter.' WHERE `command_path`='banlist';
UPDATE `server_help_command` SET `title_en`='Lift a ban', `description_en`='Administrative family to lift a block on an account, character or IP.' WHERE `command_path`='unban';
UPDATE `server_help_command` SET `title_en`='Player information', `description_en`='Shows account, character, IP, level, played time and sanctions of the target according to your permissions.' WHERE `command_path`='pinfo';
UPDATE `server_help_command` SET `title_en`='Freeze a player', `description_en`='Immobilises the given character until ".unfreeze" is used.' WHERE `command_path`='freeze';
UPDATE `server_help_command` SET `title_en`='Unfreeze a player', `description_en`='Lifts the immobilisation applied with ".freeze".' WHERE `command_path`='unfreeze';

UPDATE `server_help_command` SET `title_en`='Send server mail', `description_en`='Family to send mail, items or gold to a character; some variants also accept direct messages.' WHERE `command_path`='send';
UPDATE `server_help_command` SET `title_en`='Send items by mail', `description_en`='Sends one or several items to the character, with subject and text. Use entry:quantity pairs.' WHERE `command_path`='send items';
UPDATE `server_help_command` SET `title_en`='Send a mail', `description_en`='Sends a system mail message with no attachments.' WHERE `command_path`='send mail';
UPDATE `server_help_command` SET `title_en`='Send gold by mail', `description_en`='Sends copper attached to a system mail. Ten thousand copper equal one gold coin.' WHERE `command_path`='send money';
UPDATE `server_help_command` SET `title_en`='Direct server message', `description_en`='Sends a system message to the given character.' WHERE `command_path`='send message';

UPDATE `server_help_command` SET `title_en`='Reset a character', `description_en`='Administrative family to reset level, stats, spells, talents, gear or other data. Some operations are irreversible.' WHERE `command_path`='reset';
UPDATE `server_help_command` SET `title_en`='Reset level', `description_en`='Returns the character''s level to the server-defined starting one and recalculates its stats.' WHERE `command_path`='reset level';
UPDATE `server_help_command` SET `title_en`='Recalculate stats', `description_en`='Resets and recalculates the character''s base stats.' WHERE `command_path`='reset stats';
UPDATE `server_help_command` SET `title_en`='Reset spells', `description_en`='Removes and relearns the spells that correspond to the character.' WHERE `command_path`='reset spells';
UPDATE `server_help_command` SET `title_en`='Reset talents', `description_en`='Resets the talents of the given character.' WHERE `command_path`='reset talents';

UPDATE `server_help_command` SET `title_en`='Adjust a skill', `description_en`='Changes the value and, optionally, the maximum of a skill of the selected character. Use it to fix a character; it does not teach recipes or replace the trainer.' WHERE `command_path`='setskill';
UPDATE `server_help_command` SET `title_en`='Maximise skills', `description_en`='Raises all skills of the selected character to the maximum allowed. It is a GM tool and also alters skills that are not professions.' WHERE `command_path`='maxskill';

UPDATE `server_help_article` SET `title_en`='How this server works', `body_en`='It is a World of Warcraft 3.3.5a (AzerothCore) server designed to play alone or with very few people, with bots acting as companions:\n\n• Per-character progression: you start in Vanilla and unlock TBC and WotLK as you go (mod-individual-progression).\n• Hundreds of bots populate the world, do quests, dungeons and battlegrounds. Those in your zone and at your level are brought to your side (mod-world-bots).\n• Any queue in the game is filled with bots: dungeon finder, raids, battlegrounds, arenas (mod-queue-bots).\n• You can found your own guild and fill it with bots that log in with you (mod-home-guild), and form a group wherever you are with ".grupo".\n• Any race can be any class (ARAC), transmogrification, a materials bank and more services in the eleven capitals.\n\nThis help tab shows all the commands your account can use. Type a word in the search box (for example "grupo" or "dungeon") or choose a category.' WHERE `id`=1;
UPDATE `server_help_article` SET `title_en`='Playing in a group with bots', `body_en`='Three ways to have a group, from less to more control:\n\n1. Dungeon finder / raids / battlegrounds: sign up as usual. The queue fills with bots of your level range and the roles that are missing (mod-queue-bots). In a finder dungeon, if there is a bot tank and you are not the tank, the tank runs the dungeon by itself (mod-dungeon-clear, ".dc").\n\n2. ".grupo": wherever you are, a group of five with a real tank and healer appears at your side. ".grupo banda 10|25|40" for a raid. ".grupo fuera" to dismiss them. When you accept a quest that suggests several players, one or two join you by themselves and leave when you hand it in.\n\n3. Your own characters as bots: ".playerbots bot add <name>" puts a character of your account driven by the AI into your group.\n\nThe gear of the bots that join your group is adjusted to your average and to the cap of your progression phase, so they are not better dressed than the content.' WHERE `id`=2;
UPDATE `server_help_article` SET `title_en`='Your guild', `body_en`='The server does not create or adopt a guild for you automatically. When you found one through the normal game system, it is marked as your home guild as long as you remain its leader (with HomeGuild.AutoAdopt = 0 you have to mark it by hand with ".hermandad activar"). A previous guild, or one where you are only a member, is not modified.\n\nThe home guild receives up to 15 bots of your faction and level. They log in with you, level up when they fall behind and are the first ones ".grupo" and the finder call: the same tank every week. They chat in guild chat.\n\n".hermandad estado" shows the roster; ".hermandad desactivar" kicks the bots without dissolving the guild. If you dissolve it, hand over leadership or stop logging in for many days, the module stops managing it and its bots return to the pool.' WHERE `id`=3;
UPDATE `server_help_article` SET `title_en`='Races and classes: any combination', `body_en`='This server allows any race with any class (mod-arac): a tauren paladin, a human shaman, a draenei rogue... For the character creation screen to offer them you need the client patch "Patch-Arac.MPQ" in the Data folder of your WoW (it is in the installer''s cliente/ folder).\n\nThe new combinations have a class trainer in their starting zone, their first town and their capital: they are the generic "<class> Trainer", friendly to both factions. They only respond to characters of their class.' WHERE `id`=4;
UPDATE `server_help_article` SET `title_en`='Level-up rewards', `body_en`='On reaching certain levels you receive things that remove friction when playing alone (mod-congrats-on-level):\n\n• 10: 1 gold and four 10-slot bags (the warlock also gets a soul bag).\n• 20: 5 gold and Apprentice Riding.\n• 40: 25 gold, four 14-slot bags, Journeyman Riding and dual specialisation.\n• 60: 75 gold, four 16-slot bags and Expert flying.\n• 70: 150 gold and epic flying.\n• 77: Cold Weather Flying (without it you cannot fly in Northrend).\n• 80: 50 gold and four 20-slot bags.\n\nIf it does not fit in the bag, it arrives by mail. It is not retroactive: a character that has already passed the level does not receive it.' WHERE `id`=5;
UPDATE `server_help_article` SET `title_en`='Progression Vanilla → TBC → WotLK', `body_en`='Each character advances through phases (mod-individual-progression): you start with the Vanilla content (Molten Core, Blackwing Lair, Ahn''Qiraj, Naxxramas 40) and on completing it Outland opens, and then Northrend. Item stats, talents and bots respect the phase you are in.\n\nThe 40-player raids are tuned so they can be done with a small group and bots. The Ahn''Qiraj War Effort is completed by a single player.' WHERE `id`=6;
UPDATE `server_help_article` SET `title_en`='Services in the capitals', `body_en`='In the eleven capitals, around the Dungeon Master NPC, there are:\n\n• Warpweaver: transmogrification (also ".transmog claim" from the bag).\n• Ling: profession materials bank.\n• Swirl: racial trait swap, for gold.\n• 1v1 arena master: skirmish against a bot (level 80).\n\nAnd the Dungeon Master, for procedural dungeons and Roguelike mode.' WHERE `id`=7;
UPDATE `server_help_article` SET `title_en`='Asking for help and server notices', `body_en`='To talk to the administrator, use "Talk to a GM" or "Report a problem" in this same window: it opens an enquiry (ticket) that the GM sees on connecting.\n\nIf you get stuck, "Stuck character" uses the hearthstone first and, if it cannot, pushes you out of the spot.\n\nThe server restarts every day at 00:00 (it warns at 23:55) and on Sundays at 05:00 the whole machine restarts.' WHERE `id`=8;
UPDATE `server_help_article` SET `title_en`='GM quick guide on this server', `body_en`='What is used most, with the administrator account:\n\n• ".gm on" for GM mode.\n• ".go creature id 500000 <n>" takes you to the Dungeon Master of capital n (1 Thunder Bluff ... 11 Stormwind).\n• ".ahbot update" several times at the start to fill the auction house.\n• ".wpvp iniciar Southshore" forces a bot skirmish; ".wpvp estado" and ".wpvp parar".\n• ".actualizaciones" repeats the new-versions notice.\n• ".reload config" reloads worldserver.conf; modules usually need a restart.\n• ".account set gmlevel <account> <0-3> -1" changes an account''s level (reconnect for it to show in this help).\n\nThe server log is at env/dist/bin/Server.log; each own module writes its lines with its name in square brackets ([world-bots], [queue-bots], [party-here]...).' WHERE `id`=9;
UPDATE `server_help_article` SET `title_en`='World war', `body_en`='Every minute a die is rolled (25 %): if it comes up, a hotspot is chosen in a zone with a player (Southshore, Tarren Mill, the Crossroads, Astranaar, Sun Rock Retreat...) and a group of bots from one faction marches on it while the other defends. Those in the zone are told ("The Horde marches on Southshore") and it lasts between 5 and 18 minutes. The side that holds more live bots next to the objective gradually controls it; if it keeps control long enough, it captures the point and the skirmish ends early with a winner announcement. At the gates of Stormwind and Orgrimmar, instead of war, there are duels.\n\nThe 50-60 hotspots (Searing Gorge, Blackrock Mountain, Light''s Hope Chapel) switch on by themselves when there are bots of that level on both factions. The war bots wear gear in line with your progression phase.\n\nOnly free bots take part (never those of your group, your guild or a queue) and nobody appears within 160 yards of a player.' WHERE `id`=10;
UPDATE `server_help_article` SET `title_en`='Professions and the materials bank', `body_en`='You can learn two primary professions, such as alchemy, blacksmithing, enchanting, engineering, herbalism, inscription, jewelcrafting, mining, leatherworking or tailoring. Cooking, fishing and first aid are secondary and do not take one of those two slots. They are learned and levelled by talking to the trainers and using their recipes, just like in the original game.\n\nIn the capitals there is an NPC called Ling: it is the materials bank. It stores profession reagents separately so they do not take space in your normal bank. Each character''s profession and recipes remain individual; the bank only stores the materials. The guildhouse can buy its own Ling, which shares that same bank.\n\nContent progression also applies to recipes, reagents and crafted items: on unlocking a phase you will have access to its corresponding content.\n\nWith profession experience enabled, orange activities give 1 % of the XP needed to level up from your current level, yellow ones 0.5 %, green ones 0.25 % and grey ones nothing, before challenge modifiers. Fishing junk also counts and it is not necessary to gain a skill point. On reaching the maximum of the learned rank, it stops giving XP until the next rank is trained; at 450/450 it gives no XP either.\n\nIt includes the configurable activities of all professions, smelting, disenchanting and lockpicking. Prospecting and milling give no XP. Bots follow the same rules; this XP does not contribute guild experience nor receive its bonus. Locked XP, maximum level, progression limits and challenges are respected: Quest-only prevents this XP and Slow XP reduces it. There is no additional reduction for using starter materials with a high-level character.' WHERE `id`=11;
UPDATE `server_help_article` SET `title_en`='Group quest loot', `body_en`='When killing a creature in a group, each nearby eligible member can pick up a copy of the white items that appear as quest loot. It also works in dungeons and with the group''s bots. Each person loots their own copy; the item does not always appear nor is it handed over automatically.\n\nWith area loot enabled, opening a corpse also collects your copies from nearby corpses, as long as they fit whole in your bags. Other people''s copies remain available. Whoever joins or arrives after the creature died does not get a new copy. Solo, normal materials and items of other qualities follow the usual rules.\n\nA bot you bring with ".grupo" also carries your quests (while it remains in your group) so it can pick up its own copy like a player; you are told in chat when this happens.' WHERE `id`=12;
UPDATE `server_help_article` SET `title_en`='The guildhouse as a professions hub', `body_en`='The guild buys a guildhouse on GM Island (the leader, 1,000 gold, by talking to Talamortis in the capitals). You enter with the Guildhouse Stone (10 seconds, 30-minute cooldown, not shared with your hearthstone''s), with ".gh teleport" or with the vendor. Whoever has purchase rank asks the steward Xrispins for upgrades; there are trade ones:\n\n• Primary and secondary profession trainers (50 gold each). They teach the same as their in-game counterparts: the high ranks of each trade are trained in Shattrath or Dalaran, to which the guildhouse has a portal according to your stage. The leatherworking one only appears after TBC tier 2.\n• Forge and anvil (50 gold), needed to smelt and forge.\n• Ling, the materials banker (100 gold): stores each character''s profession materials. It is the same bank as in the capitals, it is yours and not the guild''s, and it stays there if the guildhouse is sold. A guest in your group uses their own bank.\n\nTypical route: you open a treasure chest, deposit the loot with Ling ("Deposit all materials"), withdraw what you need, smelt at the forge and craft. If a service does not show in the steward''s menu, check that your guild rank allows purchasing upgrades.' WHERE `id`=13;
