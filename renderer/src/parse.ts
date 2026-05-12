import {
  NO_DATA,
  type Level,
  type LevelJson,
  type Seed,
  type SeedJson,
} from "./types";

export function parseSeed(json: SeedJson): Seed {
  return {
    seed: json.seed >>> 0,
    levels: json.levels.map(parseLevel),
  };
}

function parseLevel(j: LevelJson): Level {
  // Collision payload is kept compressed here. The tight bbox is computed
  // lazily on first ensureCollision() call, since computing it requires the
  // decoded grid we are explicitly deferring.
  return {
    levelNo: j.levelNo,
    name: j.name,
    displayName: j.displayName,
    act: j.act,
    originX: j.origin[0],
    originY: j.origin[1],
    sizeX: j.collisionWidth ?? j.size[0],
    sizeY: j.collisionHeight ?? j.size[1],
    tightMinX: 0,
    tightMinY: 0,
    tightMaxX: 0,
    tightMaxY: 0,
    coll: null,
    collDeflateB64: j.collisionDeflateB64 ?? null,
    presets: j.presets ?? [],
    adjacents: j.adjacents ?? [],
    rooms: j.rooms ?? [],
    tells: j.tells ?? [],
  };
}

// Lazily decompress and cache a level's collision grid. Returns the same
// Uint16Array on subsequent calls. Resolves to null for levels that never
// had a collision payload in the JSON.
//
// Decompression uses the browser-native DecompressionStream("deflate"), so
// no JS dependency is needed. The compressed blob stays attached to the
// Level so we can re-decode after a release; current code never releases.
const inFlight = new WeakMap<Level, Promise<Uint16Array | null>>();

export function ensureCollision(level: Level): Promise<Uint16Array | null> {
  if (level.coll) return Promise.resolve(level.coll);
  if (!level.collDeflateB64) return Promise.resolve(null);
  const existing = inFlight.get(level);
  if (existing) return existing;
  const p = decodeCollision(level.collDeflateB64, level.sizeX * level.sizeY)
    .then((arr) => {
      level.coll = arr;
      const bbox = computeTightBbox(arr, level.sizeX, level.sizeY);
      level.tightMinX = bbox.minX;
      level.tightMinY = bbox.minY;
      level.tightMaxX = bbox.maxX;
      level.tightMaxY = bbox.maxY;
      inFlight.delete(level);
      return arr;
    })
    .catch((e) => {
      inFlight.delete(level);
      throw e;
    });
  inFlight.set(level, p);
  return p;
}

async function decodeCollision(
  b64: string,
  expected: number,
): Promise<Uint16Array> {
  const compressed = base64ToBytes(b64);
  const stream = new Blob([compressed])
    .stream()
    .pipeThrough(new DecompressionStream("deflate"));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());

  // The decompressed buffer should be exactly 2*expected bytes. Aliasing
  // bytes.buffer would only work when byteOffset is 2-aligned (Response
  // gives us a fresh buffer at offset 0, so it always is), but copy anyway
  // so callers can keep this around without holding the original ArrayBuffer.
  const out = new Uint16Array(expected);
  const view = new Uint16Array(
    bytes.buffer,
    bytes.byteOffset,
    Math.min(expected, Math.floor(bytes.byteLength / 2)),
  );
  if (view.length === expected) {
    out.set(view);
  } else {
    out.fill(NO_DATA);
    out.set(view);
  }
  return out;
}

// Returns a Uint8Array whose backing store is a plain ArrayBuffer (not
// SharedArrayBuffer). The explicit ArrayBuffer construction keeps TS 5.6's
// narrower BufferSource typing happy when we feed this into Blob/Response.
function base64ToBytes(b64: string): Uint8Array<ArrayBuffer> {
  const bin = atob(b64);
  const len = bin.length;
  const buf = new ArrayBuffer(len);
  const bytes = new Uint8Array(buf);
  for (let i = 0; i < len; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

function computeTightBbox(
  coll: Uint16Array,
  w: number,
  h: number,
): { minX: number; minY: number; maxX: number; maxY: number } {
  let minX = w,
    minY = h,
    maxX = 0,
    maxY = 0;
  let any = false;
  for (let y = 0; y < h; y++) {
    const row = y * w;
    for (let x = 0; x < w; x++) {
      if (coll[row + x] !== NO_DATA) {
        if (!any) {
          minX = x;
          minY = y;
          maxX = x + 1;
          maxY = y + 1;
          any = true;
        } else {
          if (x < minX) minX = x;
          if (y < minY) minY = y;
          if (x + 1 > maxX) maxX = x + 1;
          if (y + 1 > maxY) maxY = y + 1;
        }
      }
    }
  }
  if (!any) return { minX: 0, minY: 0, maxX: 0, maxY: 0 };
  return { minX, minY, maxX, maxY };
}
