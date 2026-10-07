// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
const HEALER_TALENTS = new Set([
  53563, // Paladín: Señal de la Luz
  47540, 47788, 34861, // Sacerdote: Penitencia, Espíritu guardián, Círculo de sanación
  61295, // Chamán: Mareas vivas
  48438, // Druida: Crecimiento salvaje
]);

const TANK_TALENTS = new Set([
  46968, 12975, 20243, // Guerrero: Ola de choque, Última carga, Devastar
  53595, // Paladín: Martillo del honrado
  55233, 50150, 51271, 49222, // Caballero de la Muerte: talentos defensivos
  33853, 61336, // Druida: Protector de la manada, Instintos de supervivencia
]);

export function inferRole(characterClass, spells = []) {
  if (spells.some((spell) => HEALER_TALENTS.has(Number(spell)))) return 'healer';
  if (spells.some((spell) => TANK_TALENTS.has(Number(spell)))) return 'tank';
  // Las clases puras sólo pueden ser DPS; las híbridas sin marcador también
  // se presentan como DPS, evitando atribuir sanación/tanque sin evidencia.
  return 'dps';
}
