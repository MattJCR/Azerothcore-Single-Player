-- =============================================================================
--  ARAC 03 — Elimina razas inexistentes de las tablas de creación de personaje
--
--  En WotLK 3.3.5a las razas jugables son 1-8, 10 y 11 (la 9, Goblin, existe en
--  los DBC pero no es jugable). Cualquier fila con otra raza hace que el core
--  escupa en cada arranque:
--
--      Wrong race NN in `playercreateinfo` table, ignoring.
--
--  Esto pasa, por ejemplo, si se ejecuta por error la plantilla
--  "data/sql/db-world/(Optional)/Copy for Custom Race.sql" que trae mod-arac:
--  no es un script instalable, es un EJEMPLO de cómo clonar la raza 1 hacia una
--  raza 12 ("Fel'Orc") para servidores con razas personalizadas. Si se lanza tal
--  cual deja filas basura de raza 12 y luego falla a medias.
--
--  El instalador ya excluye las carpetas "(Optional)" al aplicar SQL
--  (ver apply_sql_dir en lib/utils.sh); esto es la red de seguridad para bases
--  de datos que ya vengan contaminadas.
--
--  Las tablas se comprueban en information_schema antes de tocarlas: los
--  nombres cambian entre versiones de AzerothCore (player_levelstats pasó a ser
--  player_race_stats) y una tabla ausente abortaría el script entero, dejando
--  las demás sin limpiar.
--
--  Si de verdad quieres una raza personalizada, saca este fichero de
--  patches/arac/ o el instalador te la borrará en cada pasada.
--
--  Idempotente: si no hay nada que borrar, no hace nada.
-- =============================================================================

-- playercreateinfo (posición y zona de inicio)
SET @sql := (SELECT IF(COUNT(*) > 0,
    'DELETE FROM `playercreateinfo` WHERE `race` NOT IN (1,2,3,4,5,6,7,8,10,11)',
    'DO 0')
  FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'playercreateinfo');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

-- playercreateinfo_action (barra de acción inicial)
SET @sql := (SELECT IF(COUNT(*) > 0,
    'DELETE FROM `playercreateinfo_action` WHERE `race` NOT IN (1,2,3,4,5,6,7,8,10,11)',
    'DO 0')
  FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'playercreateinfo_action');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

-- playercreateinfo_item (equipo inicial)
SET @sql := (SELECT IF(COUNT(*) > 0,
    'DELETE FROM `playercreateinfo_item` WHERE `race` NOT IN (1,2,3,4,5,6,7,8,10,11)',
    'DO 0')
  FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'playercreateinfo_item');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

-- player_race_stats (estadísticas base por raza; en versiones antiguas del
-- core esta tabla se llamaba player_levelstats)
SET @sql := (SELECT IF(COUNT(*) > 0,
    'DELETE FROM `player_race_stats` WHERE `race` NOT IN (1,2,3,4,5,6,7,8,10,11)',
    'DO 0')
  FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'player_race_stats');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;

SET @sql := (SELECT IF(COUNT(*) > 0,
    'DELETE FROM `player_levelstats` WHERE `race` NOT IN (1,2,3,4,5,6,7,8,10,11)',
    'DO 0')
  FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'player_levelstats');
PREPARE st FROM @sql; EXECUTE st; DEALLOCATE PREPARE st;
