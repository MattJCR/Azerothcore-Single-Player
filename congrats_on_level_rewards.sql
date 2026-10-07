-- =============================================================================
--  Recompensas de mod-congrats-on-level — tabla v3 (02/09/2026)
--                                       + capa impar (10/09/2026)
--  Base: acore_world. La aplica sola la fase 5 del instalador (y la fase 8 si
--  la base aún no existía). Lanzarla a mano es inofensivo, es idempotente:
--      mysql -u acore -pacore acore_world < congrats_on_level_rewards.sql
--
--  Ojo: la capa impar entrega cajones de pícaro y necesita el objeto propio
--  600000 (patches/recompensas-nivel/ganzua.sql). El instalador aplica esa
--  ganzúa ANTES que este fichero. A mano, aplícala tú primero.
--
--  ─────────────────────────────────────────────────────────────────────────
--  DOS CAPAS, DOS CRITERIOS
--  ─────────────────────────────────────────────────────────────────────────
--  CAPA 1 — QoL permanente (niveles 10,20..70, 77, 80). Criterio del
--    02/09/2026: solo cosas PERMANENTES que quitan fricción —espacio,
--    desplazamiento, cambio de rol— y oro justo para lo que ese nivel obliga
--    a pagar. Nada de juguetes, consumibles de un uso ni reliquias (XP).
--
--  CAPA 2 — premio de sabor (impares 11..79, sin el 77). Un contenedor
--    abrible por nivel, botín ajustado al tramo, rotando Equipo → Gemas →
--    Materiales. En un servidor de una persona con bots esto no toca el
--    equilibrio: el botín se ajusta al nivel y no da XP. money = 0 en toda la
--    capa 2 (el oro sigue viviendo solo en la capa 1). Decisión del
--    10/09/2026. Detalle y tablas de contenedores en Recompensa.md.
--
--  ─────────────────────────────────────────────────────────────────────────
--  CRITERIO DE LA CAPA 1 (decisión del 02/09/2026, revisión de la tabla v2)
--  ─────────────────────────────────────────────────────────────────────────
--  Servidor de una persona con bots. Las recompensas tienen que ser cosas
--  PERMANENTES que quiten fricción —espacio, desplazamiento, cambio de rol—
--  y oro suficiente para lo que ese nivel obliga a pagar, no más. Nada de
--  juguetes, efectos, consumibles de un uso ni reliquias (tocan la XP).
--
--  Lo que se quitó de la v2 y por qué:
--    - Pergaminos de aguante I-IV: consumible de un uso; no aporta nada.
--    - 1 tela (lino/seda/paño rúnico): la tabla no admite cantidades, así
--      que era UNA tela. Inútil.
--    - Rayo Descombobulador (4388): juguete. Justo lo que no se quiere.
--    - 45 oro a nivel 20: a ese nivel un personaje tiene 5. Con la
--      equitación regalada, la montura cuesta 1 oro. Era inflación.
--
--  Lo que se descartó a propósito (para no volver a evaluarlo):
--    - Carcajes/bolsas de munición para cazador: en 3.3.5 ya no dan
--      velocidad de ataque y la munición se apila de 1000 en 1000 (el SQL
--      opcional ammo_stack_size de individual-progression NO está
--      aplicado). Es una bolsa peor que una bolsa.
--    - Reliquias: +XP, tocan el balance. Consumibles y buffs: un uso.
--    - Oro para la doble especialización (1000 oro a nivel 40): rompería la
--      economía vanilla. Se da la especialización directamente (ver nivel 40).
--
--  CORRECCIÓN 13/09/2026: el nivel 20 daba la equitación pero no la montura.
--    Sin montura, la habilidad no sirve de nada hasta que el jugador se
--    acuerda de comprar una aparte en el entrenador.
--
--  Primer intento (REVERTIDO, ver CHANGELOG): dar el objeto de la montura
--  racial clásica, bajándole el RequiredLevel de fábrica (40, 30 en elfo
--  sangriento/draenei) a 20 con un UPDATE de item_template. No sirvió: el
--  RequiredLevel de un objeto también vive en datos de cliente (item-sparse),
--  no sólo en la BD del servidor, así que el objeto seguía bloqueado hasta
--  nivel 40 aunque el servidor ya lo permitiera — se recibía a nivel 20 y no
--  se podía usar hasta veinte niveles después.
--
--  Solución: en vez de dar el objeto, se enseña el hechizo de la montura
--  directamente (`spell` + `learn`=1, igual que la equitación de la fila de
--  arriba), saltándose el objeto y su bloqueo de cliente. Los diez hechizos
--  son los que cada objeto clásico enseña al usarse (columna spellid_2 con
--  spelltrigger_2=6 "Learn spell" en item_template de esta BD):
--    1 humano   458   Brown Horse            2 orco       580   Timber Wolf
--    3 enano    6777  Gray Ram               4 elfo noche 10793 Striped Nightsaber
--    5 no-muerto 17464 Brown Skeletal Horse  6 tauren     18990 Brown Kodo
--    7 gnomo    17454 Unpainted Mechanostrider  8 trol    8395  Emerald Raptor
--    10 elfo sangriento 35020 Blue Hawkstrider  11 draenei 34406 Brown Elekk
--  No hay raza 9 en WotLK (hueco de Blizzard). No es retroactivo: el
--  personaje que ya pasó el nivel 20 no recibe la fila.
--
--  ─────────────────────────────────────────────────────────────────────────
--  LO QUE ADMITE EL MÓDULO (leído en src/mod_congratsonlevel.cpp @ fed4752e)
--  ─────────────────────────────────────────────────────────────────────────
--    - VARIAS FILAS POR NIVEL: giveAward() recorre todas las filas del nivel.
--      Así se dan más de dos objetos, o premios distintos por clase.
--    - `race` y `class`: 0 = todos. Filtran la fila entera (oro incluido).
--      Clases: 1 guerrero, 2 paladín, 3 cazador, 4 pícaro, 5 sacerdote,
--      6 caballero de la muerte, 7 chamán, 8 mago, 9 brujo, 11 druida.
--    - `money` va en ORO (el código multiplica por GOLD). 5 = 5 oro.
--    - `spell` con `learn`=1: lo aprende para siempre (equitación).
--      `spell` con `learn`=0: se lo lanza a sí mismo, disparado (parche 03).
--    - `itemId1`/`itemId2`: UN objeto de cada, cantidad 1. Sin pilas.
--    - Cualquier nivel del 2 al 80 (parche 01). Sin él, sólo múltiplos de 10.
--    - Si no cabe en la bolsa va POR CORREO (parche 03). Sin él se perdía.
--    - Los bots no reciben nada (Congrats.IgnoreBots, parche 02).
--    - NO es retroactivo: un personaje que ya pasó el nivel no recibe la
--      fila. Para dárselo a mano: `.send items`, `.learn`, `.modify money`.
--
--  ─────────────────────────────────────────────────────────────────────────
--  IDs verificados contra item_template de acore_world (volcado 01/09/2026)
--  ─────────────────────────────────────────────────────────────────────────
--    4245  Small Silk Pack        10 huecos  calidad 1 (blanca)  sin req. nivel
--    14046 Runecloth Bag          14 huecos  calidad 1 (blanca)  sin req. nivel
--    14155 Mooncloth Bag          16 huecos  calidad 2 (verde)   sin req. nivel
--    41599 Frostweave Bag         20 huecos  calidad 2 (verde)   sin req. nivel
--    22243 Small Soul Pouch       12 huecos  sólo brujo (almas)
--    22244 Box of Souls           16 huecos  sólo brujo (almas)
--    21340 Soul Pouch             20 huecos  sólo brujo (almas)
--    21341 Felcloth Bag           24 huecos  sólo brujo (almas)
--    41597 Abyssal Bag            32 huecos  sólo brujo (almas)
--  Hechizos (los mismos que enseñan los entrenadores):
--    33388 Equitación aprendiz (60 %, nivel 20)   33391 Oficial (100 %, nivel 40)
--    34090 Equitación experto (vuelo 150 %, 60)   34091 Artesano (vuelo 280 %, 70)
--    54197 Vuelo en clima frío (Rasganorte, nivel 77)
--    63680 + 63624 Doble especialización: los dos hechizos que lanza el
--          entrenador al cobrar 1000 oro (PlayerGossip.cpp del core).
--
--  Para comprobar que los objetos existen en TU acore_world:
--    SELECT entry, name, ContainerSlots, Quality FROM item_template
--    WHERE entry IN (4245,14046,14155,41599,22243,22244,21340,21341,41597);
--
--  ─────────────────────────────────────────────────────────────────────────
--  DOS AVISOS SOBRE LOS MODOS DE DESAFÍO (mod-challenge-modes)
--  ─────────────────────────────────────────────────────────────────────────
--    - "Sólo Fabricado": ninguna bolsa regalada se puede equipar (exige la
--      firma del propio personaje). Se quedan en el correo, sin molestar.
--    - "Sólo Normal" e "Iron Man": sólo se equipan objetos de calidad <= 1.
--      Las bolsas de 10 y 14 son blancas y valen; las de 16 y 20 son verdes
--      y esos personajes se quedan con la de 14. Es deliberado: no hay
--      bolsa blanca de más de 14 huecos en el juego.
--
--  ─────────────────────────────────────────────────────────────────────────
--  RESUMEN DE LA CAPA 1 (oro total: 341; la v2 daba 392, peor repartido)
--  ─────────────────────────────────────────────────────────────────────────
--    10:  1 oro · 4 bolsas de 10 · brujo: bolsa de almas 12
--    20:  5 oro · equitación aprendiz · montura terrestre básica (10 filas
--         por raza, ver corrección 13/09/2026)
--    30: 10 oro · brujo: bolsa de almas 16
--    40: 25 oro · equitación oficial · 4 bolsas de 14 · doble especialización
--    50: 25 oro · brujo: bolsa de almas 20
--    60: 75 oro · vuelo normal · 4 bolsas de 16 · brujo: bolsa de almas 24
--    70: 150 oro · vuelo épico
--    77:  0 oro · vuelo en clima frío
--    80: 50 oro · 4 bolsas de 20 · brujo: bolsa de almas 32
--
--  ─────────────────────────────────────────────────────────────────────────
--  RESUMEN DE LA CAPA 2 (impares 11..79 sin el 77 · 34 niveles · money = 0)
--  ─────────────────────────────────────────────────────────────────────────
--  Rotación Equipo (E) → Gemas (G) → Materiales (M) desde el nivel 11.
--    E (12): cajón de pícaro del tramo + ganzúa de recompensa (600000)
--       11,17 Battered Junkbox 16882   ·  23,29 Worn Junkbox 16883
--       35,41 Sturdy Junkbox  16884    ·  47,53 Heavy Junkbox 16885
--       59,65 Strong Junkbox  29569    ·  71,79 Reinforced Junkbox 43575
--    G (11): un saco de gemas del tramo
--       13,19,25 Sack of Gems 11938    ·  31,37 Blue Sack of Gems 17962
--       43,49 Red Sack of Gems 17969   ·  55,61 Unmarked Bag of Gems 25419
--       67 Bag of Premium Gems 25423   ·  73 Small Velvet Bag 41888
--    M (11): una caja de materiales del tramo (nativa; algunas se abren unos
--       niveles más tarde por su RequiredLevel, es deliberado)
--       15 Dented Crate 6351   ·  21 Waterlogged Crate 6352
--       27 Sealed Crate 6357   ·  33,39,45 Box of Supplies 6827
--       51,57 Curious Crate 27513  ·  63 Heavy Supply Crate 27481
--       69,75 Reinforced Crate 44475
--
--  NO es retroactivo: un personaje que ya pasó el nivel no recibe la fila.
--  Para el personaje actual se decidió NO recuperar lo atrasado (10/09/2026).
-- =============================================================================

-- Limpieza previa: TODA la tabla, no sólo unos niveles. Antes se borraban
-- los múltiplos de 5 y una fila en otro nivel (77) habría sobrevivido con
-- valores viejos a la siguiente reaplicación. El módulo mete sus 8 filas de
-- ejemplo (juguetes) al crear la base; esto las sustituye.
DELETE FROM mod_congrats_on_level_items;


-- ── Nivel 10: 1 oro + 4 bolsas de 10 huecos (Small Silk Pack) ────────────────
-- El mayor QoL del juego es espacio. A nivel 10 se llevan las bolsas de 6 de
-- las misiones iniciales; cuatro de 10 son 16 huecos más. Blanca: vale para
-- los desafíos Sólo Normal e Iron Man. Dos filas porque cada fila da dos.
-- 1 oro: a nivel 10 el gasto son las habilidades del entrenador.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (10, 1, 0, 0, 4245, 4245, 0, 0);
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (10, 0, 0, 0, 4245, 4245, 0, 0);

-- Brujo (clase 9): Small Soul Pouch, 12 huecos. Los fragmentos de alma no se
-- apilan y ocupan la bolsa; la bolsa de almas es QoL puro sin efecto en el
-- combate. Sube de tamaño en 30, 50, 60 y 80.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (10, 0, 0, 0, 22243, 0, 0, 9);


-- ── Nivel 20: 5 oro + Equitación aprendiz (33388, 60 % terrestre) ───────────
-- La habilidad cuesta 4 oro y la montura 1: con 5 oro sale la montura y
-- sobra algo para el entrenador. La v2 daba 45, que a nivel 20 es una
-- fortuna (inflación sin motivo). Hechizo en fila aparte porque `race`
-- filtra toda la fila y aquí hace falta una por raza (ver corrección
-- 13/09/2026).
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (20, 5, 33388, 1, 0, 0, 0, 0);

-- Montura terrestre básica de la raza, aprendida como hechizo directamente
-- (corrección 13/09/2026, segunda vuelta: dar el objeto no basta, se queda
-- bloqueado hasta nivel 40 por el RequiredLevel de cliente — ver arriba).
-- Una fila por raza porque el filtro `race` afecta a la fila entera;
-- money=0 porque el oro ya lo da la fila de arriba. No hay raza 9 en WotLK.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class) VALUES
    (20, 0,   458, 1, 0, 0,  1, 0),   -- Humano: Brown Horse
    (20, 0,   580, 1, 0, 0,  2, 0),   -- Orco: Timber Wolf
    (20, 0,  6777, 1, 0, 0,  3, 0),   -- Enano: Gray Ram
    (20, 0, 10793, 1, 0, 0,  4, 0),   -- Elfo de la noche: Striped Nightsaber
    (20, 0, 17464, 1, 0, 0,  5, 0),   -- No-muerto: Brown Skeletal Horse
    (20, 0, 18990, 1, 0, 0,  6, 0),   -- Tauren: Brown Kodo
    (20, 0, 17454, 1, 0, 0,  7, 0),   -- Gnomo: Unpainted Mechanostrider
    (20, 0,  8395, 1, 0, 0,  8, 0),   -- Trol: Emerald Raptor
    (20, 0, 35020, 1, 0, 0, 10, 0),   -- Elfo sangriento: Blue Hawkstrider
    (20, 0, 34406, 1, 0, 0, 11, 0);   -- Draenei: Brown Elekk


-- ── Nivel 30: 10 oro ─────────────────────────────────────────────────────────
-- Entrenador de nivel 30 y primeras reparaciones serias. Sin objetos para
-- todos: las bolsas de 14 llegan en el 40.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (30, 10, 0, 0, 0, 0, 0, 0);

-- Brujo: Box of Souls, 16 huecos.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (30, 0, 0, 0, 22244, 0, 0, 9);


-- ── Nivel 40: 25 oro + Equitación oficial (33391, 100 %) + 4 bolsas de 14 ───
-- Montura 10 oro. Runecloth Bag es la ÚLTIMA bolsa blanca que existe (14):
-- los desafíos Sólo Normal / Iron Man se quedan aquí.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (40, 25, 33391, 1, 14046, 14046, 0, 0);
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (40, 0, 0, 0, 14046, 14046, 0, 0);

-- Doble especialización (los dos hechizos que lanza el entrenador al cobrar
-- 1000 oro: 63680 y 63624, ambos con learn=0 = lanzados). Jugando solo con
-- bots, poder pasar de tanque/sanador a daño sin pagar 1000 oro en la fase
-- vanilla es el QoL grande de este nivel. No toca el balance: quita un
-- sumidero de oro, nada más. Si el personaje ya la tenía, no hace nada.
-- Una fila por hechizo porque la tabla admite un hechizo por fila.
-- >>> Pendiente de verificar en el juego (PRUEBAS-EN-JUEGO.md 1.15) <<<
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (40, 0, 63680, 0, 0, 0, 0, 0);
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (40, 0, 63624, 0, 0, 0, 0, 0);


-- ── Nivel 50: 25 oro ─────────────────────────────────────────────────────────
-- Entrenador de 50 y montura del 40 si aún no se compró.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (50, 25, 0, 0, 0, 0, 0, 0);

-- Brujo: Soul Pouch, 20 huecos.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (50, 0, 0, 0, 21340, 0, 0, 9);


-- ── Nivel 60: 75 oro + Equitación experto (34090, vuelo 150 %) + 4 de 16 ────
-- Fin de la fase vanilla. La habilidad vale 250 oro y se regala; la montura
-- voladora cuesta 50 y sólo se compra en Terrallende, así que el vuelo no
-- sirve hasta que individual-progression abra el Portal Oscuro: es
-- inofensivo darlo aquí. Mooncloth Bag (16) es verde: ver el aviso de los
-- desafíos arriba.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (60, 75, 34090, 1, 14155, 14155, 0, 0);
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (60, 0, 0, 0, 14155, 14155, 0, 0);

-- Brujo: Felcloth Bag, 24 huecos.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (60, 0, 0, 0, 21341, 0, 0, 9);


-- ── Nivel 70: 150 oro + Equitación artesano (34091, vuelo 280 %) ────────────
-- Fin de la fase TBC. La habilidad vale 5000 oro: es EL regalo grande de la
-- tabla y está bien así, jugando solo es comodidad, no ventaja. La montura
-- épica voladora cuesta 100; sobran 50 para reparaciones. Sin bolsas: de 16
-- a 18 no compensa vaciar cuatro bolsas.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (70, 150, 34091, 1, 0, 0, 0, 0);


-- ── Nivel 77: Vuelo en clima frío (54197) ────────────────────────────────────
-- El hueco más claro de la v2: sin esto no se vuela en Rasganorte hasta pagar
-- 1000 oro en Dalaran. Es el nivel en que lo enseña el entrenador. Sin oro:
-- el premio es la habilidad.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (77, 0, 54197, 1, 0, 0, 0, 0);


-- ── Nivel 80: 50 oro + 4 bolsas de 20 (Frostweave Bag) ──────────────────────
-- Nivel máximo: bolsa definitiva de WotLK y algo de oro para las primeras
-- heroicas (reparaciones, gemas). Nada de equipo: eso lo dan las mazmorras y
-- la subasta de mod-ah-bot-plus.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (80, 50, 0, 0, 41599, 41599, 0, 0);
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (80, 0, 0, 0, 41599, 41599, 0, 0);

-- Brujo: Abyssal Bag, 32 huecos.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class)
VALUES (80, 0, 0, 0, 41597, 0, 0, 9);


-- ═════════════════════════════════════════════════════════════════════════════
--  CAPA 2 — premio de sabor en niveles impares (11..79, sin el 77)
--  Un contenedor por nivel, money = 0, sin hechizo. Rotación E → G → M.
--  Ver la cabecera para el porqué y Recompensa.md para el detalle de botín.
-- ═════════════════════════════════════════════════════════════════════════════

-- ── EQUIPO: cajón de pícaro del tramo + ganzúa de recompensa (600000) ────────
-- El botín de los cajones de pícaro escala por tramo (raro entre los
-- contenedores nativos). La ganzúa 600000 (patches/recompensas-nivel/ganzua.sql)
-- los abre sin necesidad de Herrería y se gasta en un uso.
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class) VALUES
    (11, 0, 0, 0, 16882, 600000, 0, 0),   -- Battered Junkbox
    (17, 0, 0, 0, 16882, 600000, 0, 0),
    (23, 0, 0, 0, 16883, 600000, 0, 0),   -- Worn Junkbox
    (29, 0, 0, 0, 16883, 600000, 0, 0),
    (35, 0, 0, 0, 16884, 600000, 0, 0),   -- Sturdy Junkbox
    (41, 0, 0, 0, 16884, 600000, 0, 0),
    (47, 0, 0, 0, 16885, 600000, 0, 0),   -- Heavy Junkbox
    (53, 0, 0, 0, 16885, 600000, 0, 0),
    (59, 0, 0, 0, 29569, 600000, 0, 0),   -- Strong Junkbox
    (65, 0, 0, 0, 29569, 600000, 0, 0),
    (71, 0, 0, 0, 43575, 600000, 0, 0),   -- Reinforced Junkbox (req. nivel 70)
    (79, 0, 0, 0, 43575, 600000, 0, 0);

-- ── GEMAS: un saco de gemas del tramo ───────────────────────────────────────
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class) VALUES
    (13, 0, 0, 0, 11938, 0, 0, 0),   -- Sack of Gems (clásicas bajas/medias)
    (19, 0, 0, 0, 11938, 0, 0, 0),
    (25, 0, 0, 0, 11938, 0, 0, 0),
    (31, 0, 0, 0, 17962, 0, 0, 0),   -- Blue Sack of Gems
    (37, 0, 0, 0, 17962, 0, 0, 0),
    (43, 0, 0, 0, 17969, 0, 0, 0),   -- Red Sack of Gems
    (49, 0, 0, 0, 17969, 0, 0, 0),
    (55, 0, 0, 0, 25419, 0, 0, 0),   -- Unmarked Bag of Gems (Terrallende)
    (61, 0, 0, 0, 25419, 0, 0, 0),
    (67, 0, 0, 0, 25423, 0, 0, 0),   -- Bag of Premium Gems (Terrallende altas)
    (73, 0, 0, 0, 41888, 0, 0, 0);   -- Small Velvet Bag (gema perfecta Rasganorte)

-- ── MATERIALES: una caja de materiales del tramo ────────────────────────────
-- Nativas. Algunas tienen RequiredLevel por encima del nivel que las regala y
-- se abren unos niveles más tarde; es deliberado (no hay caja de materiales
-- nativa bien escalada sin este desfase).
INSERT INTO mod_congrats_on_level_items (level, money, spell, learn, itemId1, itemId2, race, class) VALUES
    (15, 0, 0, 0,  6351, 0, 0, 0),   -- Dented Crate (ingeniería, cobre)
    (21, 0, 0, 0,  6352, 0, 0, 0),   -- Waterlogged Crate (se abre a nivel 25)
    (27, 0, 0, 0,  6357, 0, 0, 0),   -- Sealed Crate (se abre a nivel 35)
    (33, 0, 0, 0,  6827, 0, 0, 0),   -- Box of Supplies (se abre a nivel 40)
    (39, 0, 0, 0,  6827, 0, 0, 0),
    (45, 0, 0, 0,  6827, 0, 0, 0),
    (51, 0, 0, 0, 27513, 0, 0, 0),   -- Curious Crate (se abre a nivel 60)
    (57, 0, 0, 0, 27513, 0, 0, 0),
    (63, 0, 0, 0, 27481, 0, 0, 0),   -- Heavy Supply Crate (se abre a nivel 60)
    (69, 0, 0, 0, 44475, 0, 0, 0),   -- Reinforced Crate (se abre a nivel 70)
    (75, 0, 0, 0, 44475, 0, 0, 0);


-- Verificación final: 64 filas.
--   Capa 1: 30 filas en 10,20..80 y el 77 (incluye las 10 monturas del 20).
--   Capa 2: 34 filas en los impares 11..79 (sin el 77).
SELECT level, money, spell, itemId1, itemId2, class,
       CASE WHEN level % 2 = 0 OR level = 77 THEN 'capa1' ELSE 'capa2' END AS capa
FROM mod_congrats_on_level_items ORDER BY level, class;
