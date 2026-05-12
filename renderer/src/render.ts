// Main render loop: clears the canvas, blits each level texture under the
// camera transform, then draws preset markers and optional overlays on top.

import { markerFor, rgb, COLORS, type LegendVisibility } from "./colors";
import { classifyStairDirection } from "./connectivity";
import { projectLocal, type Projection } from "./projection";
import { type LevelTexture } from "./texture";
import { type Bounds, type Viewport } from "./viewport";
import { COLL, NO_DATA, hasOrifice, type AdjacentJson, type Level, type PresetJson, type RoomJson, type TellLocationJson } from "./types";

export interface RenderState {
  proj: Projection;
  textures: LevelTexture[];
  vis: LegendVisibility;
  showAdjacents: boolean;
  showRooms: boolean;
  showLabels: boolean;
  showTells: boolean;
  // Highlighted level (dim others). Set only via the sidebar — canvas hover
  // does not change this so the map itself never flickers under the mouse.
  hoveredLevelNo: number | null;
  selectedPreset: { level: Level; preset: PresetJson } | null;
}

export function computeContentBounds(textures: LevelTexture[]): Bounds | null {
  if (textures.length === 0) return null;
  let minX = Infinity,
    minY = Infinity,
    maxX = -Infinity,
    maxY = -Infinity;
  for (const t of textures) {
    if (t.worldX < minX) minX = t.worldX;
    if (t.worldY < minY) minY = t.worldY;
    if (t.worldX + t.worldW > maxX) maxX = t.worldX + t.worldW;
    if (t.worldY + t.worldH > maxY) maxY = t.worldY + t.worldH;
  }
  return { minX, minY, maxX, maxY };
}

export function renderFrame(
  ctx: CanvasRenderingContext2D,
  vp: Viewport,
  state: RenderState,
) {
  const { width: w, height: h } = vp;
  ctx.fillStyle = rgb(COLORS.bg);
  ctx.fillRect(0, 0, w, h);

  // Set transform once, then everything draws in world coords.
  ctx.save();
  ctx.translate(w / 2, h / 2);
  ctx.scale(vp.scale, vp.scale);
  ctx.translate(-vp.cx, -vp.cy);

  // Crisp pixels at high zoom; smooth when zoomed out so distant levels blur
  // instead of moiré.
  ctx.imageSmoothingEnabled = vp.scale < 1;

  for (const t of state.textures) {
    const dim = state.hoveredLevelNo !== null && state.hoveredLevelNo !== t.level.levelNo;
    if (dim) ctx.globalAlpha = 0.45;
    ctx.drawImage(t.canvas, t.worldX, t.worldY, t.worldW, t.worldH);
    if (dim) ctx.globalAlpha = 1;
  }

  if (state.showRooms) drawRooms(ctx, state);
  if (state.showAdjacents) drawAdjacents(ctx, state);
  drawPresets(ctx, state);
  if (state.showTells) drawTells(ctx, state);
  if (state.selectedPreset) drawSelectionRing(ctx, state, vp.scale);

  ctx.restore();

  // Labels are drawn after restore so font size doesn't scale with zoom.
  if (state.showLabels) drawLabels(ctx, vp, state);
  if (state.showTells) drawTellLabels(ctx, vp, state);
}

function drawRooms(ctx: CanvasRenderingContext2D, state: RenderState) {
  ctx.lineWidth = Math.max(0.4, 1);
  ctx.strokeStyle = "rgba(180, 220, 255, 0.4)";
  for (const t of state.textures) {
    const lvl = t.level;
    for (const r of lvl.rooms) {
      drawWorldRectOutline(ctx, state.proj, lvl.originX, lvl.originY,
        r.x, r.y, r.sizeX, r.sizeY);
    }
  }
}

function drawWorldRectOutline(
  ctx: CanvasRenderingContext2D,
  proj: Projection,
  ox: number,
  oy: number,
  lx: number,
  ly: number,
  sx: number,
  sy: number,
) {
  const corners: [number, number][] = [
    projectLocal(proj, ox, oy, lx, ly),
    projectLocal(proj, ox, oy, lx + sx, ly),
    projectLocal(proj, ox, oy, lx + sx, ly + sy),
    projectLocal(proj, ox, oy, lx, ly + sy),
  ];
  ctx.beginPath();
  ctx.moveTo(corners[0][0], corners[0][1]);
  for (let i = 1; i < 4; i++) ctx.lineTo(corners[i][0], corners[i][1]);
  ctx.closePath();
  ctx.stroke();
}

function drawAdjacents(ctx: CanvasRenderingContext2D, state: RenderState) {
  // Collapse duplicates: many Room2->Room2 entries can target the same level
  // along the same border. The C++ extractor emits one per pair.
  ctx.lineWidth = 1;
  ctx.strokeStyle = "rgba(255, 220, 90, 0.6)";
  ctx.fillStyle = "rgba(255, 220, 90, 0.9)";
  for (const t of state.textures) {
    const lvl = t.level;
    const seen = new Set<string>();
    for (const a of lvl.adjacents) {
      const key = `${a.levelNo}:${a.bridgeX}:${a.bridgeY}`;
      if (seen.has(key)) continue;
      seen.add(key);
      const [wx, wy] = projectLocal(state.proj, lvl.originX, lvl.originY, a.bridgeX, a.bridgeY);
      ctx.beginPath();
      ctx.arc(wx, wy, 2, 0, Math.PI * 2);
      ctx.fill();
    }
  }
}

function drawPresets(ctx: CanvasRenderingContext2D, state: RenderState) {
  // Two passes: lower priority first so important markers stay on top.
  // Build a single sorted list across all levels — fine, totals stay <50k.
  type Item = { lvl: Level; p: PresetJson; m: ReturnType<typeof markerFor> };
  const items: Item[] = [];
  for (const t of state.textures) {
    for (const p of t.level.presets) {
      if (p.type !== "npc" && p.type !== "obj" && p.type !== "exit") continue;
      const m = markerFor(p);
      if (!state.vis.markers[m.kind]) continue;
      items.push({ lvl: t.level, p, m });
    }
  }
  items.sort((a, b) => a.m.priority - b.m.priority);

  for (const it of items) {
    const [wx, wy] = projectLocal(state.proj, it.lvl.originX, it.lvl.originY, it.p.x, it.p.y);
    // Marker radius is in tile units; convert to world units.
    // Flat: 1 tile = 1 world unit.
    // Iso : 1 tile diagonal = ~2 world X, ~1 world Y. We use the larger
    // (X) so dots stay visible at iso.
    const r = state.proj === "iso" ? it.m.radius * 2 : it.m.radius;
    ctx.fillStyle = rgb(it.m.color);
    ctx.beginPath();
    ctx.arc(wx, wy, r, 0, Math.PI * 2);
    ctx.fill();

    // Exit dots get an up/down arrow inside indicating where the stairs lead.
    if (it.p.type === "exit" && it.p.destLevelNo !== undefined) {
      const dir = classifyStairDirection(it.lvl.levelNo, it.p.destLevelNo);
      if (dir) drawStairArrow(ctx, wx, wy, r, dir);
    }
  }
}

// Small white arrow (shaft + head) inside an exit dot, pointing the
// direction stairs lead. Drawn as a single filled polygon.
function drawStairArrow(
  ctx: CanvasRenderingContext2D,
  wx: number,
  wy: number,
  r: number,
  dir: "up" | "down",
) {
  const shaftHalf = r * 0.18; // shaft half-width
  const headHalf = r * 0.5;  // arrowhead half-width
  const tailY = r * 0.72;    // tail end (opposite the tip)
  const shoulderY = r * 0.05; // where shaft meets arrowhead base
  const tipY = r * 0.75;     // arrowhead tip
  const s = dir === "down" ? 1 : -1; // flip Y for "up"
  ctx.fillStyle = "rgba(255, 255, 255, 0.95)";
  ctx.beginPath();
  ctx.moveTo(wx - shaftHalf, wy - tailY * s);
  ctx.lineTo(wx + shaftHalf, wy - tailY * s);
  ctx.lineTo(wx + shaftHalf, wy + shoulderY * s);
  ctx.lineTo(wx + headHalf,  wy + shoulderY * s);
  ctx.lineTo(wx,             wy + tipY * s);
  ctx.lineTo(wx - headHalf,  wy + shoulderY * s);
  ctx.lineTo(wx - shaftHalf, wy + shoulderY * s);
  ctx.closePath();
  ctx.fill();
}

// Outline each tell's location rectangle (or a small box for point-only
// tells). Label drawing is split out into drawTellLabels so we can render
// labels at fixed screen size after the world transform is restored.
function drawTells(ctx: CanvasRenderingContext2D, state: RenderState) {
  ctx.strokeStyle = "rgba(120, 220, 255, 0.85)";
  ctx.fillStyle   = "rgba(120, 220, 255, 0.18)";
  ctx.lineWidth   = 1;
  for (const t of state.textures) {
    const lvl = t.level;
    for (const tell of lvl.tells) {
      for (const loc of tell.locations) {
        drawTellLocationOutline(ctx, state.proj, lvl.originX, lvl.originY, loc);
      }
    }
  }
}

function drawTellLocationOutline(
  ctx: CanvasRenderingContext2D,
  proj: Projection,
  ox: number,
  oy: number,
  loc: TellLocationJson,
) {
  const isPoint = loc.w === 0 && loc.h === 0;
  // Treat points as a 1×1 cell at (x,y) so they get a tiny visible square.
  const w = isPoint ? 1 : loc.w;
  const h = isPoint ? 1 : loc.h;
  const corners: [number, number][] = [
    projectLocal(proj, ox, oy, loc.x,     loc.y),
    projectLocal(proj, ox, oy, loc.x + w, loc.y),
    projectLocal(proj, ox, oy, loc.x + w, loc.y + h),
    projectLocal(proj, ox, oy, loc.x,     loc.y + h),
  ];
  ctx.beginPath();
  ctx.moveTo(corners[0][0], corners[0][1]);
  for (let i = 1; i < 4; i++) ctx.lineTo(corners[i][0], corners[i][1]);
  ctx.closePath();
  if (!isPoint) ctx.fill();
  ctx.stroke();
}

// Draw "name=value" tags near each tell location. Done at screen-space font
// size (after the world transform is restored) so they stay legible at any
// zoom. One tag per (tell, location) pair; tags are skipped offscreen.
function drawTellLabels(
  ctx: CanvasRenderingContext2D,
  vp: Viewport,
  state: RenderState,
) {
  const dpr = window.devicePixelRatio || 1;
  ctx.font = `${11 * dpr}px ui-sans-serif, system-ui, sans-serif`;
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  for (const t of state.textures) {
    const lvl = t.level;
    for (const tell of lvl.tells) {
      for (const loc of tell.locations) {
        const cx = loc.x + (loc.w === 0 ? 0 : loc.w) / 2;
        const cy = loc.y + (loc.h === 0 ? 0 : loc.h) / 2;
        const [wx, wy] = projectLocal(state.proj, lvl.originX, lvl.originY, cx, cy);
        const [sx, sy] = vp.worldToScreen(wx, wy);
        if (sx < -200 || sy < -40 || sx > vp.width + 200 || sy > vp.height + 40) continue;
        const text = `${tell.name}=${tell.value}`;
        const m = ctx.measureText(text);
        const padX = 4 * dpr, padY = 2 * dpr;
        const tx = sx + 4 * dpr;
        const ty = sy + 4 * dpr;
        ctx.fillStyle = "rgba(0, 0, 0, 0.7)";
        ctx.fillRect(tx - padX, ty - padY, m.width + padX * 2, 14 * dpr + padY * 2);
        ctx.fillStyle = "#bce8ff";
        ctx.fillText(text, tx, ty);
      }
    }
  }
}

function drawSelectionRing(
  ctx: CanvasRenderingContext2D,
  state: RenderState,
  scale: number,
) {
  const sel = state.selectedPreset!;
  const [wx, wy] = projectLocal(state.proj, sel.level.originX, sel.level.originY,
    sel.preset.x, sel.preset.y);
  const m = markerFor(sel.preset);
  const r = (state.proj === "iso" ? m.radius * 2 : m.radius) + 4 / Math.max(scale, 0.5);
  ctx.lineWidth = 2 / Math.max(scale, 0.5);
  ctx.strokeStyle = "white";
  ctx.beginPath();
  ctx.arc(wx, wy, r, 0, Math.PI * 2);
  ctx.stroke();
}

function drawLabels(
  ctx: CanvasRenderingContext2D,
  vp: Viewport,
  state: RenderState,
) {
  const dpr = window.devicePixelRatio || 1;
  ctx.font = `${12 * dpr}px ui-sans-serif, system-ui, sans-serif`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  for (const t of state.textures) {
    const lvl = t.level;
    // Center of the texture in world coords.
    const wx = t.worldX + t.worldW / 2;
    const wy = t.worldY + t.worldH / 2;
    const [sx, sy] = vp.worldToScreen(wx, wy);
    if (sx < -200 || sy < -100 || sx > vp.width + 200 || sy > vp.height + 100) continue;
    const text = `${lvl.displayName ?? lvl.name}${hasOrifice(lvl) ? " ★" : ""} (${lvl.levelNo})`;
    ctx.fillStyle = "rgba(0,0,0,0.6)";
    const m = ctx.measureText(text);
    ctx.fillRect(sx - m.width / 2 - 6, sy - 10 * dpr, m.width + 12, 20 * dpr);
    ctx.fillStyle = state.hoveredLevelNo === lvl.levelNo ? "#fff" : "#cfd2d8";
    ctx.fillText(text, sx, sy);
  }
}

// Hit-test a click in world coords against preset markers. Returns the
// closest preset within a generous click radius, or null.
export function pickPreset(
  state: RenderState,
  worldX: number,
  worldY: number,
  scale: number,
): { level: Level; preset: PresetJson } | null {
  const pickRadiusWorld = 8 / Math.max(scale, 0.05);
  let best: { level: Level; preset: PresetJson; d2: number } | null = null;
  for (const t of state.textures) {
    for (const p of t.level.presets) {
      if (p.type !== "npc" && p.type !== "obj" && p.type !== "exit") continue;
      const m = markerFor(p);
      if (!state.vis.markers[m.kind]) continue;
      const [wx, wy] = projectLocal(state.proj, t.level.originX, t.level.originY, p.x, p.y);
      const dx = wx - worldX;
      const dy = wy - worldY;
      const d2 = dx * dx + dy * dy;
      const rWorld = state.proj === "iso" ? m.radius * 2 : m.radius;
      const limit = Math.max(rWorld, pickRadiusWorld);
      if (d2 <= limit * limit) {
        if (!best || d2 < best.d2) best = { level: t.level, preset: p, d2 };
      }
    }
  }
  return best ? { level: best.level, preset: best.preset } : null;
}

// Pick adjacent-bridge or room handles when those layers are visible. Used
// for tooltips so every painted overlay is interrogable.
export function pickAdjacent(
  state: RenderState,
  worldX: number,
  worldY: number,
  scale: number,
): { level: Level; adjacent: AdjacentJson } | null {
  if (!state.showAdjacents) return null;
  const radius = state.proj === "iso" ? 4 : 3;
  const slack = 6 / Math.max(scale, 0.05);
  const limit = Math.max(radius, slack);
  let best: { level: Level; adjacent: AdjacentJson; d2: number } | null = null;
  for (const t of state.textures) {
    for (const a of t.level.adjacents) {
      const [wx, wy] = projectLocal(state.proj, t.level.originX, t.level.originY, a.bridgeX, a.bridgeY);
      const dx = wx - worldX;
      const dy = wy - worldY;
      const d2 = dx * dx + dy * dy;
      if (d2 <= limit * limit && (!best || d2 < best.d2)) {
        best = { level: t.level, adjacent: a, d2 };
      }
    }
  }
  return best ? { level: best.level, adjacent: best.adjacent } : null;
}

export function pickRoom(
  state: RenderState,
  worldX: number,
  worldY: number,
): { level: Level; room: RoomJson } | null {
  if (!state.showRooms) return null;
  // Match against the room's local-tile rect projected to world space. For
  // iso the projected polygon is a diamond; we approximate by checking the
  // inverse-projected game tile (gx,gy) against the room's local rect.
  const [gx, gy] = worldToGameTile(state.proj, worldX, worldY);
  for (const t of state.textures) {
    const lvl = t.level;
    const lx = gx - lvl.originX;
    const ly = gy - lvl.originY;
    for (const r of lvl.rooms) {
      if (lx >= r.x && lx < r.x + r.sizeX && ly >= r.y && ly < r.y + r.sizeY) {
        return { level: lvl, room: r };
      }
    }
  }
  return null;
}

// Read the raw collision WORD at a given world coord. Returns null when no
// level covers that tile; returns NO_DATA (0xFFFF) when the tile is in a
// level's bbox but unpainted.
export function pickCollision(
  state: RenderState,
  worldX: number,
  worldY: number,
): { level: Level; lx: number; ly: number; value: number } | null {
  const [gx, gy] = worldToGameTile(state.proj, worldX, worldY);
  for (const t of state.textures) {
    const lvl = t.level;
    if (!lvl.coll) continue;
    const lx = gx - lvl.originX;
    const ly = gy - lvl.originY;
    if (lx < 0 || lx >= lvl.sizeX || ly < 0 || ly >= lvl.sizeY) continue;
    const v = lvl.coll[ly * lvl.sizeX + lx];
    return { level: lvl, lx, ly, value: v };
  }
  return null;
}

export function describeCollision(v: number): string {
  if (v === NO_DATA) return "no-data (0xFFFF)";
  if (v === 0) return "floor (0)";
  const bits: string[] = [];
  for (const [name, mask] of Object.entries(COLL)) {
    if (v & mask) bits.push(name);
  }
  const unknown = v & ~Object.values(COLL).reduce((a, b) => a | b, 0);
  if (unknown) bits.push(`0x${unknown.toString(16).toUpperCase()}`);
  return `0x${v.toString(16).toUpperCase()} (${bits.join("|") || "?"})`;
}

// Inverse projection for cursor display. Given a world coord, return the
// best-guess game tile (gx, gy).
export function worldToGameTile(
  proj: Projection,
  wx: number,
  wy: number,
): [number, number] {
  if (proj === "flat") return [Math.floor(wx), Math.floor(wy)];
  // iso: wx = 2*(gx - gy), wy = gx + gy → gx = (wx/2 + wy)/2, gy = (wy - wx/2)/2
  const dx = wx / 2;
  const gx = (dx + wy) / 2;
  const gy = (wy - dx) / 2;
  return [Math.floor(gx), Math.floor(gy)];
}

// Hit-test which level texture covers a given screen-buffer position. Used
// by hover so the labels light up the active level.
export function pickLevelAt(
  state: RenderState,
  vp: Viewport,
  bx: number,
  by: number,
): number | null {
  // Test against texture rects in world space.
  const [wx, wy] = vp.screenToWorld(bx, by);
  for (let i = state.textures.length - 1; i >= 0; i--) {
    const t = state.textures[i];
    if (wx >= t.worldX && wx <= t.worldX + t.worldW &&
        wy >= t.worldY && wy <= t.worldY + t.worldH) {
      return t.level.levelNo;
    }
  }
  return null;
}
