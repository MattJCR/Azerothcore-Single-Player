-- =============================================================================
--  ARAC 02 — Hechizos de paladín y chamán atados a la raza en vez de a la facción
--
--  Origen: pull request #46 de heyitsbench/mod-arac ("Fix for Paladin and
--  Shaman Spells", Flerp, 2025-11-11), ABIERTO y SIN MERGEAR en el upstream.
--  https://github.com/heyitsbench/mod-arac/pull/46
--
--  El problema: Blizzard ató estos cuatro hechizos a RAZAS concretas en vez de
--  a la facción, así que las ediciones DBC normales de ARAC no los cubren y
--  solo las combinaciones raza+clase originales pueden aprenderlos. Un paladín
--  elfo de la noche o un chamán no-muerto se quedan sin ellos.
--
--  El PR upstream resuelve esto sustituyendo el binario SkillLineAbility.dbc.
--  Aquí NO descargamos ese binario: usamos la tabla de override
--  `skilllineability_dbc`, que AzerothCore superpone sobre el fichero .dbc
--  (LoadDBC en src/server/game/DataStores/DBCStores.cpp carga primero el
--  fichero y después la tabla). Sustituye exactamente 4 registros por ID y
--  deja intactos los otros ~10.215, es auditable y sobrevive a que vuelvas a
--  extraer los DBC del cliente.
--
--  Los valores de cada fila se leyeron del SkillLineAbility.dbc real del
--  servidor; lo único que cambia respecto al original es RaceMask.
--
--    RaceMask Alianza = 1101 = Humano(1)+Enano(4)+Elfo noche(8)+Gnomo(64)+Draenei(1024)
--    RaceMask Horda   =  690 = Orco(2)+No-muerto(16)+Tauren(32)+Trol(128)+Elfo sangre(512)
--
--  Idempotente: REPLACE INTO por clave primaria.
-- =============================================================================

-- Seal of Vengeance (31801) — paladín. Antes 1029 (Humano+Enano+Draenei):
-- le faltaban Elfo de la noche y Gnomo.
REPLACE INTO `skilllineability_dbc`
    (`ID`,`SkillLine`,`Spell`,`RaceMask`,`ClassMask`,`ExcludeRace`,`ExcludeClass`,
     `MinSkillLineRank`,`SupercededBySpell`,`AcquireMethod`,
     `TrivialSkillLineRankHigh`,`TrivialSkillLineRankLow`,
     `CharacterPoints_1`,`CharacterPoints_2`)
VALUES (14779,184,31801,1101,2,0,0,1,0,0,0,0,0,0);

-- Seal of Corruption (53736) — paladín. Antes 512 (solo Elfo de sangre).
REPLACE INTO `skilllineability_dbc`
    (`ID`,`SkillLine`,`Spell`,`RaceMask`,`ClassMask`,`ExcludeRace`,`ExcludeClass`,
     `MinSkillLineRank`,`SupercededBySpell`,`AcquireMethod`,
     `TrivialSkillLineRankHigh`,`TrivialSkillLineRankLow`,
     `CharacterPoints_1`,`CharacterPoints_2`)
VALUES (18311,184,53736,690,2,0,0,1,0,0,0,0,0,0);

-- Heroism (32182) — chamán. Antes 1024 (solo Draenei).
REPLACE INTO `skilllineability_dbc`
    (`ID`,`SkillLine`,`Spell`,`RaceMask`,`ClassMask`,`ExcludeRace`,`ExcludeClass`,
     `MinSkillLineRank`,`SupercededBySpell`,`AcquireMethod`,
     `TrivialSkillLineRankHigh`,`TrivialSkillLineRankLow`,
     `CharacterPoints_1`,`CharacterPoints_2`)
VALUES (14795,373,32182,1101,64,0,0,1,0,0,0,0,0,0);

-- Bloodlust (2825) — chamán. Antes 162 (Orco+Tauren+Trol).
REPLACE INTO `skilllineability_dbc`
    (`ID`,`SkillLine`,`Spell`,`RaceMask`,`ClassMask`,`ExcludeRace`,`ExcludeClass`,
     `MinSkillLineRank`,`SupercededBySpell`,`AcquireMethod`,
     `TrivialSkillLineRankHigh`,`TrivialSkillLineRankLow`,
     `CharacterPoints_1`,`CharacterPoints_2`)
VALUES (13151,373,2825,690,64,0,0,1,0,0,0,0,0,0);
