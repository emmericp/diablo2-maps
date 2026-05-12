// TowerRoom metadata table. TypeScript mirror of seedgen/src/tower_rooms.cpp's
// kRooms[]. Keep them in sync — the binary encoding (16-bit) and the BFS
// sequencing on the C++ side both depend on these IDs and slot orderings.

export type Shape =
  | "W" | "E" | "S" | "N"
  | "EW" | "NS"
  | "NE" | "NW" | "SE" | "SW"
  | "ESW" | "NEW" | "NSW" | "NES"
  | "NESW";

export const ALL_SHAPES: Shape[] = [
  "W", "E", "S", "N",
  "EW", "NS",
  "NE", "NW", "SE", "SW",
  "ESW", "NEW", "NSW", "NES",
  "NESW",
];

// One grave-slot bounding box in tile coords within the 40×40 room area.
export interface Slot { dx: number; dy: number; w: number; h: number }

export interface Variant {
  name: string;             // empty for single-variant rooms
  slots: Slot[];
}

export interface RoomDef {
  roomId: number;
  shape: Shape;
  theme: string;            // "Empty", "StairsUp", "Caskets", "Shrine", ...
  variants: Variant[];
}

const N = (): Variant => ({ name: "", slots: [] });

export const ROOMS: RoomDef[] = [
  // Dead-end family (1 exit each).
  { roomId: 109, shape: "W", theme: "Empty", variants: [
    { name: "Wide",   slots: [
      { dx: 16, dy: 10, w: 2, h: 4 }, { dx: 21, dy: 10, w: 2, h: 4 },
      { dx: 25, dy: 16, w: 4, h: 2 }, { dx: 25, dy: 21, w: 4, h: 2 },
      { dx: 21, dy: 25, w: 2, h: 4 }, { dx: 16, dy: 25, w: 2, h: 4 },
    ]},
    { name: "Narrow", slots: [{ dx: 5, dy: 21, w: 4, h: 2 }] },
  ]},
  { roomId: 110, shape: "E", theme: "Empty", variants: [
    { name: "Wide",   slots: [
      { dx: 21, dy: 25, w: 2, h: 4 }, { dx: 16, dy: 25, w: 2, h: 4 },
      { dx: 10, dy: 21, w: 4, h: 2 }, { dx: 10, dy: 16, w: 4, h: 2 },
      { dx: 16, dy: 10, w: 2, h: 4 }, { dx: 21, dy: 10, w: 2, h: 4 },
    ]},
    { name: "Narrow", slots: [{ dx: 35, dy: 21, w: 4, h: 2 }] },
  ]},
  { roomId: 112, shape: "S", theme: "Empty", variants: [
    { name: "Wide",   slots: [
      { dx: 10, dy: 21, w: 4, h: 2 }, { dx: 10, dy: 16, w: 4, h: 2 },
      { dx: 16, dy: 10, w: 2, h: 4 }, { dx: 21, dy: 10, w: 2, h: 4 },
      { dx: 25, dy: 16, w: 4, h: 2 }, { dx: 25, dy: 21, w: 4, h: 2 },
    ]},
    { name: "Narrow", slots: [{ dx: 21, dy: 30, w: 2, h: 4 }] },
  ]},
  { roomId: 116, shape: "N", theme: "Empty", variants: [
    { name: "Wide",   slots: [
      { dx: 25, dy: 16, w: 4, h: 2 }, { dx: 25, dy: 21, w: 4, h: 2 },
      { dx: 21, dy: 25, w: 2, h: 4 }, { dx: 16, dy: 25, w: 2, h: 4 },
      { dx: 10, dy: 21, w: 4, h: 2 }, { dx: 10, dy: 16, w: 4, h: 2 },
    ]},
    { name: "Narrow", slots: [
      { dx: 21, dy: 5, w: 2, h: 4 }, { dx: 16, dy: 5, w: 2, h: 4 },
    ]},
  ]},

  { roomId: 124, shape: "W", theme: "Caskets",      variants: [N()] },
  { roomId: 125, shape: "E", theme: "Candles",      variants: [N()] },
  { roomId: 127, shape: "S", theme: "SmallCandles", variants: [N()] },
  { roomId: 131, shape: "N", theme: "Braziers",     variants: [N()] },
  { roomId: 139, shape: "W", theme: "StairsUp",     variants: [N()] },
  { roomId: 140, shape: "E", theme: "StairsUp",     variants: [N()] },
  { roomId: 141, shape: "S", theme: "StairsUp",     variants: [N()] },
  { roomId: 142, shape: "N", theme: "StairsUp",     variants: [N()] },
  { roomId: 143, shape: "W", theme: "StairsDown",   variants: [N()] },
  { roomId: 144, shape: "E", theme: "StairsDown",   variants: [N()] },
  { roomId: 145, shape: "S", theme: "StairsDown",   variants: [N()] },
  { roomId: 146, shape: "N", theme: "StairsDown",   variants: [N()] },

  // Corridor family.
  { roomId: 111, shape: "EW", theme: "Empty", variants: [
    { name: "Wide",   slots: [
      { dx: 6, dy: 15, w: 2, h: 4 }, { dx: 16, dy: 15, w: 2, h: 4 },
      { dx: 26, dy: 15, w: 2, h: 4 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
    { name: "Narrow", slots: [
      { dx: 6, dy: 15, w: 2, h: 4 }, { dx: 16, dy: 15, w: 2, h: 4 },
      { dx: 26, dy: 15, w: 2, h: 4 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
  ]},
  { roomId: 120, shape: "NS", theme: "Empty", variants: [
    { name: "Wide",   slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx: 15, dy: 26, w: 4, h: 2 },
      { dx: 15, dy: 16, w: 4, h: 2 }, { dx: 15, dy:  6, w: 4, h: 2 },
    ]},
    { name: "Narrow", slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx: 15, dy: 6, w: 4, h: 2 },
    ]},
  ]},
  { roomId: 126, shape: "EW", theme: "Torches", variants: [N()] },
  { roomId: 135, shape: "NS", theme: "Shrine",  variants: [N()] },

  // Corner family.
  { roomId: 113, shape: "SW", theme: "Empty", variants: [
    { name: "DoorWest",  slots: [
      { dx: 20, dy: 36, w: 4, h: 2 }, { dx: 20, dy: 21, w: 4, h: 2 },
      { dx: 16, dy: 15, w: 2, h: 4 }, { dx:  6, dy: 15, w: 2, h: 4 },
    ]},
    { name: "DoorSouth", slots: [
      { dx: 20, dy: 36, w: 4, h: 2 }, { dx: 20, dy: 21, w: 4, h: 2 },
      { dx: 16, dy: 15, w: 2, h: 4 }, { dx:  6, dy: 15, w: 2, h: 4 },
    ]},
  ]},
  { roomId: 114, shape: "SE", theme: "Empty", variants: [
    { name: "DoorEast",  slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx: 15, dy: 21, w: 4, h: 2 },
      { dx: 21, dy: 15, w: 2, h: 4 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
    { name: "DoorSouth", slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx: 15, dy: 21, w: 4, h: 2 },
      { dx: 21, dy: 15, w: 2, h: 4 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
  ]},
  { roomId: 117, shape: "NW", theme: "Empty", variants: [
    { name: "DoorNorth", slots: [
      { dx:  6, dy: 20, w: 2, h: 4 }, { dx: 16, dy: 20, w: 2, h: 4 },
      { dx: 20, dy: 16, w: 4, h: 2 }, { dx: 20, dy:  6, w: 4, h: 2 },
    ]},
    { name: "DoorWest",  slots: [
      { dx:  6, dy: 20, w: 2, h: 4 }, { dx: 16, dy: 20, w: 2, h: 4 },
      { dx: 20, dy: 16, w: 4, h: 2 }, { dx: 20, dy:  6, w: 4, h: 2 },
    ]},
  ]},
  { roomId: 118, shape: "NE", theme: "Empty", variants: [
    { name: "DoorNorth", slots: [
      { dx: 15, dy:  6, w: 4, h: 2 }, { dx: 15, dy: 16, w: 4, h: 2 },
      { dx: 21, dy: 20, w: 2, h: 4 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
    { name: "DoorEast",  slots: [
      { dx: 15, dy:  6, w: 4, h: 2 }, { dx: 15, dy: 16, w: 4, h: 2 },
      { dx: 21, dy: 20, w: 2, h: 4 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
  ]},
  { roomId: 128, shape: "SW", theme: "Holes",           variants: [N()] },
  { roomId: 129, shape: "SE", theme: "Armory",          variants: [N()] },
  { roomId: 132, shape: "NW", theme: "SingleDeadRogue", variants: [N()] },
  { roomId: 133, shape: "NE", theme: "ArmoryLight",     variants: [
    { name: "", slots: [
      { dx: 15, dy: 6, w: 4, h: 2 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
  ]},

  // T-intersection family.
  { roomId: 115, shape: "ESW", theme: "Empty", variants: [
    { name: "DoorsWestEast", slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx:  6, dy: 15, w: 2, h: 4 },
      { dx: 15, dy: 16, w: 4, h: 2 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
    { name: "DoorSouth",     slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx:  6, dy: 15, w: 2, h: 4 },
      { dx: 16, dy: 15, w: 2, h: 4 }, { dx: 21, dy: 15, w: 2, h: 4 },
      { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
  ]},
  { roomId: 119, shape: "NEW", theme: "Empty", variants: [
    { name: "DoorsWestEast", slots: [
      { dx:  6, dy: 15, w: 2, h: 4 }, { dx: 15, dy:  6, w: 4, h: 2 },
      { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
    { name: "DoorNorth",     slots: [
      { dx: 15, dy:  6, w: 4, h: 2 }, { dx:  6, dy: 15, w: 2, h: 4 },
      { dx: 16, dy: 15, w: 2, h: 4 }, { dx: 21, dy: 15, w: 2, h: 4 },
      { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
  ]},
  { roomId: 121, shape: "NSW", theme: "Empty", variants: [
    { name: "DoorWest",         slots: [
      { dx:  6, dy: 15, w: 2, h: 4 }, { dx: 15, dy: 36, w: 4, h: 2 },
      { dx: 15, dy: 21, w: 4, h: 2 }, { dx: 15, dy: 16, w: 4, h: 2 },
      { dx: 15, dy:  6, w: 4, h: 2 },
    ]},
    { name: "DoorsNorthSouth",  slots: [
      { dx:  6, dy: 15, w: 2, h: 4 }, { dx: 15, dy: 36, w: 4, h: 2 },
      { dx: 15, dy: 21, w: 4, h: 2 }, { dx: 15, dy: 16, w: 4, h: 2 },
      { dx: 15, dy:  6, w: 4, h: 2 },
    ]},
  ]},
  { roomId: 122, shape: "NES", theme: "Empty", variants: [
    { name: "DoorEast",         slots: [
      { dx: 36, dy: 15, w: 2, h: 4 }, { dx: 15, dy: 36, w: 4, h: 2 },
      { dx: 15, dy: 21, w: 4, h: 2 }, { dx: 15, dy: 16, w: 4, h: 2 },
      { dx: 15, dy:  6, w: 4, h: 2 },
    ]},
    { name: "DoorsNorthSouth",  slots: [
      { dx: 36, dy: 15, w: 2, h: 4 }, { dx: 15, dy: 36, w: 4, h: 2 },
      { dx: 15, dy: 21, w: 4, h: 2 }, { dx: 15, dy: 16, w: 4, h: 2 },
      { dx: 15, dy:  6, w: 4, h: 2 },
    ]},
  ]},
  { roomId: 130, shape: "ESW", theme: "FirePentagram",         variants: [N()] },
  { roomId: 134, shape: "NEW", theme: "DoubleDeadRogueFire",   variants: [
    { name: "", slots: [{ dx: 15, dy: 6, w: 4, h: 2 }] },
  ]},
  { roomId: 136, shape: "NSW", theme: "Candles",               variants: [
    { name: "", slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx: 15, dy: 6, w: 4, h: 2 },
    ]},
  ]},
  { roomId: 137, shape: "NES", theme: "IntersectBraziers",     variants: [
    { name: "", slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx: 36, dy: 15, w: 2, h: 4 },
      { dx: 15, dy: 6, w: 4, h: 2 },
    ]},
  ]},

  // Crossing.
  { roomId: 123, shape: "NESW", theme: "Empty", variants: [
    { name: "DoorsWestEast",   slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx:  6, dy: 15, w: 2, h: 4 },
      { dx: 15, dy:  6, w: 4, h: 2 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
    { name: "DoorsNorthSouth", slots: [
      { dx: 15, dy: 36, w: 4, h: 2 }, { dx:  6, dy: 15, w: 2, h: 4 },
      { dx: 15, dy:  6, w: 4, h: 2 }, { dx: 36, dy: 15, w: 2, h: 4 },
    ]},
  ]},
];

export const ROOM_AREA_TILES = 40;          // every room is a 40×40 tile cell
export const SHAPE_EXIT_CHARS: Record<Shape, Set<string>> = (() => {
  const m: Record<Shape, Set<string>> = {} as any;
  for (const s of ALL_SHAPES) m[s] = new Set(s.split(""));
  return m;
})();

// Encode (roomId, variant, gravesMask) → 16-bit value used in the index.
//   high byte = (variant << 7) | (roomId - 100)
//   low byte  = graves bitmask (bit i = slot i+1 present)
export function encodeRoom(roomId: number, variant: number, graves: number): number {
  const hi = ((variant & 1) << 7) | ((roomId - 100) & 0x7F);
  return ((hi & 0xFF) << 8) | (graves & 0xFF);
}

export function decodeRoom(encoded: number): { roomId: number; variant: number; graves: number } {
  const hi = (encoded >> 8) & 0xFF;
  return {
    roomId: (hi & 0x7F) + 100,
    variant: (hi >> 7) & 1,
    graves: encoded & 0xFF,
  };
}

export function findRoom(roomId: number): RoomDef | undefined {
  return ROOMS.find((r) => r.roomId === roomId);
}

// Rooms grouped by shape, in declaration order. Useful for the picker.
export function roomsByShape(shape: Shape): RoomDef[] {
  return ROOMS.filter((r) => r.shape === shape);
}

// True for the four StairsUp encoded values (BFS anchors).
export function isStairsUp(encoded: number): boolean {
  const e = encoded & 0xFF00;
  return e === 0x2700 || e === 0x2800 || e === 0x2900 || e === 0x2A00;
}

// =============================================================================
// Iso direction relabeling — display-only.
//
// The encoding and BFS code stays on N/E/S/W (the original game axes). The UI
// presents an isometric view where the axes have rotated 45°:
//   N  →  top-right  (↗)
//   E  →  bottom-right (↘)
//   S  →  bottom-left (↙)
//   W  →  top-left  (↖)
// =============================================================================

export const DIR_ARROWS: Record<"N" | "E" | "S" | "W", string> = {
  N: "↗",
  E: "↘",
  S: "↙",
  W: "↖",
};

export function shapeAsArrows(shape: Shape): string {
  return shape.split("").map((c) => DIR_ARROWS[c as "N" | "E" | "S" | "W"]).join("");
}

// Replaces direction words inside human strings (variant names, theme names)
// with iso arrows: "DoorWest" → "Door ↖", "DoorsNorthSouth" → "Doors ↗↙".
// Also normalizes a few kebab-case names from the C++ table to title case.
export function relabelDirectionWords(s: string): string {
  let out = s.replace(/-/g, " ");
  out = out
    .replace(/North/gi, "↗")
    .replace(/East/gi,  "↘")
    .replace(/South/gi, "↙")
    .replace(/West/gi,  "↖");
  // Put a space between word characters and arrows: "Door↖" → "Door ↖".
  out = out.replace(/([A-Za-z])(?=[↗↘↙↖])/g, "$1 ");
  return out.replace(/\s+/g, " ").trim();
}
