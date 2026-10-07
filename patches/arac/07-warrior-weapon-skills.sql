-- =============================================================================
--  ARAC 07 — Guerrero: competencias de arma que faltan en
--  `playercreateinfo_skills`
--
--  Hallazgo del 20/09/2026 (E1f, ver CHANGELOG), mismo patrón que
--  06-hunter-weapon-skills.sql: Guerreror (Elfo de la Noche de prueba,
--  cuenta ADMIN) no podía equiparse una espada a dos manos. Con
--  `character_skills` ya tenía Espadas (43), Mazas (54), Mazas a dos manos
--  (160), Manos desnudas (162) y Dagas (173), pero le faltaban Hachas (44),
--  Espadas a dos manos (55), Hachas a dos manos (172), Lanzas (229) y
--  Contundentes de puño (473) — armas que un Guerrero SÍ puede usar de serie
--  (acceso a casi cualquier arma salvo varitas). Rastreado igual que el
--  Cazador: `playercreateinfo_skills` tenía esas filas con `raceMask`
--  parcial (Hachas 44 = solo Draenei/Troll/Tauren/Undead/Enano/Orco/Humano,
--  deja fuera a Elfo de la Noche y demás; Espadas a dos manos 55 = solo
--  Draenei/Undead; Hachas a dos manos 172 = solo Orco/Enano) o sin ninguna
--  fila (Lanzas 229, Contundentes de puño 473 no tenían NINGUNA fila para
--  Guerrero). `skillraceclassinfo_dbc` (la que sí lee el core para
--  elegibilidad) ya cubre Guerrero con `RaceMask` "todas las razas" para
--  las cinco, igual que se confirmó para Cazador.
--
--  Qué hace: añade las cinco competencias con `raceMask=0` (todas las
--  razas), mismo patrón que Mazas a dos manos (160) del Guerrero en esta
--  misma tabla, que ya venía así. No se toca Bastones (136): un Guerrero
--  clásico no los usa.
--
--  Solo Guerrero (classMask=1). Igual que en 06, no se audita aquí el resto
--  de clases.
--
--  Idempotente: INSERT ... ON DUPLICATE KEY UPDATE.
-- =============================================================================

INSERT INTO `playercreateinfo_skills` (`raceMask`,`classMask`,`skill`,`rank`,`comment`) VALUES
  (0, 1, 44,  0, 'Warrior - Axes (todas las razas, E1f 20/09/2026)'),
  (0, 1, 55,  0, 'Warrior - Two-Handed Swords (E1f 20/09/2026)'),
  (0, 1, 172, 0, 'Warrior - Two-Handed Axes (E1f 20/09/2026)'),
  (0, 1, 229, 0, 'Warrior - Polearms (E1f 20/09/2026)'),
  (0, 1, 473, 0, 'Warrior - Fist Weapons (E1f 20/09/2026)')
ON DUPLICATE KEY UPDATE `comment` = VALUES(`comment`);
