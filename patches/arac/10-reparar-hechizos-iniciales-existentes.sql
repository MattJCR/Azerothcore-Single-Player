-- =============================================================================
--  ARAC 10 — Reponer raciales, idiomas, competencias y hechizos "generales"
--  a los personajes ya creados
--
--  Compañero de `09-hechizos-iniciales-podados-por-nivel.sql` (E1i, 22/09/2026,
--  ver CHANGELOG). El 09 arregla las creaciones FUTURAS podando
--  `playercreateinfo_spell_custom` y dejando que la fase 5 vuelva a poner
--  `PlayerStart.CustomSpells = 1`. Este 10 repara a los que ya existen, que
--  nacieron mientras el flag estaba a 0 y por eso no recibieron nada.
--
--  Concede exactamente lo mismo que recibiría hoy un personaje nuevo de su
--  misma raza y clase: las filas de `playercreateinfo_spell_custom` que
--  sobreviven a la poda del 09. Por eso este fichero va DESPUÉS: si se
--  ejecutara antes, repartiría también el árbol de clase entero.
--
--  Por qué no sirve `.reset spells` para esto: `Player::resetSpells()` llama a
--  `LearnDefaultSkills()`, que lleva `if (HasSkill(skillId)) continue;` — en un
--  personaje que ya tiene sus habilidades no vuelve a conceder nada. Y
--  `LearnCustomSpells()` no hacía nada con el flag a 0. Comprobado en vivo el
--  22/09/2026: el reset del druida de pruebas quitó los 110 hechizos y
--  devolvió cero.
--
--  ALCANCE: sólo personajes de cuentas que NO son de playerbots (11 de los
--  ~1700 de la BD). Los bots reciben sus hechizos de `PlayerbotFactory`, no
--  los necesitan, y dejarlos fuera reduce el cambio de ~68.000 filas a unas
--  pocas cientas.
--
--  SEGURIDAD: sólo toca personajes desconectados (`online = 0`). Un personaje
--  dentro del mundo tiene su lista de hechizos en memoria y la volcaría encima
--  al guardar, así que escribirle por SQL no sirve de nada. Si alguno estaba
--  conectado al aplicar esto, basta con relanzar el fichero después.
--
--  `specMask = 255` (todas las especializaciones), igual que cualquier hechizo
--  aprendido de forma normal. La primera versión usaba 1 y con doble
--  especialización esos hechizos habrían desaparecido en la segunda;
--  corregido el 23/09/2026 también en la BD (1502 filas quedan en 255).
--
--  Idempotente: `INSERT IGNORE` contra la clave primaria (`guid`,`spell`).
-- =============================================================================

INSERT IGNORE INTO `acore_characters`.`character_spell` (`guid`, `spell`, `specMask`)
SELECT c.`guid`, ps.`Spell`, 255
  FROM `acore_characters`.`characters` c
  JOIN `playercreateinfo_spell_custom` ps
    ON (ps.`racemask`  = 0 OR (ps.`racemask`  & (1 << (c.`race`  - 1))) <> 0)
   AND (ps.`classmask` = 0 OR (ps.`classmask` & (1 << (c.`class` - 1))) <> 0)
 WHERE c.`online` = 0
   AND c.`account` NOT IN (
        SELECT `id` FROM `acore_auth`.`account` WHERE `username` LIKE 'RNDBOT%'
   );
