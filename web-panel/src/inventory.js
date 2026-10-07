// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Rangos de character_inventory.slot para un personaje (bag = 0, WotLK 3.3.5a).
const EQUIPPED_MAX = 18;
const BAG_CONTAINER_MAX = 22;
const BACKPACK_MAX = 38;
const BANK_ITEM_MAX = 66;
const BANK_CONTAINER_MAX = 73;
// 74+: devolución de compra, llavero, monedas — no se muestran aquí.

const bySlot = (a, b) => a.slot - b.slot;

// rows: objetos con { bag, slot, itemGuid, ... } ya formateados (ver items.js).
// bag = 0 significa "en el personaje" (equipo, bolsas, banco, según el slot);
// bag = <itemGuid> significa "dentro del contenedor con ese guid".
export function classifyInventory(rows) {
  const containerSlot = new Map();
  for (const row of rows) {
    if (row.bag === 0) containerSlot.set(row.itemGuid, row.slot);
  }

  const equipped = [];
  const backpack = [];
  const bagContainers = [];
  const bankLoose = [];
  const bankContainers = [];
  const bagContents = new Map();
  const bankContents = new Map();

  for (const row of rows) {
    if (row.bag === 0) {
      if (row.slot <= EQUIPPED_MAX) equipped.push(row);
      else if (row.slot <= BAG_CONTAINER_MAX) bagContainers.push(row);
      else if (row.slot <= BACKPACK_MAX) backpack.push(row);
      else if (row.slot <= BANK_ITEM_MAX) bankLoose.push(row);
      else if (row.slot <= BANK_CONTAINER_MAX) bankContainers.push(row);
      continue;
    }
    const parentSlot = containerSlot.get(row.bag);
    if (parentSlot === undefined) continue;
    if (parentSlot <= BAG_CONTAINER_MAX) {
      if (!bagContents.has(row.bag)) bagContents.set(row.bag, []);
      bagContents.get(row.bag).push(row);
    } else if (parentSlot <= BANK_CONTAINER_MAX) {
      if (!bankContents.has(row.bag)) bankContents.set(row.bag, []);
      bankContents.get(row.bag).push(row);
    }
  }

  const withContents = (containers, contents) => containers.sort(bySlot).map((container) => ({
    container,
    items: (contents.get(container.itemGuid) || []).sort(bySlot),
  }));

  return {
    equipped: equipped.sort(bySlot),
    bags: { loose: backpack.sort(bySlot), containers: withContents(bagContainers, bagContents) },
    bank: { loose: bankLoose.sort(bySlot), containers: withContents(bankContainers, bankContents) },
  };
}
