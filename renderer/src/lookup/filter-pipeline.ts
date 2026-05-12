// Thin client for /api/filter. The full multi-stage pipeline (lookup → parity
// → stairs → townexit) is implemented on the server; the browser just sends
// the BFS prefix and the user's directional answers and receives a small
// final-seed list. No tell logic lives here.

import { buildSequence, type PlacedRoom } from "./sequence";

export type Dir = "N" | "E" | "S" | "W";

export interface UserAnswers {
  stairs1?: Dir;
  stairs2?: Dir;
  stairs3?: Dir;
  level5?:  "N" | "W";        // top-right (even) or top-left (odd)
  townExit?: Dir;
}

export interface FilterResponse {
  lookup: number;
  after_parity?: number;
  after_filters?: number;
  final_count: number;
  seeds: number[];
  truncated?: boolean;
  warning?: string;
}

// Stringify each slot as either `XXXX` or `XXXX/YYYY` per the trielookup
// prefix syntax — `YYYY` marks the bits whose state we don't know (umask)
// and trielookup fans those out internally.
function seqToPrefix(seq: number[], unknownMasks: number[]): string {
  return seq.map((v, i) => {
    const base = v.toString(16).toUpperCase().padStart(4, "0");
    const um = unknownMasks[i] ?? 0;
    if (um === 0) return base;
    return `${base}/${um.toString(16).toUpperCase().padStart(4, "0")}`;
  }).join(",");
}

// Build URL params for /api/filter. Each filter field is optional — the
// server treats "" as "skip this stage".
function buildFilterUrl(rooms: PlacedRoom[], answers: UserAnswers, limit: number): string {
  const { seq, unknownMasks } = buildSequence(rooms);
  const params = new URLSearchParams();
  params.set("prefix", seqToPrefix(seq, unknownMasks));
  if (answers.level5) {
    // Contract pinned in the server: north == even, west == odd.
    params.set("parity", answers.level5 === "N" ? "even" : "odd");
  }
  if (answers.stairs1) params.set("stairs1", answers.stairs1);
  if (answers.stairs2) params.set("stairs2", answers.stairs2);
  if (answers.stairs3) params.set("stairs3", answers.stairs3);
  if (answers.townExit) params.set("townexit", answers.townExit);
  params.set("limit", String(limit));
  return `/api/filter?${params.toString()}`;
}

export async function runFilter(
  rooms: PlacedRoom[],
  answers: UserAnswers,
  limit = 64,
  signal?: AbortSignal,
): Promise<FilterResponse> {
  const url = buildFilterUrl(rooms, answers, limit);
  const r = await fetch(url, { signal });
  if (!r.ok) {
    const text = await r.text();
    throw new Error(`/api/filter: HTTP ${r.status}: ${text.trim()}`);
  }
  return r.json();
}
