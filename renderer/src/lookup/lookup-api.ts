// Real backend client for /api/lookup. Falls back to the mock implementation
// when the API isn't configured server-side (returns 404) or when running
// against a dev server without a mapserver behind /api.

import { type PlacedRoom, buildSequence } from "./sequence";
import {
  mockLookup, LOOKUP_TRIGGER_THRESHOLD, LOOKUP_SAMPLE_THRESHOLD,
  FILTER_RENDER_CAP_NO_PARITY, FILTER_RENDER_CAP_WITH_PARITY,
  type LookupResponse,
} from "./lookup-mock";

// Re-export trigger thresholds + server-side render caps so main.ts doesn't
// have to know which backend it ended up using.
export {
  LOOKUP_TRIGGER_THRESHOLD, LOOKUP_SAMPLE_THRESHOLD,
  FILTER_RENDER_CAP_NO_PARITY, FILTER_RENDER_CAP_WITH_PARITY,
};
export type { LookupResponse };

// Cached on first probe: true ⇒ /api/lookup answered; false ⇒ 404 / network
// error / non-JSON; null ⇒ not probed yet.
let apiUp: boolean | null = null;

// Build the prefix string the backend expects. Each token is 4-char hex
// optionally followed by `/UMASK` (also 4-char hex). UMASK bits mark
// "unknown" slots — trielookup fans them out internally.
function seqToPrefix(seq: number[], unknownMasks: number[]): string {
  return seq
    .map((v, i) => {
      const base = v.toString(16).toUpperCase().padStart(4, "0");
      const um = unknownMasks[i] ?? 0;
      return um === 0
        ? base
        : `${base}/${um.toString(16).toUpperCase().padStart(4, "0")}`;
    })
    .join(",");
}

export async function lookup(rooms: PlacedRoom[]): Promise<LookupResponse> {
  const { seq, unknownMasks } = buildSequence(rooms);
  if (seq.length < LOOKUP_TRIGGER_THRESHOLD) {
    return { count: -1, triggered: false };
  }

  // Once we know the API is down for this session, skip the network round-trip.
  if (apiUp === false) return mockLookup(rooms);

  const prefix = seqToPrefix(seq, unknownMasks);
  const url = `/api/lookup?prefix=${encodeURIComponent(prefix)}&list=${LOOKUP_SAMPLE_THRESHOLD}`;
  try {
    const r = await fetch(url, { headers: { Accept: "application/json" } });
    if (r.status === 404) {
      apiUp = false;
      return mockLookup(rooms);
    }
    if (!r.ok) {
      throw new Error(`HTTP ${r.status}: ${await r.text()}`);
    }
    const j = await r.json() as {
      matchCount: number;
      seeds?: number[];
    };
    apiUp = true;
    const resp: LookupResponse = { count: j.matchCount, triggered: true };
    if (j.seeds && j.seeds.length > 0 && j.matchCount <= LOOKUP_SAMPLE_THRESHOLD) {
      resp.sampleSeeds = j.seeds;
    }
    return resp;
  } catch (e) {
    console.warn("[lookup-api] falling back to mock:", e);
    apiUp = false;
    return mockLookup(rooms);
  }
}
