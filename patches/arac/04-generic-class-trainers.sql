-- =============================================================================
--  ARAC 04 — Instructores genéricos de clase: sólo los que faltan en cada sitio
--
--  AzerothCore trae de serie nueve instructores genéricos, uno por clase
--  (entradas 26324-26332: "Instructor de druidas", "Instructora de chamanes",
--  ...), con facción 35 (amistosa con las dos facciones), la misma lista de
--  hechizos que los instructores normales y el menú por defecto (entrenar,
--  olvidar talentos, doble especialización). Están en 24 sitios: las 8 zonas
--  de inicio, los 8 primeros pueblos y las 8 capitales. Son los que hacen
--  viables las combinaciones raza/clase de mod-arac: el core sólo comprueba la
--  CLASE del jugador, nunca la raza.
--
--  EL PROBLEMA: sus spawns están ligados en `game_event_creature` al evento
--  31, "Arena Tournament" (el de los reinos de torneo de Blizzard), que no
--  está activo, así que existen en la tabla pero nunca aparecen. Comprobado en
--  el juego el 02/09/2026: en Ammen Vale no había nadie.
--
--  LA SOLUCIÓN: sacar del evento SÓLO los genéricos de las clases que no
--  tienen instructor propio en ese sitio (en Northshire ya hay instructores
--  humanos de seis clases: sólo hacen falta cazador, chamán y druida). El
--  resto se queda en el evento, que es su estado original: así no hay
--  duplicados ni NPC que no responden a nadie. Qué falta en cada sitio se
--  calculó sobre la BD real (creature + creature_default_trainer + trainer
--  de tipo clase, sin contar los propios genéricos).
--
--  Además faltaban dos spawns necesarios: brujo en Meseta Nube Roja (un tauren
--  brujo no tenía instructor hasta Pezuña de Sangre) y brujo en Darnassus. Se
--  colocan en el hueco de la fila de sus vecinos, guids 8000101 y 8000102
--  (el instalador usa 8000001-8000011 para los NPC de servicio). El 8000103
--  (cazador en Coldridge) existió unas horas y se borra: los enanos ya tienen
--  instructor de cazadores allí.
--
--  Ojo al probar: estos NPC sólo tienen flag de instructor, no de
--  conversación. Con un personaje de otra clase el clic no hace nada.
--
--  Idempotente: primero saca del evento a TODOS los genéricos y luego vuelve
--  a meter la lista de los redundantes; los spawns propios borran su guid
--  antes de insertarse.
-- =============================================================================

-- 1) Todos los genéricos fuera del evento (estado limpio).
DELETE FROM `game_event_creature`
 WHERE `guid` IN (SELECT `guid` FROM `creature` WHERE `id` BETWEEN 26324 AND 26332);

-- 2) Spawns propios: los dos huecos necesarios; el de Coldridge se retira.
DELETE FROM `creature` WHERE `guid` IN (8000101, 8000102, 8000103);

INSERT INTO `creature`
    (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`,
     `position_x`, `position_y`, `position_z`, `orientation`,
     `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`,
     `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`, `VerifiedBuild`,
     `Comment`)
VALUES
    -- Meseta Nube Roja: Instructora de brujos, entre el guerrero (guid 95611) y la chamán (95620)
    (8000101, 26331, 1, 0, 0, 1, 1, 0,  -2900.40, -263.20,   56.78, 1.94, 180, 0, 0, 1, 0, 0, 0, 0, 0, '', 0,
     'ARAC 04: hueco de brujo en Meseta Nube Roja'),
    -- Darnassus: Instructora de brujos, entre la chamán (96440) y el guerrero (96432)
    (8000102, 26331, 1, 0, 0, 1, 1, 0,   9995.00,  2285.20, 1345.16, 4.56, 180, 0, 0, 1, 0, 0, 0, 0, 0, '', 0,
     'ARAC 04: hueco de brujo en Darnassus');

-- 3) De vuelta al evento 31 los genéricos cuya clase ya tiene instructor
--    propio en ese sitio. Se usa INSERT ... SELECT para que un guid que una
--    actualización del core haya borrado no deje una fila huérfana.

-- Northshire: quedan visibles Chamán, Cazador, Druida
--   ocultos (Pícaro, Sacerdote, Brujo, Guerrero, Mago, Paladín):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96180, 96184, 96185, 96186, 96187, 96188);

-- Villadorada: quedan visibles Druida, Cazador, Chamán
--   ocultos (Guerrero, Mago, Pícaro, Brujo, Sacerdote, Paladín):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96102, 96105, 96106, 96120, 96124, 96126);

-- Coldridge: quedan visibles Druida, Chamán
--   ocultos (Mago, Paladín, Sacerdote, Pícaro, Guerrero, Brujo):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96007, 96008, 96009, 96010, 96015, 96016);

-- Kharanos: quedan visibles Chamán, Druida
--   ocultos (Pícaro, Mago, Sacerdote, Brujo, Guerrero, Cazador, Paladín):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95877, 95879, 95880, 95881, 95882, 95884, 95888);

-- Shadowglen: quedan visibles Brujo, Paladín, Mago, Chamán
--   ocultos (Druida, Pícaro, Cazador, Sacerdote, Guerrero):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96571, 96575, 96577, 96578, 96582);

-- Dolanaar: quedan visibles Chamán, Brujo, Mago, Paladín
--   ocultos (Druida, Guerrero, Cazador, Sacerdote, Pícaro):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96507, 96511, 96518, 96524, 96525);

-- Valle de Ammen: quedan visibles Druida, Pícaro, Brujo
--   ocultos (Cazador, Sacerdote, Mago, Paladín, Chamán, Guerrero):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96367, 96391, 96394, 96395, 96396, 96397);

-- Vigilia Azur: quedan visibles Druida, Pícaro, Brujo
--   ocultos (Sacerdote, Cazador, Chamán, Paladín, Guerrero, Mago):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96323, 96325, 96328, 96329, 96333, 96335);

-- Valle de Pruebas: quedan visibles Druida, Paladín
--   ocultos (Chamán, Brujo, Guerrero, Sacerdote, Mago, Pícaro, Cazador):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95808, 95809, 95811, 95814, 95815, 95816, 95819);

-- Colina Navaja: quedan visibles Mago, Paladín, Druida
--   ocultos (Sacerdote, Cazador, Guerrero, Pícaro, Brujo, Chamán):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95706, 95749, 95755, 95756, 95757, 95758);

-- Deathknell: quedan visibles Druida, Chamán, Cazador, Paladín
--   ocultos (Sacerdote, Pícaro, Brujo, Guerrero, Mago):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95060, 95067, 95068, 95071, 95073);

-- Brill: quedan visibles Chamán, Cazador, Druida, Paladín
--   ocultos (Pícaro, Mago, Sacerdote, Brujo, Guerrero):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95404, 95406, 95409, 95412, 95417);

-- Meseta Nube Roja: quedan visibles Mago, Sacerdote, Pícaro, Paladín, Brujo
--   ocultos (Cazador, Druida, Guerrero, Chamán):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95607, 95608, 95611, 95620);

-- Pezuña de Sangre: quedan visibles Pícaro, Mago, Brujo, Sacerdote, Paladín
--   ocultos (Guerrero, Chamán, Druida, Cazador):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95528, 95551, 95554, 95562);

-- Isla Sunstrider: quedan visibles Guerrero, Druida, Chamán
--   ocultos (Cazador, Sacerdote, Paladín, Mago, Brujo, Pícaro):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95329, 95330, 95336, 95340, 95370, 95371);

-- Plaza Alcón: quedan visibles Guerrero, Chamán, Druida
--   ocultos (Cazador, Sacerdote, Paladín, Pícaro, Brujo, Mago):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95273, 95274, 95280, 95281, 95284, 95288);

-- Ventormenta: quedan visibles (ninguno: ya están las nueve clases)
--   ocultos (Sacerdote, Druida, Guerrero, Chamán, Paladín, Mago, Pícaro, Brujo, Cazador):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96041, 96044, 96045, 96046, 96061, 96064, 96067, 96071, 96076);

-- Forjaz: quedan visibles Druida
--   ocultos (Pícaro, Chamán, Brujo, Guerrero, Paladín, Sacerdote, Mago, Cazador):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95896, 95897, 95898, 95899, 95902, 95903, 95920, 95921);

-- Darnassus: quedan visibles Chamán, Brujo
--   ocultos (Guerrero, Cazador, Mago, Sacerdote, Druida, Paladín, Pícaro):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96432, 96433, 96434, 96435, 96437, 96441, 96442);

-- Exodar: quedan visibles Pícaro, Brujo
--   ocultos (Mago, Cazador, Paladín, Druida, Chamán, Sacerdote, Guerrero):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (96226, 96228, 96229, 96236, 96237, 96240, 96257);

-- Orgrimmar: quedan visibles Druida
--   ocultos (Cazador, Mago, Paladín, Guerrero, Sacerdote, Pícaro, Brujo, Chamán):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95675, 95676, 95678, 95687, 95688, 95689, 95690, 95691);

-- Entrañas: quedan visibles Cazador, Chamán, Druida
--   ocultos (Mago, Sacerdote, Guerrero, Pícaro, Brujo, Paladín):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95122, 95125, 95127, 95129, 95130, 95131);

-- Cima del Trueno: quedan visibles Pícaro, Brujo, Paladín
--   ocultos (Cazador, Guerrero, Mago, Chamán, Druida, Sacerdote):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95476, 95489, 95493, 95495, 95496, 95497);

-- Lunargenta: quedan visibles Guerrero, Chamán
--   ocultos (Brujo, Pícaro, Druida, Cazador, Paladín, Sacerdote, Mago):
INSERT INTO `game_event_creature` (`guid`, `eventEntry`)
    SELECT `guid`, 31 FROM `creature` WHERE `guid` IN (95206, 95213, 95226, 95228, 95234, 95236, 95243);
