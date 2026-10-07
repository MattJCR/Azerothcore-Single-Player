-- mod-bot-operations: instantanea agregada y cola de acciones del panel web
-- "Operaciones de bots" (acore_world). Ver la cabecera de
-- src/mod_bot_operations.cpp para el porque.

-- Instantanea unica (id siempre 1: no es un historico, es "el estado ahora
-- mismo"). Cada seccion es un objeto JSON que construye mod_bot_operations.cpp
-- a partir de modules/shared/BotOperations.h, BotPopulationCoordinator.h y
-- BotClaims.h; el panel solo los lee y los pinta, nunca los interpreta como
-- comandos.
CREATE TABLE IF NOT EXISTS `bot_operations_snapshot` (
  `id` tinyint unsigned NOT NULL,
  `reservations` JSON NOT NULL,
  `world_stage` JSON NOT NULL,
  `world_pvp` JSON NOT NULL,
  `queues` JSON NOT NULL,
  `groups` JSON NOT NULL,
  `quest_mates` JSON NOT NULL,
  `guilds` JSON NOT NULL,
  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Cola de acciones. El panel inserta una fila 'pending' (GM3, con CSRF y
-- auditoria propios del panel); mod_bot_operations.cpp la encola contra el
-- modulo destino via BotOperations.h y actualiza status/result/completed_at
-- cuando ese modulo responde. `action` es uno de los nombres de
-- BotOperations::ActionName() (world_bots_pass, stop_world_pvp,
-- queue_bots_pass, party_here_pass, home_guild_pass) o 'refresh_snapshot',
-- que resuelve el propio modulo sin pasar por ningun otro.
CREATE TABLE IF NOT EXISTS `bot_operations_action` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `action` varchar(32) NOT NULL,
  `param` varchar(64) NOT NULL DEFAULT '',
  `reason` varchar(255) NOT NULL DEFAULT '',
  `actor_account` int unsigned DEFAULT NULL,
  `actor_name` varchar(32) DEFAULT NULL,
  `status` varchar(16) NOT NULL DEFAULT 'pending',
  `result` varchar(255) DEFAULT NULL,
  `requested_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `completed_at` TIMESTAMP NULL DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_bot_operations_action_status` (`status`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- el panel comprobaba con un SELECT si ya había una fila
-- 'pending' de la misma acción+param y luego insertaba sin exclusión mutua —
-- dos peticiones simultáneas podían duplicar el trabajo. `pending_key` vale
-- NULL salvo en status='pending' (MySQL/MariaDB permiten varios NULL en una
-- clave ÚNICA), así que sólo puede existir una fila 'pending' por
-- acción+param — el propio INSERT pasa a ser la comprobación atómica (ver
-- ER_DUP_ENTRY en app.js). Mismo patrón de sentencia preparada que
-- mod-server-help/data/sql/db-world/base/server_help.sql, idempotente en
-- MySQL 8 y MariaDB.
SET @c := (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
           AND TABLE_NAME = 'bot_operations_action' AND COLUMN_NAME = 'pending_key');
SET @s := IF(@c = 0,
  'ALTER TABLE `bot_operations_action` ADD COLUMN `pending_key` VARCHAR(97) GENERATED ALWAYS AS (IF(`status` = ''pending'', CONCAT(`action`, ''|'', `param`), NULL)) VIRTUAL AFTER `status`, ADD UNIQUE KEY `uniq_bot_operations_action_pending` (`pending_key`)',
  'SELECT 1');
PREPARE m FROM @s; EXECUTE m; DEALLOCATE PREPARE m;
