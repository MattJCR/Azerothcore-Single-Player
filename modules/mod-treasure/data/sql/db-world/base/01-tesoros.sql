-- SP03. Estado y catálogo propios: no modificar los cofres originales.
CREATE TABLE IF NOT EXISTS `sp_treasure_zone` (
  `zone_id` SMALLINT UNSIGNED NOT NULL PRIMARY KEY,
  `map_id` SMALLINT UNSIGNED NOT NULL,
  `basic_slots` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `rare_slots` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `epic_slots` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `basic_item` INT UNSIGNED NOT NULL,
  `rare_item` INT UNSIGNED NOT NULL,
  `epic_item` INT UNSIGNED NOT NULL DEFAULT 0,
  `enabled` TINYINT(1) NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `sp_treasure_point` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  `source_guid` INT UNSIGNED NOT NULL,
  `variant` TINYINT UNSIGNED NOT NULL,
  `map_id` SMALLINT UNSIGNED NOT NULL,
  `zone_id` SMALLINT UNSIGNED NOT NULL,
  `area_id` SMALLINT UNSIGNED NOT NULL,
  `x` FLOAT NOT NULL, `y` FLOAT NOT NULL, `z` FLOAT NOT NULL,
  `orientation` FLOAT NOT NULL,
  `validated` TINYINT(1) NOT NULL DEFAULT 0,
  `checked_at` TIMESTAMP NULL DEFAULT NULL,
  `check_result` VARCHAR(40) NOT NULL DEFAULT 'sin comprobar',
  UNIQUE KEY `uq_source_variant` (`source_guid`,`variant`),
  KEY `idx_zone_validated` (`zone_id`, `validated`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `sp_treasure_slot` (
  `zone_id` SMALLINT UNSIGNED NOT NULL,
  `quality` TINYINT UNSIGNED NOT NULL,
  `slot_no` TINYINT UNSIGNED NOT NULL,
  `point_id` INT UNSIGNED DEFAULT NULL,
  `previous_point_id` INT UNSIGNED DEFAULT NULL,
  `location_until` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `respawn_at` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `opened` TINYINT(1) NOT NULL DEFAULT 0,
  PRIMARY KEY (`zone_id`, `quality`, `slot_no`),
  KEY `idx_point` (`point_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Primera calibración de cuotas y premios. La activación exige puntos validados.
INSERT INTO `sp_treasure_zone`
 (`zone_id`,`map_id`,`basic_slots`,`rare_slots`,`epic_slots`,`basic_item`,`rare_item`,`epic_item`,`enabled`) VALUES
 (12,0,3,0,0,2589,818,0,0),   -- Bosque de Elwynn
 (14,1,3,0,0,2589,818,0,0),   -- Durotar
 (40,0,3,1,0,2592,1210,0,0), -- Páramos de Poniente
 (44,0,3,1,0,2592,1210,0,0), -- Montañas Crestagrana
 (10,0,4,1,0,4306,1705,0,0), -- Bosque del Ocaso
 (11,0,4,1,0,2592,1210,0,0), -- Los Humedales
 (17,1,6,2,0,2592,1210,0,0), -- Los Baldíos
 (33,0,6,2,0,4306,1705,0,0), -- Vega de Tuercespina
 (1,0,3,0,0,2589,818,0,0),    -- Dun Morogh
 (38,0,3,1,0,2592,1210,0,0),  -- Loch Modan
 (3,0,4,1,0,4338,7909,0,0),   -- Tierras Inhóspitas
 (4,0,4,1,0,14047,12361,0,0), -- Tierras Devastadas
 (8,0,4,1,0,4338,7909,0,0),   -- Pantano de las Penas
 (28,0,5,1,0,14047,12361,0,0), -- Tierras de la Peste del Oeste
 (36,0,3,1,0,4306,1705,0,0), -- Montañas de Alterac
 (45,0,4,1,0,4306,1705,0,0), -- Tierras Altas de Arathi
 (46,0,4,1,0,14047,12361,0,0), -- Las Estepas Ardientes
 (47,0,4,1,0,4338,7909,0,0), -- Tierras del Interior
 (51,0,4,1,0,14047,12361,0,0), -- La Garganta de Fuego
 (85,0,3,0,0,2589,818,0,0), -- Claros de Tirisfal
 (130,0,3,1,0,2592,1210,0,0), -- Bosque de Argénteos
 (139,0,5,1,1,14047,12361,12363,0), -- Tierras de la Peste del Este
 (267,0,4,1,0,2592,1210,0,0), -- Laderas de Trabalomas
 (15,1,4,1,0,4306,1705,0,0), -- Marjal Revolcafango
 (16,1,3,1,0,4338,7909,0,0), -- Azshara
 (141,1,3,0,0,2589,818,0,0), -- Teldrassil
 (148,1,3,1,0,2592,1210,0,0), -- Costa Oscura
 (215,1,3,0,0,2589,818,0,0), -- Mulgore
 (331,1,4,1,0,4306,1705,0,0), -- Vallefresno
 (357,1,5,1,0,4338,7909,0,0), -- Feralas
 (361,1,4,1,0,14047,12361,0,0), -- Frondavil
 (400,1,4,1,0,4306,1705,0,0), -- Las Mil Agujas
 (405,1,4,1,0,4306,1705,0,0), -- Desolace
 (406,1,4,1,0,2592,1210,0,0), -- Sierra Espolón
 (440,1,5,1,0,4338,7909,0,0), -- Tanaris
 (490,1,4,1,0,14047,12361,0,0), -- Cráter de Un'Goro
 (618,1,5,1,1,14047,12361,12363,0), -- Cuna del Invierno
 (1377,1,5,1,1,14047,12361,12363,0), -- Silithus
 (3430,530,3,0,0,2589,818,0,0), -- Bosque Canción Eterna
 (3433,530,3,1,0,2592,1210,0,0), -- Tierras Fantasma
 (3524,530,3,0,0,2589,818,0,0), -- Isla Bruma Azur
 (3525,530,3,1,0,2592,1210,0,0), -- Isla Bruma de Sangre
 (3483,530,4,1,0,21877,23107,0,0), -- Península del Fuego Infernal
 (3521,530,4,1,0,21877,23107,0,0), -- Marisma de Zangar
 (3519,530,4,1,0,21877,23107,0,0), -- Bosque de Terokkar
 (3518,530,5,1,0,21877,23107,0,0), -- Nagrand
 (3522,530,5,1,0,21877,23107,0,0), -- Montañas Filospada
 (3523,530,5,1,1,21877,23107,23438,0), -- Tormenta Abisal
 (3520,530,5,1,1,21877,23107,23438,0), -- Valle Sombraluna
 (3537,571,4,1,0,33470,36917,0,0), -- Tundra Boreal
 (495,571,4,1,0,33470,36917,0,0),  -- Fiordo Aquilonal
 (65,571,4,1,0,33470,36917,0,0),   -- Cementerio de Dragones
 (394,571,4,1,0,33470,36917,0,0),  -- Colinas Pardas
 (66,571,4,1,0,33470,36917,0,0),   -- Zul'Drak
 (3711,571,5,1,0,33470,36917,0,0), -- Cuenca de Sholazar
 (67,571,5,2,1,33470,36917,36930,0), -- Cumbres Tormentosas
 (210,571,5,2,1,33470,36917,36930,0) -- Corona de Hielo
-- Actualiza la calibración si se reedita este fichero y se reaplica (fase 8);
-- sólo `zone_id`=VALUES(`zone_id`) dejaba la fila ya instalada intacta.
-- `enabled` no entra aquí a propósito: lo fija la UPDATE de la línea siguiente.
ON DUPLICATE KEY UPDATE `map_id`=VALUES(`map_id`),`basic_slots`=VALUES(`basic_slots`),
 `rare_slots`=VALUES(`rare_slots`),`epic_slots`=VALUES(`epic_slots`),
 `basic_item`=VALUES(`basic_item`),`rare_item`=VALUES(`rare_item`),`epic_item`=VALUES(`epic_item`);

-- Las zonas funcionan desde el primer acceso humano: la validación de ruta
-- se hace automáticamente y de forma acotada antes de asignar cada cofre.
UPDATE `sp_treasure_zone` SET `enabled`=1;

-- Nodos de recolección como candidatos, nunca como validación automática.
INSERT IGNORE INTO `sp_treasure_point`
 (`source_guid`,`variant`,`map_id`,`zone_id`,`area_id`,`x`,`y`,`z`,`orientation`)
SELECT g.`guid`,v.`variant`,g.`map`,g.`zoneId`,g.`areaId`,
       g.`position_x`+v.`dx`,g.`position_y`+v.`dy`,
       g.`position_z`,g.`orientation`
FROM `gameobject` g
JOIN `sp_treasure_zone` z ON z.`zone_id`=g.`zoneId` AND z.`map_id`=g.`map`
CROSS JOIN (SELECT 1 AS `variant`,3.5 AS `dx`,0.0 AS `dy`
            UNION ALL SELECT 2,-3.5,0.0
            UNION ALL SELECT 3,0.0,3.5
            UNION ALL SELECT 4,0.0,-3.5) v
WHERE g.`id` IN
 (1617,1618,1619,1620,1621,1622,1623,1624,1628,1731,1732,1733,1734,1735,
  2040,2041,2042,2043,2044,2045,2046,2047,2048,2049,2050,
  176636,176637,176638,176639,176640,176641,176642,176643,176644,176645,
  181108,181109,181555,181556,181557,181569,181270,181271,181276,181277,181279,181280,
  183043,183044,189978,189979,189980,189981,191133,189973,190169,190170,
  190171,190172,190175,190176,191019)
  AND (g.`phaseMask` & 1)<>0 AND (g.`spawnMask` & 1)<>0;

-- Entradas por zona/calidad: cada zona mantiene su propio botín.
INSERT INTO `gameobject_template`
 (`entry`,`type`,`displayId`,`name`,`size`,`Data0`,`Data1`,`Data3`,`Data10`,`Data12`,`Data15`,`ScriptName`)
SELECT 700000+z.`zone_id`*3+q.`quality`,3,
       CASE q.`quality` WHEN 1 THEN 8686 WHEN 2 THEN 8630 ELSE 8627 END,
       CASE q.`quality` WHEN 1 THEN 'Cofre de tesoro' WHEN 2 THEN 'Cofre de tesoro raro' ELSE 'Cofre de tesoro épico' END,
       0.65,1689,700000+z.`zone_id`*3+q.`quality`,1,1,1,1,'SPTreasureChest'
FROM `sp_treasure_zone` z
JOIN (SELECT 1 AS `quality` UNION ALL SELECT 2 UNION ALL SELECT 3) q
  ON (q.`quality`=1 AND z.`basic_slots`>0)
  OR (q.`quality`=2 AND z.`rare_slots`>0)
  OR (q.`quality`=3 AND z.`epic_slots`>0)
ON DUPLICATE KEY UPDATE `displayId`=VALUES(`displayId`),`name`=VALUES(`name`),
 `Data0`=VALUES(`Data0`);

-- Borra antes de insertar (mismo patrón que el bloque de equipo, más abajo):
-- si el objeto de una zona cambiara, un simple upsert por (Entry,Item) dejaría
-- la fila vieja intacta y añadiría una segunda en vez de sustituirla.
DELETE `l` FROM `gameobject_loot_template` `l`
JOIN `sp_treasure_zone` `z` ON `l`.`Entry` IN
 (700000+`z`.`zone_id`*3+1, 700000+`z`.`zone_id`*3+2, 700000+`z`.`zone_id`*3+3);

INSERT INTO `gameobject_loot_template`
 (`Entry`,`Item`,`Reference`,`Chance`,`QuestRequired`,`LootMode`,`GroupId`,`MinCount`,`MaxCount`,`Comment`)
SELECT 700000+z.`zone_id`*3+q.`quality`,
       CASE q.`quality` WHEN 1 THEN z.`basic_item` WHEN 2 THEN z.`rare_item` ELSE z.`epic_item` END,
       0,100,0,1,0,1,CASE q.`quality` WHEN 1 THEN 3 ELSE 1 END,'SP03 tesoro por zona'
FROM `sp_treasure_zone` z
JOIN (SELECT 1 AS `quality` UNION ALL SELECT 2 UNION ALL SELECT 3) q
  ON (q.`quality`=1 AND z.`basic_slots`>0)
  OR (q.`quality`=2 AND z.`rare_slots`>0)
  OR (q.`quality`=3 AND z.`epic_slots`>0);

-- Muestras de equipo para .tesoro prueba equipo: nunca ocupan plazas normales.
-- Dos piezas intermedias (nivel de objeto 43-47) y dos finales (65-70).
INSERT INTO `gameobject_template`
 (`entry`,`type`,`displayId`,`name`,`size`,`Data0`,`Data1`,`Data3`,`Data10`,`Data12`,`Data15`,`ScriptName`)
VALUES
 (799001,3,8686,'Cofre de equipo Vanilla intermedio I',0.65,1689,799001,1,1,1,1,'SPTreasureChest'),
 (799002,3,8630,'Cofre de equipo Vanilla intermedio II',0.65,1689,799002,1,1,1,1,'SPTreasureChest'),
 (799003,3,8630,'Cofre de equipo Vanilla final I',0.65,1689,799003,1,1,1,1,'SPTreasureChest'),
 (799004,3,8627,'Cofre de equipo Vanilla final epico',0.65,1689,799004,1,1,1,1,'SPTreasureChest')
ON DUPLICATE KEY UPDATE `name`=VALUES(`name`),`displayId`=VALUES(`displayId`),
 `Data0`=VALUES(`Data0`),`Data1`=VALUES(`Data1`),`ScriptName`=VALUES(`ScriptName`);

DELETE FROM `gameobject_loot_template` WHERE `Entry` BETWEEN 799001 AND 799004;
INSERT INTO `gameobject_loot_template`
 (`Entry`,`Item`,`Reference`,`Chance`,`QuestRequired`,`LootMode`,`GroupId`,`MinCount`,`MaxCount`,`Comment`)
VALUES
 (799001,1718,0,100,0,1,0,1,1,'SP03 prueba Vanilla intermedio: Basilisk Hide Pants'),
 (799002,4091,0,100,0,1,0,1,1,'SP03 prueba Vanilla intermedio: Widowmaker'),
 (799003,19099,0,100,0,1,0,1,1,'SP03 prueba Vanilla final: Glacial Blade'),
 (799004,18805,0,100,0,1,0,1,1,'SP03 prueba Vanilla final epico: Core Hound Tooth');
