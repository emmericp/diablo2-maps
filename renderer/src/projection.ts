// World-coordinate projection from D2 game tile coordinates.
// "world" units in this viewer are pixels-at-zoom-1.
//
// Flat: 1 world unit = 1 tile.
//   wx = gx, wy = gy
//
// Iso: 1 world unit X = half a tile-diagonal X (so each tile is 2 world-X
// pixels wide).
//   wx = 2 * (gx - gy)
//   wy =      gx + gy

export type Projection = "flat" | "iso";

export function projectTile(
  proj: Projection,
  gx: number,
  gy: number,
): [number, number] {
  if (proj === "iso") return [2 * (gx - gy), gx + gy];
  return [gx, gy];
}

// Project a level-local tile (lx, ly) to world coords using its origin.
export function projectLocal(
  proj: Projection,
  originX: number,
  originY: number,
  lx: number,
  ly: number,
): [number, number] {
  return projectTile(proj, originX + lx, originY + ly);
}

// Inverse of projectTile: world coords → (possibly fractional) game tile.
// Used when toggling projection so we can re-center on the same game tile
// the user was looking at.
export function inverseProjectTile(
  proj: Projection,
  wx: number,
  wy: number,
): [number, number] {
  if (proj === "iso") return [wx / 4 + wy / 2, wy / 2 - wx / 4];
  return [wx, wy];
}
