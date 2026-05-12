// Per-room "sprite" drawing. The base image comes from the atlas (rendered
// with the same color classification as the main map), and grave-slot
// overlays + outlines are drawn on top in iso-projected coords.

import { tileToIsoPx, isoCanvasSize } from "./atlas";
import { ROOM_AREA_TILES, type RoomDef, type Variant } from "./rooms";

const COL = {
  graveOpenFill:     "rgba(35, 6, 6, 0.92)",       // near-black dark red
  graveOpenStroke:   "rgba(255, 95, 95, 1.0)",     // bright red, opaque
  graveClosedStroke: "rgba(15, 30, 110, 1.0)",     // dark navy, opaque — readable against the bright atlas floor
  graveUnknownFill:  "rgba(180, 30, 30, 0.32)",    // translucent red wash so the slot reads as "wait, look here"
  graveUnknownStroke:"rgba(255, 60, 60, 1.0)",     // bright red border for the unknown state
  slotHint:          "rgba(106, 166, 255, 0.30)",
  outline:           "rgba(58, 63, 73, 0.85)",
  outlineSel:        "rgba(106, 166, 255, 0.95)",
};

// Stroke width for grave slot borders. Bright + thick so the slot positions
// stand out even on the dim atlas tile underneath.
const GRAVE_STROKE_WIDTH = 1.5;
// Grow each slot's bounding rectangle by this many tiles on every side. 1
// is the largest inflation that doesn't make adjacent slots in the empty
// dead-end rooms (closest spacing in the table) overlap.
const SLOT_INFLATE_TILES = 1;

export interface DrawRoomOptions {
  cellW: number;
  cellH: number;
  // When true, slot fill depends on gravesMask / gravesUnknownMask. When
  // false, slots are drawn faintly (hint only) — used in shape pickers.
  showGraves: boolean;
  // Bitmask of grave slots set (bit i = slot i+1 "open"). Only meaningful
  // for slots whose corresponding gravesUnknownMask bit is zero.
  gravesMask: number;
  // Bitmask of grave slots in the "unknown" state. The slot is drawn as a
  // red ‘!’ regardless of gravesMask.
  gravesUnknownMask?: number;
  // Picker selection ring.
  selected?: boolean;
  // Dim (disabled cell).
  dim?: boolean;
  // Atlas base canvas (rendered at native iso size). When null, we skip
  // the base and just draw outline + slot overlays — used as a transient
  // state before the atlas finishes loading.
  baseCanvas: HTMLCanvasElement | null;
}

// Project a tile-space rect to a polygon in cellW × cellH coords.
function tileRectPolyInCell(
  dx: number, dy: number, w: number, h: number,
  cellW: number, cellH: number,
): [number, number][] {
  const native = isoCanvasSize(ROOM_AREA_TILES);
  const sx = cellW / native.w;
  const sy = cellH / native.h;
  // Project each corner to native iso px, then scale to cell size.
  const proj = (lx: number, ly: number): [number, number] => {
    const [px, py] = tileToIsoPx(lx, ly, ROOM_AREA_TILES);
    return [px * sx, py * sy];
  };
  return [
    proj(dx,     dy),
    proj(dx + w, dy),
    proj(dx + w, dy + h),
    proj(dx,     dy + h),
  ];
}

// Same as tileRectPolyInCell but inflates the source rect by `pad` tiles on
// every side. Visual / hit-target padding for grave slots.
function inflatedSlotPoly(
  dx: number, dy: number, w: number, h: number,
  cellW: number, cellH: number,
): [number, number][] {
  const p = SLOT_INFLATE_TILES;
  return tileRectPolyInCell(dx - p, dy - p, w + 2 * p, h + 2 * p, cellW, cellH);
}

export function slotIsoPoly(
  variant: Variant,
  slotIndex: number,
  cellW: number, cellH: number,
): [number, number][] | null {
  const s = variant.slots[slotIndex];
  if (!s) return null;
  return inflatedSlotPoly(s.dx, s.dy, s.w, s.h, cellW, cellH);
}

function strokeAndFillPoly(
  ctx: CanvasRenderingContext2D,
  pts: [number, number][],
  fill: string | null, stroke: string | null,
  lineWidth = 1.5,
) {
  ctx.beginPath();
  ctx.moveTo(pts[0][0], pts[0][1]);
  for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
  ctx.closePath();
  if (fill) { ctx.fillStyle = fill; ctx.fill(); }
  if (stroke) {
    ctx.strokeStyle = stroke;
    ctx.lineWidth = lineWidth;
    ctx.stroke();
  }
}

export function drawRoom(
  ctx: CanvasRenderingContext2D,
  room: RoomDef,
  variantIdx: number,
  opts: DrawRoomOptions,
) {
  const { cellW, cellH } = opts;
  ctx.save();
  if (opts.dim) ctx.globalAlpha = 0.35;

  // Base atlas image scaled to cell size. We keep imageSmoothing disabled
  // for the blit so the high-density atlas pixels stay crisp through the
  // (downstream) CSS transform; smoothing comes back on after for the
  // anti-aliased polygon strokes.
  if (opts.baseCanvas) {
    const prev = ctx.imageSmoothingEnabled;
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(opts.baseCanvas, 0, 0, cellW, cellH);
    ctx.imageSmoothingEnabled = prev;
  }
  ctx.imageSmoothingEnabled = true;

  const variant = room.variants[variantIdx];
  // Slot overlays. Inflated by SLOT_INFLATE_TILES so the bright stroke (and
  // click target — see slotIsoPoly) reads as a chunky marker rather than a
  // thin sliver. Three states: unknown ("!") / closed (border only) / open
  // (filled). Bit i of gravesUnknownMask overrides the gravesMask bit.
  const unknownMask = opts.gravesUnknownMask ?? 0;
  for (let i = 0; i < variant.slots.length; i++) {
    const s = variant.slots[i];
    const poly = inflatedSlotPoly(s.dx, s.dy, s.w, s.h, cellW, cellH);
    const unknown = ((unknownMask >> i) & 1) !== 0;
    const on = ((opts.gravesMask >> i) & 1) !== 0;
    if (!opts.showGraves) {
      strokeAndFillPoly(ctx, poly, COL.slotHint, null, 1);
      continue;
    }
    if (unknown) {
      strokeAndFillPoly(ctx, poly, COL.graveUnknownFill, COL.graveUnknownStroke, GRAVE_STROKE_WIDTH);
      drawUnknownMark(ctx, s, cellW, cellH);
    } else if (on) {
      strokeAndFillPoly(ctx, poly, COL.graveOpenFill, COL.graveOpenStroke, GRAVE_STROKE_WIDTH);
    } else {
      strokeAndFillPoly(ctx, poly, null, COL.graveClosedStroke, GRAVE_STROKE_WIDTH);
    }
  }

  // Diamond outline.
  const outline = tileRectPolyInCell(0, 0, ROOM_AREA_TILES, ROOM_AREA_TILES, cellW, cellH);
  strokeAndFillPoly(ctx, outline, null,
    opts.selected ? COL.outlineSel : COL.outline,
    opts.selected ? 2 : 1);

  ctx.restore();
}

// Center a bold red "!" inside a slot. Sized off the slot's projected
// bounding box so it scales with the room sprite — fixed-pixel labels
// would be invisible at L4 grid resolution and oversized in the picker.
function drawUnknownMark(
  ctx: CanvasRenderingContext2D,
  slot: { dx: number; dy: number; w: number; h: number },
  cellW: number, cellH: number,
) {
  const poly = tileRectPolyInCell(slot.dx, slot.dy, slot.w, slot.h, cellW, cellH);
  const { x, y, w, h } = polyBBox(poly);
  const cx = x + w / 2;
  const cy = y + h / 2;
  const size = Math.max(8, Math.min(w, h) * 1.1);
  ctx.font = `bold ${size}px ui-sans-serif, system-ui, sans-serif`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.lineJoin = "round";
  ctx.strokeStyle = "rgba(0, 0, 0, 0.85)";
  ctx.lineWidth = Math.max(1, size * 0.18);
  ctx.strokeText("!", cx, cy);
  ctx.fillStyle = "rgb(255, 90, 90)";
  ctx.fillText("!", cx, cy);
}

// Build an offscreen canvas for a (room, variant) and return it.
export function makeRoomCanvas(
  room: RoomDef,
  variantIdx: number,
  opts: DrawRoomOptions,
): HTMLCanvasElement {
  const dpr = window.devicePixelRatio || 1;
  const c = document.createElement("canvas");
  c.width = opts.cellW * dpr;
  c.height = opts.cellH * dpr;
  c.style.width = `${opts.cellW}px`;
  c.style.height = `${opts.cellH}px`;
  const ctx = c.getContext("2d")!;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  drawRoom(ctx, room, variantIdx, opts);
  return c;
}

// CSS clip-path: polygon(...) for a slot's hit target. Used by the graves
// picker to make clickable parallelogram regions.
export function polyToClipPath(pts: [number, number][]): string {
  return "polygon(" + pts.map(([x, y]) => `${x}px ${y}px`).join(", ") + ")";
}

export function polyBBox(pts: [number, number][]): { x: number; y: number; w: number; h: number } {
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (const [x, y] of pts) {
    if (x < minX) minX = x; if (y < minY) minY = y;
    if (x > maxX) maxX = x; if (y > maxY) maxY = y;
  }
  return { x: minX, y: minY, w: maxX - minX, h: maxY - minY };
}

export function diamondClipPath(): string {
  return "polygon(50% 0, 100% 50%, 50% 100%, 0 50%)";
}

// Aspect ratio of one iso-rendered room: 2:1 (160×80 native for a 40×40 room).
// Use this when sizing sprite containers so they fit without letterboxing.
export const ROOM_ASPECT_W = 2;
export const ROOM_ASPECT_H = 1;
