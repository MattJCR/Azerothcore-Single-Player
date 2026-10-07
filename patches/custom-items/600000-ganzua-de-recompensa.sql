-- =============================================================================
--  Ganzúa de recompensa — objeto propio 600000
--  Base: acore_world. Idempotente. Todos los .sql de patches/custom-items/ los
--  aplican las fases 5 y 8 del instalador (apply_sql_dir), antes de
--  congrats_on_level_rewards.sql. A mano:
--      mysql -u acore -pacore acore_world < patches/custom-items/600000-ganzua-de-recompensa.sql
--  Necesita reinicio del worldserver: item_template no se recarga en caliente.
--  El icono y el texto del hechizo 59403 en el cliente los pone el parche
--  cliente/Data/{esES,enUS}/patch-<idioma>-4.MPQ (tools/construir-parche-cliente-items.py).
--
--  ─────────────────────────────────────────────────────────────────────────
--  PARA QUÉ
--  ─────────────────────────────────────────────────────────────────────────
--  La capa de premios de niveles impares (congrats_on_level_rewards.sql)
--  entrega cajones de pícaro (Battered/Worn/Sturdy/Heavy/Strong/Reinforced
--  Junkbox) como recompensa de equipo: su botín es de lo poco que escala por
--  tramo de nivel. Pero para abrirlos hace falta forzar cerraduras.
--
--  Las ganzúas de herrería (Silver..Titanium Skeleton Key) NO sirven: en el
--  core, Player::CanUseItem (PlayerStorage.cpp) rechaza con
--  EQUIP_ERR_NO_REQUIRED_PROFICIENCY a quien no tenga Herrería al rango que
--  pide la ganzúa (RequiredSkill=164). Un no-herrero no puede usarlas.
--
--  Esta es una copia de la Titanium Skeleton Key (43853) SIN ese requisito:
--    - RequiredSkill / RequiredSkillRank = 0  → la usa cualquier clase.
--    - spellid_1 = 59403 (OPEN_LOCK nivel 400): abre CUALQUIER cajón de pícaro.
--    - spelltrigger_1 = 0, spellcharges_1 = -1 → se gasta en un uso.
--    - bonding = 1 (LIGADA AL RECOGER): sin intercambio, sin correo a terceros,
--      sin subasta. Petición explícita del proyecto.
--    - BuyPrice / SellPrice = 0.
--    - BagFamily = 256 heredado → va al llavero, no ocupa hueco de bolsa.
--
--  Se clona con una tabla temporal en vez de un INSERT con 150 columnas a mano,
--  para que un cambio de esquema del core aguas arriba no rompa esto.
--
--  El rango de identificadores propios de item_template es >= 600000
--  (ver REFERENCES.md). MAX(entry) del core es 57576.
-- =============================================================================

DELETE FROM `item_template` WHERE `entry` = 600000;

DROP TEMPORARY TABLE IF EXISTS `_ganzua_tmp`;
CREATE TEMPORARY TABLE `_ganzua_tmp` SELECT * FROM `item_template` WHERE `entry` = 43853;

-- displayid 57272 (Ability_Rogue_TricksOftheTrade, "Secretos del oficio").
-- El nombre y la descripción amarilla los
-- manda el servidor (no hacen falta parche ni DBC). El ICONO y la línea verde
-- "Uso: abre cerraduras..." los saca el cliente 3.3.5a de Item.dbc y Spell.dbc,
-- que no conocen el entry 600000 → hay que instalar el parche de cliente
-- cliente/Data/{esES,enUS}/patch-<idioma>-4.MPQ (lo genera
-- tools/construir-parche-cliente-items.py; lo despliega cliente/instalar-cliente.ps1).
-- Sin él la ganzúa funciona igual, pero sale con "?" y el texto de la Titanium
-- Skeleton Key.
UPDATE `_ganzua_tmp` SET
    `entry`             = 600000,
    `name`              = 'Ganzua de recompensa',
    `description`       = 'Un premio del servidor. Abre cualquier cajon cerrado sin ser picaro. Un solo uso.',
    `displayid`         = 57272,
    `Quality`           = 1,
    `Flags`             = 64,
    `FlagsExtra`        = 0,
    `BuyPrice`          = 0,
    `SellPrice`         = 0,
    `RequiredSkill`     = 0,
    `RequiredSkillRank` = 0,
    `requiredspell`     = 0,
    `bonding`           = 1,
    `ScriptName`        = '',
    `VerifiedBuild`     = 0;

INSERT INTO `item_template` SELECT * FROM `_ganzua_tmp`;
DROP TEMPORARY TABLE `_ganzua_tmp`;

-- Nombre y descripción en español. Se añade esES y esMX: algunos clientes
-- "españoles" reportan la sesión como esMX y el servidor caería al name base.
DELETE FROM `item_template_locale` WHERE `ID` = 600000;
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES
    (600000, 'esES', 'Ganzúa de recompensa',
     'Un premio del servidor. Abre cualquier cajón cerrado sin ser pícaro. Un solo uso.', 0),
    (600000, 'esMX', 'Ganzúa de recompensa',
     'Un premio del servidor. Abre cualquier cajón cerrado sin ser pícaro. Un solo uso.', 0);

-- Verificación:
--   SELECT entry,name,description,displayid,Quality,RequiredSkill,bonding,
--          spellid_1,spellcharges_1,BagFamily FROM item_template WHERE entry=600000;
--
-- IMPORTANTE en el cliente: WoW cachea las consultas de objeto en
-- Cache/WDB/<locale>/itemcache.wdb. Un cliente que se usó antes con otro
-- servidor (p. ej. ChromieCraft, que también usa entries altos) puede tener el
-- 600000 cacheado con OTROS datos. Para ver el objeto bien: borrar la carpeta
-- Cache\ del cliente (o Cache\WDB\) y volver a entrar.
