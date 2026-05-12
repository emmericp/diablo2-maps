// DOM glue: sidebar, file picker, drag-drop, dev seed list, inspector, legend
// checkboxes, and the connected-component-grouped levels list.

import {
  COLLISION_LEGEND,
  MARKER_LEGEND,
  defaultLegendVisibility,
  markerFor,
  rgb,
  type CollisionKind,
  type LegendVisibility,
  type MarkerKind,
} from "./colors";
import {
  type ActGroup,
  type Hierarchy,
  type RenderUnit,
} from "./connectivity";
import { hasOrifice, type Level, type PresetJson, type Seed } from "./types";

// Suffix appended to level names that contain the Horadric Staff orifice
// (the "right" Tal Rasha's tomb).
const ORIFICE_STAR = " ★";

export interface UiCallbacks {
  onSeedFile: (file: File) => void;
  onSeedFromDir: (name: string) => void;
  onSeedLookup: (seed: number) => void;
  onActLoad: (actNo: number) => void;
  onTogglesChanged: () => void;
  onVisibilityChanged: (vis: LegendVisibility) => void;
  onResetView: () => void;
  onLevelClick: (levelNo: number) => void;
  onLevelHover: (levelNo: number | null) => void;
  onUnitActivate: (unitId: number) => void;
}

export const ACT_NUMBERS: ReadonlyArray<number> = [1, 2, 3, 4, 5];

export interface Toggles {
  iso: boolean;
  adjacents: boolean;
  rooms: boolean;
  labels: boolean;
  tells: boolean;
}

export class Ui {
  private cb: UiCallbacks;
  private vis: LegendVisibility = defaultLegendVisibility();

  private $seedLabel = byId<HTMLElement>("seed-label");
  // Dropdown + file input only exist in dev (stripped from prod HTML).
  private $seedSelect = byIdOrNull<HTMLSelectElement>("seed-select");
  private $seedInput = byId<HTMLInputElement>("seed-input");
  private $seedLoadBtn = byId<HTMLButtonElement>("seed-load-btn");
  private $seedRandomBtn = byId<HTMLButtonElement>("seed-random-btn");
  private $fileInput = byIdOrNull<HTMLInputElement>("file-input");
  private $openBtn = byIdOrNull<HTMLButtonElement>("open-btn");
  private $resetBtn = byId<HTMLButtonElement>("reset-view-btn");
  private $iso = byId<HTMLInputElement>("iso-toggle");
  private $adjacents = byId<HTMLInputElement>("adjacents-toggle");
  private $rooms = byId<HTMLInputElement>("rooms-toggle");
  private $labels = byId<HTMLInputElement>("labels-toggle");
  // Stripped from the prod HTML (dev-only label), so absent at runtime there.
  private $tells = byIdOrNull<HTMLInputElement>("tells-toggle");
  private $legend = byId<HTMLElement>("legend");
  private $components = byId<HTMLElement>("components-list");
  private $inspector = byId<HTMLElement>("inspector-body");
  private $sbCoord = byId<HTMLElement>("sb-coord");
  private $sbTile = byId<HTMLElement>("sb-tile");
  private $sbColl = byId<HTMLElement>("sb-coll");
  private $sbZoom = byId<HTMLElement>("sb-zoom");
  private $sbMsg = byId<HTMLElement>("sb-message");
  private $loadingOverlay = byId<HTMLElement>("loading-overlay");
  private $loadingMsg = byId<HTMLElement>("loading-msg");
  private $sidebar = byId<HTMLElement>("sidebar");
  private $sidebarToggle = byId<HTMLButtonElement>("sidebar-toggle");
  private $sidebarClose = byId<HTMLButtonElement>("sidebar-close");
  private $sidebarBackdrop = byId<HTMLElement>("sidebar-backdrop");

  constructor(cb: UiCallbacks) {
    this.cb = cb;
    this.renderLegend();
    this.wire();
    // In prod the .dev-only DOM is stripped at build time, so the dropdown
    // doesn't exist and there's nothing to populate.
    if (import.meta.env.DEV) this.tryLoadSeedDir();
  }

  private wire() {
    if (this.$openBtn && this.$fileInput) {
      const fileInput = this.$fileInput;
      this.$openBtn.addEventListener("click", () => fileInput.click());
      fileInput.addEventListener("change", () => {
        const f = fileInput.files?.[0];
        if (f) this.cb.onSeedFile(f);
        fileInput.value = "";
      });
    }
    this.$resetBtn.addEventListener("click", () => this.cb.onResetView());
    if (this.$seedSelect) {
      const seedSelect = this.$seedSelect;
      seedSelect.addEventListener("change", () => {
        const v = seedSelect.value;
        if (v) this.cb.onSeedFromDir(v);
      });
    }

    const submitSeed = () => {
      const raw = this.$seedInput.value.trim();
      if (!raw) return;
      const n = Number(raw);
      // uint32 range, no fractional input.
      if (!Number.isInteger(n) || n < 0 || n > 0xFFFFFFFF) {
        this.setStatus(`invalid seed: ${raw}`);
        return;
      }
      this.$seedInput.blur(); // hide on-screen keyboard before the panel closes
      this.maybeCloseSidebarAfterPick();
      this.cb.onSeedLookup(n);
    };
    this.$seedLoadBtn.addEventListener("click", submitSeed);
    this.$seedInput.addEventListener("keydown", (e) => {
      if (e.key === "Enter") submitSeed();
    });

    // D2 only ever generates 31-bit seeds (see CLAUDE.md), so we pick from
    // [0, INT32_MAX] — crypto for an unbiased draw across 2^31 values.
    this.$seedRandomBtn.addEventListener("click", () => {
      const buf = new Uint32Array(1);
      crypto.getRandomValues(buf);
      const n = buf[0]! & 0x7fffffff;
      this.$seedInput.value = String(n);
      this.maybeCloseSidebarAfterPick();
      this.cb.onSeedLookup(n);
    });

    const onToggle = () => this.cb.onTogglesChanged();
    this.$iso.addEventListener("change", onToggle);
    this.$adjacents.addEventListener("change", onToggle);
    this.$rooms.addEventListener("change", onToggle);
    this.$labels.addEventListener("change", onToggle);
    this.$tells?.addEventListener("change", onToggle);

    // Drag-and-drop on the canvas host (dev only — drop a seed_*.json).
    if (import.meta.env.DEV) {
      const host = byId<HTMLElement>("canvas-host");
      let overlay: HTMLDivElement | null = null;
      const showOverlay = (show: boolean) => {
        if (!overlay) {
          overlay = document.createElement("div");
          overlay.className = "drop-overlay";
          host.appendChild(overlay);
        }
        overlay.classList.toggle("show", show);
      };
      host.addEventListener("dragover", (e) => {
        e.preventDefault();
        showOverlay(true);
      });
      host.addEventListener("dragleave", () => showOverlay(false));
      host.addEventListener("drop", (e) => {
        e.preventDefault();
        showOverlay(false);
        const f = e.dataTransfer?.files?.[0];
        if (f) this.cb.onSeedFile(f);
      });
    }

    // Keyboard shortcuts.
    window.addEventListener("keydown", (e) => {
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement) return;
      if (e.key === "r" || e.key === "R") this.cb.onResetView();
      if (e.key === "i" || e.key === "I") {
        this.$iso.checked = !this.$iso.checked;
        onToggle();
      }
      if (e.key === "Escape" && this.$sidebar.classList.contains("open")) {
        this.setSidebarOpen(false);
      }
    });

    // Sidebar open/close (only does anything on the mobile breakpoint, but
    // the buttons are wired at all sizes since they're CSS-hidden on desktop).
    this.$sidebarToggle.addEventListener("click", () => {
      this.setSidebarOpen(!this.$sidebar.classList.contains("open"));
    });
    this.$sidebarClose.addEventListener("click", () => this.setSidebarOpen(false));
    this.$sidebarBackdrop.addEventListener("click", () => this.setSidebarOpen(false));

    // If the viewport grows back past the mobile breakpoint while the
    // overlay sidebar is open, the backdrop would otherwise hang around on
    // top of the desktop layout. Force-close it on the transition.
    const mq = window.matchMedia("(max-width: 720px)");
    mq.addEventListener("change", (e) => {
      if (!e.matches) this.setSidebarOpen(false);
    });
  }

  // Open / close the slide-in sidebar. Backdrop visibility is toggled in
  // lockstep so it only intercepts taps while the sidebar is showing.
  private setSidebarOpen(open: boolean) {
    this.$sidebar.classList.toggle("open", open);
    this.$sidebarBackdrop.hidden = !open;
    this.$sidebarToggle.setAttribute("aria-expanded", open ? "true" : "false");
  }

  // Called after the user picks something in the sidebar that updates the
  // map view. On mobile this closes the overlay so they can see the result;
  // on desktop the sidebar isn't an overlay and the class toggle is a no-op.
  private maybeCloseSidebarAfterPick() {
    if (window.matchMedia("(max-width: 720px)").matches) {
      this.setSidebarOpen(false);
    }
  }

  // Always reads the DOM directly. No cache — eliminates any drift between
  // what the user sees in the checkboxes and what RenderState believes,
  // including after browser form-state restoration on tab duplication.
  getToggles(): Toggles {
    return {
      iso: this.$iso.checked,
      adjacents: this.$adjacents.checked,
      rooms: this.$rooms.checked,
      labels: this.$labels.checked,
      tells: this.$tells?.checked ?? false,
    };
  }

  getVisibility(): LegendVisibility {
    return this.vis;
  }

  setStatus(msg: string) {
    this.$sbMsg.textContent = msg;
  }

  // Show / hide the spinner overlay over the canvas. We use this while
  // textures decompress for the active unit — short enough to feel snappy
  // when cached, but on first activation of a big wilderness unit the
  // DEFLATE work can take a couple hundred ms.
  setBusy(busy: boolean, msg?: string) {
    if (busy) {
      this.$loadingMsg.textContent = msg ?? "loading…";
      this.$loadingOverlay.hidden = false;
    } else {
      this.$loadingOverlay.hidden = true;
    }
  }

  setSeedLabel(seed: Seed | null) {
    if (!seed) {
      this.$seedLabel.textContent = "no seed loaded";
      return;
    }
    this.$seedLabel.textContent = `seed ${seed.seed}  •  ${seed.levels.length} levels loaded`;
  }

  // Render all 5 acts. Acts present in `h` get their full unit tree; acts
  // that haven't been fetched yet render as a clickable placeholder header
  // that triggers onActLoad. `loadingActs` shows a busy state on placeholders
  // that have an in-flight request.
  //
  // Called on every state change (a fetch starts, a fetch finishes, a fresh
  // seed loads), so we preserve per-act collapse state and scroll position
  // across rebuilds — otherwise clicking "load act N" would scroll back to
  // the top and re-expand acts the user had collapsed.
  setHierarchy(h: Hierarchy, loadedActs: Set<number>, loadingActs: Set<number>) {
    const collapsed = new Set<number>();
    for (const wrap of this.$components.querySelectorAll<HTMLElement>(".act")) {
      if (wrap.classList.contains("collapsed")) {
        collapsed.add(Number(wrap.dataset.act));
      }
    }
    const scrollTop = this.$components.scrollTop;

    this.$components.innerHTML = "";
    const byAct = new Map<number, ActGroup>();
    for (const a of h.acts) byAct.set(a.act, a);
    for (const act of ACT_NUMBERS) {
      let el: HTMLElement;
      if (loadedActs.has(act)) {
        const group = byAct.get(act) ?? { act, units: [] };
        el = this.buildAct(group);
      } else {
        el = this.buildPlaceholderAct(act, loadingActs.has(act));
      }
      if (collapsed.has(act)) el.classList.add("collapsed");
      this.$components.appendChild(el);
    }
    this.$components.scrollTop = scrollTop;
  }

  private buildPlaceholderAct(actNo: number, loading: boolean): HTMLElement {
    const wrap = document.createElement("div");
    wrap.className = "act act-placeholder";
    wrap.dataset.act = String(actNo);
    if (loading) wrap.classList.add("loading");

    const header = document.createElement("div");
    header.className = "act-header";
    const title = document.createElement("span");
    title.className = "act-title";
    title.textContent = `Act ${actNo}`;
    const hint = document.createElement("span");
    hint.className = "comp-count";
    hint.textContent = loading ? "loading…" : "click to load";
    header.appendChild(title);
    header.appendChild(hint);
    if (!loading) {
      header.addEventListener("click", () => this.cb.onActLoad(actNo));
    }
    wrap.appendChild(header);
    return wrap;
  }

  private buildAct(act: ActGroup): HTMLElement {
    const wrap = document.createElement("div");
    wrap.className = "act";
    wrap.dataset.act = String(act.act);

    const header = document.createElement("div");
    header.className = "act-header";
    const twirl = document.createElement("span");
    twirl.className = "twirl";
    twirl.textContent = "▶";
    const title = document.createElement("span");
    title.className = "act-title";
    title.textContent = `Act ${act.act}`;
    const count = document.createElement("span");
    count.className = "comp-count";
    count.textContent = `${act.units.length}`;
    count.title = `${act.units.length} render units in this act`;
    header.appendChild(twirl);
    header.appendChild(title);
    header.appendChild(count);
    header.addEventListener("click", () => wrap.classList.toggle("collapsed"));
    wrap.appendChild(header);

    const body = document.createElement("div");
    body.className = "act-body";
    for (const unit of act.units) body.appendChild(this.buildUnit(unit));
    wrap.appendChild(body);
    return wrap;
  }

  // One render unit = one consistent leaf row. Multi-level units expand a
  // floor list inline when activated, but the row itself is what triggers
  // rendering and looks identical regardless of size.
  private buildUnit(unit: RenderUnit): HTMLElement {
    const wrap = document.createElement("div");
    wrap.className = "unit";
    wrap.dataset.unitId = String(unit.id);

    const row = document.createElement("div");
    row.className = "unit-row";
    row.dataset.unitId = String(unit.id);
    const lbl = document.createElement("span");
    lbl.className = "unit-label";
    lbl.textContent = unit.displayName || unit.name;
    if (unit.displayName && unit.displayName !== unit.name) {
      lbl.title = unit.name;
    }
    const meta = document.createElement("span");
    meta.className = "unit-meta";
    meta.textContent = unit.levels.length === 1
      ? `#${unit.levels[0]!.levelNo}`
      : `${unit.levels.length}`;
    row.appendChild(lbl);
    row.appendChild(meta);
    row.title = unit.levels.length === 1
      ? `Level #${unit.levels[0]!.levelNo} — click to render`
      : `${unit.levels.length} levels — click to render together`;
    row.addEventListener("click", () => {
      this.maybeCloseSidebarAfterPick();
      this.cb.onUnitActivate(unit.id);
    });
    wrap.appendChild(row);

    if (unit.levels.length > 1) {
      const ul = document.createElement("ul");
      ul.className = "unit-floors";
      for (const lvl of unit.levels) {
        const li = document.createElement("li");
        li.className = "floor-row";
        li.dataset.lvlNo = String(lvl.levelNo);
        li.dataset.unitId = String(unit.id);
        const name = document.createElement("span");
        const baseLbl = lvl.displayName ?? lvl.name;
        name.textContent = baseLbl + (hasOrifice(lvl) ? ORIFICE_STAR : "");
        if (lvl.displayName && lvl.displayName !== lvl.name) name.title = lvl.name;
        const no = document.createElement("span");
        no.className = "lvlno";
        no.textContent = `#${lvl.levelNo}`;
        li.appendChild(name);
        li.appendChild(no);
        li.addEventListener("click", (e) => {
          e.stopPropagation();
          this.maybeCloseSidebarAfterPick();
          this.cb.onLevelClick(lvl.levelNo);
        });
        li.addEventListener("mouseenter", () => this.cb.onLevelHover(lvl.levelNo));
        li.addEventListener("mouseleave", () => this.cb.onLevelHover(null));
        ul.appendChild(li);
      }
      wrap.appendChild(ul);
    }
    return wrap;
  }

  setActiveUnit(unitId: number | null) {
    for (const wrap of this.$components.querySelectorAll<HTMLElement>(".unit")) {
      const on = wrap.dataset.unitId === String(unitId);
      wrap.classList.toggle("active", on);
      if (on) {
        let el: HTMLElement | null = wrap.parentElement;
        while (el && el !== this.$components) {
          el.classList.remove("collapsed");
          el = el.parentElement;
        }
      }
    }
  }

  setActiveLevel(levelNo: number | null) {
    for (const li of this.$components.querySelectorAll<HTMLLIElement>(".floor-row")) {
      li.classList.toggle("active", li.dataset.lvlNo === String(levelNo));
    }
  }

  setStatusBar(parts: { coord: string; tile: string; coll: string; zoom: string }) {
    this.$sbCoord.textContent = parts.coord;
    this.$sbTile.textContent = parts.tile;
    this.$sbColl.textContent = parts.coll;
    this.$sbZoom.textContent = parts.zoom;
  }

  showInspector(level: Level | null, preset: PresetJson | null) {
    if (!level || !preset) {
      this.$inspector.classList.add("empty");
      this.$inspector.textContent = "click a preset on the map…";
      return;
    }
    this.$inspector.classList.remove("empty");
    const m = markerFor(preset);
    const lines: string[] = [];
    const levelLbl = (level.displayName ?? level.name) + (hasOrifice(level) ? ORIFICE_STAR : "");
    lines.push(`level   ${levelLbl} (#${level.levelNo})`);
    if (level.displayName && level.displayName !== level.name) {
      lines.push(`        internal: ${level.name}`);
    }
    lines.push(`type    ${preset.type}  •  ${m.label}`);
    if (preset.kind && preset.kind !== "Generic") lines.push(`kind    ${preset.kind}`);
    if (preset.displayName || preset.name) {
      lines.push(`name    ${preset.displayName ?? preset.name}`);
      // When both are present and differ, surface the raw txt key too so
      // the inspector still doubles as a debugging readout.
      if (preset.displayName && preset.name && preset.displayName !== preset.name) {
        lines.push(`        internal: ${preset.name}`);
      }
    }
    if (preset.description) lines.push(`desc    ${preset.description}`);
    lines.push(`txtFile ${preset.txtFileNo}`);
    const gx = level.originX + preset.x;
    const gy = level.originY + preset.y;
    lines.push(`local   (${preset.x}, ${preset.y})`);
    lines.push(`global  (${gx}, ${gy})`);
    if (preset.destLevelNo) {
      const dest = preset.destDisplayName ?? preset.destName ?? "?";
      lines.push(`dest    ${dest} (#${preset.destLevelNo})`);
      if (preset.destDisplayName && preset.destName && preset.destDisplayName !== preset.destName) {
        lines.push(`        internal: ${preset.destName}`);
      }
    }
    this.$inspector.textContent = lines.join("\n");
  }

  // --- Legend with checkbox toggles ------------------------------------

  private renderLegend() {
    this.$legend.innerHTML = "";
    this.$legend.appendChild(
      this.buildLegendGroup("Collision", COLLISION_LEGEND, "collision"),
    );
    this.$legend.appendChild(
      this.buildLegendGroup("Markers", MARKER_LEGEND, "markers"),
    );
  }

  private buildLegendGroup<K extends CollisionKind | MarkerKind>(
    title: string,
    items: { id: K; label: string; color: { r: number; g: number; b: number } }[],
    bucket: "collision" | "markers",
  ): HTMLDivElement {
    const wrap = document.createElement("div");
    wrap.className = "legend-group";

    const head = document.createElement("div");
    head.className = "group-head";
    const h2 = document.createElement("span");
    h2.textContent = title;
    head.appendChild(h2);
    const allBtn = document.createElement("button");
    allBtn.type = "button";
    allBtn.textContent = "all / none";
    allBtn.addEventListener("click", () => {
      const map = this.vis[bucket] as Record<string, boolean>;
      const allOn = items.every((it) => map[it.id as string]);
      for (const it of items) map[it.id as string] = !allOn;
      this.refreshLegendChecks();
      this.emitVisibility();
    });
    head.appendChild(allBtn);
    wrap.appendChild(head);

    const ul = document.createElement("ul");
    for (const it of items) {
      const li = document.createElement("li");
      li.dataset.bucket = bucket;
      li.dataset.id = it.id as string;
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.checked = (this.vis[bucket] as Record<string, boolean>)[it.id as string];
      const sw = document.createElement("span");
      sw.className = "swatch";
      sw.style.background = rgb(it.color);
      const name = document.createElement("span");
      name.textContent = it.label;
      li.appendChild(cb);
      li.appendChild(sw);
      li.appendChild(name);
      // Whole-row click toggles too.
      li.addEventListener("click", (e) => {
        if (e.target !== cb) cb.checked = !cb.checked;
        (this.vis[bucket] as Record<string, boolean>)[it.id as string] = cb.checked;
        li.classList.toggle("off", !cb.checked);
        this.emitVisibility();
      });
      li.classList.toggle("off", !cb.checked);
      ul.appendChild(li);
    }
    wrap.appendChild(ul);
    return wrap;
  }

  // Pass a fresh snapshot — main.ts compares prev vs next to decide whether
  // to rebuild textures, which only works if the objects aren't aliased.
  private emitVisibility() {
    this.cb.onVisibilityChanged({
      collision: { ...this.vis.collision },
      markers: { ...this.vis.markers },
    });
  }

  private refreshLegendChecks() {
    for (const li of this.$legend.querySelectorAll<HTMLLIElement>("li[data-id]")) {
      const bucket = li.dataset.bucket as "collision" | "markers" | undefined;
      const id = li.dataset.id;
      if (!bucket || !id) continue;
      const cb = li.querySelector<HTMLInputElement>("input");
      if (!cb) continue;
      const on = (this.vis[bucket] as Record<string, boolean>)[id];
      cb.checked = on;
      li.classList.toggle("off", !on);
    }
  }

  // Populate the dev-mode seed dropdown by hitting the Vite plugin endpoint.
  private async tryLoadSeedDir() {
    const sel = this.$seedSelect;
    if (!sel) return;
    try {
      const res = await fetch("/api/seeds");
      if (!res.ok) return;
      const json = (await res.json()) as { dir: string; files: string[] };
      sel.innerHTML = "";
      const placeholder = document.createElement("option");
      placeholder.value = "";
      placeholder.textContent = `${json.files.length} seeds in ${shortPath(json.dir)}`;
      placeholder.disabled = true;
      placeholder.selected = true;
      sel.appendChild(placeholder);
      for (const f of json.files) {
        const o = document.createElement("option");
        o.value = f;
        o.textContent = f;
        sel.appendChild(o);
      }
      sel.disabled = json.files.length === 0;
    } catch {
      // Static build — endpoint absent.
    }
  }

  // Reflect the current seed in the textbox (used by the URL-param auto-load
  // so the user sees what's loaded without having typed it).
  setSeedInputValue(n: number) {
    this.$seedInput.value = String(n >>> 0);
  }
}

function byId<T extends HTMLElement>(id: string): T {
  const el = document.getElementById(id);
  if (!el) throw new Error(`missing #${id}`);
  return el as T;
}

function byIdOrNull<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

function shortPath(p: string): string {
  const parts = p.replace(/\\/g, "/").split("/");
  return parts.slice(-2).join("/");
}
