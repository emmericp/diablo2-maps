// Group a seed's levels into render units, then group those into acts.
//
// Render unit: a set of levels that should be drawn on the same canvas.
// Two levels join the same unit when either:
//   (a) they have a Room2-border adjacency between them AND no stairs
//       preset connects them — i.e. they're truly part of the same
//       continuous-world chunk (e.g. Stony Field ↔ Cold Plains), OR
//   (b) they share a stripped-of-trailing-digits name within the same act
//       — i.e. they're floors of the same logical dungeon (e.g.
//       CaveLevel1 + CaveLevel2; TowerCellarLevel1..5;
//       TalRashasTomb1..7). These are connected by stairs in the game,
//       but the viewer treats them as one unit so the sidebar reads
//       uniformly: every leaf is a unit, never a one-floor-per-line list.

import { type Level, type Seed } from "./types";

export interface RenderUnit {
  id: number; // root level number from union-find
  name: string; // stripped internal name of the lowest-numbered level
  // Stripped localized display name (e.g. "Cave" from "Cave Level 1").
  // Falls back to `name` when no displayName is available.
  displayName: string;
  levels: Level[];
  // True when this unit was formed (in part) by the same-name pass —
  // i.e. its members are stairs-linked floors of one logical dungeon
  // rather than a continuous wilderness chain. Such units typically have
  // their floors at far-apart world origins, so the camera should zoom
  // to the first floor rather than fit-view the bounding box of all of
  // them (which would be uselessly zoomed out).
  viaStairs: boolean;
}

export interface ActGroup {
  act: number;
  units: RenderUnit[];
}

export interface Hierarchy {
  acts: ActGroup[];
  unitById: Map<number, RenderUnit>;
  unitOfLevel: Map<number, number>;
}

export function computeHierarchy(seed: Seed): Hierarchy {
  const present = new Set(seed.levels.map((l) => l.levelNo));
  const stairs = stairsPairs(seed);

  const parent = new Map<number, number>();
  for (const l of seed.levels) parent.set(l.levelNo, l.levelNo);

  const find = (x: number): number => {
    let r = x;
    while (parent.get(r)! !== r) r = parent.get(r)!;
    let c = x;
    while (parent.get(c)! !== r) {
      const next = parent.get(c)!;
      parent.set(c, r);
      c = next;
    }
    return r;
  };
  const union = (a: number, b: number) => {
    const ra = find(a);
    const rb = find(b);
    if (ra !== rb) parent.set(ra, rb);
  };

  // Pass 1: real wilderness adjacencies (skip stairs-mediated pairs).
  for (const l of seed.levels) {
    for (const a of l.adjacents) {
      if (!present.has(a.levelNo)) continue;
      if (stairs.has(pairKey(l.levelNo, a.levelNo))) continue;
      union(l.levelNo, a.levelNo);
    }
  }

  // Pass 2: same logical dungeon — same stripped name within an act.
  const byNameKey = new Map<string, number[]>();
  for (const l of seed.levels) {
    const key = `${l.act}\0${stripLevelSuffix(l.name)}`;
    let arr = byNameKey.get(key);
    if (!arr) {
      arr = [];
      byNameKey.set(key, arr);
    }
    arr.push(l.levelNo);
  }
  for (const lvlNos of byNameKey.values()) {
    for (let i = 1; i < lvlNos.length; i++) {
      union(lvlNos[0]!, lvlNos[i]!);
    }
  }

  // Materialize units.
  const buckets = new Map<number, Level[]>();
  for (const l of seed.levels) {
    const r = find(l.levelNo);
    let g = buckets.get(r);
    if (!g) {
      g = [];
      buckets.set(r, g);
    }
    g.push(l);
  }

  const units: RenderUnit[] = [];
  for (const [id, levels] of buckets) {
    levels.sort((a, b) => a.levelNo - b.levelNo);
    // Multiple levels in one unit sharing the same stripped name proves
    // pass 2 merged them — the wilderness pass-1 only ever merges levels
    // with distinct names.
    const distinctNames = new Set(levels.map((l) => stripLevelSuffix(l.name)));
    const viaStairs = distinctNames.size < levels.length;
    const firstDisplay = levels[0]!.displayName ?? levels[0]!.name;
    units.push({
      id,
      name: stripLevelSuffix(levels[0]!.name),
      displayName: stripDisplayLevelSuffix(firstDisplay),
      levels,
      viaStairs,
    });
  }

  const unitById = new Map<number, RenderUnit>();
  const unitOfLevel = new Map<number, number>();
  for (const u of units) {
    unitById.set(u.id, u);
    for (const l of u.levels) unitOfLevel.set(l.levelNo, u.id);
  }

  // Group by act.
  const actMap = new Map<number, RenderUnit[]>();
  for (const u of units) {
    const act = u.levels[0]!.act;
    let arr = actMap.get(act);
    if (!arr) {
      arr = [];
      actMap.set(act, arr);
    }
    arr.push(u);
  }

  const acts: ActGroup[] = [];
  for (const [act, unitList] of actMap) {
    unitList.sort((a, b) => (a.levels[0]?.levelNo ?? 0) - (b.levels[0]?.levelNo ?? 0));
    acts.push({ act, units: unitList });
  }
  acts.sort((a, b) => a.act - b.act);

  return { acts, unitById, unitOfLevel };
}

// Stair pairs where the *higher* levelNo sits at higher physical elevation,
// overriding the default "lower id = above" assumption. Hand-curated from
// in-game observation; add new pairs here when a stair renders the wrong way.
//
// Within a multi-floor dungeon (Jail, Catacombs, TowerCellar, ...) the lower
// id is the entry floor and sits above the deeper floors, so the default
// already handles it. These exceptions are crossings where a deep floor
// exits *back up* to a surface area whose id happens to be higher (Jail3→
// InnerCloister) or where the act-5 mountain ascent continues into a higher
// dungeon (TheAncientsWay→WorldstoneKeep).
const STAIR_HEIGHT_OVERRIDES: ReadonlySet<string> = new Set([
  pairKey(31, 32),   // JailLevel3            ↔ InnerCloister
  pairKey(51, 52),   // HaremLevel2           ↔ PalaceCellarLevel1
  pairKey(115, 117), // CrystalizedCavernLvl2 ↔ FrozenTundra
  pairKey(118, 120), // CrystallinePassage    ↔ TheAncientsWay
  pairKey(120, 128), // TheAncientsWay        ↔ WorldstoneKeepLevel1
]);

// Classify a stair preset as leading "up" or "down". Default: between two
// connected levels, the lower levelNo is the higher-elevation side, so going
// from the lower id to the higher id is going down. STAIR_HEIGHT_OVERRIDES
// flips that for known exceptions.
export function classifyStairDirection(
  srcLevelNo: number,
  destLevelNo: number,
): "up" | "down" | null {
  if (destLevelNo === srcLevelNo) return null;
  const lo = Math.min(srcLevelNo, destLevelNo);
  const hi = Math.max(srcLevelNo, destLevelNo);
  const higher = STAIR_HEIGHT_OVERRIDES.has(pairKey(lo, hi)) ? hi : lo;
  return srcLevelNo === higher ? "down" : "up";
}

// Build the set of {min,max} level pairs that have a stairs preset between
// them. Direction-agnostic so we treat A→B and B→A as the same link.
function stairsPairs(seed: Seed): Set<string> {
  const out = new Set<string>();
  for (const lvl of seed.levels) {
    for (const p of lvl.presets) {
      if (p.type === "exit" && p.destLevelNo) {
        out.add(pairKey(lvl.levelNo, p.destLevelNo));
      }
    }
  }
  return out;
}

function pairKey(a: number, b: number): string {
  return a < b ? `${a}:${b}` : `${b}:${a}`;
}

// Strip a trailing "Level\d+" or bare "\d+" (with optional "Act\d+" tail)
// so dungeon floors collapse onto a single name. Names without a digit
// suffix — "Tristram", "DenOfEvil", "CowLevel" — are returned unchanged.
export function stripLevelSuffix(name: string): string {
  return name.replace(/(?:Level)?\d+(?:Act\d+)?$/, "");
}

// Same idea for the human-readable display name. Strips a trailing
// " Level N" (e.g. "Cave Level 1" -> "Cave", "Tower Cellar Level 3" ->
// "Tower Cellar"). Names without that suffix come through unchanged.
export function stripDisplayLevelSuffix(name: string): string {
  return name.replace(/\s+Level\s+\d+$/, "");
}
