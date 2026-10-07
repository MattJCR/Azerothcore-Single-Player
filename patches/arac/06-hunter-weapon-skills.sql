-- =============================================================================
--  ARAC 06 — Cazador: competencias de arma cuerpo a cuerpo que faltan en
--  `playercreateinfo_skills`
--
--  Hallazgo del 20/09/2026 (E1f, ver CHANGELOG): Huntcore (Cazador Humano de
--  prueba, cuenta ADMIN) no podía equiparse un hacha a dos manos. Con 98
--  hechizos conocidos pero NINGUNA fila de competencia de arma cuerpo a
--  cuerpo en `character_skills`, se rastreó hasta `Player::LearnDefaultSkills`
--  (`PlayerInfo::skills`, cargado de `playercreateinfo_skills`): a esta tabla
--  le faltan las filas de Cazador para Espadas (43), Hachas (44, solo
--  parcial: raceMask 166 deja fuera a Humano/Elfo de la Noche/Gnomo/Draenei),
--  Espadas a dos manos (55), Bastones (136), Hachas a dos manos (172),
--  Lanzas (229) y Armas contundentes de puño (473) — todas armas que un
--  Cazador SÍ puede usar de serie (clásico: acceso a casi cualquier arma
--  salvo mazas/mazas a dos manos/escudos/varitas). Sin fila aquí,
--  `LearnDefaultSkill` nunca llama `SetSkill` para esas competencias, así que
--  el cliente bloquea el equipo del arma — no es un problema de la subida de
--  nivel por SOAP.
--
--  Confirmado que NO es un problema de elegibilidad: `skillraceclassinfo_dbc`
--  (que si lee el core, ver `01-fix-starting-gear-and-skills.sql`) YA tiene
--  fila de Cazador con RaceMask "todas las razas" para las siete
--  competencias — el hueco está solo en `playercreateinfo_skills`.
--
--  Qué hace: añade las cinco competencias que faltaban del todo con
--  `raceMask=0` (todas las razas, mismo patrón que Arcos/Armas de
--  fuego/Ballestas del Cazador en esta misma tabla) y ensancha Espadas/Hachas
--  de `raceMask` parcial a `raceMask=0` — no se borran las filas estrechas
--  originales, son redundantes pero inofensivas.
--
--  Solo Cazador (classMask=4). No se audita aquí el resto de clases/lanzas
--  para otras clases (Lanzas de Guerrero/Paladín/Druida, etc.): fuera de
--  alcance de este hallazgo, pendiente si aparece evidencia similar.
--
--  Idempotente: INSERT ... ON DUPLICATE KEY UPDATE.
-- =============================================================================

INSERT INTO `playercreateinfo_skills` (`raceMask`,`classMask`,`skill`,`rank`,`comment`) VALUES
  (0, 4, 43,  0, 'Hunter - Swords (todas las razas, E1f 20/09/2026)'),
  (0, 4, 44,  0, 'Hunter - Axes (todas las razas, E1f 20/09/2026)'),
  (0, 4, 55,  0, 'Hunter - Two-Handed Swords (E1f 20/09/2026)'),
  (0, 4, 136, 0, 'Hunter - Staves (E1f 20/09/2026)'),
  (0, 4, 172, 0, 'Hunter - Two-Handed Axes (E1f 20/09/2026)'),
  (0, 4, 229, 0, 'Hunter - Polearms (E1f 20/09/2026)'),
  (0, 4, 473, 0, 'Hunter - Fist Weapons (E1f 20/09/2026)')
ON DUPLICATE KEY UPDATE `comment` = VALUES(`comment`);
