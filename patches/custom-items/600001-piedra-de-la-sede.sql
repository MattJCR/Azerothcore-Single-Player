-- SP02: piedra de la sede, separada de la piedra de hogar común.
-- Idempotente. El hechizo 600001 vive en Spell.dbc del servidor y del cliente.
DELETE FROM `item_template` WHERE `entry` = 600001;
DROP TEMPORARY TABLE IF EXISTS `_piedra_sede_tmp`;
CREATE TEMPORARY TABLE `_piedra_sede_tmp` SELECT * FROM `item_template` WHERE `entry` = 6948;
UPDATE `_piedra_sede_tmp` SET
    `entry` = 600001,
    `name` = 'Piedra de la sede',
    `description` = 'Te transporta a la sede de tu hermandad.',
    `displayid` = 6418,
    `Quality` = 1,
    `BuyPrice` = 0,
    `SellPrice` = 0,
    `maxcount` = 1,
    `stackable` = 1,
    `bonding` = 1,
    `spellid_1` = 600001,
    `spelltrigger_1` = 0,
    `spellcharges_1` = 0,
    `spellcooldown_1` = 1800000,
    `spellcategory_1` = 0,
    `spellcategorycooldown_1` = 0,
    `ScriptName` = '',
    `VerifiedBuild` = 0;
INSERT INTO `item_template` SELECT * FROM `_piedra_sede_tmp`;
DROP TEMPORARY TABLE `_piedra_sede_tmp`;

DELETE FROM `item_template_locale` WHERE `ID` = 600001;
INSERT INTO `item_template_locale` (`ID`, `locale`, `Name`, `Description`, `VerifiedBuild`) VALUES
(600001, 'esES', 'Piedra de la sede', 'Te transporta a la sede de tu hermandad.', 0),
(600001, 'esMX', 'Piedra de la sede', 'Te transporta a la sede de tu hermandad.', 0);
