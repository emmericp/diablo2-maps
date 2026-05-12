// Color tables for the HTML viewer.

import { COLL, NO_DATA, type ObjectKind, type PresetJson } from "./types";

export interface RGB {
  r: number;
  g: number;
  b: number;
}

export const COLORS = {
  // Off-map / no-data background — was near-black, now mid gray to make
  // walls (now black) read as the strong feature instead.
  bg: { r: 60, g: 60, b: 60 },
  // Collision is just background — keep it grayscale so preset markers carry
  // the visual weight. Water sits slightly *darker* than bg because it
  // typically forms the outer frame around playable areas, and we want it
  // to read as "edge of map" rather than an interior feature.
  floor:     { r: 235, g: 235, b: 235 },
  alternate: { r: 200, g: 200, b: 200 },
  wall:      { r:  15, g:  15, b:  15 },
  water:     { r:  45, g:  45, b:  45 },

  exit: { r: 30, g: 80, b: 255 },
  npc: { r: 230, g: 50, b: 50 },
  // Was bright cyan {0,220,230} — too bright against the near-white floor.
  // Darker teal still reads as cyan-family but stands out on the map.
  waypoint: { r: 0, g: 140, b: 180 },
  shrine: { r: 220, g: 60, b: 220 },
  well: { r: 255, g: 140, b: 0 },
  superChest: { r: 255, g: 230, b: 60 },
  // Chest tracks Generic — same family, slightly darker so the two are
  // distinguishable without making either stand out.
  chest: { r: 195, g: 195, b: 195 },
  // Was olive {160,160,60} — moved here from Generic. Olive reads as
  // "treasure / scroll" without competing with the new SuperChest yellow.
  quest: { r: 160, g: 160, b: 60 },
  door: { r: 100, g: 90, b: 60 },
  // Was the same olive as Quest — swapped for a hard-to-spot light gray.
  // Generic markers are everywhere; intentionally subdued so the colored
  // markers (Waypoint, Quest, NPC, Shrine) carry the visual weight.
  generic: { r: 220, g: 220, b: 220 },
  stairs: { r: 30, g: 80, b: 255 },
} satisfies Record<string, RGB>;

export function rgb(c: RGB, alpha = 1): string {
  return `rgba(${c.r},${c.g},${c.b},${alpha})`;
}

// --- Collision classification --------------------------------------------

export type CollisionKind = "floor" | "alternate" | "wall" | "water";

export interface CollisionClass {
  kind: CollisionKind;
  color: RGB;
}

// Buckets a raw collision WORD into a visual category:
//   !BlockWalk → floor; Wall flag → wall; else → water (blocks walk for
//   some other reason — water, pit, ledge, …).
export function classifyCollision(v: number): CollisionClass | null {
  if (v === NO_DATA) return null;
  if ((v & COLL.BlockWalk) === 0) {
    if ((v & COLL.AlternateTile) !== 0) return { kind: "alternate", color: COLORS.alternate };
    return { kind: "floor", color: COLORS.floor };
  }
  if ((v & COLL.Wall) !== 0)      return { kind: "wall",  color: COLORS.wall  };
  return { kind: "water", color: COLORS.water };
}

// --- Marker classification -----------------------------------------------

export type MarkerKind =
  | "exit"
  | "npc"
  | "waypoint"
  | "shrine"
  | "well"
  | "superChest"
  | "chest"
  | "quest"
  | "door"
  | "generic";

export interface PresetMarker {
  kind: MarkerKind;
  color: RGB;
  // Radius in *tile units* (the canvas draw scales these to world units).
  radius: number;
  // Higher = drawn on top.
  priority: number;
  label: string;
}

// Marker sizes in tile units. Every "important" marker (exits, waypoints,
// shrines, wells, quest, super chest, big-NPC quest bosses) uses BIG_RADIUS
// so they all read at a consistent visual weight on the map; minor things
// (regular chests, generic objects, regular NPCs) drop to SMALL_RADIUS so
// they don't compete for attention.
const BIG_RADIUS = 5;
const SMALL_RADIUS = 2;

// MonStats.txt hcIdx values for quest-boss enemies we draw enlarged on the
// map. Comments give the NameStr from MonStats.txt.
const BIG_NPC_TXT_NOS = new Set<number>([
  250, // Summoner    - Arcane Sanctuary boss
  256, // izual       - Plains of Despair boss
  543, // baalthrone  - Throne of Destruction
]);

export function markerFor(p: PresetJson): PresetMarker {
  if (p.type === "exit") {
    return { kind: "exit", color: COLORS.exit, radius: BIG_RADIUS, priority: 30, label: "Exit" };
  }
  if (p.type === "npc") {
    const big = BIG_NPC_TXT_NOS.has(p.txtFileNo);
    return {
      kind: "npc",
      color: COLORS.npc,
      radius: big ? BIG_RADIUS : SMALL_RADIUS,
      priority: big ? 35 : 10,
      label: "NPC",
    };
  }
  const k: ObjectKind = p.kind ?? "Generic";
  switch (k) {
    case "Waypoint":
      return { kind: "waypoint", color: COLORS.waypoint, radius: BIG_RADIUS, priority: 40, label: "Waypoint" };
    case "Shrine":
      return { kind: "shrine", color: COLORS.shrine, radius: BIG_RADIUS, priority: 40, label: "Shrine" };
    case "Well":
      return { kind: "well", color: COLORS.well, radius: BIG_RADIUS, priority: 40, label: "Well" };
    case "SuperChest":
      return { kind: "superChest", color: COLORS.superChest, radius: BIG_RADIUS, priority: 40, label: "Super chest" };
    case "Quest":
      return { kind: "quest", color: COLORS.quest, radius: BIG_RADIUS, priority: 40, label: "Quest" };
    case "Chest":
      return { kind: "chest", color: COLORS.chest, radius: SMALL_RADIUS, priority: 20, label: "Chest" };
    case "Door":
      return { kind: "door", color: COLORS.door, radius: 1, priority: 20, label: "Door" };
    case "Stairs":
      // Stairs are drawn the same as exits — same color, treated as same toggle.
      return { kind: "exit", color: COLORS.stairs, radius: BIG_RADIUS, priority: 30, label: "Stairs" };
    default:
      return { kind: "generic", color: COLORS.generic, radius: SMALL_RADIUS, priority: 5, label: "Object" };
  }
}

// --- Visibility (legend toggles) ------------------------------------------

export interface LegendVisibility {
  collision: Record<CollisionKind, boolean>;
  markers: Record<MarkerKind, boolean>;
}

export function defaultLegendVisibility(): LegendVisibility {
  return {
    collision: { floor: true, alternate: true, wall: true, water: true },
    markers: {
      exit: true,
      npc: true,
      waypoint: true,
      shrine: true,
      well: true,
      superChest: true,
      chest: true,
      quest: true,
      door: true,
      generic: true,
    },
  };
}

export const COLLISION_LEGEND: { id: CollisionKind; label: string; color: RGB }[] = [
  { id: "floor",     label: "Floor",       color: COLORS.floor     },
  { id: "alternate", label: "Alt. floor",  color: COLORS.alternate },
  { id: "wall",      label: "Wall",        color: COLORS.wall      },
  { id: "water",     label: "Water / pit", color: COLORS.water     },
];

export const MARKER_LEGEND: { id: MarkerKind; label: string; color: RGB }[] = [
  { id: "exit", label: "Exit / Stairs", color: COLORS.exit },
  { id: "npc", label: "NPC", color: COLORS.npc },
  { id: "waypoint", label: "Waypoint", color: COLORS.waypoint },
  { id: "shrine", label: "Shrine", color: COLORS.shrine },
  { id: "well", label: "Well", color: COLORS.well },
  { id: "superChest", label: "Super chest", color: COLORS.superChest },
  { id: "quest", label: "Quest", color: COLORS.quest },
  { id: "chest", label: "Chest", color: COLORS.chest },
  { id: "door", label: "Door", color: COLORS.door },
  { id: "generic", label: "Generic obj.", color: COLORS.generic },
];
