// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
import { iconForDisplayId } from './item-icons.js';

const WEAPON_CLASS = 2;
const STAT_PAIRS = 5;

// item_template.class == 2 (Weapon): calcula el DPS a partir del daño base y
// la cadencia (delay en ms), igual que el tooltip del cliente.
export function buildWeaponStats(row) {
  if (Number(row.itemClass) !== WEAPON_CLASS) return null;
  const dmgMin = Number(row.dmgMin) || 0;
  const dmgMax = Number(row.dmgMax) || 0;
  const delay = Number(row.delay) || 0;
  if (!delay) return null;
  return {
    dmgMin,
    dmgMax,
    delay,
    dps: Math.round(((dmgMin + dmgMax) / 2 / (delay / 1000)) * 10) / 10,
  };
}

// stat_type1..5 / stat_value1..5 de item_template: sólo los pares con valor
// distinto de cero viajan al cliente.
export function nonZeroStats(row) {
  const stats = [];
  for (let index = 1; index <= STAT_PAIRS; index += 1) {
    const type = Number(row[`statType${index}`]);
    const value = Number(row[`statValue${index}`]);
    if (value) stats.push({ type, value });
  }
  return stats;
}

export function formatItemRow(row) {
  const displayId = Number(row.displayId) || 0;
  return {
    itemGuid: Number(row.itemGuid),
    entry: Number(row.entry),
    name: row.name,
    quality: Number(row.quality),
    count: Number(row.count) || 1,
    ilvl: Number(row.ilvl),
    requiredLevel: Number(row.requiredLevel),
    inventoryType: Number(row.inventoryType),
    displayId,
    icon: iconForDisplayId(displayId),
    armor: Number(row.armor) || 0,
    stats: nonZeroStats(row),
    weapon: buildWeaponStats(row),
  };
}
