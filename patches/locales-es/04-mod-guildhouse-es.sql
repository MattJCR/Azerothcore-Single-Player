-- SP02: nombres y textos visibles de mod-guildhouse en español.
-- Idempotente; se aplica después del SQL del módulo en las fases 5 y 8.
DELETE FROM `creature_template_locale`
 WHERE `entry` IN (500030, 500031, 500032)
   AND `locale` IN ('esES', 'esMX');
INSERT INTO `creature_template_locale` (`entry`, `locale`, `Name`, `Title`) VALUES
(500030, 'esES', 'Talamortis', 'Vendedor de sedes de hermandad'),
(500030, 'esMX', 'Talamortis', 'Vendedor de sedes de hermandad'),
(500031, 'esES', 'Xrispins', 'Mayordomo de la sede'),
(500031, 'esMX', 'Xrispins', 'Mayordomo de la sede'),
(500032, 'esES', 'Mónica', 'Posadera de la sede'),
(500032, 'esMX', 'Mónica', 'Posadera de la sede');

DELETE FROM `gameobject_template_locale`
 WHERE `entry` BETWEEN 500000 AND 500009
   AND `locale` IN ('esES', 'esMX');
INSERT INTO `gameobject_template_locale` (`entry`, `locale`, `name`) VALUES
(500000, 'esES', 'Portal a Ventormenta'), (500000, 'esMX', 'Portal a Ventormenta'),
(500001, 'esES', 'Portal a Darnassus'), (500001, 'esMX', 'Portal a Darnassus'),
(500002, 'esES', 'Portal al Exodar'), (500002, 'esMX', 'Portal al Exodar'),
(500003, 'esES', 'Portal a Forjaz'), (500003, 'esMX', 'Portal a Forjaz'),
(500004, 'esES', 'Portal a Orgrimmar'), (500004, 'esMX', 'Portal a Orgrimmar'),
(500005, 'esES', 'Portal a Lunargenta'), (500005, 'esMX', 'Portal a Lunargenta'),
(500006, 'esES', 'Portal a Cima del Trueno'), (500006, 'esMX', 'Portal a Cima del Trueno'),
(500007, 'esES', 'Portal a Entrañas'), (500007, 'esMX', 'Portal a Entrañas'),
(500008, 'esES', 'Portal a Shattrath'), (500008, 'esMX', 'Portal a Shattrath'),
(500009, 'esES', 'Portal a Dalaran'), (500009, 'esMX', 'Portal a Dalaran');

-- La tabla del módulo ya trae esES/esMX. Estas dos filas conservaban inglés.
DELETE FROM `mod_guildhouse_locale`
 WHERE `Id` IN (413, 414) AND `Locale` IN ('esES', 'esMX');
INSERT INTO `mod_guildhouse_locale` (`Id`, `Locale`, `Text`) VALUES
(413, 'esES', 'Isla de los MJ'), (413, 'esMX', 'Isla de los MJ'),
(414, 'esES', '¿Comprar una sede de hermandad en la Isla de los MJ?'),
(414, 'esMX', '¿Comprar una sede de hermandad en la Isla de los MJ?');

-- SP07: Ling, el banquero de materiales, como mejora del mayordomo.
DELETE FROM `mod_guildhouse_locale`
 WHERE `Id` IN (415, 416) AND `Locale` IN ('esES', 'esMX');
INSERT INTO `mod_guildhouse_locale` (`Id`, `Locale`, `Text`) VALUES
(415, 'esES', 'Invocar banquero de materiales'), (415, 'esMX', 'Invocar banquero de materiales'),
(416, 'esES', '¿Invocar banquero de materiales?'), (416, 'esMX', '¿Invocar banquero de materiales?');
