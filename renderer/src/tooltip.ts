// Tooltip rendering for hover/inspector reuse. Keep a single DOM element
// pinned to body and update its content + position on demand. Designed to
// expose every available field — this is the debugging entry point.

import { markerFor, rgb, type RGB } from "./colors";
import {
  hasOrifice,
  type AdjacentJson,
  type Level,
  type PresetJson,
  type RoomJson,
} from "./types";

export interface TooltipModel {
  title: string;
  swatch?: RGB;
  rows: { key: string; val: string }[];
  // Optional second-section rows separated by a divider.
  extra?: { key: string; val: string }[];
}

export class Tooltip {
  private el: HTMLDivElement;
  constructor(host: HTMLElement) {
    this.el = host.querySelector<HTMLDivElement>("#tooltip")!;
  }

  hide() {
    this.el.classList.remove("show");
  }

  showAt(model: TooltipModel, clientX: number, clientY: number) {
    this.el.innerHTML = "";
    const title = document.createElement("div");
    title.className = "tt-title";
    if (model.swatch) {
      const sw = document.createElement("span");
      sw.className = "tt-swatch";
      sw.style.background = rgb(model.swatch);
      title.appendChild(sw);
    }
    const t = document.createElement("span");
    t.textContent = model.title;
    title.appendChild(t);
    this.el.appendChild(title);

    for (const row of model.rows) this.el.appendChild(rowDiv(row.key, row.val));
    if (model.extra && model.extra.length) {
      const sep = document.createElement("div");
      sep.className = "tt-section";
      this.el.appendChild(sep);
      for (const row of model.extra) sep.appendChild(rowDiv(row.key, row.val));
    }

    // Position relative to the canvas host (the tooltip is its child).
    const host = this.el.parentElement!;
    const hostRect = host.getBoundingClientRect();
    let x = clientX - hostRect.left + 14;
    let y = clientY - hostRect.top + 14;
    this.el.classList.add("show");
    // Clamp inside host bounds.
    const r = this.el.getBoundingClientRect();
    if (x + r.width > hostRect.width - 8) x = clientX - hostRect.left - r.width - 14;
    if (y + r.height > hostRect.height - 8) y = clientY - hostRect.top - r.height - 14;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    this.el.style.left = `${x}px`;
    this.el.style.top = `${y}px`;
  }
}

function rowDiv(key: string, val: string): HTMLDivElement {
  const d = document.createElement("div");
  d.className = "tt-row";
  const k = document.createElement("span");
  k.className = "tt-key";
  k.textContent = key;
  const v = document.createElement("span");
  v.className = "tt-val";
  v.textContent = val;
  d.appendChild(k);
  d.appendChild(v);
  return d;
}

// Tooltip builders — one per pickable kind. Each exposes every field on the
// underlying JSON record so the viewer doubles as a debugging readout.

export function presetTooltip(level: Level, p: PresetJson): TooltipModel {
  const m = markerFor(p);
  const gx = level.originX + p.x;
  const gy = level.originY + p.y;
  const rows: { key: string; val: string }[] = [];
  rows.push({ key: "type", val: p.type });
  rows.push({ key: "kind", val: p.kind ?? "Generic" });
  const display = p.displayName ?? p.name;
  if (display) rows.push({ key: "name", val: display });
  // Always surface the raw internal name when it differs from the display
  // name (or when display is missing). Keeps the tooltip useful for debugging.
  if (p.name && p.name !== display) rows.push({ key: "internal", val: p.name });
  if (p.description) rows.push({ key: "desc", val: p.description });
  rows.push({ key: "txtFile#", val: `${p.txtFileNo}` });
  rows.push({ key: "local", val: `(${p.x}, ${p.y})` });
  rows.push({ key: "global", val: `(${gx}, ${gy})` });
  if (p.destLevelNo) {
    const dest = p.destDisplayName ?? p.destName ?? "?";
    rows.push({ key: "→ level", val: `${dest} (#${p.destLevelNo})` });
    if (p.destName && p.destName !== dest) {
      rows.push({ key: "→ internal", val: p.destName });
    }
  }
  const levelLbl = level.displayName ?? level.name;
  rows.push({ key: "in", val: `${levelLbl} (#${level.levelNo}, act ${level.act})` });
  return {
    title: m.label + (display ? `: ${display}` : ""),
    swatch: m.color,
    rows,
  };
}

export function adjacentTooltip(
  level: Level,
  a: AdjacentJson,
  duplicates: number,
): TooltipModel {
  const fromLbl = level.displayName ?? level.name;
  const toLbl   = a.displayName ?? a.name;
  const rows: { key: string; val: string }[] = [
    { key: "from", val: `${fromLbl} (#${level.levelNo})` },
    { key: "to", val: `${toLbl} (#${a.levelNo})` },
    { key: "bridge", val: `local (${a.bridgeX}, ${a.bridgeY})` },
    { key: "global", val: `(${level.originX + a.bridgeX}, ${level.originY + a.bridgeY})` },
    { key: "duplicates", val: `${duplicates} Room2-pair entries here` },
  ];
  if (a.displayName && a.name !== a.displayName) {
    rows.splice(2, 0, { key: "internal", val: a.name });
  }
  return {
    title: `Adjacent → ${toLbl}`,
    swatch: { r: 255, g: 220, b: 90 },
    rows,
  };
}

export function roomTooltip(level: Level, r: RoomJson): TooltipModel {
  const levelLbl = level.displayName ?? level.name;
  return {
    title: `Room2 in ${levelLbl}`,
    swatch: { r: 180, g: 220, b: 255 },
    rows: [
      { key: "rooms.txt", val: `${r.roomNo}` },
      { key: "subNo", val: `${r.subNo}` },
      { key: "local", val: `(${r.x}, ${r.y}) ${r.sizeX}×${r.sizeY}` },
      { key: "global", val: `(${level.originX + r.x}, ${level.originY + r.y})` },
      { key: "level", val: `${levelLbl} (#${level.levelNo}, act ${level.act})` },
    ],
  };
}

// Level summary tooltip — used when hovering over a level texture without
// hitting any specific feature.
export function levelTooltip(level: Level): TooltipModel {
  const presetCounts = new Map<string, number>();
  for (const p of level.presets) {
    const k = p.type === "obj" && p.kind ? `${p.type}:${p.kind}` : p.type;
    presetCounts.set(k, (presetCounts.get(k) ?? 0) + 1);
  }
  const presetSummary =
    [...presetCounts.entries()].map(([k, n]) => `${k}=${n}`).join(", ") || "none";
  const titleLbl =
    (level.displayName ?? level.name) + (hasOrifice(level) ? " ★" : "");
  const rows: { key: string; val: string }[] = [];
  if (level.displayName && level.displayName !== level.name) {
    rows.push({ key: "internal", val: level.name });
  }
  if (hasOrifice(level)) {
    rows.push({ key: "★", val: "contains the Horadric Staff orifice" });
  }
  rows.push(
    { key: "act", val: `${level.act}` },
    { key: "origin", val: `(${level.originX}, ${level.originY})` },
    { key: "size", val: `${level.sizeX}×${level.sizeY}` },
    { key: "tight", val: `(${level.tightMinX},${level.tightMinY}) → (${level.tightMaxX},${level.tightMaxY})` },
    { key: "presets", val: `${level.presets.length} (${presetSummary})` },
    { key: "rooms", val: `${level.rooms.length}` },
    { key: "adjacents", val: `${level.adjacents.length} (raw entries)` },
  );
  return {
    title: `${titleLbl} (#${level.levelNo})`,
    rows,
  };
}
