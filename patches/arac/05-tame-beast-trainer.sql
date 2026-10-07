-- =============================================================================
--  ARAC 05 — Domar Bestia por el instructor genérico de cazador, no por flag
--
--  Origen: fase 3.4 del plan. Domar bestia (hechizo 1515) no lo enseña ningún
--  instructor, ni el genérico ni los normales (comprobado en `trainer_spell`
--  real: ningún cazador, de ninguna raza, lo tenía). El primer intento de
--  arreglarlo fue encender `PlayerStart.CustomSpells`, que aplica entera la
--  tabla `playercreateinfo_spell_custom` de mod-arac al crear personaje —
--  pero esa tabla no son sólo los hechizos iniciales que faltan: trae TODO
--  el árbol de habilidades de cada clase sin distinción de nivel (p. ej.
--  Sello de Venganza, un hechizo de nivel 60 de paladín, está en la fila de
--  Enano/Paladín). Con el flag encendido, cualquier personaje nuevo
--  aprendía de golpe habilidades muy por encima de su nivel, rompiendo la
--  progresión. Revertido en 05_configure_server.sh.
--
--  LA SOLUCIÓN CORRECTA (la propuesta original del plan, antes del desvío):
--  dar Domar Bestia como hechizo ENTRENABLE en el instructor genérico de
--  cazador a partir de nivel 10, igual que cualquier otra habilidad de
--  cazador. El instalador ya expone ese NPC para todas las combinaciones
--  raza/clase (ver 04-generic-class-trainers.sql: "el core sólo comprueba
--  la CLASE del jugador, nunca la raza"), así que un cazador humano y
--  cualquier cazador de raza nativa (a los que tampoco se lo enseña nadie)
--  quedan cubiertos con esta única fila, sin tocar la configuración global.
--
--  TrainerId 7 = "Hunter Trainer" (creature_template 26325, el genérico),
--  vía creature_default_trainer. Gratis (no es una compra real de Blizzard,
--  es sustituir el hechizo inicial que a este cazador nunca le llegó) y sin
--  requisito de habilidad, igual que el resto de hechizos base de nivel 10
--  de este instructor.
--
--  Idempotente: INSERT IGNORE por la clave primaria (TrainerId, SpellId).
-- =============================================================================

INSERT IGNORE INTO `trainer_spell`
    (`TrainerId`, `SpellId`, `MoneyCost`, `ReqSkillLine`, `ReqSkillRank`,
     `ReqAbility1`, `ReqAbility2`, `ReqAbility3`, `ReqLevel`, `VerifiedBuild`)
VALUES
    (7, 1515, 0, 0, 0, 0, 0, 0, 10, 0);
