// Pre-render each level's collision grid to an offscreen canvas. This runs
// once per (level, projection, collision-visibility) and then the main draw
// loop just blits the canvases via drawImage with the camera transform —
// fast even for whole-act stitched views.

import { classifyCollision, type CollisionKind } from "./colors";
import { ensureCollision } from "./parse";
import { type Projection } from "./projection";
import { NO_DATA, type Level } from "./types";

export type CollisionVisibility = Record<CollisionKind, boolean>;

export interface LevelTexture {
  level: Level;
  proj: Projection;
  canvas: HTMLCanvasElement;
  // World coords (pixels-at-zoom-1) of the texture's top-left pixel.
  worldX: number;
  worldY: number;
  // World-pixel size of the texture (== canvas.width/height).
  worldW: number;
  worldH: number;
}

export async function buildLevelTexture(
  level: Level,
  proj: Projection,
  vis: CollisionVisibility,
): Promise<LevelTexture | null> {
  // Decode lazily: ensureCollision also populates the tight bbox on first
  // call. Levels with no collision payload resolve to null.
  const coll = await ensureCollision(level);
  if (!coll) return null;
  const cropW = level.tightMaxX - level.tightMinX;
  const cropH = level.tightMaxY - level.tightMinY;
  if (cropW <= 0 || cropH <= 0) return null;

  if (proj === "flat") return buildFlat(level, cropW, cropH, vis);
  return buildIso(level, cropW, cropH, vis);
}

function buildFlat(
  level: Level,
  cropW: number,
  cropH: number,
  vis: CollisionVisibility,
): LevelTexture {
  const canvas = document.createElement("canvas");
  canvas.width = cropW;
  canvas.height = cropH;
  const ctx = canvas.getContext("2d", { willReadFrequently: false })!;
  const img = ctx.createImageData(cropW, cropH);
  const data = img.data;
  const coll = level.coll!;
  const sx = level.sizeX;
  const x0 = level.tightMinX;
  const y0 = level.tightMinY;
  for (let ly = 0; ly < cropH; ly++) {
    for (let lx = 0; lx < cropW; lx++) {
      const v = coll[(y0 + ly) * sx + (x0 + lx)];
      const cls = classifyCollision(v);
      const di = (ly * cropW + lx) * 4;
      if (!cls || !vis[cls.kind]) {
        data[di + 0] = 0;
        data[di + 1] = 0;
        data[di + 2] = 0;
        data[di + 3] = 0;
      } else {
        data[di + 0] = cls.color.r;
        data[di + 1] = cls.color.g;
        data[di + 2] = cls.color.b;
        data[di + 3] = 255;
      }
    }
  }
  ctx.putImageData(img, 0, 0);
  return {
    level,
    proj: "flat",
    canvas,
    worldX: level.originX + level.tightMinX,
    worldY: level.originY + level.tightMinY,
    worldW: cropW,
    worldH: cropH,
  };
}

function buildIso(
  level: Level,
  cropW: number,
  cropH: number,
  vis: CollisionVisibility,
): LevelTexture {
  // Iso canvas dims:
  //   destW = (cropW + cropH) * 2; destH = (cropW + cropH);
  const destW = (cropW + cropH) * 2;
  const destH = cropW + cropH;
  const canvas = document.createElement("canvas");
  canvas.width = destW;
  canvas.height = destH;
  const ctx = canvas.getContext("2d", { willReadFrequently: false })!;
  const img = ctx.createImageData(destW, destH);
  const data = img.data;
  const coll = level.coll!;
  const sx = level.sizeX;
  const x0 = level.tightMinX;
  const y0 = level.tightMinY;

  const stamp2x2 = (px: number, py: number, r: number, g: number, b: number) => {
    for (let dy = 0; dy < 2; dy++) {
      const yy = py + dy;
      if (yy < 0 || yy >= destH) continue;
      for (let dx = 0; dx < 2; dx++) {
        const xx = px + dx;
        if (xx < 0 || xx >= destW) continue;
        const di = (yy * destW + xx) * 4;
        data[di + 0] = r;
        data[di + 1] = g;
        data[di + 2] = b;
        data[di + 3] = 255;
      }
    }
  };

  for (let ly = 0; ly < cropH; ly++) {
    for (let lx = 0; lx < cropW; lx++) {
      const v = coll[(y0 + ly) * sx + (x0 + lx)];
      if (v === NO_DATA) continue;
      const cls = classifyCollision(v);
      if (!cls || !vis[cls.kind]) continue;
      const isoX = (lx - ly) + (cropH - 1);
      const isoY = lx + ly;
      stamp2x2(isoX * 2, isoY, cls.color.r, cls.color.g, cls.color.b);
    }
  }
  ctx.putImageData(img, 0, 0);

  // World-coord position of texture pixel (0, 0). See projection.ts for
  // the iso transform formulas.
  const worldX = 2 * (level.originX - level.originY + level.tightMinX - level.tightMaxY + 1);
  const worldY = level.originX + level.originY + level.tightMinX + level.tightMinY;
  return {
    level,
    proj: "iso",
    canvas,
    worldX,
    worldY,
    worldW: destW,
    worldH: destH,
  };
}
