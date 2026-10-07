-- =============================================================================
--  ARAC 08 — Druida: Forma de oso/Zarpazo brutal no existen en ningún
--  instructor, y los requisitos que sólo viven en `spell_required` son
--  invisibles para el cliente
--
--  Hallazgo del 22/09/2026 (E1h, ver CHANGELOG). Síntoma comunicado: con la
--  ventana del instructor abierta se compra un hechizo y, al pulsar
--  "Aprender" sobre otro que también se ve EN VERDE, no pasa nada — ni
--  aprendizaje, ni cobro, ni mensaje, ni error de Lua. Cerrando y reabriendo
--  la ventana se puede comprar otro y vuelve a atascarse.
--
--  NO es el cliente ni un addon. La traza de `mod-arac-trainer-audit`
--  (22/09/2026 17:59, cuenta 1) lo deja cerrado:
--      RX BUY  spell=3029  -> TX SUCCEEDED          (Zarpazo r2, se aprende)
--      RX BUY  spell=99    -> TX FAILED reason=2    (x6, Rugido desmoralizador)
--  El cliente SÍ manda `CMSG_TRAINER_BUY_SPELL`; lo rechaza el core.
--  `reason=2` es `Trainer::FailReason::NotEnoughSkill`, que sale de
--  `Trainer::TeachSpell` -> `CanTeachSpell` -> `GetDefaultSpellState`.
--
--  CADENA COMPLETA:
--
--  1) `GetDefaultSpellState` (Trainer.cpp) comprueba, además de
--     ReqLevel/ReqSkillLine/ReqAbility, los requisitos de `spell_required`:
--         for (requirePair : GetSpellsRequiredForSpellBounds(SpellId))
--             if (!player->HasSpell(requirePair.second)) return Unavailable;
--     `spell_required` tiene 99 -> 5487 (Rugido desmoralizador exige Forma de
--     oso). El personaje de prueba (druida gnomo ARAC, guid 9001711, nivel 60
--     por GM) no conoce 5487, así que el rechazo del core es CORRECTO.
--
--  2) `spell_required` NO viaja en `SMSG_TRAINER_LIST`: el paquete sólo lleva
--     ReqLevel, ReqSkillLine/Rank y ReqAbility1/2/3. La fila de 99 los tiene
--     todos a 0 salvo ReqLevel=10, así que el cliente 3.3.5a — que recalcula
--     el estado de cada fila por su cuenta (rutina en 0x596b00, ver
--     `modules/mod-arac-trainer-audit/README.md`) — la pinta en verde y deja
--     pulsar "Aprender". Y `SMSG_TRAINER_BUY_FAILED` con motivo 2 no tiene
--     texto en la interfaz de 3.3.5a: de ahí el silencio absoluto.
--     Lo de "una compra por apertura" era casualidad de qué fila se tocaba
--     después: las de Equilibrio/Restauración no dependen de forma alguna y
--     encadenaban compras sin problema.
--
--  3) Por qué el personaje no tiene Forma de oso: **no se puede conseguir en
--     esta base de datos**. Comprobado sobre la BD real de la VM:
--       - `trainer_spell`: 5487 no lo vende NINGÚN instructor (tampoco el
--         6807, Zarpazo brutal r1). Sí están 768 (nivel 20), 783 (30) y 9634
--         (40, con ReqAbility1=5487). Es dato de serie de AzerothCore:
--         `data/sql/base/db_world/trainer_spell.sql` sólo cita 5487 como
--         requisito de 9634, nunca como fila propia.
--       - `quest_template`: ninguna misión tiene RewardSpell/RewardDisplaySpell
--         5487 ni 6807. La cadena "Great Bear Spirit" (5929/5930, nivel 10) y
--         "Back to Darnassus/Thunder Bluff" (5931/5932) tienen los dos campos
--         a 0 — en 3.0.2 Blizzard quitó las misiones de forma y pasó las
--         formas al instructor; a los datos de AC les faltó el bloque de
--         nivel 10 del oso.
--       - `SkillLineAbility.dbc`: 5487 y 6807 son AcquireMethod=0, o sea que
--         tampoco se aprenden solos al recibir la habilidad 134 (a diferencia
--         de 5176/5185, que son AcquireMethod=2).
--       - `playercreateinfo_spell_custom`: no los trae, y además
--         `PlayerStart.CustomSpells=0` en esta instalación (a propósito, ver
--         `scripts/phases/05_configure_server.sh`).
--     Un druida nativo pasa desapercibido sólo porque los 116 druidas de la
--     BD son bots y `PlayerbotFactory.cpp:3631` hace `learnSpell(5487)` a
--     mano. El único druida de jugador es el del usuario, y es el único sin
--     Forma de oso.
--
--  ALCANCE — auditoría sobre la BD real, no sobre suposiciones:
--    - Filas de `trainer_spell` cuyo requisito de `spell_required` NO está en
--      ReqAbility1/2/3 (o sea, mienten al cliente): 23, TODAS del druida
--      (TrainerId 33). 16 cuelgan de 768 (Forma felina) y 7 de 5487. Ninguna
--      otra clase tiene el desajuste. Las 23 tienen hueco libre en ReqAbility.
--    - Requisitos de `spell_required` que no vende nadie ni concede misión:
--      sólo 5487, 14752 y 20911. Los dos últimos los retiraba a propósito
--      el módulo de talentos por era, que se quitó del proyecto el
--      24/09/2026 (CHANGELOG). El único hueco no intencionado es 5487.
--
--  QUÉ HACE ESTE PARCHE — dos cosas independientes:
--
--  A) Pone en venta en el instructor de druidas (TrainerId 33, compartido por
--     los 16 instructores nativos y por el genérico ARAC 26324) las dos filas
--     que faltan de nivel 10: 5487 Forma de oso y 6807 Zarpazo brutal r1.
--     Precio 300 (3 plata), el mismo que todas las demás filas de nivel 10 de
--     este catálogo (99, 1058, 5232, 8924). Sin esto, ocho hechizos del árbol
--     Feral y toda la cadena de Zarpazo brutal son inalcanzables para
--     cualquier druida de jugador.
--
--  B) Copia a `ReqAbility1` el requisito que hoy sólo vive en
--     `spell_required`, para las 23 filas del druida. No cambia NADA del lado
--     del servidor (`GetDefaultSpellState` ya aplicaba `spell_required`): lo
--     que cambia es que ahora el requisito VIAJA en `SMSG_TRAINER_LIST` y el
--     cliente pinta la fila en gris con "Necesitas: <hechizo>" en vez de en
--     verde. Es el arreglo de fondo del síntoma: aunque mañana aparezca otro
--     requisito no cumplido, la ventana dejará de mentir.
--
--  Para reproducir la auditoría después de aplicar esto (debe dar 0 filas):
--    SELECT ts.TrainerId, ts.SpellId, sr.req_spell
--      FROM trainer_spell ts JOIN spell_required sr ON sr.spell_id = ts.SpellId
--     WHERE sr.req_spell NOT IN (ts.ReqAbility1, ts.ReqAbility2, ts.ReqAbility3);
--
--  Requiere reiniciar el worldserver: `trainer_spell` se carga al arrancar.
--
--  Idempotente: A borra sus dos filas antes de insertarlas; B es un UPDATE
--  con lista explícita de SpellId.
-- =============================================================================

-- -----------------------------------------------------------------------------
-- A) Las dos filas de nivel 10 que faltan en el catálogo de druida.
-- -----------------------------------------------------------------------------
DELETE FROM `trainer_spell` WHERE `TrainerId` = 33 AND `SpellId` IN (5487, 6807);

INSERT INTO `trainer_spell`
    (`TrainerId`, `SpellId`, `MoneyCost`, `ReqSkillLine`, `ReqSkillRank`,
     `ReqAbility1`, `ReqAbility2`, `ReqAbility3`, `ReqLevel`, `VerifiedBuild`)
VALUES
    (33, 5487, 300, 0, 0, 0, 0, 0, 10, 0),   -- Bear Form / Forma de oso
    (33, 6807, 300, 0, 0, 0, 0, 0, 10, 0);   -- Maul r1 / Zarpazo brutal r1

-- -----------------------------------------------------------------------------
-- B) Hacer visible al cliente el requisito de `spell_required`.
--    7 filas dependen de 5487 (Forma de oso):
-- -----------------------------------------------------------------------------
UPDATE `trainer_spell`
   SET `ReqAbility1` = 5487
 WHERE `TrainerId` = 33
   AND `ReqAbility1` = 0
   AND `SpellId` IN (
        99,     -- Demoralizing Roar     (nivel 10)
        5211,   -- Bash                  (nivel 14)
        779,    -- Swipe (Bear)          (nivel 16)
        5209,   -- Challenging Roar      (nivel 28)
        22842,  -- Frenzied Regeneration (nivel 36)
        62600,  -- Savage Defense        (nivel 40)
        33745   -- Lacerate              (nivel 66)
   );

--    16 filas dependen de 768 (Forma felina):
UPDATE `trainer_spell`
   SET `ReqAbility1` = 768
 WHERE `TrainerId` = 33
   AND `ReqAbility1` = 0
   AND `SpellId` IN (
        1079,   -- Rip                   (nivel 20)
        1082,   -- Claw                  (nivel 20)
        5215,   -- Prowl                 (nivel 20)
        5221,   -- Shred                 (nivel 22)
        1822,   -- Rake                  (nivel 24)
        5217,   -- Tigers Fury           (nivel 24)
        1850,   -- Dash                  (nivel 26)
        8998,   -- Cower                 (nivel 28)
        22568,  -- Ferocious Bite        (nivel 32)
        5225,   -- Track Humanoids       (nivel 32)
        6785,   -- Ravage                (nivel 32)
        9005,   -- Pounce                (nivel 36)
        20719,  -- Feline Grace          (nivel 40)
        22570,  -- Maim                  (nivel 62)
        62078,  -- Swipe (Cat)           (nivel 71)
        52610   -- Savage Roar           (nivel 75)
   );
