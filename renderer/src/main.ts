// Entry point — wires together the parser, viewport, renderer, and UI.

import { defaultLegendVisibility, type LegendVisibility } from "./colors";
import { computeHierarchy, type Hierarchy } from "./connectivity";
import { parseSeed } from "./parse";
import {
  computeContentBounds,
  describeCollision,
  pickAdjacent,
  pickCollision,
  pickPreset,
  pickRoom,
  renderFrame,
  worldToGameTile,
  type RenderState,
} from "./render";
import { buildLevelTexture, type LevelTexture } from "./texture";
import { inverseProjectTile, projectTile, type Projection } from "./projection";
import { Ui } from "./ui";
import {
  Tooltip,
  adjacentTooltip,
  presetTooltip,
  roomTooltip,
} from "./tooltip";
import { NO_DATA, type Seed, type SeedJson } from "./types";
import { Viewport, attachInputs } from "./viewport";

const canvas = document.getElementById("map") as HTMLCanvasElement;
const ctx = canvas.getContext("2d")!;
const canvasHost = document.getElementById("canvas-host")!;
const tooltip = new Tooltip(canvasHost);

const vp = new Viewport();
const initialVis: LegendVisibility = defaultLegendVisibility();
// Toggle-derived fields are getters that read live from the DOM via Ui.
// Eliminates any drift between RenderState and the checkboxes — e.g. on tab
// duplication, where browsers restore checkbox state without firing change
// events. `ui` is declared below; the getters resolve it lazily at call time.
const state: RenderState = {
  textures: [],
  vis: initialVis,
  hoveredLevelNo: null,
  selectedPreset: null,
  get proj(): Projection { return ui.getToggles().iso ? "iso" : "flat"; },
  get showAdjacents() { return ui.getToggles().adjacents; },
  get showRooms() { return ui.getToggles().rooms; },
  get showLabels() { return ui.getToggles().labels; },
  get showTells() { return ui.getToggles().tells; },
};

let seed: Seed | null = null;
let hierarchy: Hierarchy | null = null;
let activeUnitId: number | null = null;
// Most recently focused level. Maintained so that swapping the seed via the
// dev dropdown / JSON file picker can re-activate the equivalent unit in the
// new seed by looking up unitOfLevel(activeLevelNo).
let activeLevelNo: number | null = null;
const loadedActs = new Set<number>();
const loadingActs = new Set<number>();
// Token used to drop the result of an in-flight texture build when the
// active unit / projection / visibility changes mid-build. Without this we
// can paint stale textures on top of new ones.
let textureBuildId = 0;

let needsDraw = true;
const draw = () => {
  needsDraw = true;
};
vp.onChange = draw;

const ui = new Ui({
  onSeedFile: async (f) => {
    try {
      const text = await f.text();
      const json = JSON.parse(text) as SeedJson;
      await loadSeed(json);
      ui.setStatus(`loaded ${f.name}`);
    } catch (e: unknown) {
      ui.setStatus(`failed: ${errMsg(e)}`);
    }
  },
  onSeedFromDir: async (name) => {
    try {
      const res = await fetch(`/api/seed?name=${encodeURIComponent(name)}`);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const json = (await res.json()) as SeedJson;
      await loadSeed(json);
      ui.setStatus(`loaded ${name}`);
    } catch (e: unknown) {
      ui.setStatus(`failed: ${errMsg(e)}`);
    }
  },
  onSeedLookup: (n) => loadSeedAct1(n),
  onActLoad: (actNo) => loadAct(actNo),
  onTogglesChanged: () => { void applyToggles(); },
  onDifficultyChanged: () => { void applyDifficulty(); },
  onVisibilityChanged: (vis) => applyVisibility(vis),
  onResetView: () => fitView(),
  onLevelClick: (lvlNo) => onLevelChosen(lvlNo),
  onLevelHover: (lvlNo) => {
    // Only the sidebar drives the dim/highlight. Canvas hover never does.
    state.hoveredLevelNo = lvlNo;
    ui.setActiveLevel(lvlNo);
    draw();
  },
  onUnitActivate: (id) => activateUnit(id, /*fit=*/ true),
});

// The last projection we rebuilt textures for. Compared against the live
// state.proj on each toggle change to decide whether the textures need a
// rebuild (and the view a re-center).
let lastBuiltProj: Projection = state.proj;

attachInputs(
  canvas,
  vp,
  (bx, by) => onPointerMove(bx, by),
  (bx, by) => onClick(bx, by),
);

let lastClientX = 0;
let lastClientY = 0;
let lastBx = 0;
let lastBy = 0;
let pointerInside = false;
// On touch the natural interaction is tap-to-inspect, not hover-to-inspect.
// Tracking the most recent pointer type lets us suppress the tooltip during a
// pan (where pointermove fires every frame) and only show it explicitly on
// tap via onClick.
let lastPointerType: string = "mouse";
canvas.addEventListener("pointerdown", (e) => {
  // Static taps fire no pointermove between down and up, so we'd otherwise
  // position the tooltip with stale clientX/Y from a previous gesture.
  lastClientX = e.clientX;
  lastClientY = e.clientY;
  lastPointerType = e.pointerType;
  pointerInside = true;
});
canvas.addEventListener("pointermove", (e) => {
  lastClientX = e.clientX;
  lastClientY = e.clientY;
  lastPointerType = e.pointerType;
  pointerInside = true;
});
canvas.addEventListener("pointerleave", (e) => {
  pointerInside = false;
  // On touch the leave fires the instant the finger lifts, which would wipe
  // the tooltip we just put up in onClick. Mouse/pen still hide on leave.
  if (e.pointerType === "touch") return;
  tooltip.hide();
  ui.setStatusBar({ coord: "", tile: "", coll: "", zoom: `${vp.scale.toFixed(2)}×` });
});

// Re-run hover logic with the last known cursor position. Used after loading
// a new seed or switching levels/units so the tooltip + status bar don't
// keep showing stale picks from the old textures.
function refreshHover() {
  if (!pointerInside) return;
  onPointerMove(lastBx, lastBy);
}

function onPointerMove(bx: number, by: number) {
  lastBx = bx;
  lastBy = by;
  pointerInside = true;
  const [wx, wy] = vp.screenToWorld(bx, by);
  const [gx, gy] = worldToGameTile(state.proj, wx, wy);

  const collHit = pickCollision(state, wx, wy);
  const collStr = !collHit
    ? "off-map"
    : collHit.value === NO_DATA
      ? "no-data"
      : `coll ${describeCollision(collHit.value)}  •  ${collHit.level.displayName ?? collHit.level.name}#${collHit.level.levelNo} local (${collHit.lx},${collHit.ly})`;
  ui.setStatusBar({
    coord: `world (${wx.toFixed(1)}, ${wy.toFixed(1)})`,
    tile: `tile (${gx}, ${gy})`,
    coll: collStr,
    zoom: `${vp.scale.toFixed(2)}×`,
  });

  // Touch pointermoves come from panning/pinching, not hovering — keep the
  // tooltip out of the way and let onClick decide when to show it.
  if (lastPointerType === "touch") {
    tooltip.hide();
    return;
  }

  updateTooltipAt(wx, wy);
}

// Tooltip priority: preset → adjacent → room. Bare hovers (collision-only
// or off-map) hide the tooltip — that data lives in the status bar.
function updateTooltipAt(wx: number, wy: number) {
  const preset = pickPreset(state, wx, wy, vp.scale);
  if (preset) {
    tooltip.showAt(presetTooltip(preset.level, preset.preset), lastClientX, lastClientY);
    return;
  }
  const adj = pickAdjacent(state, wx, wy, vp.scale);
  if (adj) {
    let dups = 0;
    for (const a of adj.level.adjacents) {
      if (a.levelNo === adj.adjacent.levelNo &&
          a.bridgeX === adj.adjacent.bridgeX &&
          a.bridgeY === adj.adjacent.bridgeY) dups++;
    }
    tooltip.showAt(adjacentTooltip(adj.level, adj.adjacent, dups), lastClientX, lastClientY);
    return;
  }
  const room = pickRoom(state, wx, wy);
  if (room) {
    tooltip.showAt(roomTooltip(room.level, room.room), lastClientX, lastClientY);
    return;
  }
  tooltip.hide();
}

function onClick(bx: number, by: number) {
  const [wx, wy] = vp.screenToWorld(bx, by);
  const hit = pickPreset(state, wx, wy, vp.scale);
  state.selectedPreset = hit;
  ui.showInspector(hit?.level ?? null, hit?.preset ?? null);
  // Tap-to-inspect on touch (and mouse click too — harmless, since hover has
  // already populated the same tooltip).
  updateTooltipAt(wx, wy);
  draw();
}

// Click on a level in the sidebar — switch to its render unit (if needed)
// and fit camera to that level. Awaits the unit switch so the zoom lookup
// finds the freshly built texture rather than a stale one.
async function onLevelChosen(lvlNo: number) {
  const unitId = hierarchy?.unitOfLevel.get(lvlNo);
  if (unitId !== undefined && unitId !== activeUnitId) {
    await activateUnit(unitId, /*fit=*/ false);
  }
  zoomToLevel(lvlNo);
}

async function loadSeed(json: SeedJson) {
  // Path used by drag/drop, the dev-mode dropdown, and the JSON file picker
  // — these load a whole seed at once, so mark every act loaded. We try to
  // keep the user's current view: capture the previously active level, then
  // look up the unit containing it in the new hierarchy. Adjacency-based
  // unit IDs can differ across seeds, so we match by levelNo (which is
  // seed-invariant).
  const prevLevelNo = activeLevelNo;

  seed = parseSeed(json);
  hierarchy = computeHierarchy(seed);
  loadedActs.clear();
  loadingActs.clear();
  for (const a of hierarchy.acts) loadedActs.add(a.act);

  ui.setSeedLabel(seed);
  ui.setHierarchy(hierarchy, loadedActs, loadingActs);

  let targetUnitId: number | null = null;
  let targetLevelNo: number | null = null;
  if (prevLevelNo !== null) {
    const u = hierarchy.unitOfLevel.get(prevLevelNo);
    if (u !== undefined) {
      targetUnitId = u;
      targetLevelNo = prevLevelNo;
    }
  }
  if (targetUnitId === null) {
    targetUnitId = hierarchy.acts[0]?.units[0]?.id ?? null;
  }

  if (targetLevelNo !== null) {
    await activateUnit(targetUnitId, /*fit=*/ false);
    zoomToLevel(targetLevelNo);
  } else {
    await activateUnit(targetUnitId, /*fit=*/ true);
  }
}

async function fetchAct(seedNo: number, actNo: number): Promise<SeedJson> {
  const url = `/api/render?seed=${seedNo}&acts=${actNo}&difficulty=${ui.getDifficulty()}`;
  const res = await fetch(url);
  if (!res.ok) {
    const body = (await res.text()).trim();
    throw new Error(body || `HTTP ${res.status}`);
  }
  const json = (await res.json()) as SeedJson & { error?: string };
  if (json.error) throw new Error(json.error);
  return json;
}

// Initial seed entry: reset everything, fetch one act, populate. By default
// loads act 1, but if a level was previously focused we load the act
// containing it so we can restore the user's view across seed swaps.
async function loadSeedAct1(seedNo: number) {
  seedNo = seedNo >>> 0;
  ui.setSeedInputValue(seedNo);
  // Keep the URL shareable. replaceState (not pushState) so the back button
  // doesn't fill up with intermediate seeds the user typed.
  const url = new URL(window.location.href);
  url.searchParams.set("seed", String(seedNo));
  window.history.replaceState(null, "", url.toString());

  // Level IDs are seed-invariant, so we can re-focus the same level in the
  // new seed. Capture before the reset below clears activeLevelNo.
  const prevLevelNo = activeLevelNo;
  const targetAct = prevLevelNo !== null ? actOfLevel(prevLevelNo) : 1;

  ui.setStatus(`requesting seed ${seedNo} (act ${targetAct})…`);
  ui.setBusy(true, `rendering act ${targetAct}…`);

  // Reset state up front so the sidebar shows the target act = loading and
  // the others as click-to-load placeholders immediately.
  state.textures = [];
  state.selectedPreset = null;
  activeUnitId = null;
  activeLevelNo = null;
  seed = { seed: seedNo >>> 0, levels: [] };
  hierarchy = { acts: [], unitById: new Map(), unitOfLevel: new Map() };
  loadedActs.clear();
  loadingActs.clear();
  loadingActs.add(targetAct);
  ui.setSeedLabel(seed);
  ui.setHierarchy(hierarchy, loadedActs, loadingActs);

  try {
    const json = await fetchAct(seedNo, targetAct);
    if (!seed || seed.seed !== (json.seed >>> 0)) return; // stale, dropped
    seed = parseSeed(json);
    loadedActs.add(targetAct);
    loadingActs.delete(targetAct);
    hierarchy = computeHierarchy(seed);
    ui.setSeedLabel(seed);
    ui.setHierarchy(hierarchy, loadedActs, loadingActs);

    let targetUnitId: number | null = null;
    let targetLevelNo: number | null = null;
    if (prevLevelNo !== null) {
      const u = hierarchy.unitOfLevel.get(prevLevelNo);
      if (u !== undefined) {
        targetUnitId = u;
        targetLevelNo = prevLevelNo;
      }
    }
    if (targetUnitId === null) {
      targetUnitId = hierarchy.acts[0]?.units[0]?.id ?? null;
    }

    if (targetLevelNo !== null) {
      await activateUnit(targetUnitId, /*fit=*/ false);
      zoomToLevel(targetLevelNo);
    } else {
      await activateUnit(targetUnitId, /*fit=*/ true);
    }
    ui.setStatus(`loaded seed ${seedNo}, act ${targetAct}`);
  } catch (e: unknown) {
    loadingActs.delete(targetAct);
    ui.setHierarchy(hierarchy, loadedActs, loadingActs);
    ui.setStatus(`failed: ${errMsg(e)}`);
  } finally {
    ui.setBusy(false);
  }
}

function actOfLevel(lvlNo: number): number {
  for (let i = 0; i < ACT_RANGES.length; i++) {
    const [lo, hi] = ACT_RANGES[i]!;
    if (lvlNo >= lo && lvlNo <= hi) return i + 1;
  }
  return 1;
}

async function loadAct(actNo: number) {
  if (!seed || loadedActs.has(actNo) || loadingActs.has(actNo)) return;
  const seedNo = seed.seed;
  loadingActs.add(actNo);
  ui.setHierarchy(hierarchy ?? { acts: [], unitById: new Map(), unitOfLevel: new Map() },
                  loadedActs, loadingActs);
  ui.setStatus(`requesting act ${actNo}…`);
  try {
    const json = await fetchAct(seedNo, actNo);
    if (!seed || seed.seed !== (json.seed >>> 0)) return; // user switched seeds mid-flight
    const incoming = parseSeed(json);
    // Drop any levels for this act that might already be present (defensive
    // against re-fetches racing) before appending.
    const range = ACT_RANGES[actNo - 1]!;
    seed.levels = seed.levels.filter(
      (l) => !(l.levelNo >= range[0] && l.levelNo <= range[1]),
    );
    seed.levels.push(...incoming.levels);
    loadedActs.add(actNo);
    hierarchy = computeHierarchy(seed);
    ui.setSeedLabel(seed);
    ui.setStatus(`loaded act ${actNo}`);
  } catch (e: unknown) {
    ui.setStatus(`failed act ${actNo}: ${errMsg(e)}`);
  } finally {
    loadingActs.delete(actNo);
    ui.setHierarchy(hierarchy ?? { acts: [], unitById: new Map(), unitOfLevel: new Map() },
                    loadedActs, loadingActs);
  }
}

// Level-id ranges per act (1-indexed). Used to dedupe levels when a worker
// returns level data for a previously-loaded act (shouldn't happen but cheap
// to guard).
const ACT_RANGES: ReadonlyArray<[number, number]> = [
  [1, 39],
  [40, 74],
  [75, 102],
  [103, 108],
  [109, 132],
];

// Rebuild textures for the active unit only. Returns once textures are in
// place; under the hood it kicks off async DEFLATE decoding per level.
//
// A monotonic build id lets us throw away results when the active unit /
// projection / visibility changes mid-build. Without it a slow first
// activation could clobber a fast second one.
async function rebuildActiveTextures() {
  const myId = ++textureBuildId;

  if (!seed || activeUnitId === null || !hierarchy) {
    state.textures = [];
    draw();
    return;
  }
  const unit = hierarchy.unitById.get(activeUnitId);
  const levels = unit?.levels ?? [];

  // Show the spinner only when there's something to wait on. If everything
  // we need is already cached (e.g. re-activating a previously visited
  // unit), the awaits resolve synchronously-ish and the overlay would just
  // flash — annoying.
  const needsDecode = levels.some((l) => l.coll === null && l.collDeflateB64);
  if (needsDecode) ui.setBusy(true, `decoding ${levels.length} levels…`);

  const builds = await Promise.all(
    levels.map((lvl) => buildLevelTexture(lvl, state.proj, state.vis.collision)),
  );
  if (myId !== textureBuildId) {
    // Superseded — discard.
    return;
  }
  const ts: LevelTexture[] = [];
  for (const t of builds) if (t) ts.push(t);
  state.textures = ts;

  if (state.selectedPreset) {
    const lvlNo = state.selectedPreset.level.levelNo;
    if (!ts.some((t) => t.level.levelNo === lvlNo)) {
      state.selectedPreset = null;
      ui.showInspector(null, null);
    }
  }
  if (needsDecode) ui.setBusy(false);
  draw();
  refreshHover();
}

async function activateUnit(id: number | null, fit: boolean) {
  activeUnitId = id;
  ui.setActiveUnit(id);
  await rebuildActiveTextures();
  if (!fit) return;
  // Stairs-linked units (CaveLevel1+2, TowerCellar, TalRashasTomb...) have
  // their floors at far-apart world origins. Fitting the bounding box of
  // all of them produces a uselessly zoomed-out view, so we zoom to just
  // the first floor instead. Real wilderness units stay on fit-view.
  const unit = id !== null ? hierarchy?.unitById.get(id) : undefined;
  if (unit?.viaStairs && unit.levels.length > 1) {
    zoomToLevel(unit.levels[0]!.levelNo);
  } else {
    fitView();
    setActiveLevel(unit?.levels[0]?.levelNo ?? null);
  }
}

function fitView() {
  const b = computeContentBounds(state.textures);
  if (!b) return;
  vp.fitTo(b, 16);
  draw();
  refreshHover();
}

function zoomToLevel(lvlNo: number) {
  const t = state.textures.find((x) => x.level.levelNo === lvlNo);
  if (!t) return;
  vp.fitTo(
    { minX: t.worldX, minY: t.worldY, maxX: t.worldX + t.worldW, maxY: t.worldY + t.worldH },
    64,
  );
  setActiveLevel(lvlNo);
  ui.setActiveLevel(lvlNo);
  draw();
  refreshHover();
}

// Persist the focused level to ?level= in the URL. replaceState (not
// pushState) so clicking through levels doesn't fill up browser history.
function setActiveLevel(lvlNo: number | null) {
  activeLevelNo = lvlNo;
  const url = new URL(window.location.href);
  if (lvlNo === null) url.searchParams.delete("level");
  else url.searchParams.set("level", String(lvlNo));
  window.history.replaceState(null, "", url.toString());
}

// Difficulty change invalidates all loaded acts (the backend keys its cache on
// (seed, acts, difficulty)). Reset state and re-fetch the act containing the
// currently focused level so the view stays put.
async function applyDifficulty() {
  const url = new URL(window.location.href);
  url.searchParams.set("difficulty", String(ui.getDifficulty()));
  window.history.replaceState(null, "", url.toString());
  if (seed) await loadSeedAct1(seed.seed);
}

async function applyToggles() {
  // state.* are getters reading the live DOM; the change event fires after
  // the user's click has already flipped the checkbox, so reading here gets
  // the new value. Compare against lastBuiltProj to decide whether the
  // current textures are still valid.
  const proj = state.proj;
  if (proj !== lastBuiltProj) {
    const oldProj = lastBuiltProj;
    lastBuiltProj = proj;
    // Iso stretches each tile to ~2 world units in X (vs 1 in flat), so the
    // same vp.scale would either zoom in (flat→iso) or out (iso→flat). Adjust
    // scale by 2× and re-center on the same game tile so toggling feels like
    // changing the projection, not the camera.
    const [gx, gy] = inverseProjectTile(oldProj, vp.cx, vp.cy);
    const [ncx, ncy] = projectTile(proj, gx, gy);
    vp.cx = ncx;
    vp.cy = ncy;
    const factor = proj === "iso" ? 0.5 : 2;
    vp.scale = Math.min(vp.maxScale, Math.max(vp.minScale, vp.scale * factor));
    await rebuildActiveTextures();
  }
  draw();
}

async function applyVisibility(vis: LegendVisibility) {
  // Collision changes invalidate the pre-rendered textures; marker changes
  // only affect the next draw.
  const collChanged = (Object.keys(vis.collision) as (keyof typeof vis.collision)[])
    .some((k) => vis.collision[k] !== state.vis.collision[k]);
  state.vis = vis;
  if (collChanged) await rebuildActiveTextures();
  else draw();
}

function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  const w = Math.max(1, Math.floor(rect.width * dpr));
  const h = Math.max(1, Math.floor(rect.height * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  vp.resize(w, h);
  draw();
}

function errMsg(e: unknown): string {
  if (e instanceof Error) return e.message;
  return String(e);
}

const ro = new ResizeObserver(resizeCanvas);
ro.observe(canvas);
window.addEventListener("resize", resizeCanvas);
resizeCanvas();

function frame() {
  if (needsDraw) {
    needsDraw = false;
    renderFrame(ctx, vp, state);
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// Auto-load the seed from ?seed=N (default 0) so URLs are shareable and
// every cold open lands on something. ?level=L preselects a level; we seed
// activeLevelNo so loadSeedAct1 picks the containing act and re-zooms after
// the fetch resolves (same code path as a level switch across seeds).
{
  const params = new URLSearchParams(window.location.search);
  const raw = params.get("seed") ?? "0";
  const n = Number(raw);
  if (Number.isInteger(n) && n >= 0 && n <= 0xFFFFFFFF) {
    const rawDiff = params.get("difficulty");
    if (rawDiff !== null) {
      const d = Number(rawDiff);
      if (d === 0 || d === 1 || d === 2) {
        ui.setDifficulty(d);
      } else {
        ui.setStatus(`ignoring invalid ?difficulty=${rawDiff}`);
      }
    }
    const rawLevel = params.get("level");
    if (rawLevel !== null) {
      const lvl = Number(rawLevel);
      if (Number.isInteger(lvl) && lvl >= 1 && lvl <= 132) {
        activeLevelNo = lvl;
      } else {
        ui.setStatus(`ignoring invalid ?level=${rawLevel}`);
      }
    }
    void loadSeedAct1(n);
  } else {
    ui.setStatus(`ignoring invalid ?seed=${raw}`);
  }
}
