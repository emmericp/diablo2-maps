// Loads the tower room atlas (produced by `mapdump.exe roomatlas`) and
// renders each canonical (roomId, variant) example as an iso canvas using
// the same color logic as the main map renderer.
//
// The atlas captures the room with graves==0 (all "closed"); grave overlays
// for "open" slots are layered on top of this base canvas at draw time so
// the user's clicks toggle visibly without re-rendering the whole room.

import { classifyCollision } from "../colors";
import { NO_DATA } from "../types";

export interface AtlasEntry {
  roomId: number;
  variant: number;
  shape: string;
  theme: string;
  variantName: string;
  exampleSeed: number;
  exampleLevelId: number;
  roomX: number;
  roomY: number;
  collDeflateB64: string;
}

export interface AtlasData {
  tileSize: number;
  rooms: AtlasEntry[];
  byKey: Map<string, AtlasEntry>;     // key = "roomId_variant"
}

export const atlasKey = (roomId: number, variant: number) => `${roomId}_${variant}`;

let loaded: AtlasData | null = null;
let inflight: Promise<AtlasData> | null = null;

export function getAtlasSync(): AtlasData | null {
  return loaded;
}

export async function loadAtlas(url = "/tower-atlas.json"): Promise<AtlasData> {
  if (loaded) return loaded;
  if (inflight) return inflight;
  inflight = (async () => {
    const json = await fetch(url).then((r) => r.json());
    const byKey = new Map<string, AtlasEntry>();
    for (const r of json.rooms as AtlasEntry[]) byKey.set(atlasKey(r.roomId, r.variant), r);
    loaded = { tileSize: json.tileSize ?? 40, rooms: json.rooms, byKey };
    return loaded;
  })();
  return inflight;
}

// ---- Collision decode + iso canvas rendering ---------------------------

const cachedColl = new Map<string, Uint16Array>();
const cachedCanvas = new Map<string, HTMLCanvasElement>();

export async function getRoomColl(entry: AtlasEntry, tileSize: number): Promise<Uint16Array> {
  const key = atlasKey(entry.roomId, entry.variant);
  const cached = cachedColl.get(key);
  if (cached) return cached;
  const coll = await decodeCollision(entry.collDeflateB64, tileSize * tileSize);
  cachedColl.set(key, coll);
  return coll;
}

// Pixel size of one game tile in the rasterized iso atlas. The original
// texture.ts uses 2×2 stamps (STAMP_W = STAMP_H = 2). Bumping to 4 doubles
// each axis of the cached atlas canvas (160×80 → 320×160 for a 40-tile
// room), which gives crisper grid cells once the user zooms in past 1×.
// Memory cost: 4× per atlas entry, ~10 MB total across the 52-room atlas
// — fine.
const STAMP_W = 4;
const STAMP_H = 4;

// Native iso canvas dims big enough to fit every stamp. Adjacent stamps in
// the x-direction shift by (STAMP_W, STAMP_H/2) pixels; in the y-direction
// by (-STAMP_W, STAMP_H/2) — the half-height overlap is what produces the
// iso diamond mosaic.
export function isoCanvasSize(tileSize: number) {
  return { w: 2 * tileSize * STAMP_W, h: tileSize * STAMP_H };
}

// Project a tile-space point (lx, ly) into iso pixel coords inside an
// isoCanvasSize canvas. Mirrors texture.ts::buildIso (just scaled).
export function tileToIsoPx(lx: number, ly: number, tileSize: number): [number, number] {
  const isoX = (lx - ly) + (tileSize - 1);
  const isoY = lx + ly;
  return [isoX * STAMP_W, isoY * (STAMP_H / 2)];
}

export async function getRoomCanvas(entry: AtlasEntry, tileSize: number): Promise<HTMLCanvasElement> {
  const key = atlasKey(entry.roomId, entry.variant);
  const cached = cachedCanvas.get(key);
  if (cached) return cached;
  const coll = await getRoomColl(entry, tileSize);
  const c = renderRoomIso(coll, tileSize);
  cachedCanvas.set(key, c);
  return c;
}

// Render one room's 40×40 collision grid as an iso canvas, using the main
// renderer's color classification. Output canvas is the native iso size
// (2 * tileSize * ISO_PX_PER_TILE wide, 2 * tileSize tall).
export function renderRoomIso(coll: Uint16Array, tileSize: number): HTMLCanvasElement {
  const { w: destW, h: destH } = isoCanvasSize(tileSize);
  const canvas = document.createElement("canvas");
  canvas.width = destW;
  canvas.height = destH;
  const ctx = canvas.getContext("2d", { willReadFrequently: false })!;
  const img = ctx.createImageData(destW, destH);
  const data = img.data;

  for (let ly = 0; ly < tileSize; ly++) {
    for (let lx = 0; lx < tileSize; lx++) {
      const v = coll[ly * tileSize + lx];
      if (v === NO_DATA) continue;
      const cls = classifyCollision(v);
      if (!cls) continue;
      const [px, py] = tileToIsoPx(lx, ly, tileSize);
      stamp(data, destW, destH, px, py, cls.color.r, cls.color.g, cls.color.b);
    }
  }
  ctx.putImageData(img, 0, 0);
  return canvas;
}

function stamp(
  data: Uint8ClampedArray, destW: number, destH: number,
  px: number, py: number, r: number, g: number, b: number,
) {
  for (let dy = 0; dy < STAMP_H; dy++) {
    const yy = py + dy;
    if (yy < 0 || yy >= destH) continue;
    for (let dx = 0; dx < STAMP_W; dx++) {
      const xx = px + dx;
      if (xx < 0 || xx >= destW) continue;
      const di = (yy * destW + xx) * 4;
      data[di + 0] = r;
      data[di + 1] = g;
      data[di + 2] = b;
      data[di + 3] = 255;
    }
  }
}

// ---- Base64-deflate decode (mirrors parse.ts::decodeCollision) ---------

async function decodeCollision(b64: string, expected: number): Promise<Uint16Array> {
  const compressed = base64ToBytes(b64);
  const stream = new Blob([compressed])
    .stream()
    .pipeThrough(new DecompressionStream("deflate"));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  const out = new Uint16Array(expected);
  const view = new Uint16Array(
    bytes.buffer,
    bytes.byteOffset,
    Math.min(expected, Math.floor(bytes.byteLength / 2)),
  );
  if (view.length === expected) out.set(view);
  else {
    out.fill(NO_DATA);
    out.set(view);
  }
  return out;
}

function base64ToBytes(b64: string): Uint8Array<ArrayBuffer> {
  const bin = atob(b64);
  const len = bin.length;
  const buf = new ArrayBuffer(len);
  const bytes = new Uint8Array(buf);
  for (let i = 0; i < len; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}
