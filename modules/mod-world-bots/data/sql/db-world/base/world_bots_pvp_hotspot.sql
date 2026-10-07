-- mod-world-bots: puntos calientes de la guerra de mundo (acore_world).
--
-- Fork propio de la tabla de mod-playerbot-world-pvp (TopHatMan, ed7962cc):
-- mismas columnas y mismas 23 filas de semilla (coordenadas aproximadas: el
-- modulo corrige la altura al soltar a los bots), mas una columna `label` con
-- el nombre que se anuncia a los jugadores de la zona. Idempotente: se puede
-- relanzar; lo editado a mano en la BD se conserva salvo en las columnas que
-- lista el ON DUPLICATE KEY UPDATE (todas menos `enabled`, `weight` y
-- `label`, que son las que uno toca para afinar el servidor).
--
-- attacker_team / defender_team: 0 Alianza, 1 Horda, 2 cualquiera.
-- Misma faccion en los dos = punto de duelos (a las puertas de la capital).
-- Se aplica desde la fase 5. Recargar sin reiniciar: ".wpvp recargar".

CREATE TABLE IF NOT EXISTS `world_bots_pvp_hotspot` (
  `id` int unsigned NOT NULL AUTO_INCREMENT,
  `name` varchar(64) NOT NULL,
  `label` varchar(96) NOT NULL DEFAULT '',
  `enabled` tinyint unsigned NOT NULL DEFAULT 1,
  `attacker_team` tinyint unsigned NOT NULL DEFAULT 1 COMMENT '0 Alianza, 1 Horda, 2 cualquiera',
  `defender_team` tinyint unsigned NOT NULL DEFAULT 0 COMMENT '0 Alianza, 1 Horda, 2 cualquiera',
  `min_level` tinyint unsigned NOT NULL DEFAULT 20,
  `max_level` tinyint unsigned NOT NULL DEFAULT 60,
  `map_id` int unsigned NOT NULL DEFAULT 0,
  `rally_x` float NOT NULL DEFAULT 0,
  `rally_y` float NOT NULL DEFAULT 0,
  `rally_z` float NOT NULL DEFAULT 0,
  `rally_o` float NOT NULL DEFAULT 0,
  `target_x` float NOT NULL DEFAULT 0,
  `target_y` float NOT NULL DEFAULT 0,
  `target_z` float NOT NULL DEFAULT 0,
  `target_o` float NOT NULL DEFAULT 0,
  `attackers_min` int unsigned NOT NULL DEFAULT 2,
  `attackers_max` int unsigned NOT NULL DEFAULT 5,
  `defenders_min` int unsigned NOT NULL DEFAULT 2,
  `defenders_max` int unsigned NOT NULL DEFAULT 5,
  `duration_min` int unsigned NOT NULL DEFAULT 5 COMMENT 'minutos',
  `duration_max` int unsigned NOT NULL DEFAULT 12 COMMENT 'minutos',
  `weight` int unsigned NOT NULL DEFAULT 100,
  `cooldown_seconds` int unsigned NOT NULL DEFAULT 1800,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_world_bots_pvp_hotspot_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO `world_bots_pvp_hotspot`
(`name`,`label`,`enabled`,`attacker_team`,`defender_team`,`min_level`,`max_level`,`map_id`,`rally_x`,`rally_y`,`rally_z`,`rally_o`,`target_x`,`target_y`,`target_z`,`target_o`,`attackers_min`,`attackers_max`,`defenders_min`,`defenders_max`,`duration_min`,`duration_max`,`weight`,`cooldown_seconds`)
VALUES
-- Lakeshire: la Horda asalta Lakeshire, la Alianza responde.
('Lakeshire','Lakeshire',1,1,0,20,35,0,-9325,-2285,70,0,-9215,-2150,70,0,2,5,2,5,5,12,140,1800),
-- Trabalomas, la guerra de siempre, en las dos direcciones.
('Southshore','Costasur',1,1,0,25,45,0,-760,-930,58,0,-845,-535,55,0,3,7,3,7,8,18,180,1500),
('TarrenMill','Molino Tarren',1,0,1,25,45,0,-860,-560,55,0,-20,-915,57,0,3,7,3,7,8,18,180,1500),
-- Vallefresno.
('Astranaar','Astranaar',1,1,0,20,35,1,2300,-2500,110,0,2750,-430,110,0,2,5,2,5,5,12,120,1800),
('Splintertree','Puesto Astillero',1,0,1,20,35,1,2700,-500,110,0,2300,-2500,110,0,2,5,2,5,5,12,100,1800),
-- Los Baldios.
('Crossroads','El Cruce',1,0,1,20,35,1,-940,-3720,10,0,-456,-2650,95,0,2,5,2,5,5,12,130,1800),
('RatchetRoad','el camino de Ratchet',1,1,0,20,35,1,-455,-2650,95,0,-955,-3740,8,0,2,5,2,5,5,12,90,1800),
-- Sierra Espolon y Mil Agujas.
('SunRockRetreat','Refugio Roca del Sol',1,0,1,22,35,1,1040,760,105,0,960,915,105,0,2,5,2,5,5,12,80,2400),
('FreewindPost','Puesto Viento Libre',1,0,1,25,38,1,-5400,-2500,-50,0,-5440,-2440,90,0,2,5,2,5,5,12,70,2400),
-- Tierras Altas de Arathi.
('RefugePointe','Punta del Refugio',1,1,0,30,45,0,-920,-3490,70,0,-1260,-2520,35,0,3,7,3,7,8,18,130,1800),
('Hammerfall','Caemartillo',1,0,1,30,45,0,-1260,-2520,35,0,-920,-3490,70,0,3,7,3,7,8,18,130,1800),
-- Stranglethorn.
('NesingwaryCamp','el campamento de Nesingwary',1,1,0,30,45,0,-11670,-50,5,0,-11620,-70,10,0,2,6,2,6,6,14,100,1800),
('BootyBayRoad','el camino de Bahia del Botin',1,0,1,35,50,0,-14450,460,15,0,-14300,520,20,0,2,6,2,6,6,14,80,2400),
-- Marjal Revolcafango, Desolace, Tierras Inhospitas.
('TheramoreRoad','el camino de Theramore',1,1,0,35,45,1,-3600,-4500,10,0,-3700,-4400,15,0,2,5,2,5,6,14,70,2400),
('Brackenwall','Poblado Muro de Helechos',1,0,1,35,45,1,-3150,-2850,35,0,-2920,-3170,35,0,2,5,2,5,6,14,60,2400),
('NijelsPoint','Punta de Nijel',1,1,0,30,42,1,280,-2380,105,0,150,-1780,105,0,2,5,2,5,6,14,60,2400),
('Shadowprey','Presa de las Sombras',1,0,1,30,42,1,-1650,3100,90,0,-1600,3050,90,0,2,5,2,5,6,14,60,2400),
('Kargath','Kargath',1,0,1,35,50,0,-6700,-2400,240,0,-6650,-2200,245,0,2,5,2,5,6,14,70,2400),
-- 45+ / 50+: apagados hasta tener poblacion de ese nivel.
('SearingGorge','las Quebradas Abrasadas',0,2,2,45,55,0,-6500,-1200,180,0,-6550,-1150,180,0,3,8,3,8,8,18,80,2400),
('BlackrockMountain','la Montana Roca Negra',0,2,2,50,60,0,-7500,-1100,270,0,-7550,-1250,270,0,4,10,4,10,10,25,100,2400),
('LightHopeChapel','la Capilla de la Esperanza de la Luz',0,1,0,55,60,0,2250,-5300,85,0,2280,-5310,85,0,4,10,4,10,10,25,90,3600),
-- Duelos a las puertas de las capitales (misma faccion en los dos bandos).
('StormwindDuel','Ventormenta',1,0,0,20,60,0,-8834,622,94,0,-8795,585,96,0,2,5,2,5,5,10,90,1200),
('OrgrimmarDuel','Orgrimmar',1,1,1,20,60,1,1502,-4415,22,0,1450,-4418,25,0,2,5,2,5,5,10,90,1200)
ON DUPLICATE KEY UPDATE
  `attacker_team`=VALUES(`attacker_team`),
  `defender_team`=VALUES(`defender_team`),
  `min_level`=VALUES(`min_level`),
  `max_level`=VALUES(`max_level`),
  `map_id`=VALUES(`map_id`),
  `rally_x`=VALUES(`rally_x`),
  `rally_y`=VALUES(`rally_y`),
  `rally_z`=VALUES(`rally_z`),
  `rally_o`=VALUES(`rally_o`),
  `target_x`=VALUES(`target_x`),
  `target_y`=VALUES(`target_y`),
  `target_z`=VALUES(`target_z`),
  `target_o`=VALUES(`target_o`),
  `attackers_min`=VALUES(`attackers_min`),
  `attackers_max`=VALUES(`attackers_max`),
  `defenders_min`=VALUES(`defenders_min`),
  `defenders_max`=VALUES(`defenders_max`),
  `duration_min`=VALUES(`duration_min`),
  `duration_max`=VALUES(`duration_max`),
  `cooldown_seconds`=VALUES(`cooldown_seconds`);

-- Estado en runtime: momento del ultimo arranque de cada punto caliente, en
-- segundos unix. Lo escribe el modulo de forma asincrona al empezar un evento,
-- para que el enfriamiento sobreviva a un reinicio del worldserver (los de
-- memoria solo sobreviven a ".wpvp recargar"). Sin filas de semilla.
CREATE TABLE IF NOT EXISTS `world_bots_pvp_state` (
  `name` varchar(64) NOT NULL,
  `last_start_unix` bigint unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
