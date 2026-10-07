-- =============================================================================
--  ARAC 09 — Poda por nivel de `playercreateinfo_spell_custom`, para poder
--  volver a encender `PlayerStart.CustomSpells`
--
--  GENERADO por tools/gen-poda-hechizos-iniciales.py. No editar a mano.
--
--  Hallazgo del 22/09/2026 (E1i, ver CHANGELOG). Síntoma: **ningún** personaje
--  de jugador tiene raciales, idiomas, competencias de armadura ni los
--  hechizos "generales" (Ataque, Duelo, Atascado, Abrir/Cerrar, Quitar
--  insignia, Detectar...). Comprobado sobre la BD real: los cuatro personajes
--  del usuario dan 0/30 de la lista básica.
--
--  **No es cosa de mod-arac**, aunque lo pareciera: `Prieste` (guid 9001712,
--  elfo nocturno sacerdote) es una combinación NATIVA y está igual de pelado.
--  En AzerothCore esos hechizos llegan por un único camino,
--  `Player::LearnCustomSpells()` leyendo `playercreateinfo_spell_custom`, y
--  esta instalación tiene `PlayerStart.CustomSpells = 0`.
--
--  Por qué estaba a 0, y por qué no basta con volver a encenderlo: esa tabla
--  no trae sólo los hechizos iniciales, trae **el árbol de clase entero sin
--  filtrar por nivel**. Verificado contra el `Spell.dbc` real: la lista de
--  gnomo druida incluye 33745 (Laceración, nivel 66) y 40121 (Forma de vuelo
--  veloz, nivel 68). Con el flag a 1 y la tabla intacta, un personaje de
--  nivel 1 los aprendería todos — que es el fallo que nos hizo apagarlo
--  (CHANGELOG, "Domar Bestia rompía la progresión").
--
--  LA SOLUCIÓN: podar la tabla dejando sólo lo que un personaje de nivel 1
--  puede tener, y volver a encender el flag (lo hace la fase 5, no este SQL).
--  DOS criterios, los dos objetivos y reproducibles. Se conserva la fila si:
--    1) su hechizo tiene `Spell.dbc` `baseLevel <= 1` **y** `spellLevel <= 1`; y
--    2) su hechizo EXISTE en el `Spell.dbc` efectivo del cliente
--       (`patch-<idioma>-4.MPQ` ya construido, no el `patch-3` pelado).
--
--  El criterio 2 se añadió el 22/09/2026 después de meter la pata: la primera
--  versión de esta poda sólo miraba el nivel y dejaba pasar 42 hechizos que el
--  cliente NO conoce — entre ellos dos ids imposibles, `426884` y `427448`, que
--  vienen corruptos en la fila Elfo de la Noche/Pícaro de mod-arac. Mandar al
--  cliente 3.3.5a un hechizo que no está en su `Spell.dbc` lo cuelga con
--  ERROR #132. De aquellos 42 sólo llegó a repartirse uno (`25359`, a un
--  personaje, ningún bot), pero 367 filas los tenían listos para el siguiente
--  personaje que se creara. Verificado leyendo el MPQ real con `mpyq`, no el
--  DBC del servidor, que no es el mismo fichero.
--
--  Comprobaciones de que el criterio no deja fuera nada que haga falta:
--    - 668 Idioma común (baseLevel 0, spellLevel 1), 7340 Idioma gnomo,
--      20589/20591/20592/20593 (raciales de gnomo), 9077 Cuero, 9078 Tela,
--      6603 Ataque, 7355 Atascado, 22027 Quitar insignia, 203 Sin armas,
--      204 Defensa -> **se quedan**.
--    - 5176 Ira y 5185 Toque sanador (baseLevel 1) -> se quedan, y de paso
--      tapan un hueco real: el instructor 33 vende el rango 2 de las dos
--      cadenas pero NO el rango 1.
--    - 5487 Forma de oso y 6807 Zarpazo brutal (nivel 10) -> se van, y es
--      correcto: desde `08-druid-bear-form-y-requisitos-visibles.sql` los
--      vende el instructor.
--    - De los hechizos descartados, 51 no los vende ningún instructor.
--      Revisados: son pasivas de forma (1178 Bear Form Passive, 3025 Cat
--      Form, 5419 Travel Form, 9635 Dire Bear, 24905 Moonkin, 33948 Flight
--      Form, 34123 Tree of Life...), pasivas de postura de guerrero (7376,
--      7381), invocaciones de brujo por misión (691, 697, 712) y 45438
--      Bloque de hielo, que borramos nosotros a propósito por el gating de
--      talento. Ninguna se aprende por instructor **por diseño**: llegan con
--      el hechizo padre. No hay nada que reponer.
--
--  ESTE SQL NO REPARA LOS PERSONAJES YA CREADOS — sólo arregla las creaciones
--  futuras. Los existentes se reparan con
--  `10-reparar-hechizos-iniciales-existentes.sql`.
--
--  Orden: la fase 5 aplica primero el SQL de mod-arac (que rellena la tabla
--  entera) y DESPUÉS `patches/arac/`, así que esta poda siempre manda.
--
--  Idempotente: DELETE con lista explícita; relanzarlo no borra nada más.
-- =============================================================================

-- 403 hechizos distintos de 544 se van: 361 por nivel, 42 por no existir
-- en el Spell.dbc del cliente (0 de ellos por los dos motivos a la vez).
-- Los que se van SOLO por no existir en el cliente:
--   277, 1020, 2870, 5627, 8166, 9186, 10912, 20554, 20580, 22018, 22019, 22055
--   25359, 25380, 25429, 25441, 25485, 25577, 25587, 25596, 25895, 25908, 26296, 26993
--   26999, 27015, 27018, 27020, 27144, 27145, 27148, 27155, 27158, 27160, 27166, 27169
--   27226, 27229, 30908, 31895, 426884, 427448
DELETE FROM `playercreateinfo_spell_custom` WHERE `Spell` IN (
    66, 71, 126, 130, 131, 132, 277, 355, 408, 421, 469, 475,
    526, 528, 546, 552, 556, 676, 691, 697, 698, 712, 768, 783,
    871, 883, 921, 982, 988, 1002, 1020, 1038, 1044, 1066, 1161, 1178,
    1462, 1515, 1543, 1680, 1706, 1719, 1725, 1787, 1833, 1842, 1860, 1953,
    2048, 2062, 2094, 2139, 2458, 2484, 2565, 2641, 2645, 2687, 2782, 2825,
    2836, 2842, 2870, 2893, 2894, 3025, 3043, 3045, 3411, 3738, 4987, 5116,
    5118, 5149, 5209, 5225, 5229, 5246, 5384, 5419, 5421, 5500, 5502, 5627,
    5697, 5938, 6196, 6197, 6215, 6346, 6495, 6554, 6774, 6795, 6991, 7376,
    7381, 8012, 8143, 8166, 8170, 8177, 8643, 8946, 8983, 9186, 9634, 9635,
    9846, 9913, 10278, 10308, 10326, 10890, 10909, 10912, 10955, 11297, 11305, 11578,
    11585, 11719, 11726, 12051, 12678, 12826, 13159, 13161, 13163, 13809, 14311, 14325,
    14327, 17928, 18499, 18540, 18647, 18658, 18960, 19746, 19752, 19801, 19878, 19879,
    19880, 19882, 19883, 19884, 19885, 20230, 20271, 20554, 20580, 20608, 20719, 20773,
    20777, 22018, 22019, 22055, 22570, 22812, 23161, 23214, 23920, 24248, 24905, 25203,
    25208, 25212, 25213, 25218, 25222, 25225, 25231, 25235, 25236, 25242, 25264, 25266,
    25275, 25308, 25312, 25359, 25364, 25368, 25375, 25380, 25384, 25389, 25392, 25396,
    25420, 25423, 25429, 25431, 25433, 25435, 25437, 25441, 25442, 25449, 25454, 25457,
    25464, 25472, 25485, 25489, 25500, 25505, 25509, 25525, 25528, 25533, 25547, 25552,
    25557, 25560, 25563, 25567, 25570, 25574, 25577, 25587, 25596, 25780, 25895, 25898,
    25908, 26296, 26669, 26679, 26862, 26863, 26865, 26866, 26867, 26884, 26889, 26979,
    26980, 26982, 26983, 26985, 26986, 26988, 26989, 26990, 26991, 26992, 26993, 26994,
    26995, 26996, 26997, 26998, 26999, 27000, 27002, 27003, 27004, 27005, 27006, 27008,
    27012, 27014, 27015, 27016, 27018, 27019, 27020, 27021, 27022, 27023, 27025, 27044,
    27045, 27046, 27070, 27072, 27074, 27079, 27082, 27085, 27086, 27087, 27088, 27101,
    27124, 27125, 27126, 27127, 27128, 27131, 27136, 27137, 27138, 27139, 27140, 27141,
    27142, 27143, 27144, 27145, 27148, 27149, 27150, 27151, 27152, 27153, 27154, 27155,
    27158, 27160, 27166, 27169, 27173, 27180, 27209, 27212, 27213, 27215, 27216, 27217,
    27218, 27220, 27222, 27223, 27226, 27228, 27229, 27230, 27238, 27243, 27250, 27259,
    27260, 27441, 27448, 28172, 28189, 28271, 28272, 28610, 29166, 29704, 29858, 29893,
    30324, 30357, 30449, 30451, 30455, 30459, 30482, 30545, 30908, 30909, 30910, 31224,
    31789, 31801, 31884, 31895, 32182, 32223, 32231, 32375, 32546, 32684, 32796, 32996,
    32999, 33076, 33357, 33736, 33745, 33763, 33786, 33944, 33946, 33948, 34026, 34074,
    34120, 34123, 34428, 34433, 34477, 34600, 34767, 36916, 36936, 38704, 38764, 38768,
    39374, 40120, 40121, 43987, 45438, 426884, 427448
);
