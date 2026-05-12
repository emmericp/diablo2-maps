// BFS from StairsUp over user-placed rooms, with N→E→S→W priority and
// shape-based connectivity. Must match seedgen/src/trielookup.cpp::BuildSequence
// exactly — the mock lookup keys the index the same way.

import { encodeRoom, findRoom, isStairsUp, SHAPE_EXIT_CHARS, type Shape } from "./rooms";

// One room the user has placed on the grid. Each grave slot exists in one
// of three states; we represent them as two 8-bit masks instead of two bits
// per slot so the bitmask still fits the low byte of the trie's uint16
// encoded value:
//   gravesUnknown bit i  →  slot i is in the "?" state (umask in the lookup)
//   graves bit i         →  slot i is OPEN (only meaningful when not unknown)
// Closed = neither bit set.
export interface PlacedRoom {
  cellX: number;            // 0..15
  cellY: number;            // 0..15
  roomId: number;
  variant: number;
  graves: number;           // bit i = slot i+1 OPEN
  gravesUnknown: number;    // bit i = slot i+1 UNKNOWN (server fans this out)
}

export const MAX_SEQ_SLOTS = 8;

export interface SequenceResult {
  // Encoded uint16 values in BFS visit order. Unknown grave bits are masked
  // to 0 in the value itself; the per-slot unknown mask lives in
  // `unknownMasks` so callers can build a /UMASK prefix for trielookup.
  seq: number[];
  unknownMasks: number[];   // same length as `seq`; bit i = unknown bit
  // Cells visited, in the same order — useful for highlighting the BFS path.
  path: { cellX: number; cellY: number }[];
  // Anchor cell (the StairsUp room), or null if no StairsUp placed yet.
  anchor: { cellX: number; cellY: number } | null;
}

interface Dir { dx: number; dy: number; ours: string; theirs: string }
const DIRS: Dir[] = [
  { dx:  0, dy: -1, ours: "N", theirs: "S" },
  { dx:  1, dy:  0, ours: "E", theirs: "W" },
  { dx:  0, dy:  1, ours: "S", theirs: "N" },
  { dx: -1, dy:  0, ours: "W", theirs: "E" },
];

export function buildSequence(rooms: PlacedRoom[]): SequenceResult {
  // 16×16 grid, encoded value + unknown mask at each occupied cell.
  const grid = new Int32Array(256).fill(-1);
  const gridUm = new Uint8Array(256);
  let start: { cellX: number; cellY: number } | null = null;
  for (const r of rooms) {
    const idx = (r.cellY << 4) | r.cellX;
    // Clear the unknown bits from the encoded value — they're not "0" they're
    // "?" and must match anything in the index.
    const cleanGraves = r.graves & ~r.gravesUnknown;
    const enc = encodeRoom(r.roomId, r.variant, cleanGraves);
    grid[idx] = enc;
    gridUm[idx] = r.gravesUnknown & 0xFF;
    if (start === null && isStairsUp(enc)) {
      start = { cellX: r.cellX, cellY: r.cellY };
    }
  }
  if (!start) return { seq: [], unknownMasks: [], path: [], anchor: null };

  const seen = new Uint8Array(256);
  const queue: { x: number; y: number }[] = [{ x: start.cellX, y: start.cellY }];
  seen[(start.cellY << 4) | start.cellX] = 1;
  const seq: number[] = [];
  const unknownMasks: number[] = [];
  const path: { cellX: number; cellY: number }[] = [];

  while (queue.length > 0 && seq.length < MAX_SEQ_SLOTS) {
    const c = queue.shift()!;
    const idx = (c.y << 4) | c.x;
    const here = grid[idx];
    // Real seeds have every BFS-priority neighbor filled — the C++ index
    // never sees a gap in the middle of a sequence. If the user's partial
    // layout hits a cell that BFS *would* visit but they haven't placed
    // anything there, the only safe answer is to stop and ship a proper
    // prefix. Continuing past the gap would skip to a later priority
    // neighbor (e.g. east-of-ESW after W-of-NEW is empty), producing a
    // sequence no real seed actually has and a 0-match lookup.
    if (here < 0) break;
    seq.push(here);
    unknownMasks.push(gridUm[idx]);
    path.push({ cellX: c.x, cellY: c.y });
    const meHi = here & 0xFF00;
    const meRoom = findRoom(((meHi >> 8) & 0x7F) + 100);
    if (!meRoom) continue;
    const myExits = SHAPE_EXIT_CHARS[meRoom.shape];
    for (const d of DIRS) {
      if (!myExits.has(d.ours)) continue;
      const nx = c.x + d.dx, ny = c.y + d.dy;
      if (nx < 0 || nx > 15 || ny < 0 || ny > 15) continue;
      const ni = (ny << 4) | nx;
      if (seen[ni]) continue;
      const nb = grid[ni];
      if (nb >= 0) {
        // Filled neighbor: only add if shape connects back.
        const nbHi = nb & 0xFF00;
        const nbRoom = findRoom(((nbHi >> 8) & 0x7F) + 100);
        if (!nbRoom) continue;
        if (!SHAPE_EXIT_CHARS[nbRoom.shape].has(d.theirs)) continue;
      }
      // Empty cells are added as queue placeholders. The pop-loop's
      // `if (here < 0) break` above truncates the sequence the moment one
      // of these comes off the front, which is exactly the priority-order
      // position the real-seed BFS would have visited next.
      seen[ni] = 1;
      queue.push({ x: nx, y: ny });
    }
  }
  return { seq, unknownMasks, path, anchor: start };
}

// True when at least one placed neighbor has an exit pointing toward this
// cell. Used to enable/disable grid cells: empty cells without an inbound
// exit can't be reached and shouldn't be clickable.
export function cellIsConnected(
  cellX: number, cellY: number, placed: Map<string, PlacedRoom>,
): boolean {
  for (const d of DIRS) {
    const nx = cellX + d.dx, ny = cellY + d.dy;
    const nb = placed.get(`${nx},${ny}`);
    if (!nb) continue;
    const def = findRoom(nb.roomId);
    if (!def) continue;
    if (SHAPE_EXIT_CHARS[def.shape].has(d.theirs)) return true;
  }
  return false;
}

// True when `shape` is consistent with every placed neighbor of the target
// cell: an exit on side X is required iff the neighbor on side X has its
// own exit pointing back; otherwise an exit on side X would dangle into a
// wall, which never appears in real maps.
export function shapeValidForCell(
  shape: Shape,
  cellX: number, cellY: number,
  placed: Map<string, PlacedRoom>,
): boolean {
  const candidateExits = SHAPE_EXIT_CHARS[shape];
  for (const d of DIRS) {
    const nx = cellX + d.dx, ny = cellY + d.dy;
    const nb = placed.get(`${nx},${ny}`);
    if (!nb) continue;
    const def = findRoom(nb.roomId);
    if (!def) continue;
    const neighborOpens = SHAPE_EXIT_CHARS[def.shape].has(d.theirs);
    const mineOpens     = candidateExits.has(d.ours);
    if (neighborOpens !== mineOpens) return false;
  }
  return true;
}

// "Next cell to fill" hint for the BFS-nudge UI: the first unfilled neighbor
// of any visited room that would extend the BFS by one step. Returns null
// when nothing's placed yet or every reachable neighbor cell is already
// occupied.
export function nextBfsCell(
  rooms: PlacedRoom[],
): { cellX: number; cellY: number } | null {
  const occ = new Uint8Array(256);
  for (const r of rooms) occ[(r.cellY << 4) | r.cellX] = 1;
  const res = buildSequence(rooms);
  if (!res.anchor) return null;
  for (const p of res.path) {
    const here = rooms.find((r) => r.cellX === p.cellX && r.cellY === p.cellY)!;
    const def = findRoom(here.roomId);
    if (!def) continue;
    const exits = SHAPE_EXIT_CHARS[def.shape];
    for (const d of DIRS) {
      if (!exits.has(d.ours)) continue;
      const nx = p.cellX + d.dx, ny = p.cellY + d.dy;
      if (nx < 0 || nx > 15 || ny < 0 || ny > 15) continue;
      if (occ[(ny << 4) | nx]) continue;
      return { cellX: nx, cellY: ny };
    }
  }
  return null;
}
