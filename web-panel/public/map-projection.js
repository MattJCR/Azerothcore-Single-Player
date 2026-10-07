// SPDX-FileCopyrightText: 2026 The Azerothcore-Single-Player contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Valores extraídos de WorldMapArea.dbc y WorldMapTransforms.dbc de WoW 3.3.5a
// (build 12340). Los ejes del mundo se cruzan al proyectarlos sobre el mapa.
export const MAP_BOUNDS = {
  0: { left: 18171.97, right: -22569.21, top: 11176.34, bottom: -15973.34 },
  1: { left: 17066.6, right: -19733.21, top: 12799.9, bottom: -11733.3 },
  530: { left: 12996.04, right: -4468.039, top: 5821.359, bottom: -5821.359 },
  571: { left: 9217.152, right: -8534.246, top: 10593.38, bottom: -1240.89 },
};

const WORLD_MAP_TRANSFORMS = [
  { map: 530, minX: 4800, minY: -10133.33, maxX: 16000, maxY: -2666.667, displayMap: 0, offsetX: -2400, offsetY: 2400 },
  { map: 530, minX: -6933.333, minY: -16000, maxX: 533.3333, maxY: -8000, displayMap: 1, offsetX: 10133.33, offsetY: 17600 },
];

export function projectMapPlayer(player) {
  let displayMap = Number(player.map);
  let worldX = Number(player.x);
  let worldY = Number(player.y);

  const transform = WORLD_MAP_TRANSFORMS.find((entry) => (
    displayMap === entry.map
    && worldX >= entry.minX && worldX <= entry.maxX
    && worldY >= entry.minY && worldY <= entry.maxY
  ));
  if (transform) {
    displayMap = transform.displayMap;
    worldX += transform.offsetX;
    worldY += transform.offsetY;
  }

  const bounds = MAP_BOUNDS[displayMap];
  if (!bounds) return { ...player, displayMap, onMap: false };

  const mapX = (worldY - bounds.left) / (bounds.right - bounds.left);
  const mapY = (worldX - bounds.top) / (bounds.bottom - bounds.top);
  const onMap = Number.isFinite(mapX) && Number.isFinite(mapY)
    && mapX >= 0 && mapX <= 1 && mapY >= 0 && mapY <= 1;

  return { ...player, displayMap, mapX, mapY, onMap };
}
