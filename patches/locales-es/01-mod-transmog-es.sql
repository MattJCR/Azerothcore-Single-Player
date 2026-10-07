-- =============================================================================
--  mod-transmog: los dos textos que el modulo NO trae traducidos
--
--  IMPORTANTE: mod-transmog YA ships su propia traduccion al espanol (43 de sus
--  45 cadenas, en module_string_locale, usando "transfiguracion", que es el
--  termino oficial de Blizzard). No hay que retraducirlo: seria trabajo
--  duplicado y peor, porque la del modulo lleva las tildes correctas.
--
--  Solo faltan los identificadores 18 y 19, que son los mensajes del comando
--  que cambia entre mostrar las apariencias como vendedor o como lista.
--
--  OJO CON LA COLACION: no se puede usar una variable de usuario (@M) para
--  comparar contra la columna `module`. La variable toma la colacion por
--  defecto de la conexion (utf8mb4_0900_ai_ci) y la columna es
--  utf8mb4_unicode_ci, asi que MySQL aborta con
--     ERROR 1267 Illegal mix of collations ... for operation '='
--  y, como el cliente mysql para en el primer error, el fichero entero se
--  queda sin aplicar en silencio. Por eso aqui va el nombre del modulo
--  literal en cada sentencia.
-- =============================================================================

DELETE FROM `module_string_locale`
 WHERE `module` = 'mod-transmog' AND `id` IN (18, 19) AND `locale` IN ('esES','esMX');

INSERT INTO `module_string_locale` (`module`, `id`, `locale`, `string`) VALUES
('mod-transmog', 18, 'esES', 'El PNJ de transfiguración mostrará ahora las apariencias disponibles como un vendedor, para poder previsualizarlas.\nAVISO: si tienes demasiadas apariencias, algunas no se mostrarán por una limitación del cliente. En ese caso, desactiva esta opción.'),
('mod-transmog', 19, 'esES', 'El PNJ de transfiguración mostrará ahora las apariencias como una lista de diálogo.'),
('mod-transmog', 18, 'esMX', 'El PNJ de transfiguración mostrará ahora las apariencias disponibles como un vendedor, para poder previsualizarlas.\nAVISO: si tienes demasiadas apariencias, algunas no se mostrarán por una limitación del cliente. En ese caso, desactiva esta opción.'),
('mod-transmog', 19, 'esMX', 'El PNJ de transfiguración mostrará ahora las apariencias como una lista de diálogo.');
