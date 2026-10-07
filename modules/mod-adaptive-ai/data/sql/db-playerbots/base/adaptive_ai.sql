-- mod-adaptive-ai: tablas en acore_playerbots. Idempotente.
--
--   adaptive_model       versiones de la tabla aprendida (validada / candidata)
--   adaptive_q           la tabla: valor por (modelo, clase, clase enemiga, estado, accion)
--   adaptive_bot         perfil por bot: rating Elo, dificultad, personalidad, XP
--   adaptive_match       historial de partidas (se puede vaciar sin perder nada mas)
--   adaptive_match_bot   cada bot de cada partida (equipos y campos de batalla)
--   adaptive_experience  decisiones individuales (solo con AdaptiveAI.Log.Decisiones = 1)

CREATE TABLE IF NOT EXISTS `adaptive_model` (
  `version` INT UNSIGNED NOT NULL,
  `creado` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `validada` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `tasa_victoria` FLOAT NOT NULL DEFAULT 0,
  `combates` INT UNSIGNED NOT NULL DEFAULT 0,
  `nota` VARCHAR(4096) NOT NULL DEFAULT '',
  `generacion` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`version`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Aprobado por clase y ronda de entrenamiento (la fila clase = 0 lleva la ronda)
CREATE TABLE IF NOT EXISTS `adaptive_clase` (
  `clase` TINYINT UNSIGNED NOT NULL,
  `aprobada` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `ronda` INT UNSIGNED NOT NULL DEFAULT 1,
  `version` INT UNSIGNED NOT NULL DEFAULT 0,
  `fecha` DATETIME NULL,
  `cand_gana` INT UNSIGNED NOT NULL DEFAULT 0,
  `cand_pierde` INT UNSIGNED NOT NULL DEFAULT 0,
  `val_gana` INT UNSIGNED NOT NULL DEFAULT 0,
  `val_pierde` INT UNSIGNED NOT NULL DEFAULT 0,
  `ciclos` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`clase`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `adaptive_q` (
  `modelo` INT UNSIGNED NOT NULL,
  `clase` TINYINT UNSIGNED NOT NULL,
  `clase_enemiga` TINYINT UNSIGNED NOT NULL,
  `estado` SMALLINT UNSIGNED NOT NULL,
  `accion` TINYINT UNSIGNED NOT NULL,
  `q` FLOAT NOT NULL DEFAULT 0,
  `visitas` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`modelo`, `clase`, `clase_enemiga`, `estado`, `accion`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- La escalera medida: que peldano (version) es mas fuerte en cada clase. Se
-- recalcula sola de adaptive_match, pero se guarda para que sobreviva a un
-- reinicio y, sobre todo, para que viaje en la exportacion: sin esto una
-- instalacion limpia recibe las generaciones pero no sabe cual es mejor, y
-- juega de serie hasta volver a medirlas.
CREATE TABLE IF NOT EXISTS `adaptive_escalera` (
  `clase` TINYINT UNSIGNED NOT NULL,
  `version` INT UNSIGNED NOT NULL,
  `partidas` INT UNSIGNED NOT NULL DEFAULT 0,
  `victorias` FLOAT NOT NULL DEFAULT 0.5,
  `rating` FLOAT NOT NULL DEFAULT 1500,
  `actualizado` DATETIME NULL,
  PRIMARY KEY (`clase`, `version`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `adaptive_bot` (
  `guid` INT UNSIGNED NOT NULL,
  `nombre` VARCHAR(12) NOT NULL DEFAULT '',
  `clase` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `spec` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `rating` FLOAT NOT NULL DEFAULT 1500,
  `victorias` INT UNSIGNED NOT NULL DEFAULT 0,
  `derrotas` INT UNSIGNED NOT NULL DEFAULT 0,
  `empates` INT UNSIGNED NOT NULL DEFAULT 0,
  `dificultad` TINYINT UNSIGNED NOT NULL DEFAULT 3,
  `agresividad` FLOAT NOT NULL DEFAULT 0.5,
  `riesgo` FLOAT NOT NULL DEFAULT 0.5,
  `defensa` FLOAT NOT NULL DEFAULT 0.5,
  `prioridad` FLOAT NOT NULL DEFAULT 0.5,
  `movilidad` FLOAT NOT NULL DEFAULT 0.5,
  `xp` INT UNSIGNED NOT NULL DEFAULT 0,
  `modelo` INT UNSIGNED NOT NULL DEFAULT 0,
  `spec_pve` TINYINT NOT NULL DEFAULT -1 COMMENT 'indice de spec premade de playerbots (spec 0)',
  `spec_pvp` TINYINT NOT NULL DEFAULT -1 COMMENT 'idem, spec 1',
  `proposito` VARCHAR(4) NOT NULL DEFAULT 'pve',
  `ilvl` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `actualizado` DATETIME NULL,
  PRIMARY KEY (`guid`),
  KEY `nombre` (`nombre`),
  KEY `rating` (`rating`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `adaptive_match` (
  `id` INT UNSIGNED NOT NULL,
  `fecha` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `fuente` VARCHAR(16) NOT NULL DEFAULT 'auto',
  `mapa` INT UNSIGNED NOT NULL DEFAULT 0,
  `tipo` VARCHAR(8) NOT NULL DEFAULT '1c1',
  `modelo` INT UNSIGNED NOT NULL DEFAULT 0,
  `bot_a` INT UNSIGNED NOT NULL DEFAULT 0,
  `nombre_a` VARCHAR(12) NOT NULL DEFAULT '',
  `clase_a` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `spec_a` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `modo_a` VARCHAR(12) NOT NULL DEFAULT '',
  `rating_a` FLOAT NOT NULL DEFAULT 0,
  `bot_b` INT UNSIGNED NOT NULL DEFAULT 0,
  `nombre_b` VARCHAR(12) NOT NULL DEFAULT '',
  `clase_b` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `spec_b` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `modo_b` VARCHAR(12) NOT NULL DEFAULT '',
  `rating_b` FLOAT NOT NULL DEFAULT 0,
  `ganador` CHAR(1) NOT NULL DEFAULT 'x' COMMENT 'a, b, e (empate), x (abortada)',
  `duracion` INT UNSIGNED NOT NULL DEFAULT 0,
  `dec_a` INT UNSIGNED NOT NULL DEFAULT 0,
  `rec_a` FLOAT NOT NULL DEFAULT 0,
  `int_ok_a` INT UNSIGNED NOT NULL DEFAULT 0,
  `int_mal_a` INT UNSIGNED NOT NULL DEFAULT 0,
  `cc_a` INT UNSIGNED NOT NULL DEFAULT 0,
  `cc_suf_a` INT UNSIGNED NOT NULL DEFAULT 0,
  `dano_a` INT UNSIGNED NOT NULL DEFAULT 0,
  `dano_suf_a` INT UNSIGNED NOT NULL DEFAULT 0,
  `dec_b` INT UNSIGNED NOT NULL DEFAULT 0,
  `rec_b` FLOAT NOT NULL DEFAULT 0,
  `int_ok_b` INT UNSIGNED NOT NULL DEFAULT 0,
  `int_mal_b` INT UNSIGNED NOT NULL DEFAULT 0,
  `cc_b` INT UNSIGNED NOT NULL DEFAULT 0,
  `cc_suf_b` INT UNSIGNED NOT NULL DEFAULT 0,
  `dano_b` INT UNSIGNED NOT NULL DEFAULT 0,
  `dano_suf_b` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `fecha` (`fecha`),
  KEY `fuente` (`fuente`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Cada bot de cada partida (las de equipo y los campos tienen mas de uno por lado)
CREATE TABLE IF NOT EXISTS `adaptive_match_bot` (
  `partida` INT UNSIGNED NOT NULL,
  `lado` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `guid` INT UNSIGNED NOT NULL,
  `nombre` VARCHAR(12) NOT NULL DEFAULT '',
  `clase` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `spec` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `modo` VARCHAR(12) NOT NULL DEFAULT '',
  `rating` FLOAT NOT NULL DEFAULT 0,
  `decisiones` INT UNSIGNED NOT NULL DEFAULT 0,
  `recompensa` FLOAT NOT NULL DEFAULT 0,
  `int_ok` INT UNSIGNED NOT NULL DEFAULT 0,
  `int_mal` INT UNSIGNED NOT NULL DEFAULT 0,
  `cc` INT UNSIGNED NOT NULL DEFAULT 0,
  `cc_suf` INT UNSIGNED NOT NULL DEFAULT 0,
  `dano` INT UNSIGNED NOT NULL DEFAULT 0,
  `dano_suf` INT UNSIGNED NOT NULL DEFAULT 0,
  `objetivos` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`partida`, `guid`),
  KEY `guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `adaptive_experience` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `partida` INT UNSIGNED NOT NULL DEFAULT 0,
  `bot` INT UNSIGNED NOT NULL DEFAULT 0,
  `t` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'ms desde el inicio del combate',
  `estado` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `accion` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `q` FLOAT NOT NULL DEFAULT 0,
  `recompensa` FLOAT NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `partida` (`partida`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `adaptive_model` (`version`, `validada`, `nota`)
VALUES (1, 1, 'inicial: tabla vacia, equivale a playerbots de serie');

-- Migraciones sobre bases que ya existen (este fichero se re-aplica en cada
-- ./install.sh --only 5, así que todo lo de aquí tiene que ser idempotente).
--
-- 03/09/2026: `nota` era VARCHAR(1024) y el módulo le va pegando una línea por
-- calibración con CONCAT. Al llegar al tope, el UPDATE entero falla ("Data too
-- long for column 'nota'", errno 1406) y con él se pierden `tasa_victoria` y
-- `combates` de esa calibración. Se amplía a 4096 y el código recorta por la
-- izquierda a 4000 (RIGHT(CONCAT(...))), que es lo que de verdad lo cierra.
ALTER TABLE `adaptive_model` MODIFY COLUMN `nota` VARCHAR(4096) NOT NULL DEFAULT '';
