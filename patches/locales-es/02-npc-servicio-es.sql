-- =============================================================================
--  Nombres en espanol de los NPC que anaden los modulos
--
--  El cliente pide los textos en su idioma y el servidor los saca de las tablas
--  *_locale. Las criaturas del juego base ya vienen traducidas en el volcado del
--  core; las que anaden los modulos, no: se ven en ingles aunque juegues con el
--  cliente en espanol.
--
--  Se traduce solo el SUBTITULO (Title), que es la etiqueta que explica para que
--  sirve el NPC. El nombre propio se deja tal cual, igual que hace Blizzard con
--  los nombres propios.
--
--  Idempotente. Las filas de entradas de modulos que no tengas instalados
--  simplemente no las vera nadie: no molestan ni dan error.
-- =============================================================================

DELETE FROM `creature_template_locale`
 WHERE `entry` IN (190010, 290011, 98888, 999991, 500000)
   AND `locale` IN ('esES','esMX');

INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`) VALUES
-- mod-transmog
(190010, 'esES', 'Warpweaver', 'Transmogrificador'),
(190010, 'esMX', 'Warpweaver', 'Transmogrificador'),
-- mod-reagent-bank
(290011, 'esES', 'Ling',       'Banquero de materiales'),
(290011, 'esMX', 'Ling',       'Banquero de materiales'),
-- mod-racial-trait-swap
(98888,  'esES', 'Swirl',      'Cambio de rasgos raciales'),
(98888,  'esMX', 'Swirl',      'Cambio de rasgos raciales'),
-- mod-1v1-arena
(999991, 'esES', 'Maestro de batalla', 'Arena 1c1'),
(999991, 'esMX', 'Maestro de batalla', 'Arena 1c1'),
-- mod-dungeon-master
(500000, 'esES', 'Dungeon Master', 'Mazmorras aleatorias'),
(500000, 'esMX', 'Dungeon Master', 'Mazmorras aleatorias');
