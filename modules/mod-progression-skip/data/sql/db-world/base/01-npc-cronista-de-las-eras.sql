-- =============================================================================
--  mod-progression-skip — "Cronista de las Eras" (plantilla 600200)
--
--  Base: acore_world. Idempotente (borra sus filas antes de insertarlas).
--  Lo aplican la fase 5 y la fase 8 del instalador con apply_sql_dir; el
--  actualizador del core también lo recoge al arrancar. A mano:
--      mysql -u acore -pacore acore_world < 01-npc-cronista-de-las-eras.sql
--  Necesita reinicio del worldserver: creature_template no se recarga en caliente.
--
--  QUÉ ES
--  Un NPC neutral (facción 35, sólo conversación) que deja AVANZAR de forma
--  irreversible en mod-individual-progression. El comportamiento está en el
--  CreatureScript 'npc_progression_skip' (src/mod_progression_skip.cpp); aquí
--  sólo va el NPC, sus textos y sus cuatro apariciones.
--
--  RANGOS PROPIOS (ver patches/README.md → "Rangos de identificadores propios")
--    creature_template.entry     600200
--    creature.guid               8000110 – 8000113
--    npc_text.ID                 600200 (saludo) · 600201 (confirmación) · 600202 (sin etapas)
--
--  UBICACIONES (revisadas en el cliente el 10/09/2026). Cada spawn se ancla a un
--  NPC existente que ya está sobre suelo válido, así que la z va fija:
--    8000110  Gadgetzan (Tanaris)         — girado 180° respecto al sitio inicial
--    8000111  Bahía del Botín             — a la derecha de Corsario Bloads (Privateer Bloads, 2494)
--    8000112  Shattrath, Terraza de la Luz — a la derecha del General Tiras'alan (25167)
--    8000113  Dalaran                     — enfrente de Aquanos (36851), mirándolo
--  Para volver a revisarlos, con un GM:  .go creature 8000110 … 8000113
-- =============================================================================

SET @ENTRY   := 600200;
SET @MODEL   := 22596;   -- Priestess Delrissa: elfa de sangre, sacerdotisa (Terraza de los Magos)
SET @SCRIPT  := 'npc_progression_skip';

-- ── Limpieza ────────────────────────────────────────────────────────────────
DELETE FROM `creature`                WHERE `guid` BETWEEN 8000110 AND 8000113;
DELETE FROM `creature`                WHERE `id` = @ENTRY;
DELETE FROM `creature_template`       WHERE `entry` = @ENTRY;
DELETE FROM `creature_template_model` WHERE `CreatureID` = @ENTRY;
DELETE FROM `creature_template_locale` WHERE `entry` = @ENTRY;
DELETE FROM `npc_text`                WHERE `ID` IN (600200, 600201, 600202);
DELETE FROM `npc_text_locale`         WHERE `ID` IN (600200, 600201, 600202);

-- ── Plantilla ───────────────────────────────────────────────────────────────
--  faction 35  = amistosa con Alianza y Horda (NEUTRAL, requisito explícito)
--  npcflag 1   = sólo conversación: ni vendedor, ni instructor, ni misiones
--  unit_flags 768 = inmune a jugador (256) + inmune a NPC (512): no se puede atacar
--  type 7      = humanoide · unit_class 1 = guerrero (irrelevante, no combate)
INSERT INTO `creature_template`
  (`entry`, `difficulty_entry_1`, `difficulty_entry_2`, `difficulty_entry_3`, `KillCredit1`, `KillCredit2`,
   `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`,
   `speed_walk`, `speed_run`, `speed_swim`, `speed_flight`, `detection_range`, `rank`, `dmgschool`,
   `DamageModifier`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`,
   `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `type`, `type_flags`, `lootid`, `pickpocketloot`,
   `skinloot`, `PetSpellDataId`, `VehicleId`, `mingold`, `maxgold`, `AIName`, `MovementType`, `HoverHeight`,
   `HealthModifier`, `ManaModifier`, `ArmorModifier`, `ExperienceModifier`, `RacialLeader`, `movementId`,
   `RegenHealth`, `CreatureImmunitiesId`, `flags_extra`, `ScriptName`, `VerifiedBuild`)
VALUES
  (@ENTRY, 0, 0, 0, 0, 0,
   'Chronicler of the Ages', 'Progression skip', NULL, 0, 80, 80, 2, 35, 1,
   1, 1.14286, 1, 1, 20, 0, 0,
   1, 2000, 2000, 1, 1, 1,
   768, 0, 0, 0, 7, 0, 0, 0,
   0, 0, 0, 0, 0, '', 0, 1,
   1, 1, 1, 1, 0, 0,
   1, 0, 0, @SCRIPT, 0);

INSERT INTO `creature_template_model`
  (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
VALUES
  (@ENTRY, 0, @MODEL, 1, 1, 0);

-- ── Nombre/subtítulo en español para clientes esES/esMX (la tabla base va en inglés) ──
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`) VALUES
  (@ENTRY, 'esES', 'Cronista de las Eras', 'Salto de progresión'),
  (@ENTRY, 'esMX', 'Cronista de las Eras', 'Salto de progresión');

-- ── Textos del NPC ─────────────────────────────────────────────────────────
--  La tabla base va en inglés (es lo que ve cualquier cliente que no sea
--  esES/esMX, igual que el resto de textos del core, que son enUS) y el español
--  va en npc_text_locale para esES y esMX.
INSERT INTO `npc_text` (`ID`, `text0_0`, `text0_1`, `lang0`, `Probability0`) VALUES
  (600200,
   'Time is not a river, traveller: it is a library. I can close for you the chapters you do not wish to read. Choose, and it will be written forever on this character.',
   'Time is not a river, traveller: it is a library. I can close for you the chapters you do not wish to read. Choose, and it will be written forever on this character.',
   0, 1),
  (600201,
   'This decision is permanent for this character. You will not receive the rewards or achievements of the skipped stages. Do you wish to continue?',
   'This decision is permanent for this character. You will not receive the rewards or achievements of the skipped stages. Do you wish to continue?',
   0, 1),
  (600202,
   'You have no era left to advance. Your story is already up to date.',
   'You have no era left to advance. Your story is already up to date.',
   0, 1);

INSERT INTO `npc_text_locale` (`ID`, `Locale`, `Text0_0`, `Text0_1`) VALUES
  (600200, 'esES', 'El tiempo no es un río, viajero: es una biblioteca. Puedo cerrar por ti los capítulos que no quieras leer. Elige, y quedará escrito para siempre en este personaje.', 'El tiempo no es un río, viajero: es una biblioteca. Puedo cerrar por ti los capítulos que no quieras leer. Elige, y quedará escrito para siempre en este personaje.'),
  (600200, 'esMX', 'El tiempo no es un río, viajero: es una biblioteca. Puedo cerrar por ti los capítulos que no quieras leer. Elige, y quedará escrito para siempre en este personaje.', 'El tiempo no es un río, viajero: es una biblioteca. Puedo cerrar por ti los capítulos que no quieras leer. Elige, y quedará escrito para siempre en este personaje.'),
  (600201, 'esES', 'Esta decisión es permanente para este personaje. No recibirás las recompensas ni los logros de las etapas omitidas. ¿Deseas continuar?', 'Esta decisión es permanente para este personaje. No recibirás las recompensas ni los logros de las etapas omitidas. ¿Deseas continuar?'),
  (600201, 'esMX', 'Esta decisión es permanente para este personaje. No recibirás las recompensas ni los logros de las etapas omitidas. ¿Deseas continuar?', 'Esta decisión es permanente para este personaje. No recibirás las recompensas ni los logros de las etapas omitidas. ¿Deseas continuar?'),
  (600202, 'esES', 'No te queda ninguna era por adelantar. Tu historia ya está al día.', 'No te queda ninguna era por adelantar. Tu historia ya está al día.'),
  (600202, 'esMX', 'No te queda ninguna era por adelantar. Tu historia ya está al día.', 'No te queda ninguna era por adelantar. Tu historia ya está al día.');

-- ── Cuatro apariciones ─────────────────────────────────────────────────────
--  Vanilla: DOS spawns neutrales (Gadgetzan y Bahía del Botín), sin copias por
--  facción. TBC: Shattrath. WotLK: Dalaran.
--  Posiciones fijadas el 10/09/2026 respecto a NPCs de referencia (todos sobre
--  suelo válido), así que la z va explícita y no hace falta autoajuste:
--    8000110  Gadgetzan  — mismo punto que la primera colocación, girado 180°
--    8000111  Bahía del Botín — 2,5 yd a la derecha de Privateer Bloads (2494), su misma orientación
--    8000112  Shattrath  — 2,5 yd a la derecha de General Tiras'alan (25167), su misma orientación
--    8000113  Dalaran    — 3,5 yd enfrente de Aquanos (36851), orientado hacia él
--  npcflag/unit_flags/ScriptName a 0/'' → heredan de la plantilla.
INSERT INTO `creature`
  (`guid`, `id`, `map`, `zoneId`, `areaId`, `spawnMask`, `phaseMask`, `equipment_id`,
   `position_x`, `position_y`, `position_z`, `orientation`,
   `spawntimesecs`, `wander_distance`, `currentwaypoint`, `curhealth`, `curmana`,
   `MovementType`, `npcflag`, `unit_flags`, `dynamicflags`, `ScriptName`, `VerifiedBuild`, `Comment`)
VALUES
  (8000110, @ENTRY, 1,    0, 0, 1, 1, 0,  -7156.000, -3767.000,    9.360, 1.2584, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0,
   'mod-progression-skip: Gadgetzan (Tanaris)'),
  (8000111, @ENTRY, 0,    0, 0, 1, 1, 0, -14420.578,   512.690,    5.082, 5.0265, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0,
   'mod-progression-skip: Bahia del Botin (a la derecha de Privateer Bloads)'),
  (8000112, @ENTRY, 530,  0, 0, 1, 1, 0,  -1855.454,  5437.108,  -10.364, 3.7525, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0,
   'mod-progression-skip: Shattrath (a la derecha de General Tiras alan)'),
  (8000113, @ENTRY, 571,  0, 0, 1, 1, 0,   5800.215,   695.254,  658.352, 3.5256, 300, 0, 0, 1, 0, 0, 0, 0, 0, '', 0,
   'mod-progression-skip: Dalaran (enfrente de Aquanos)');
