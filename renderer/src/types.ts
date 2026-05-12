// Types matching the JSON emitted by seedgen/src/mapdump_full.cpp::WriteJsonSeed.
// Keep field names in sync with that file.

export type PresetType = "npc" | "obj" | "exit" | "?";

export type ObjectKind =
  | "Generic"
  | "Waypoint"
  | "Shrine"
  | "Well"
  | "SuperChest"
  | "Chest"
  | "Door"
  | "Stairs"
  | "Quest";

export interface PresetJson {
  type: PresetType;
  txtFileNo: number;
  x: number;
  y: number;
  kind?: ObjectKind;
  destLevelNo?: number;
  destName?: string;
  // Localized in-game name for the destination level (e.g. "Cold Plains")
  // when destName is set. Only emitted if the lookup succeeded.
  destDisplayName?: string;
  name?: string;
  // Localized in-game name for the preset itself (e.g. "Magic Shrine" for
  // an object whose raw name key is "magic shrine"). Renderer code should
  // prefer this when present and fall back to `name`.
  displayName?: string;
  description?: string;
}

export interface AdjacentJson {
  levelNo: number;
  name: string;
  // Localized in-game level name; same fallback policy as PresetJson.displayName.
  displayName?: string;
  bridgeX: number;
  bridgeY: number;
}

export interface RoomJson {
  x: number;
  y: number;
  sizeX: number;
  sizeY: number;
  roomNo: number;
  subNo: number;
}

export interface TellLocationJson {
  x: number;
  y: number;
  w: number;
  h: number;  // w == 0 && h == 0 → point; otherwise [x,x+w) × [y,y+h) rect.
}

export interface TellJson {
  name: string;
  value: string;
  locations: TellLocationJson[];  // filtered to the level this tell appears in.
}

export interface LevelJson {
  levelNo: number;
  name: string;
  // Localized in-game level name (e.g. "Rogue Encampment").
  displayName?: string;
  act: number;
  origin: [number, number];
  size: [number, number];
  tells?: TellJson[];
  rooms: RoomJson[];
  adjacents: AdjacentJson[];
  presets: PresetJson[];
  collisionWidth?: number;
  collisionHeight?: number;
  // Zlib-deflated little-endian uint16 collision bytes, base64-encoded.
  // Decode with DecompressionStream("deflate"). See parse.ts.
  collisionDeflateB64: string | null;
}

export interface SeedJson {
  seed: number;
  levels: LevelJson[];
}

// Internal model used by the renderer. Collision data is kept compressed
// until a level is actually rendered (see ensureCollision in parse.ts) —
// for whole-seed dumps with ~100 levels but only ~10 visible at a time, this
// keeps the JS heap small.
export interface Level {
  levelNo: number;
  name: string;
  // Localized in-game level name (e.g. "Rogue Encampment"). Optional; absent
  // when the JSON came from a build that didn't carry display names.
  displayName?: string;
  act: number;
  originX: number;
  originY: number;
  sizeX: number;
  sizeY: number;
  // Tight bbox of painted tiles in level-local tile coords. Populated when
  // collision is first decoded; (0,0,0,0) if no collision payload exists.
  tightMinX: number;
  tightMinY: number;
  tightMaxX: number; // exclusive
  tightMaxY: number; // exclusive
  // Decoded collision (row-major, length = sizeX*sizeY). null until the level
  // is first rendered; stays null if the JSON had no collision payload.
  coll: Uint16Array | null;
  // Compressed payload kept alive so we can re-decode after a release.
  // null when the JSON had no collision payload to begin with.
  collDeflateB64: string | null;
  presets: PresetJson[];
  adjacents: AdjacentJson[];
  rooms: RoomJson[];
  tells: TellJson[];
}

export interface Seed {
  seed: number;
  levels: Level[];
}

// txtFileNo of the orifice — the marker for the one Tal Rasha's Tomb
// (levels 66-72) that holds the real Duriel encounter. The other six tombs
// are decoys with the same name/layout family; the orifice's presence is
// the only signal that distinguishes the "right" tomb.
export const ORIFICE_TXT_FILE_NO = 152;

// True iff this level contains the Horadric Staff orifice. Renderer
// surfaces use this to flag the right tomb with a star.
export function hasOrifice(level: Level): boolean {
  for (const p of level.presets) {
    if (p.type === "obj" && p.txtFileNo === ORIFICE_TXT_FILE_NO) return true;
  }
  return false;
}

export const NO_DATA = 0xffff;

// Collision WORD bit flags. Mirrors `enum CollisionFlag` in
// seedgen/src/mapdata.h — keep in sync.
export const COLL = {
  BlockWalk:        0x0001,
  BlockLineOfSight: 0x0002,
  Wall:             0x0004,
  BlockPlayer:      0x0008,
  AlternateTile:    0x0010,
  Blank:            0x0020,
  Missile:          0x0040,
  Player:           0x0080,
  NPCLocation:      0x0100,
  Item:             0x0200,
  Object:           0x0400,
  ClosedDoor:       0x0800,
  NPCCollision:     0x1000,
} as const;
