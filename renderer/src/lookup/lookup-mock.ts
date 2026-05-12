// Fake lookup backend. Returns a plausible-looking candidate count for a
// given prefix length, so the UI can demo the "narrowing" feel. Wire this
// to the real /api/lookup endpoint when the backend lands.

import { type PlacedRoom, buildSequence } from "./sequence";

// Approximate distinct-room-variant cardinalities at each BFS position. The
// first slot is always StairsUp (4 orientations), so n ≈ 4. After that the
// branching factor grows, then collapses as the layout closes in on a
// concrete sequence. These multipliers were eyeballed from the real index
// collision histogram on level 24.
const BRANCHING = [4, 8, 12, 14, 12, 8, 5, 3];
const TOTAL_SEEDS = 563_099_040;        // approx — matches the built level24.idx

export interface LookupResponse {
  count: number;
  triggered: boolean;             // false when prefix too short to be worth asking
  sampleSeeds?: number[];         // only filled when count is small enough
}

// Trigger thresholds — keeps load off the (eventual) real backend.
const MIN_PREFIX_LEN_TO_QUERY = 3;        // run lookup once we have ≥ this many rooms in BFS
const SHOW_SAMPLE_BELOW = 50;              // show actual seeds once we drop below this

export function mockLookup(rooms: PlacedRoom[]): LookupResponse {
  const { seq } = buildSequence(rooms);
  if (seq.length < MIN_PREFIX_LEN_TO_QUERY) {
    return { count: -1, triggered: false };
  }
  let denom = 1;
  for (let i = 0; i < seq.length; i++) {
    denom *= BRANCHING[Math.min(i, BRANCHING.length - 1)];
  }
  // Spread evenly across the cardinality estimate, with a floor of 1.
  const count = Math.max(1, Math.round(TOTAL_SEEDS / denom));
  const resp: LookupResponse = { count, triggered: true };
  if (count <= SHOW_SAMPLE_BELOW) {
    // Deterministic fake seeds derived from the sequence so reload-the-page
    // gives the same demo set.
    const seed0 = hashSeq(seq);
    const samples: number[] = [];
    for (let i = 0; i < count; i++) {
      samples.push((seed0 + i * 0x9E3779B1) >>> 1);   // squash to 31-bit
    }
    resp.sampleSeeds = samples;
  }
  return resp;
}

export const LOOKUP_TRIGGER_THRESHOLD = MIN_PREFIX_LEN_TO_QUERY;
export const LOOKUP_SAMPLE_THRESHOLD = SHOW_SAMPLE_BELOW;

// Server-side per-seed render budget. /api/filter refuses to run the
// renders past these caps:
//   • no parity given          → at most 100 candidates
//   • parity (Countess) given  → at most 200 candidates (halved server-side)
// The renderer mirrors the gate so the user sees the right button state
// instead of finding out via a 400. Bump both values together in
// mapserver/filter.go to keep client and server in agreement.
export const FILTER_RENDER_CAP_NO_PARITY   = 100;
export const FILTER_RENDER_CAP_WITH_PARITY = 200;

function hashSeq(seq: number[]): number {
  let h = 0x9E3779B1;
  for (const v of seq) {
    h = Math.imul(h ^ v, 0x85EBCA6B);
    h ^= h >>> 13;
  }
  return h >>> 0;
}
