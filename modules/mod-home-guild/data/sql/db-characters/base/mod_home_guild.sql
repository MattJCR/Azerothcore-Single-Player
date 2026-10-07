-- Hermandades creadas manualmente por un jugador mientras mod-home-guild está
-- activo. La marca es la única fuente de propiedad: el módulo no deduce que una
-- guild es suya por el nombre ni por tener un líder humano.
CREATE TABLE IF NOT EXISTS `mod_home_guild` (
    `guild_id` INT UNSIGNED NOT NULL,
    `owner_account` INT UNSIGNED NOT NULL,
    `owner_guid` INT UNSIGNED NOT NULL,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`guild_id`),
    KEY `idx_mod_home_guild_owner` (`owner_account`, `owner_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Roster propio de los bots de una hermandad de casa: rol (cuando se conocen,
-- al conectarse el bot), y las marcas de "fijado" (KeepOnline lo conecta
-- primero, se muestra primero) y "excluido" (Care lo expulsa y Recruit no lo
-- vuelve a meter). Sobrevive a un wipe de guild_member.
CREATE TABLE IF NOT EXISTS `mod_home_guild_member` (
    `guild_id` INT UNSIGNED NOT NULL,
    `bot_guid` INT UNSIGNED NOT NULL,
    `role` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 desconocido, 1 tanque, 2 sanador, 3 dano',
    `pinned` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `excluded` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`guild_id`, `bot_guid`),
    KEY `idx_mod_home_guild_member_guild` (`guild_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Migraciones de datos que deben ejecutarse una sola vez. legacy_cleanup_v2
-- se escribe después de disolver mediante Guild::Disband las guilds que creó
-- automáticamente la versión anterior.
CREATE TABLE IF NOT EXISTS `mod_home_guild_meta` (
    `meta_key` VARCHAR(64) NOT NULL,
    `meta_value` VARCHAR(255) NOT NULL,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`meta_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
