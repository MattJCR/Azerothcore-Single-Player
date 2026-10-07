-- =============================================================================
--  mod-challenge-modes en espanol: el "Santuario del Desafio"
--
--  El modulo (commit 1930525) coloca 9 gameobjects entry 254605 -uno junto al
--  camposanto de cada zona inicial- pero:
--    * el nombre solo viene en ingles ("Shrine of Challenge"): no hay fila en
--      gameobject_template_locale, asi que se ve en ingles aunque juegues con el
--      cliente en espanol;
--    * al hablar con el santuario manda SendGossipMenuFor(..., 12669, ...), y el
--      volcado del core trae npc_text 12669 en ingles y sin fila esES/esMX.
--
--  Los textos de las OPCIONES del menu y el mensaje de confirmacion NO se tocan
--  aqui: van a fuego en el codigo (en chino) y los traduce el parche
--  patches/mod-challenge-modes/02-textos-es-y-visible-gm.patch.
--
--  Idempotente: borra sus filas antes de insertarlas. Relanzar la fase 5 o la 8
--  no duplica nada.
-- =============================================================================

-- --- Nombre del gameobject -----------------------------------------------------
DELETE FROM `gameobject_template_locale`
 WHERE `entry` = 254605 AND `locale` IN ('esES','esMX');

INSERT INTO `gameobject_template_locale`
    (`entry`, `locale`, `name`, `castBarCaption`, `VerifiedBuild`) VALUES
(254605, 'esES', 'Santuario del Desafío', '', 0),
(254605, 'esMX', 'Santuario del Desafío', '', 0);

-- --- Texto de saludo del santuario (npc_text 12669, "ancient idol") -----------
-- Original ingles: "You feel a strange presence as you stand before this
-- ancient idol." Ya tiene deDE/frFR/zhCN en el volcado del core, pero no esES.
DELETE FROM `npc_text_locale`
 WHERE `ID` = 12669 AND `Locale` IN ('esES','esMX');

INSERT INTO `npc_text_locale` (`ID`, `Locale`, `Text0_0`, `Text0_1`) VALUES
(12669, 'esES', 'Sientes una presencia extraña al plantarte ante este ídolo ancestral.', 'Sientes una presencia extraña al plantarte ante este ídolo ancestral.'),
(12669, 'esMX', 'Sientes una presencia extraña al plantarte ante este ídolo ancestral.', 'Sientes una presencia extraña al plantarte ante este ídolo ancestral.');
