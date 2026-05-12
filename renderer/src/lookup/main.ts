import {
  ALL_SHAPES, findRoom, roomsByShape,
  shapeAsArrows, relabelDirectionWords,
  type RoomDef, type Shape,
} from "./rooms";
import {
  drawRoom, slotIsoPoly, polyToClipPath, polyBBox,
} from "./sprite";
import {
  buildSequence, nextBfsCell,
  cellIsConnected, shapeValidForCell,
  type PlacedRoom,
} from "./sequence";
import {
  lookup, LOOKUP_TRIGGER_THRESHOLD, LOOKUP_SAMPLE_THRESHOLD,
  FILTER_RENDER_CAP_NO_PARITY, FILTER_RENDER_CAP_WITH_PARITY,
  type LookupResponse,
} from "./lookup-api";
import {
  loadAtlas, getRoomCanvas, atlasKey,
} from "./atlas";
import {
  runFilter, type Dir, type FilterResponse,
} from "./filter-pipeline";
import "./style.css";

// Configurable: where the seed links in the results screen go. Override at
// build time with VITE_MAP_VIEWER_URL.
const MAP_VIEWER_URL =
  (import.meta.env.VITE_MAP_VIEWER_URL as string | undefined)
  ?? "https://maps.diablo.deadlybossmods.com/";

// Always format counts with en-US grouping (1,234) — a German "1.234" looks
// like "one point two three four" to most users. Use everywhere instead of
// bare `n.toLocaleString()`.
const fmt = (n: number): string => n.toLocaleString("en-US");

// User-facing phase order:
//   stairs1 → stairs2 → stairs3 → stairs4 → level4 → (filtering →) level5 → townexit → results
// L5 / townexit are skipped automatically once the candidate set is small
// enough. Stairs1-3 are optional (skippable) tell filters; stairs4 is
// required because the answer pre-seeds the L4 grid's stairs-up cell.
type Phase =
  | "stairs1" | "stairs2" | "stairs3" | "stairs4"
  | "level4"
  | "filtering"
  | "level5"
  | "townexit"
  | "results";

type StairsPhase = "stairs1" | "stairs2" | "stairs3" | "stairs4";

interface Answers {
  stairs1?: Dir;
  stairs2?: Dir;
  stairs3?: Dir;
  stairs4?: Dir;
  level5?:  "N" | "W";
  townExit?: Dir;
}

let phase: Phase = "stairs1";
const answers: Answers = {};
// Latest candidate list as it flows through the filtering pipeline.
let pipelineSeeds: number[] = [];
// Free-form status message shown on the filtering screen.
let filteringStatus = "";
let filteringProgress: { done: number; total: number } | null = null;

// Build a screenshot <img> that loads the server-generated thumbnail first
// (small, cropped, ~25% of the source) and swaps to the cropped full-size
// version on first hover. Falling back on the `error` event covers the case
// where the dev server isn't running the Go thumb handler — the browser
// just goes straight to the full URL.
// URLs always use .jpg regardless of the on-disk source extension — the
// server reads PNGs and other supported formats and emits JPEG. This keeps
// the cache path stable per source name and lets `tower-descriptions.json`
// reference `foo.png` or `foo.jpg` interchangeably.
function withExt(filename: string, ext: string): string {
  const dot = filename.lastIndexOf(".");
  if (dot < 0) return filename + ext;
  return filename.slice(0, dot) + ext;
}
function thumbName(filename: string): string  { return withExt(filename, "-thumb.jpg"); }
function jpgName(filename: string): string   { return withExt(filename, ".jpg"); }

// Returns a wrapper that paints the thumbnail at its declared size and pops
// up the cropped full-size image (lazy-loaded on first hover) absolutely
// positioned next to it. The hover trigger is a small "+" badge in the
// bottom-right of the thumbnail, not the whole tile — keeps the rest of
// the tile clickable as part of the picker without accidental popover
// triggers when the mouse passes through.
function makeScreenshotImg(filename: string, className: string, alt: string): HTMLElement {
  const wrap = document.createElement("div");
  wrap.className = className + " shot-wrap";

  const thumb = document.createElement("img");
  thumb.className = "shot-thumb-img";
  thumb.src = `/tower-screenshots/${thumbName(filename)}`;
  thumb.alt = alt;
  thumb.loading = "lazy";
  wrap.appendChild(thumb);

  // Zoom badge — the only thing that triggers the popover. The "+" is inline
  // SVG (not text) so it's geometrically centered regardless of the
  // browser/OS font metrics.
  const zoom = document.createElement("div");
  zoom.className = "shot-zoom";
  zoom.title = "Hover to see full size";
  zoom.innerHTML =
    '<svg viewBox="0 0 14 14" width="16" height="16" fill="none" ' +
    'stroke="currentColor" stroke-width="2" stroke-linecap="round" ' +
    'aria-hidden="true">' +
    '<path d="M7 2 V12 M2 7 H12"/>' +
    '</svg>';
  wrap.appendChild(zoom);

  const pop = document.createElement("div");
  pop.className = "shot-popover";
  pop.hidden = true;
  const fullImg = document.createElement("img");
  fullImg.alt = alt;
  pop.appendChild(fullImg);
  wrap.appendChild(pop);

  const fullUrl = `/tower-screenshots/${jpgName(filename)}`;
  let fullSrcSet = false;
  zoom.addEventListener("mouseenter", () => {
    if (!fullSrcSet) {
      fullImg.src = fullUrl;
      fullSrcSet = true;
    }
    pop.hidden = false;
    positionPopover(wrap, pop);
  });
  zoom.addEventListener("mouseleave", () => { pop.hidden = true; });
  return wrap;
}

// Anchor the popover to the right of the thumbnail when there's room (so
// the small preview stays visible). When the right side doesn't fit, fall
// back to viewport-centered. Recomputed on every mouseenter so a window
// resize between hovers doesn't strand the popover off-screen.
const POPOVER_MAX_W_PX  = 860;
const POPOVER_MAX_H_PX  = 560;
const POPOVER_VW_FRAC   = 0.7;
const POPOVER_VH_FRAC   = 0.7;
const POPOVER_GAP_PX    = 12;
const POPOVER_MARGIN_PX = 8;

function positionPopover(wrap: HTMLElement, pop: HTMLElement) {
  const rect = wrap.getBoundingClientRect();
  const vw = window.innerWidth;
  const vh = window.innerHeight;
  const popW = Math.min(POPOVER_MAX_W_PX, vw * POPOVER_VW_FRAC);
  const popH = Math.min(POPOVER_MAX_H_PX, vh * POPOVER_VH_FRAC);

  if (vw - rect.right - POPOVER_GAP_PX - POPOVER_MARGIN_PX >= popW) {
    const top = Math.max(POPOVER_MARGIN_PX,
                         Math.min(vh - popH - POPOVER_MARGIN_PX, rect.top));
    pop.style.left = `${rect.right + POPOVER_GAP_PX}px`;
    pop.style.top = `${top}px`;
    pop.style.transform = "";
    return;
  }
  // Not enough space on the right — center on the viewport instead.
  pop.style.left = "50%";
  pop.style.top = "50%";
  pop.style.transform = "translate(-50%, -50%)";
}

// Cardinal directions ordered for the stairs picker grid: top-left,
// top-right, bottom-left, bottom-right — so a 2×2 layout reads naturally.
const STAIRS_DIRS: Array<{ dir: Dir; label: string; arrow: string }> = [
  { dir: "W", label: "top left",     arrow: "↖" },
  { dir: "N", label: "top right",    arrow: "↗" },
  { dir: "S", label: "bottom left",  arrow: "↙" },
  { dir: "E", label: "bottom right", arrow: "↘" },
];

// ---- Geometry ------------------------------------------------------------
const GRID_DIM = 7;
const GRID_OFFSET = 4;
const GRID_CENTER = GRID_OFFSET + Math.floor(GRID_DIM / 2);     // (7, 7)
const CELL_W = 120;
const CELL_H = 60;
const STEP_X = CELL_W / 2;
const STEP_Y = CELL_H / 2;

const MIN_DX = -(GRID_DIM - 1);
const MIN_SY = 2 * GRID_OFFSET;
const ORIGIN_X = -MIN_DX * STEP_X + CELL_W / 2;
const ORIGIN_Y = -MIN_SY * STEP_Y + CELL_H / 2;
const CONTAINER_W = 2 * ORIGIN_X;
const CONTAINER_H = 2 * (GRID_DIM - 1) * STEP_Y + CELL_H;

// Modal sprite sizes — bumped up so the user can actually read the room.
const SPRITE_PX_BIG_W = 560;
const SPRITE_PX_BIG_H = 280;
const SPRITE_PX_TILE_W = 200;
const SPRITE_PX_TILE_H = 100;

// ---- State --------------------------------------------------------------

const placed = new Map<string, PlacedRoom>();
const cellKey = (x: number, y: number) => `${x},${y}`;

type PickerStep = "room" | "graves";

let activeModalCell: { cellX: number; cellY: number } | null = null;
let editingExisting: PlacedRoom | null = null;
let pickerStep: PickerStep = "room";
let pickerRoom: RoomDef | null = null;
let pickerVariant = 0;
let pickerGraves = 0;
let pickerGravesUnknown = 0;

// Global cap on the total number of grave slots in the "unknown" state across
// every placed room. Trielookup fans out 2^N internal queries to cover an
// N-bit umask, so we keep it tight.
const MAX_UNKNOWN_GRAVES = 4;
function popcount8(b: number): number {
  b = b & 0xFF;
  b = b - ((b >> 1) & 0x55);
  b = (b & 0x33) + ((b >> 2) & 0x33);
  return (b + (b >> 4)) & 0x0F;
}
// Sum of unknown bits across every placed room, optionally substituting the
// picker's in-flight (current room) state for that room's stored value.
function totalUnknownGraves(useLiveForCurrent = true): number {
  const here = activeModalCell ? cellKey(activeModalCell.cellX, activeModalCell.cellY) : null;
  let n = 0;
  for (const [k, r] of placed) {
    if (useLiveForCurrent && here === k) continue;
    n += popcount8(r.gravesUnknown);
  }
  if (useLiveForCurrent && activeModalCell && pickerRoom) {
    n += popcount8(pickerGravesUnknown);
  }
  return n;
}

let descriptions: Descriptions = { rooms: {}, graves: { default: {} } };
const baseCanvases = new Map<string, HTMLCanvasElement>();   // key = roomId_variant

// ---- Categories ---------------------------------------------------------
// Order matches the user's preferred display: corner / corridor / intersection
// / dead end / stairs / cross. Stairs are a subset of dead-end *shapes* but
// get their own bucket because they're the BFS anchor and players think of
// them as their own thing.
const CATEGORIES: { id: string; label: string }[] = [
  { id: "corner",       label: "Corner pieces" },
  { id: "corridor",     label: "Corridor pieces" },
  { id: "intersection", label: "T-intersections" },
  { id: "deadend",      label: "Dead-end rooms" },
  { id: "stairs",       label: "Stairs" },
  { id: "cross",        label: "Four-way crossings" },
];

function categoryOf(room: RoomDef): string {
  if (room.theme === "StairsUp" || room.theme === "StairsDown") return "stairs";
  const len = room.shape.length;
  if (len === 1) return "deadend";
  if (room.shape === "EW" || room.shape === "NS") return "corridor";
  if (len === 2) return "corner";
  if (len === 3) return "intersection";
  return "cross";
}

// ---- Descriptions config -----------------------------------------------

interface RoomDescription {
  displayName?: string;
  description?: string;
  screenshots?: string[];
}
interface GravesDescription {
  title?: string;
  intro?: string;
  screenshot?: string;
}
interface Descriptions {
  rooms: Record<string, RoomDescription>;
  graves: { default: GravesDescription } & Record<string, GravesDescription>;
}

async function loadDescriptions(): Promise<Descriptions> {
  try {
    const r = await fetch("/tower-descriptions.json");
    if (!r.ok) return descriptions;
    const json = await r.json();
    return {
      rooms: json.rooms ?? {},
      graves: { default: json.graves?.default ?? {}, ...(json.graves ?? {}) },
    };
  } catch {
    return descriptions;
  }
}

function roomDesc(roomId: number, variant: number): RoomDescription {
  return descriptions.rooms[atlasKey(roomId, variant)] ?? {};
}
function gravesDescFor(roomId: number, variant: number): GravesDescription {
  return descriptions.graves[atlasKey(roomId, variant)] ?? descriptions.graves.default;
}

// ---- DOM init -----------------------------------------------------------

const app = document.getElementById("app")!;
app.innerHTML = `
  <button id="sidebar-toggle" type="button" aria-label="Open menu" aria-controls="sidebar" aria-expanded="false">
    <span class="hamburger"></span>
  </button>
  <div id="sidebar-backdrop" hidden></div>
  <aside id="sidebar">
    <header>
      <button id="sidebar-close" type="button" aria-label="Close menu">×</button>
      <h1>D2 Seed Lookup</h1>
      <div class="subtitle">
        Building your <b>Tower Cellar Level 4</b> map. Place every room
        you walked through; the BFS sequence on the right narrows the
        seed candidates as you go.
      </div>
    </header>
    <section>
      <h2>BFS sequence</h2>
      <div id="seq-chips" class="seq-chips"></div>
      <div class="count-card pending" id="count-card">
        <div class="big" id="count-big">—</div>
        <div class="label" id="count-label">place ${LOOKUP_TRIGGER_THRESHOLD}+ rooms to query</div>
      </div>
      <div id="sample-seeds" class="sample-seeds" hidden></div>
    </section>
    <section>
      <h2>Extra filters</h2>
      <div id="extra-filters-summary" class="seq-chips"></div>
      <button id="edit-extra-filters" class="btn" style="width:100%; margin-top:8px;">Edit extra filters</button>
    </section>
    <section>
      <button id="find-seeds" class="btn primary" style="width:100%;">Find seeds</button>
    </section>
    <section>
      <button id="reset-view" class="btn">Reset view</button>
      <button id="clear-all" class="btn danger" style="margin-left:6px;">Clear all rooms</button>
    </section>
    ${import.meta.env.DEV ? `
    <section id="dev-section">
      <h2>Debug — full sequence</h2>
      <div class="subtitle" style="margin-bottom:6px;">paste into <code>trielookup decode</code></div>
      <div id="dbg-hex" class="dbg-hex" title="click to select"></div>
      <ul id="dbg-decode" class="dbg-decode"></ul>
    </section>
    ` : ""}
  </aside>
  <main id="grid-host">
    <div id="loading" class="loading-overlay">loading tower atlas…</div>
    <div id="grid" class="iso-grid"></div>
  </main>

  <div id="modal" class="modal-backdrop">
    <div class="modal">
      <header>
        <h2 id="modal-title">Add a room</h2>
        <button id="modal-close" class="modal-close" aria-label="Close">×</button>
      </header>
      <div class="body" id="modal-body"></div>
      <footer>
        <div class="steps" id="modal-steps"></div>
        <div style="display:flex; gap:8px;">
          <button id="modal-back" class="btn">Back</button>
          <button id="modal-next" class="btn primary">Next</button>
        </div>
      </footer>
    </div>
  </div>

  <div id="phase-overlay" class="phase-overlay" hidden></div>
`;

// ---- Atlas-driven base canvases ----------------------------------------

function baseCanvasFor(roomId: number, variant: number): HTMLCanvasElement | null {
  return baseCanvases.get(atlasKey(roomId, variant)) ?? null;
}

// ---- Grid + viewport ---------------------------------------------------

const gridHostEl = document.getElementById("grid-host")! as HTMLElement;
const gridEl = document.getElementById("grid")! as HTMLDivElement;
gridEl.style.width = `${CONTAINER_W}px`;
gridEl.style.height = `${CONTAINER_H}px`;

// Viewport state — translate+scale applied to the grid container. The grid
// itself is flex-centered inside gridHost (its layout box sits at (Lx, Ly)
// = ((host - container)/2)) and `transform-origin: 0 0`, so scaling expands
// down-right from that top-left point. Each scale change therefore needs a
// matching translate to keep the visual center of the grid pinned. We bake
// that recentering offset into the initial pan so the very first frame
// already shows the grid centered.
const INITIAL_SCALE = 3;
function centeringPan(scale: number): { x: number; y: number } {
  return {
    x: CONTAINER_W * (1 - scale) / 2,
    y: CONTAINER_H * (1 - scale) / 2,
  };
}
const initPan = centeringPan(INITIAL_SCALE);
const view = { scale: INITIAL_SCALE, panX: initPan.x, panY: initPan.y };
function applyViewport() {
  gridEl.style.transform = `translate(${view.panX}px, ${view.panY}px) scale(${view.scale})`;
}
function resetView() {
  const p = centeringPan(INITIAL_SCALE);
  view.scale = INITIAL_SCALE; view.panX = p.x; view.panY = p.y;
  applyViewport();
}
// Apply the initial transform on boot. Without this the gridEl stays at
// its CSS default (no transform = scale 1) and only catches up on the
// first wheel/pan, leading to a confusing first zoom where the displayed
// scale and the state's scale disagree.
applyViewport();

// ---- Mobile sidebar toggle -----------------------------------------------
const sidebarEl       = document.getElementById("sidebar")!;
const sidebarToggleEl = document.getElementById("sidebar-toggle")!;
const sidebarCloseEl  = document.getElementById("sidebar-close")!;
const sidebarBackdrop = document.getElementById("sidebar-backdrop")!;
function openSidebar() {
  sidebarEl.classList.add("open");
  sidebarBackdrop.hidden = false;
  sidebarToggleEl.setAttribute("aria-expanded", "true");
}
function closeSidebar() {
  sidebarEl.classList.remove("open");
  sidebarBackdrop.hidden = true;
  sidebarToggleEl.setAttribute("aria-expanded", "false");
}
sidebarToggleEl.addEventListener("click", openSidebar);
sidebarCloseEl.addEventListener("click", closeSidebar);
sidebarBackdrop.addEventListener("click", closeSidebar);

// Wheel zoom (anchored at cursor).
//
// The grid is flex-centered inside gridHost, so its pre-transform top-left
// sits at (Lx, Ly) = ((host - container) / 2). A point (gx, gy) in gridEl's
// native coords paints at (Lx + panX + gx * scale, Ly + panY + gy * scale)
// in gridHost-relative pixels. To keep the point under the cursor stationary
// across a zoom, solve for the new panX such that
//   mx - Lx == panX_new + gx * scale_new
// where gx = (mx - Lx - panX_old) / scale_old.
gridHostEl.addEventListener("wheel", (e: WheelEvent) => {
  if (e.target instanceof Element && e.target.closest(".modal-backdrop.open")) return;
  e.preventDefault();
  const rect = gridHostEl.getBoundingClientRect();
  const mx = e.clientX - rect.left;
  const my = e.clientY - rect.top;
  const Lx = (gridHostEl.clientWidth  - CONTAINER_W) / 2;
  const Ly = (gridHostEl.clientHeight - CONTAINER_H) / 2;
  const ax = mx - Lx;
  const ay = my - Ly;
  const dz = e.deltaY > 0 ? 1 / 1.12 : 1.12;
  const oldScale = view.scale;
  view.scale = Math.max(0.3, Math.min(6.0, view.scale * dz));
  const k = view.scale / oldScale;
  view.panX = ax - (ax - view.panX) * k;
  view.panY = ay - (ay - view.panY) * k;
  applyViewport();
}, { passive: false });

// Pointer drag pan + pinch zoom.
//
//   - 1 pointer (mouse / single touch): drag-pan after DRAG_THRESHOLD. Below
//     the threshold the gesture is treated as a click.
//   - 2 pointers (multi-touch): pinch zoom anchored at the midpoint, with
//     midpoint translation also panning the view ("Maps"-style two-finger
//     drag).
//
// We track every active pointer in a Map and switch modes implicitly based
// on the count. setPointerCapture is deferred to "definitely dragging" so
// clicks on cells still land.
let dragStart: { x: number; y: number; panX: number; panY: number; pointerId: number; moved: boolean } | null = null;
let suppressNextClick = false;
const DRAG_THRESHOLD = 4;

const activePointers = new Map<number, { x: number; y: number }>();
let pinchStart: { dist: number; midX: number; midY: number; scale: number; panX: number; panY: number } | null = null;

function gridHostRect() { return gridHostEl.getBoundingClientRect(); }
function midPointDist(): { mx: number; my: number; d: number } {
  const pts = [...activePointers.values()];
  const mx = (pts[0].x + pts[1].x) / 2;
  const my = (pts[0].y + pts[1].y) / 2;
  const dx = pts[0].x - pts[1].x;
  const dy = pts[0].y - pts[1].y;
  return { mx, my, d: Math.hypot(dx, dy) };
}

gridHostEl.addEventListener("pointerdown", (e: PointerEvent) => {
  if (e.button !== 0 && e.pointerType === "mouse") return;
  activePointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (activePointers.size === 1) {
    // Do NOT setPointerCapture on pointerdown. Capturing here would
    // retarget the subsequent click event to gridHostEl instead of the
    // cell underneath, so the cell's click handler would never fire even
    // for a simple click. We defer capture to pointermove.
    dragStart = { x: e.clientX, y: e.clientY, panX: view.panX, panY: view.panY, pointerId: e.pointerId, moved: false };
    pinchStart = null;
  } else if (activePointers.size === 2) {
    // Switch from drag mode (if any) to pinch mode.
    dragStart = null;
    gridHostEl.classList.remove("dragging");
    const { mx, my, d } = midPointDist();
    pinchStart = { dist: d, midX: mx, midY: my, scale: view.scale, panX: view.panX, panY: view.panY };
    // Capture both pointers so the gesture survives moving off-grid.
    try { gridHostEl.setPointerCapture(e.pointerId); } catch {}
  }
});

gridHostEl.addEventListener("pointermove", (e: PointerEvent) => {
  if (activePointers.has(e.pointerId)) {
    activePointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  }

  if (pinchStart && activePointers.size >= 2) {
    const { mx, my, d } = midPointDist();
    const k = d / pinchStart.dist;
    const newScale = Math.max(0.3, Math.min(6.0, pinchStart.scale * k));
    const realK = newScale / pinchStart.scale;
    // Anchor the original midpoint in grid-coords + add the midpoint
    // translation since the start (so the "two-finger drag" component
    // pans naturally on top of the zoom).
    const rect = gridHostRect();
    const Lx = (gridHostEl.clientWidth  - CONTAINER_W) / 2;
    const Ly = (gridHostEl.clientHeight - CONTAINER_H) / 2;
    const ax = pinchStart.midX - rect.left - Lx;
    const ay = pinchStart.midY - rect.top  - Ly;
    view.scale = newScale;
    view.panX = ax - (ax - pinchStart.panX) * realK + (mx - pinchStart.midX);
    view.panY = ay - (ay - pinchStart.panY) * realK + (my - pinchStart.midY);
    applyViewport();
    return;
  }

  if (!dragStart || dragStart.pointerId !== e.pointerId) return;
  const dx = e.clientX - dragStart.x;
  const dy = e.clientY - dragStart.y;
  if (!dragStart.moved && Math.abs(dx) + Math.abs(dy) > DRAG_THRESHOLD) {
    dragStart.moved = true;
    gridHostEl.classList.add("dragging");
    try { gridHostEl.setPointerCapture(e.pointerId); } catch {}
  }
  if (dragStart.moved) {
    view.panX = dragStart.panX + dx;
    view.panY = dragStart.panY + dy;
    applyViewport();
  }
});

function endPointer(e: PointerEvent) {
  activePointers.delete(e.pointerId);
  if (pinchStart && activePointers.size < 2) {
    pinchStart = null;
    // If a finger is still down, re-seed a drag at its current position so
    // pinching → 1-finger pan flows naturally.
    if (activePointers.size === 1) {
      const [id] = [...activePointers.keys()];
      const p = activePointers.get(id)!;
      dragStart = { x: p.x, y: p.y, panX: view.panX, panY: view.panY, pointerId: id, moved: true };
    }
    // Pinch counts as a non-click gesture too.
    suppressNextClick = true;
  }
  if (dragStart && dragStart.pointerId === e.pointerId) {
    const wasDrag = dragStart.moved;
    try { gridHostEl.releasePointerCapture(e.pointerId); } catch {}
    dragStart = null;
    gridHostEl.classList.remove("dragging");
    if (wasDrag) suppressNextClick = true;
  }
}
gridHostEl.addEventListener("pointerup", endPointer);
gridHostEl.addEventListener("pointercancel", endPointer);
// Capture-phase click handler swallows the post-drag click before it
// reaches a cell's bubbling-phase listener.
gridHostEl.addEventListener("click", (e: MouseEvent) => {
  if (suppressNextClick) {
    e.stopPropagation();
    e.preventDefault();
    suppressNextClick = false;
  }
}, true);

document.getElementById("reset-view")!.addEventListener("click", resetView);

function cellCenter(cx: number, cy: number): { x: number; y: number } {
  return {
    x: (cx - cy) * STEP_X + ORIGIN_X,
    y: (cx + cy) * STEP_Y + ORIGIN_Y,
  };
}

function makeCellEl(cx: number, cy: number): HTMLDivElement {
  const cell = document.createElement("div");
  cell.className = "iso-cell";
  const center = cellCenter(cx, cy);
  cell.style.left = `${center.x - CELL_W / 2}px`;
  cell.style.top  = `${center.y - CELL_H / 2}px`;
  cell.style.width  = `${CELL_W}px`;
  cell.style.height = `${CELL_H}px`;
  cell.dataset.cx = String(cx);
  cell.dataset.cy = String(cy);
  return cell;
}

function renderGrid() {
  gridEl.innerHTML = "";
  const rooms = [...placed.values()];

  // Initial state: only one cell at the visible center is available. The
  // first placement is always StairsUp by construction (see allowedShapes).
  if (rooms.length === 0) {
    const cell = makeCellEl(GRID_CENTER, GRID_CENTER);
    cell.classList.add("empty", "next");
    const label = document.createElement("div");
    label.className = "next-label";
    label.textContent = "place stairs up";
    cell.appendChild(label);
    cell.addEventListener("click", () => openPicker(GRID_CENTER, GRID_CENTER));
    gridEl.appendChild(cell);
    return;
  }

  const next = nextBfsCell(rooms);
  const seqResult = buildSequence(rooms);
  const pathIdx = new Map<string, number>();
  seqResult.path.forEach((p, i) => pathIdx.set(cellKey(p.cellX, p.cellY), i + 1));

  for (let row = 0; row < GRID_DIM; row++) {
    for (let col = 0; col < GRID_DIM; col++) {
      const cx = col + GRID_OFFSET;
      const cy = row + GRID_OFFSET;
      const cell = makeCellEl(cx, cy);

      const here = placed.get(cellKey(cx, cy));
      if (here) {
        cell.classList.add("placed");
        const def = findRoom(here.roomId)!;
        // Supersample so the cell canvas still has enough pixels when the
        // gridEl's CSS transform scales it up. The factor should be ≥ the
        // expected display scale; we use INITIAL_SCALE so the boot view
        // is pixel-perfect and zoom-out / mild zoom-in stays sharp.
        // Combined with `image-rendering: pixelated` (set on .room-canvas
        // in style.css) the CSS scale becomes nearest-neighbor on top of
        // already-supersampled pixels, which is what keeps tiles crisp.
        const dpr = (window.devicePixelRatio || 1) * INITIAL_SCALE;
        const canvas = document.createElement("canvas");
        canvas.width = CELL_W * dpr;
        canvas.height = CELL_H * dpr;
        canvas.style.width = `${CELL_W}px`;
        canvas.style.height = `${CELL_H}px`;
        const ctx = canvas.getContext("2d")!;
        ctx.imageSmoothingEnabled = false;     // keep atlas pixels crisp through the cell scale
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        drawRoom(ctx, def, here.variant, {
          cellW: CELL_W, cellH: CELL_H,
          showGraves: true,
          gravesMask: here.graves,
          gravesUnknownMask: here.gravesUnknown,
          baseCanvas: baseCanvasFor(here.roomId, here.variant),
        });
        canvas.className = "room-canvas";
        cell.appendChild(canvas);
        const idx = pathIdx.get(cellKey(cx, cy));
        if (idx !== undefined) {
          const label = document.createElement("div");
          label.className = "seq-idx";
          label.textContent = String(idx);
          cell.appendChild(label);
        }
        cell.addEventListener("click", () => openPicker(cx, cy));
      } else {
        const enabled = cellIsConnected(cx, cy, placed);
        if (!enabled) {
          cell.classList.add("disabled");
        } else {
          cell.classList.add("empty");
          if (next && next.cellX === cx && next.cellY === cy) {
            cell.classList.add("next");
            const label = document.createElement("div");
            label.className = "next-label";
            label.textContent = "next";
            cell.appendChild(label);
          }
          cell.addEventListener("click", () => openPicker(cx, cy));
        }
      }
      gridEl.appendChild(cell);
    }
  }
}

// ---- Sidebar / lookup ---------------------------------------------------

const seqChipsEl = document.getElementById("seq-chips")!;
const countCardEl = document.getElementById("count-card")!;
const countBigEl = document.getElementById("count-big")!;
const countLabelEl = document.getElementById("count-label")!;
const sampleSeedsEl = document.getElementById("sample-seeds")!;
const findSeedsBtn = document.getElementById("find-seeds")! as HTMLButtonElement;

// Most-recent raw candidate count from /api/lookup. -1 means "not yet
// queried" (prefix too short). Drives the Find seeds button gate and the
// "L5 is required" branch in the click handler — both mirror the server-
// side cap enforced in mapserver/filter.go.
let lastLookupCount = -1;

type FindSeedsGate =
  | { state: "pending" }                            // no lookup count yet
  | { state: "ok" }                                 // ≤ no-parity cap
  | { state: "needs-parity" }                       // in band, L5 unset
  | { state: "needs-parity-answered" }              // in band, L5 set → ok
  | { state: "too-many"; count: number };           // disable button

function findSeedsGate(): FindSeedsGate {
  if (lastLookupCount < 0) return { state: "pending" };
  if (lastLookupCount > FILTER_RENDER_CAP_WITH_PARITY) {
    return { state: "too-many", count: lastLookupCount };
  }
  if (lastLookupCount > FILTER_RENDER_CAP_NO_PARITY) {
    return answers.level5 !== undefined
      ? { state: "needs-parity-answered" }
      : { state: "needs-parity" };
  }
  return { state: "ok" };
}

// Apply the gate to the Find seeds button — disabled label / enabled label
// / hint text. Called from applyLookupResponse and whenever the user's
// answers change (so toggling L5 in extras re-enables the button).
function updateFindSeedsButton() {
  const g = findSeedsGate();
  findSeedsBtn.classList.remove("requires-l5");
  switch (g.state) {
    case "pending":
      findSeedsBtn.disabled = true;
      findSeedsBtn.textContent = "Find seeds";
      findSeedsBtn.title = "place more rooms first";
      break;
    case "too-many":
      findSeedsBtn.disabled = true;
      findSeedsBtn.textContent = "Find seeds";
      findSeedsBtn.title = `${fmt(g.count)} candidates — narrow to ${FILTER_RENDER_CAP_WITH_PARITY} or fewer to enable`;
      break;
    case "needs-parity":
      findSeedsBtn.disabled = false;
      findSeedsBtn.classList.add("requires-l5");
      findSeedsBtn.textContent = "Find seeds";
      findSeedsBtn.title = `${fmt(lastLookupCount)} candidates — the Countess (L5) answer is required at this size`;
      break;
    case "needs-parity-answered":
    case "ok":
      findSeedsBtn.disabled = false;
      findSeedsBtn.textContent = "Find seeds";
      findSeedsBtn.title = "";
      break;
  }
}

function renderSidebar() {
  const rooms = [...placed.values()];
  const seqResult = buildSequence(rooms);
  seqChipsEl.innerHTML = "";
  if (seqResult.seq.length === 0) {
    const empty = document.createElement("div");
    empty.className = "seq-empty";
    empty.textContent = "no stairs-up placed yet";
    seqChipsEl.appendChild(empty);
  } else {
    seqResult.seq.forEach((v, i) => {
      const def = findRoom(((v >> 8) & 0x7F) + 100);
      const chip = document.createElement("span");
      chip.className = "seq-chip";
      chip.title = `0x${v.toString(16).toUpperCase().padStart(4, "0")}`;
      const label = def
        ? `${shapeAsArrows(def.shape)} ${relabelDirectionWords(def.theme)}`
        : "?";
      chip.innerHTML = `<span class="idx">${i + 1}</span><span>${label}</span>`;
      seqChipsEl.appendChild(chip);
    });
  }

  // Sync part: figure out whether we'll even query (saves a network round-trip
  // when the prefix is too short). The async lookup races with future
  // renderSidebar calls — we discard stale responses via lookupToken below.
  countCardEl.classList.remove("pending", "narrow");
  if (seqResult.seq.length < LOOKUP_TRIGGER_THRESHOLD) {
    countCardEl.classList.add("pending");
    countBigEl.textContent = "—";
    const remaining = LOOKUP_TRIGGER_THRESHOLD - seqResult.seq.length;
    countLabelEl.textContent = `place ${remaining} more BFS-reachable room${remaining === 1 ? "" : "s"}`;
    sampleSeedsEl.hidden = true;
    sampleSeedsEl.innerHTML = "";
    lastLookupCount = -1;
    updateFindSeedsButton();
  } else {
    countCardEl.classList.add("pending");
    countBigEl.textContent = "…";
    countLabelEl.textContent = "querying…";
    lastLookupCount = -1;
    updateFindSeedsButton();
    const myToken = ++lookupToken;
    lookup(rooms).then((resp) => {
      if (myToken !== lookupToken) return;            // a newer render fired
      applyLookupResponse(resp);
    }).catch((e) => {
      if (myToken !== lookupToken) return;
      countCardEl.classList.remove("narrow");
      countBigEl.textContent = "?";
      countLabelEl.textContent = "lookup failed: " + (e as Error).message;
      lastLookupCount = -1;
      updateFindSeedsButton();
    });
  }

  renderExtraFiltersSummary();
  if (import.meta.env.DEV) renderDevDebug(seqResult.seq, seqResult.unknownMasks);
}

let lookupToken = 0;

function applyLookupResponse(resp: LookupResponse) {
  countCardEl.classList.remove("pending", "narrow");
  if (!resp.triggered) {
    countCardEl.classList.add("pending");
    countBigEl.textContent = "—";
    countLabelEl.textContent = "(no result)";
    sampleSeedsEl.hidden = true;
    sampleSeedsEl.innerHTML = "";
    lastLookupCount = -1;
    updateFindSeedsButton();
    return;
  }
  if (resp.count <= LOOKUP_SAMPLE_THRESHOLD) countCardEl.classList.add("narrow");
  countBigEl.textContent = fmt(resp.count);
  countLabelEl.textContent = "candidate seeds";
  lastLookupCount = resp.count;
  updateFindSeedsButton();
  if (resp.sampleSeeds && resp.sampleSeeds.length > 0) {
    sampleSeedsEl.hidden = false;
    sampleSeedsEl.innerHTML = `<div class="head">Sample seeds</div>` +
      resp.sampleSeeds.map((s) =>
        `<div class="seed-row"><a href="${MAP_VIEWER_URL}?seed=${s}&level=21" target="_blank" rel="noopener">${s}</a></div>`,
      ).join("");
  } else {
    sampleSeedsEl.hidden = true;
    sampleSeedsEl.innerHTML = "";
  }
}

function renderDevDebug(seq: number[], unknownMasks: number[]) {
  const hexEl = document.getElementById("dbg-hex");
  const decodeEl = document.getElementById("dbg-decode");
  if (!hexEl || !decodeEl) return;
  if (seq.length === 0) {
    hexEl.textContent = "(empty)";
    decodeEl.innerHTML = "";
    return;
  }
  // Match trielookup's BASE[/UMASK] prefix syntax — copy-paste straight in.
  const hex = seq.map((v, i) => {
    const base = v.toString(16).toUpperCase().padStart(4, "0");
    const um = unknownMasks[i] ?? 0;
    return um === 0 ? base : `${base}/${um.toString(16).toUpperCase().padStart(4, "0")}`;
  }).join(",");
  hexEl.textContent = hex;
  decodeEl.innerHTML = seq.map((v, i) => {
    const def = findRoom(((v >> 8) & 0x7F) + 100);
    const variant = (v >> 15) & 1;
    const graves = v & 0xFF;
    const um = unknownMasks[i] ?? 0;
    if (!def) return `<li>${i + 1}. (unknown room)</li>`;
    const variantName = def.variants[variant]?.name ?? "";
    const slotCount = def.variants[variant]?.slots.length ?? 0;
    // Render each slot as 1 / 0 / ? so the user can see which bits we're
    // asking the index to fan out.
    const gravesStr = slotCount > 0
      ? Array.from({ length: slotCount }, (_, b) => {
          if ((um >> b) & 1) return "?";
          return ((graves >> b) & 1) ? "1" : "0";
        }).join("")
      : "—";
    const parts = [
      `${shapeAsArrows(def.shape)}`,
      relabelDirectionWords(def.theme),
      variantName ? relabelDirectionWords(variantName) : null,
      slotCount > 0 ? `graves=${gravesStr}` : null,
    ].filter(Boolean).join(" / ");
    return `<li>${i + 1}. ${parts}</li>`;
  }).join("");
}

document.getElementById("clear-all")!.addEventListener("click", () => {
  if (placed.size === 0) return;
  if (!confirm("Remove all placed rooms?")) return;
  placed.clear();
  rerender();
});

// State machine that decides which questions to ask and where to land:
//   "initial"            — first-time flow on load. L1 → L2 → L3 → L4 → map.
//                          L4 is REQUIRED (pre-seeds the map). No close button.
//   "extras-to-l4"       — "Edit extra filters" from the L4 sidebar. The
//                          shared L1/L2/L3 screens plus L5 + town exit. L4
//                          is NOT in this flow. After town exit: back to map.
//   "extras-to-pipeline" — Same screens as extras-to-l4 but launched from a
//                          results-style screen; after town exit we re-run
//                          the filter so the result list reflects the
//                          updated answers.
//   "pipeline"           — Mid-pipeline L5 / town-exit prompts dispatched by
//                          proceedAfterFilter. Each answer (or skip) re-runs
//                          /api/filter and lets the server decide what's
//                          next. The dialog shows a close button that ejects
//                          to the L4 map.
type Flow = "initial" | "extras-to-l4" | "extras-to-pipeline" | "pipeline";
let currentFlow: Flow = "initial";

function isExtrasFlow(f: Flow): boolean {
  return f === "extras-to-l4" || f === "extras-to-pipeline";
}

document.getElementById("edit-extra-filters")!.addEventListener("click", () => {
  currentFlow = "extras-to-l4";
  // Re-ask anything the user previously skipped — they explicitly opened
  // this dialog to revise their answers.
  skippedQuestions.clear();
  setPhase("stairs1");
});

function arrowFor(d: Dir | undefined): string {
  if (!d) return "—";
  const o = STAIRS_DIRS.find((s) => s.dir === d);
  return o ? o.arrow : "—";
}

function renderExtraFiltersSummary() {
  const el = document.getElementById("extra-filters-summary");
  if (!el) return;
  el.innerHTML = "";
  // L4 isn't an extra filter — it's locked by the L4 map placement.
  type ChipDef = { idx: string; title: string; val: string; set: boolean; phase: Phase };
  const chips: ChipDef[] = [];
  for (let i = 1; i <= 3; i++) {
    const ans = (answers as Record<string, Dir | undefined>)["stairs" + i];
    chips.push({
      idx: `L${i}`,
      title: ans ? `Tower Cellar Level ${i} stairs-up: ${ans}` : `Tower Cellar Level ${i} stairs-up: (not answered)`,
      val: arrowFor(ans),
      set: !!ans,
      phase: ("stairs" + i) as Phase,
    });
  }
  chips.push({
    idx: "L5",
    title: answers.level5
      ? `Countess (L5): ${answers.level5 === "N" ? "top right" : "top left"}`
      : "Countess (L5): (not answered)",
    val: arrowFor(answers.level5 as Dir | undefined),
    set: !!answers.level5,
    phase: "level5",
  });
  chips.push({
    idx: "Town",
    title: answers.townExit
      ? `Town → Blood Moor exit: ${answers.townExit}`
      : "Town → Blood Moor exit: (not answered)",
    val: arrowFor(answers.townExit),
    set: !!answers.townExit,
    phase: "townexit",
  });
  for (const c of chips) {
    const chip = document.createElement("button");
    chip.type = "button";
    chip.className = "seq-chip seq-chip-btn";
    chip.title = c.title + " — click to edit";
    chip.innerHTML = `<span class="idx">${c.idx}</span><span>${c.val}</span>`;
    if (!c.set) chip.style.opacity = "0.55";
    chip.addEventListener("click", () => {
      // Jump straight to the relevant question in extras-to-l4 mode. After
      // answer / skip, the walkthrough continues onward; the user can hit
      // the close × to bail out at any point.
      currentFlow = "extras-to-l4";
      skippedQuestions.delete("level5");
      skippedQuestions.delete("townExit");
      setPhase(c.phase);
    });
    el.appendChild(chip);
  }
}

// ---- Modal --------------------------------------------------------------

const modalEl = document.getElementById("modal")!;
const modalBodyEl = document.getElementById("modal-body")!;
const modalStepsEl = document.getElementById("modal-steps")!;
const modalBackBtn = document.getElementById("modal-back") as HTMLButtonElement;
const modalNextBtn = document.getElementById("modal-next") as HTMLButtonElement;
const modalTitleEl = document.getElementById("modal-title")!;

document.getElementById("modal-close")!.addEventListener("click", closeModal);
modalEl.addEventListener("click", (e) => {
  if (e.target === modalEl) closeModal();
});
document.addEventListener("keydown", (e) => {
  if (e.key === "Escape" && modalEl.classList.contains("open")) closeModal();
});

function openPicker(cx: number, cy: number) {
  activeModalCell = { cellX: cx, cellY: cy };
  const existing = placed.get(cellKey(cx, cy));
  editingExisting = existing ?? null;
  if (existing) {
    const def = findRoom(existing.roomId)!;
    pickerRoom = def;
    pickerVariant = existing.variant;
    pickerGraves = existing.graves;
    pickerGravesUnknown = existing.gravesUnknown;
    pickerStep = def.variants[pickerVariant].slots.length > 0 ? "graves" : "room";
  } else {
    pickerRoom = null;
    pickerVariant = 0;
    pickerGraves = 0;
    pickerGravesUnknown = 0;
    pickerStep = "room";
  }
  modalEl.classList.add("open");
  renderModal();
}

function closeModal() {
  // Block close when the user's grave answers would exceed the global cap.
  // They have to resolve at least one slot before the dialog will let go.
  if (canCloseModal() === false) {
    flashCapWarning();
    return;
  }
  modalEl.classList.remove("open");
  activeModalCell = null;
  editingExisting = null;
  pickerRoom = null;
  pickerGravesUnknown = 0;
}

// Allowed to close iff total unknown graves (including the picker's current
// state) is within the cap. Anywhere outside the graves step the picker
// hasn't introduced unknowns yet, so close is always fine there.
function canCloseModal(): boolean {
  if (pickerStep !== "graves") return true;
  return totalUnknownGraves(true) <= MAX_UNKNOWN_GRAVES;
}

function flashCapWarning() {
  const el = document.getElementById("graves-cap-warning");
  if (!el) return;
  el.classList.remove("flash");
  void el.offsetWidth;        // restart the CSS animation
  el.classList.add("flash");
}

modalBackBtn.addEventListener("click", () => {
  if (pickerStep === "graves") pickerStep = "room";
  renderModal();
});

modalNextBtn.addEventListener("click", () => {
  if (pickerStep === "room") {
    if (!pickerRoom) return;
    advanceFromRoom();
    return;
  }
  commitPlacement();
});

// After a room+variant is chosen (either by clicking a tile or pressing Next):
// jump to the graves step if the variant has slots, otherwise commit now.
function advanceFromRoom() {
  if (!pickerRoom) return;
  const v = pickerRoom.variants[pickerVariant];
  if (v.slots.length === 0) {
    commitPlacement();
  } else {
    pickerStep = "graves";
    renderModal();
  }
}

function commitPlacement() {
  if (!activeModalCell || !pickerRoom) { closeModal(); return; }
  if (!canCloseModal()) { flashCapWarning(); return; }
  placed.set(cellKey(activeModalCell.cellX, activeModalCell.cellY), {
    cellX: activeModalCell.cellX,
    cellY: activeModalCell.cellY,
    roomId: pickerRoom.roomId,
    variant: pickerVariant,
    graves: pickerGraves & ~pickerGravesUnknown,
    gravesUnknown: pickerGravesUnknown,
  });
  closeModal();
  rerender();
}

// ---- Filtering ---------------------------------------------------------

function isFirstPlacement(): boolean {
  return placed.size === 0 && !editingExisting;
}

function allowedShapes(): Shape[] {
  if (isFirstPlacement()) return ["W", "E", "S", "N"];
  if (!activeModalCell) return [];
  const probe = new Map(placed);
  if (editingExisting) probe.delete(cellKey(activeModalCell.cellX, activeModalCell.cellY));
  return ALL_SHAPES.filter((s) =>
    shapeValidForCell(s, activeModalCell!.cellX, activeModalCell!.cellY, probe));
}

function allowedRoomsForShape(shape: Shape): { room: RoomDef; variantIdx: number }[] {
  if (isFirstPlacement()) {
    const room = findStairsUpRoomForShape(shape);
    return room ? [{ room, variantIdx: 0 }] : [];
  }
  // Editing semantics for stairs-up:
  //   - The cell being edited is the StairsUp cell → only StairsUp rooms
  //     (any orientation that passes neighbor constraints).
  //   - Any other cell (new placement or editing a non-stairs cell) →
  //     never offer StairsUp; there's at most one stairs-up per level.
  const editingStairsUp = editingExisting !== null
    && findRoom(editingExisting.roomId)?.theme === "StairsUp";
  const out: { room: RoomDef; variantIdx: number }[] = [];
  for (const r of roomsByShape(shape)) {
    const isStairsUp = r.theme === "StairsUp";
    if (editingStairsUp !== isStairsUp) continue;
    for (let vi = 0; vi < r.variants.length; vi++) {
      out.push({ room: r, variantIdx: vi });
    }
  }
  return out;
}

function findStairsUpRoomForShape(shape: Shape): RoomDef | undefined {
  for (const r of roomsByShape(shape)) {
    if (r.theme === "StairsUp") return r;
  }
  return undefined;
}

// All (room, variant) pairs valid for the current modal cell, grouped by
// category. Empty categories are dropped from the result.
function listRoomsByCategory(): { cat: typeof CATEGORIES[number]; entries: { room: RoomDef; variantIdx: number }[] }[] {
  const byCat = new Map<string, { room: RoomDef; variantIdx: number }[]>();
  for (const c of CATEGORIES) byCat.set(c.id, []);
  for (const shape of allowedShapes()) {
    for (const e of allowedRoomsForShape(shape)) {
      const cat = categoryOf(e.room);
      byCat.get(cat)!.push(e);
    }
  }
  const out: { cat: typeof CATEGORIES[number]; entries: { room: RoomDef; variantIdx: number }[] }[] = [];
  for (const c of CATEGORIES) {
    const entries = byCat.get(c.id)!;
    if (entries.length === 0) continue;
    out.push({ cat: c, entries });
  }
  return out;
}

// ---- Modal rendering ---------------------------------------------------

function renderModal() {
  modalTitleEl.textContent = editingExisting
    ? "Edit room"
    : (isFirstPlacement() ? "Place your Stairs Up room" : "Add a room");

  // Step strip — clickable.
  const stepLabels: { key: PickerStep; label: string; enabled: () => boolean }[] = [
    {
      key: "room",
      label: "Room",
      enabled: () => true,
    },
    {
      key: "graves",
      label: "Graves",
      enabled: () => pickerRoom !== null && pickerRoom.variants[pickerVariant].slots.length > 0,
    },
  ];
  modalStepsEl.innerHTML = "";
  stepLabels.forEach((s, i) => {
    const wrap = document.createElement("span");
    const cls =
      s.key === pickerStep ? "step active"
      : (stepLabels.findIndex(x => x.key === pickerStep) > i ? "step done" : "step");
    const enabled = s.enabled();
    wrap.className = cls + (enabled ? "" : " disabled");
    wrap.innerHTML = `<span class="num">${i + 1}</span>${s.label}`;
    if (enabled) {
      wrap.addEventListener("click", () => {
        if (s.key === pickerStep) return;
        pickerStep = s.key;
        renderModal();
      });
    }
    modalStepsEl.appendChild(wrap);
    if (i < stepLabels.length - 1) {
      const arrow = document.createElement("span");
      arrow.className = "arrow";
      arrow.textContent = "→";
      modalStepsEl.appendChild(arrow);
    }
  });

  modalBodyEl.innerHTML = "";
  if (pickerStep === "room") renderRoomStep();
  else renderGravesStep();

  modalBackBtn.disabled = pickerStep === "room";
  if (pickerStep === "graves") {
    modalNextBtn.textContent = editingExisting ? "Save" : "Place room";
    modalNextBtn.disabled = false;
  } else {
    modalNextBtn.textContent = "Next";
    modalNextBtn.disabled = !pickerRoom;
  }

  ensureRemoveButton();
}

function ensureRemoveButton() {
  const footer = modalEl.querySelector(".modal footer")!;
  let btn = footer.querySelector(".remove-btn") as HTMLButtonElement | null;
  if (editingExisting) {
    if (!btn) {
      btn = document.createElement("button");
      btn.className = "btn danger remove-btn";
      btn.textContent = "Remove";
      btn.addEventListener("click", () => {
        if (!activeModalCell) return;
        placed.delete(cellKey(activeModalCell.cellX, activeModalCell.cellY));
        closeModal();
        rerender();
      });
      footer.insertBefore(btn, footer.querySelector(".steps")!);
    }
  } else if (btn) {
    btn.remove();
  }
}

function makeTileCanvas(room: RoomDef, variantIdx: number, w: number, h: number): HTMLCanvasElement {
  const dpr = window.devicePixelRatio || 1;
  const c = document.createElement("canvas");
  c.width = w * dpr;
  c.height = h * dpr;
  c.style.width = `${w}px`;
  c.style.height = `${h}px`;
  const ctx = c.getContext("2d")!;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  drawRoom(ctx, room, variantIdx, {
    cellW: w, cellH: h,
    showGraves: false, gravesMask: 0,
    baseCanvas: baseCanvasFor(room.roomId, variantIdx),
  });
  return c;
}

// ---- Step bodies -------------------------------------------------------

function renderRoomStep() {
  const intro = document.createElement("div");
  intro.className = "picker-intro";
  intro.textContent = isFirstPlacement()
    ? "Pick which direction your Stairs Up exit faces."
    : "Pick the room you walked into. Tiles are grouped by shape category.";
  modalBodyEl.appendChild(intro);

  const groups = listRoomsByCategory();
  if (groups.length === 0) {
    const empty = document.createElement("div");
    empty.className = "no-graves";
    empty.textContent = "No compatible rooms fit this cell's neighbors.";
    modalBodyEl.appendChild(empty);
    return;
  }

  for (const { cat, entries } of groups) {
    const block = document.createElement("section");
    block.className = "category-block";
    const head = document.createElement("div");
    head.className = "category-head";
    head.textContent = cat.label;
    block.appendChild(head);

    const grid = document.createElement("div");
    grid.className = "tile-grid variant-grid";
    for (const { room, variantIdx } of entries) {
      const variant = room.variants[variantIdx];
      const desc = roomDesc(room.roomId, variantIdx);
      const tile = document.createElement("div");
      const selected = pickerRoom === room && pickerVariant === variantIdx;
      tile.className = "tile variant-tile" + (selected ? " selected" : "");

      const head2 = document.createElement("div");
      head2.className = "variant-head";
      head2.appendChild(makeTileCanvas(room, variantIdx, SPRITE_PX_TILE_W, SPRITE_PX_TILE_H));
      if (desc.screenshots && desc.screenshots.length > 0) {
        for (const s of desc.screenshots) {
          head2.appendChild(makeScreenshotImg(s, "shot", s));
        }
      } else {
        const placeholder = document.createElement("div");
        placeholder.className = "shot shot-empty";
        placeholder.textContent = "(no screenshot)";
        head2.appendChild(placeholder);
      }
      tile.appendChild(head2);

      const label = document.createElement("div");
      label.className = "label";
      const shapeArrows = shapeAsArrows(room.shape);
      label.innerHTML = `<span class="shape-arrows">${shapeArrows}</span>${desc.displayName ?? relabelDirectionWords(room.theme)}`;
      tile.appendChild(label);
      const sub = document.createElement("div");
      sub.className = "sublabel";
      const parts: string[] = [];
      if (variant.name) parts.push(relabelDirectionWords(variant.name));
      if (variant.slots.length > 0) parts.push(`${variant.slots.length} grave${variant.slots.length === 1 ? "" : "s"}`);
      sub.textContent = parts.join(" · ") || "—";
      tile.appendChild(sub);
      if (desc.description) {
        const blurb = document.createElement("div");
        blurb.className = "blurb";
        blurb.textContent = desc.description;
        tile.appendChild(blurb);
      }
      tile.addEventListener("click", () => {
        pickerRoom = room;
        pickerVariant = variantIdx;
        const reuse = editingExisting
          && editingExisting.roomId === room.roomId
          && editingExisting.variant === variantIdx;
        if (reuse) {
          pickerGraves = editingExisting!.graves;
          pickerGravesUnknown = editingExisting!.gravesUnknown;
        } else {
          // Fresh room: every slot starts as unknown so the user has to
          // either confirm "closed" or mark a specific guess. The cap
          // check then forces them not to leave too many unknown.
          const n = room.variants[variantIdx].slots.length;
          pickerGraves = 0;
          pickerGravesUnknown = ((1 << n) - 1) & 0xFF;
        }
        advanceFromRoom();
      });
      grid.appendChild(tile);
    }
    block.appendChild(grid);
    modalBodyEl.appendChild(block);
  }
}

function renderGravesStep() {
  if (!pickerRoom) return;
  const variant = pickerRoom.variants[pickerVariant];
  if (variant.slots.length === 0) {
    const wrap = document.createElement("div");
    wrap.className = "no-graves";
    wrap.textContent = "This room has no grave slots. Click \"Place room\" to confirm.";
    modalBodyEl.appendChild(wrap);
    return;
  }

  const intro = document.createElement("div");
  intro.className = "picker-intro";
  intro.innerHTML = `
    Each slot is <b style="color:rgb(255,90,90)">!</b> (unknown) by default.
    <b>Left-click</b> a slot to mark it closed; click again to mark it open.
    <b>Right-click</b> a slot to set it back to unknown.
    <span style="color:var(--text-dim)">
      (${variant.slots.length} possible slot${variant.slots.length === 1 ? "" : "s"})
    </span>
  `;
  modalBodyEl.appendChild(intro);

  // Cap warning bar — flashes red when the user hits an action that would
  // close the dialog with > MAX_UNKNOWN_GRAVES total.
  const usedNow = totalUnknownGraves(true);
  const warn = document.createElement("div");
  warn.id = "graves-cap-warning";
  warn.className = "graves-cap-warning" + (usedNow > MAX_UNKNOWN_GRAVES ? " over" : "");
  warn.innerHTML = `Unknown graves: <b>${usedNow}</b> / ${MAX_UNKNOWN_GRAVES} allowed across all rooms.`
    + (usedNow > MAX_UNKNOWN_GRAVES
        ? ` Resolve <b>${usedNow - MAX_UNKNOWN_GRAVES}</b> more to continue.`
        : "");
  modalBodyEl.appendChild(warn);

  const stage = document.createElement("div");
  stage.className = "graves-stage";

  const wrap = document.createElement("div");
  wrap.className = "room-wrap";
  wrap.style.width  = `${SPRITE_PX_BIG_W}px`;
  wrap.style.height = `${SPRITE_PX_BIG_H}px`;

  const canvas = document.createElement("canvas");
  const dpr = window.devicePixelRatio || 1;
  canvas.width = SPRITE_PX_BIG_W * dpr;
  canvas.height = SPRITE_PX_BIG_H * dpr;
  canvas.style.width = `${SPRITE_PX_BIG_W}px`;
  canvas.style.height = `${SPRITE_PX_BIG_H}px`;
  canvas.style.position = "absolute";
  canvas.style.left = "0";
  canvas.style.top = "0";
  const ctx = canvas.getContext("2d")!;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  drawRoom(ctx, pickerRoom, pickerVariant, {
    cellW: SPRITE_PX_BIG_W, cellH: SPRITE_PX_BIG_H,
    showGraves: true,
    gravesMask: pickerGraves,
    gravesUnknownMask: pickerGravesUnknown,
    baseCanvas: baseCanvasFor(pickerRoom.roomId, pickerVariant),
  });
  wrap.appendChild(canvas);

  for (let i = 0; i < variant.slots.length; i++) {
    const poly = slotIsoPoly(variant, i, SPRITE_PX_BIG_W, SPRITE_PX_BIG_H)!;
    const bb = polyBBox(poly);
    const localPoly: [number, number][] = poly.map(([x, y]) => [x - bb.x, y - bb.y]);
    const hit = document.createElement("div");
    const unknown = ((pickerGravesUnknown >> i) & 1) !== 0;
    const on = !unknown && ((pickerGraves >> i) & 1) !== 0;
    const cls =
      unknown ? "slot-hit unknown"
      : on    ? "slot-hit on"
      :         "slot-hit closed";
    hit.className = cls;
    hit.title = (unknown
        ? `Grave #${i + 1} — unknown (left-click: mark closed)`
        : on
          ? `Grave #${i + 1} — open (left-click: mark closed)`
          : `Grave #${i + 1} — closed (left-click: mark open)`)
      + `\nRight-click: reset to unknown`;
    hit.style.left   = `${bb.x}px`;
    hit.style.top    = `${bb.y}px`;
    hit.style.width  = `${bb.w}px`;
    hit.style.height = `${bb.h}px`;
    hit.style.clipPath = polyToClipPath(localPoly);
    hit.addEventListener("click", () => {
      // Tri-state cycle: unknown → closed → open (and then toggle between
      // closed/open on further left-clicks). Right-click is the only way
      // back to unknown.
      const u = ((pickerGravesUnknown >> i) & 1) !== 0;
      if (u) {
        pickerGravesUnknown &= ~(1 << i);
        pickerGraves       &= ~(1 << i);      // unknown → closed
      } else {
        pickerGraves ^= (1 << i);              // toggle closed ↔ open
      }
      renderModal();
    });
    hit.addEventListener("contextmenu", (e) => {
      e.preventDefault();
      pickerGravesUnknown |= (1 << i);
      pickerGraves        &= ~(1 << i);
      renderModal();
    });
    wrap.appendChild(hit);
  }
  stage.appendChild(wrap);

  const gdesc = gravesDescFor(pickerRoom.roomId, pickerVariant);
  if (gdesc.screenshot) {
    stage.appendChild(makeScreenshotImg(gdesc.screenshot, "shot shot-large", "in-game grave reference"));
  }

  modalBodyEl.appendChild(stage);
}

// ----- Boot ---------------------------------------------------------------

function rerender() {
  renderGrid();
  renderSidebar();
}

(async () => {
  const [a, d] = await Promise.all([loadAtlas(), loadDescriptions()]);
  descriptions = d;
  await Promise.all(a.rooms.map(async (r) => {
    const c = await getRoomCanvas(r, a.tileSize);
    baseCanvases.set(atlasKey(r.roomId, r.variant), c);
  }));
  document.getElementById("loading")?.remove();
  rerender();
  setPhase("stairs1");
})().catch((e) => {
  const overlay = document.getElementById("loading");
  if (overlay) overlay.textContent = "failed to load atlas: " + (e as Error).message;
  console.error(e);
});

// =========================================================================
// Phase dispatch + question / results screens
// =========================================================================

const overlayEl = document.getElementById("phase-overlay")! as HTMLDivElement;

function setPhase(p: Phase) {
  if (phase === p && p === "filtering") {
    // Allow re-entry for filtering so progress updates redraw the screen.
    document.body.dataset.phase = p;
    overlayEl.hidden = false;
    overlayEl.innerHTML = "";
    renderFilteringView(overlayEl);
    return;
  }
  phase = p;
  document.body.dataset.phase = p;
  if (p === "level4") {
    overlayEl.hidden = true;
    overlayEl.innerHTML = "";
    // Seed the L4 grid's stairs-up cell from the user's stairs4 answer so
    // they don't have to re-place it manually. Re-running this on every L4
    // entry keeps the center cell in sync if the user edited stairs4.
    ensureL4StairsUp();
    rerender();
    return;
  }
  overlayEl.hidden = false;
  overlayEl.innerHTML = "";
  if (p === "stairs1" || p === "stairs2" || p === "stairs3" || p === "stairs4") renderStairsView(overlayEl, p);
  else if (p === "filtering") renderFilteringView(overlayEl);
  else if (p === "level5")    renderLevel5View(overlayEl);
  else if (p === "townexit")  renderTownExitView(overlayEl);
  else if (p === "results")   renderResultsView(overlayEl);
}

// Place the StairsUp room corresponding to the stairs4 answer at GRID_CENTER.
// If the center cell already has a StairsUp room (from a previous prefill)
// we just update its direction; if the user placed a non-stairs room there
// during free editing we leave it alone.
function ensureL4StairsUp() {
  if (!answers.stairs4) return;
  const room = findStairsUpRoomForShape(answers.stairs4);
  if (!room) return;
  const key = cellKey(GRID_CENTER, GRID_CENTER);
  const existing = placed.get(key);
  if (existing) {
    const def = findRoom(existing.roomId);
    if (!def || def.theme !== "StairsUp") return;  // user replaced; don't clobber
  }
  placed.set(key, {
    cellX: GRID_CENTER,
    cellY: GRID_CENTER,
    roomId: room.roomId,
    variant: 0,
    graves: 0,
    gravesUnknown: 0,
  });
}

function dirOptions(opts: typeof STAIRS_DIRS, onPick: (d: Dir) => void): HTMLElement {
  const grid = document.createElement("div");
  grid.className = "dir-buttons";
  for (const o of opts) {
    const btn = document.createElement("button");
    btn.className = "dir-btn";
    btn.innerHTML = `<span class="dir-arrow">${o.arrow}</span><span class="dir-label">${o.label}</span>`;
    btn.addEventListener("click", () => onPick(o.dir));
    grid.appendChild(btn);
  }
  return grid;
}

// Stairs-room picker — the same iso-tile + screenshot layout the L4 picker
// uses, scaled across all four stairs-up orientations. Reusing the L4 tile
// component keeps the player on familiar visuals between the pre-flight
// stairs questions and the L4 build. The currently-selected direction (if
// any) gets the `.selected` highlight so users editing prior answers can
// see what they chose.
function stairsRoomOptions(
  currentAnswer: Dir | undefined,
  onPick: (d: Dir) => void,
  opts: { readOnly?: boolean } = {},
): HTMLElement {
  const grid = document.createElement("div");
  grid.className = "tile-grid variant-grid stairs-grid"
    + (opts.readOnly ? " readonly" : "");
  for (const opt of STAIRS_DIRS) {
    const room = findStairsUpRoomForShape(opt.dir);
    if (!room) continue;
    const desc = roomDesc(room.roomId, 0);
    const tile = document.createElement("div");
    const selected = currentAnswer === opt.dir;
    const classes = ["tile", "variant-tile"];
    if (selected) classes.push("selected");
    if (opts.readOnly && !selected) classes.push("disabled");
    tile.className = classes.join(" ");

    const head = document.createElement("div");
    head.className = "variant-head";
    head.appendChild(makeTileCanvas(room, 0, SPRITE_PX_TILE_W, SPRITE_PX_TILE_H));
    if (desc.screenshots && desc.screenshots.length > 0) {
      for (const s of desc.screenshots) {
        head.appendChild(makeScreenshotImg(s, "shot", s));
      }
    } else {
      const placeholder = document.createElement("div");
      placeholder.className = "shot shot-empty";
      placeholder.textContent = "(no screenshot)";
      head.appendChild(placeholder);
    }
    tile.appendChild(head);

    const label = document.createElement("div");
    label.className = "label";
    label.innerHTML =
      `<span class="dir-arrow">${opt.arrow}</span> ${opt.label}`;
    tile.appendChild(label);

    const sub = document.createElement("div");
    sub.className = "sublabel";
    sub.textContent = desc.displayName ?? `Stairs Up — exits ${opt.label}`;
    tile.appendChild(sub);

    if (!opts.readOnly) tile.addEventListener("click", () => onPick(opt.dir));
    grid.appendChild(tile);
  }
  return grid;
}

function renderStairsView(host: HTMLElement, p: StairsPhase) {
  const n = p === "stairs1" ? 1 : p === "stairs2" ? 2 : p === "stairs3" ? 3 : 4;
  const required = p === "stairs4";        // L4 stairs is required — pre-seeds the L4 grid
  const current = (answers as Record<string, Dir | undefined>)[p];
  // stairs4 is locked once it's been chosen because its value pre-seeds the
  // L4 grid's stairs-up cell. Changing it would invalidate every connected
  // placement the user has made.
  const lockedStairs4 = p === "stairs4" && current !== undefined;
  const promptExtra = required
    ? (lockedStairs4
        ? `<br><span class="question-sub" style="color:var(--warn)">locked</span>`
        : `<br><span class="question-sub" style="color:var(--warn)">required</span>`)
    : `<br><span class="question-sub">(the room you came down through from the level above)</span>`;
  const continueBtn = lockedStairs4
    ? '<button id="next-btn" class="btn primary">Continue →</button>'
    : "";
  const dontKnowBtn = (required || lockedStairs4)
    ? ""
    : '<button id="dont-know-btn" class="btn">I don\'t know</button>';
  // Step counter only on the initial walk-through. Extras / pipeline screens
  // are self-contained edits — no counter needed.
  const stepHeader = currentFlow === "initial"
    ? `<div class="question-step">Step ${n} of 4</div>`
    : "";
  host.innerHTML = `
    <div class="question-card">
      ${stepHeader}
      ${closeButtonHtml()}
      <h2>Tower Cellar Level ${n}</h2>
      <p class="question-prompt">
        Which direction does the <b>stairs-up room</b> on Tower Cellar Level ${n} face?
        ${promptExtra}
      </p>
      <div class="nav-flank">
        ${navArrowHtml(p).back}
        <div id="opts-host"></div>
        ${navArrowHtml(p).forward}
      </div>
      <div class="question-actions">
        ${dontKnowBtn}
        ${continueBtn}
      </div>
    </div>
  `;
  host.querySelector("#opts-host")!.appendChild(
    stairsRoomOptions(current, (dir) => {
      (answers as Record<string, Dir>)[p] = dir;
      advanceFromStairs(p);
    }, { readOnly: lockedStairs4 }),
  );
  const next = host.querySelector("#next-btn");
  if (next) next.addEventListener("click", () => advanceFromStairs(p));
  const dontKnow = host.querySelector("#dont-know-btn");
  if (dontKnow) {
    dontKnow.addEventListener("click", () => {
      delete (answers as Record<string, Dir | undefined>)[p];
      advanceFromStairs(p);
    });
  }
  wireNavArrows(host, p, () => advanceFromStairs(p));
  wireCloseButton(host);
}

// HTML snippet for the dialog's close (×) button. Returned for every screen
// that's part of the extras dialog or a mid-pipeline question; suppressed
// for the initial first-time-through walk-through (which the user has to
// complete to reach the map).
function closeButtonHtml(): string {
  if (currentFlow === "initial") return "";
  return `<button id="extras-close-btn" class="extras-close" type="button" aria-label="Close">×</button>`;
}

// Wire whatever close button renderXxxView painted into the host. Closing
// the extras / pipeline dialog ejects to the L4 map; the user keeps their
// answers and can resume Find seeds or open another extra filter later.
function wireCloseButton(host: HTMLElement) {
  const btn = host.querySelector("#extras-close-btn");
  if (!btn) return;
  btn.addEventListener("click", () => {
    currentFlow = "initial";
    setPhase("level4");
  });
}

// Linear sequence of question phases for the current flow. Drives the
// Back / Forward arrow buttons that flank every selection grid (both in
// the initial dialog and in the extras dialog). Pipeline mode is
// server-driven and gets no flanking arrows.
function sequenceForFlow(): Phase[] {
  if (currentFlow === "initial") return ["stairs1", "stairs2", "stairs3", "stairs4"];
  if (isExtrasFlow(currentFlow)) return ["stairs1", "stairs2", "stairs3", "level5", "townexit"];
  return [];
}
function prevPhase(p: Phase): Phase | null {
  const seq = sequenceForFlow();
  const i = seq.indexOf(p);
  return i > 0 ? seq[i - 1] : null;
}

// Big circular arrow buttons flanking the selection tiles. Back goes to
// the previous question in the current flow; Forward calls the screen's
// existing advance handler (so it knows about flow-specific transitions
// like extras-to-pipeline ending). Neither touches the current answer.
function navArrowHtml(p: Phase): { back: string; forward: string } {
  if (currentFlow === "pipeline") return { back: "", forward: "" };
  const back = prevPhase(p) !== null;
  // Forward is blocked on the very last step of the initial flow until
  // the L4 stairs-up has been picked — it pre-seeds the L4 grid and
  // skipping it would leave the map empty.
  let forward = true;
  if (currentFlow === "initial" && p === "stairs4") {
    forward = answers.stairs4 !== undefined;
  }
  return {
    back:    `<button class="nav-arrow" id="nav-back" ${back ? "" : "disabled"} aria-label="Back">‹</button>`,
    forward: `<button class="nav-arrow" id="nav-forward" ${forward ? "" : "disabled"} aria-label="Forward">›</button>`,
  };
}
function wireNavArrows(host: HTMLElement, p: Phase, advance: () => void) {
  if (currentFlow === "pipeline") return;
  const back = host.querySelector("#nav-back");
  if (back) back.addEventListener("click", () => {
    const prev = prevPhase(p);
    if (prev) setPhase(prev);
  });
  const fwd = host.querySelector("#nav-forward");
  if (fwd) fwd.addEventListener("click", advance);
}

function advanceFromStairs(p: StairsPhase) {
  if (p === "stairs1") { setPhase("stairs2"); return; }
  if (p === "stairs2") { setPhase("stairs3"); return; }
  if (p === "stairs3") {
    // Extras flow skips stairs4 (L4 stairs-up is locked from the initial
    // dialog and not part of the extras filter).
    if (isExtrasFlow(currentFlow)) setPhase("level5");
    else                           setPhase("stairs4");
    return;
  }
  // p === "stairs4" — only reachable in the initial flow.
  setPhase("level4");
}

// L4 "Find seeds" button — fires the filter pipeline. Find seeds is the
// other (besides Edit extra filters) entry point for L5 / townexit; the
// pipeline asks them via proceedAfterFilter when the candidate set is
// still > 1.
//
// In the "100 < count ≤ 200" band the server requires parity to fit the
// load cap, so we divert through the L5 question first (and mark it
// required, so the user can't "I don't know" past it). Once L5 is set,
// the pipeline runs normally — the server's parity-aware effective count
// is ≤ ~100 by then.
findSeedsBtn.addEventListener("click", () => {
  const g = findSeedsGate();
  if (g.state === "too-many" || g.state === "pending") return;
  currentFlow = "pipeline";
  closeSidebar();
  if (g.state === "needs-parity") {
    // L5 is mandatory at this size — skippedQuestions must NOT cover it,
    // and renderLevel5View consults `level5IsRequired()` to hide the
    // "I don't know" button.
    skippedQuestions.delete("level5");
    setPhase("level5");
    return;
  }
  startPipeline();
});

// True when the user reached L5 *via the pipeline* (i.e. clicked Find
// seeds) in the 100 < count ≤ 200 band with no parity answer. At that size
// the server refuses to render without parity, so the L5 question is
// mandatory — the "I don't know" button is suppressed. In the extras
// flow the same screen is informational, so the skip button stays.
function level5IsRequired(): boolean {
  return currentFlow === "pipeline"
      && lastLookupCount > FILTER_RENDER_CAP_NO_PARITY
      && lastLookupCount <= FILTER_RENDER_CAP_WITH_PARITY
      && answers.level5 === undefined;
}

// One canonical pipeline call: bundles every filter the user has answered so
// far and asks the server for survivors. Re-invoked after each new question
// (L5, townexit) so the server picks up the latest answer and re-applies the
// whole chain.
async function runPipeline(stageLabel: string) {
  const rooms = [...placed.values()];
  const seqResult = buildSequence(rooms);
  if (seqResult.seq.length < LOOKUP_TRIGGER_THRESHOLD) {
    alert(`Place at least ${LOOKUP_TRIGGER_THRESHOLD} BFS-reachable rooms before searching.`);
    return;
  }
  filteringStatus = stageLabel;
  filteringProgress = null;
  setPhase("filtering");
  let resp: FilterResponse;
  try {
    resp = await runFilter(rooms, answers, 64);
  } catch (e) {
    filteringStatus = "Filter failed: " + (e as Error).message;
    updateFilteringProgress();
    return;
  }
  pipelineSeeds = resp.seeds;
  lastFilterResponse = resp;
  proceedAfterFilter(resp);
}

let lastFilterResponse: FilterResponse | null = null;

// Real candidate count after the most recent filter pass. `pipelineSeeds`
// only holds the head of the list (capped by the server's `limit`), so
// using its length for "X candidates remaining" understates the true count
// whenever the response is truncated.
//
// We can't trust the server's `final_count`: when the trielookup overflows
// its 4096-seed cap and no tell filter narrows it back, FinalCount = len(
// truncated seeds) = 4096 instead of the real lookup total. Walk the
// stage chain ourselves so the most-narrowed accurate count wins.
function pipelineCount(): number {
  const r = lastFilterResponse;
  if (!r) return pipelineSeeds.length;
  return r.after_filters ?? r.after_parity ?? r.lookup;
}

function startPipeline() { runPipeline("Querying the index…"); }

// Questions the user has explicitly skipped. Used so proceedAfterFilter
// doesn't loop back to a question they declined to answer.
const skippedQuestions = new Set<"level5" | "townExit">();

function proceedAfterFilter(resp: FilterResponse) {
  // If lookup was already empty / unique, jump to results.
  if (resp.final_count <= 1) {
    setPhase("results");
    return;
  }
  // If the user hasn't answered L5 yet (and hasn't skipped it), ask. Parity
  // halves cheaply, so it's worth asking before stairs do per-seed renders
  // next time.
  if (!answers.level5 && !skippedQuestions.has("level5")) {
    setPhase("level5");
    return;
  }
  // L5 already applied (or skipped) and the seeds count is still > 1 → ask
  // town exit.
  if (!answers.townExit && !skippedQuestions.has("townExit")) {
    setPhase("townexit");
    return;
  }
  // All filters answered, show whatever survived.
  setPhase("results");
}

function renderFilteringView(host: HTMLElement) {
  host.innerHTML = `
    <div class="question-card">
      <h2>Filtering…</h2>
      <p class="question-prompt" id="filter-status">${filteringStatus}</p>
      <div class="filter-progress" id="filter-progress"></div>
    </div>
  `;
  updateFilteringProgress();
}

function updateFilteringProgress() {
  const statusEl = document.getElementById("filter-status");
  if (statusEl) statusEl.textContent = filteringStatus;
  const progEl = document.getElementById("filter-progress");
  if (!progEl) return;
  if (!filteringProgress) { progEl.textContent = ""; return; }
  const { done, total } = filteringProgress;
  const pct = total > 0 ? Math.round((done / total) * 100) : 0;
  progEl.innerHTML = `
    <div class="filter-bar"><div class="filter-bar-fill" style="width:${pct}%"></div></div>
    <div class="filter-count">${done} / ${total} (${pct}%)</div>
  `;
}

function renderLevel5View(host: HTMLElement) {
  const subtitle = currentFlow === "pipeline"
    ? `<div class="question-step">${fmt(pipelineCount())} candidates remaining</div>`
    : "";
  // In the 100–200 band the server refuses to render without parity, so
  // the L5 question is mandatory: no "I don't know" button, and the
  // prompt explains why. Outside the band the button is always shown.
  const required = level5IsRequired();
  const requiredNote = required
    ? `<br><span class="question-sub" style="color:var(--warn)">required at this candidate count — answer to enable seed search</span>`
    : "";
  const dontKnowBtn = required
    ? ""
    : `<button id="dont-know-btn" class="btn">I don't know</button>`;
  host.innerHTML = `
    <div class="question-card">
      ${subtitle}
      ${closeButtonHtml()}
      <h2>Tower Cellar Level 5 (Countess)</h2>
      <p class="question-prompt">
        On level 5, in which direction is <b>the Countess</b>?
        <br><span class="question-sub">click the screenshot that matches your map</span>
        ${requiredNote}
      </p>
      <div class="nav-flank">
        ${navArrowHtml("level5").back}
        <div id="opts-host"></div>
        ${navArrowHtml("level5").forward}
      </div>
      <div class="question-actions">
        ${dontKnowBtn}
      </div>
    </div>
  `;
  host.querySelector("#opts-host")!.appendChild(
    level5Options((dir) => {
      answers.level5 = dir;
      skippedQuestions.delete("level5");
      advanceFromLevel5();
    }),
  );
  // The "I don't know" button is suppressed when L5 is required (see
  // level5IsRequired) — the querySelector returns null in that case.
  host.querySelector("#dont-know-btn")?.addEventListener("click", () => {
    answers.level5 = undefined;
    skippedQuestions.add("level5");
    advanceFromLevel5();
  });
  wireNavArrows(host, "level5", advanceFromLevel5);
  wireCloseButton(host);
}

function advanceFromLevel5() {
  // L5 toggles affect the Find seeds gate (parity unblocks the 100–200
  // band) — refresh the button state right away rather than waiting on
  // the next renderSidebar.
  updateFindSeedsButton();
  // Extras walk on to town exit; pipeline mode re-runs the filter so the
  // server decides what's next.
  if (isExtrasFlow(currentFlow)) setPhase("townexit");
  else runPipeline("Applying parity and stairs L1/L2/L3 filters…");
}

// Level-5 picker: two big screenshot tiles. The contract pinned in the
// server is "north == seed is even, west == seed is odd"; we file the
// corresponding screenshots under those names so the asset path documents
// the parity binding.
function level5Options(onPick: (d: "N" | "W") => void): HTMLElement {
  const grid = document.createElement("div");
  grid.className = "tile-grid variant-grid stairs-grid";
  const opts: Array<{ dir: "N" | "W"; arrow: string; label: string; file: string }> = [
    { dir: "W", arrow: "↖", label: "top left",  file: "level-5-odd.jpg" },
    { dir: "N", arrow: "↗", label: "top right", file: "level-5-even.jpg" },
  ];
  for (const opt of opts) {
    const tile = document.createElement("div");
    tile.className = "tile variant-tile";
    const head = document.createElement("div");
    head.className = "variant-head";
    head.appendChild(makeScreenshotImg(opt.file, "shot shot-large", `Countess ${opt.label}`));
    tile.appendChild(head);
    const label = document.createElement("div");
    label.className = "label";
    label.innerHTML = `<span class="dir-arrow">${opt.arrow}</span> Countess to the ${opt.label}`;
    tile.appendChild(label);
    tile.addEventListener("click", () => onPick(opt.dir));
    grid.appendChild(tile);
  }
  return grid;
}

function renderTownExitView(host: HTMLElement) {
  const subtitle = currentFlow === "pipeline"
    ? `<div class="question-step">${fmt(pipelineCount())} candidates remaining</div>`
    : "";
  host.innerHTML = `
    <div class="question-card">
      ${subtitle}
      ${closeButtonHtml()}
      <h2>Town to Blood Moor exit</h2>
      <p class="question-prompt">
        Standing in Rogue Encampment, in which direction did you walk out the gate
        into Blood Moor?
      </p>
      <div class="nav-flank">
        ${navArrowHtml("townexit").back}
        <div id="opts-host"></div>
        ${navArrowHtml("townexit").forward}
      </div>
      <div class="question-actions">
        <button id="dont-know-btn" class="btn">I don't know</button>
      </div>
    </div>
  `;
  host.querySelector("#opts-host")!.appendChild(
    dirOptions(STAIRS_DIRS, (dir) => {
      answers.townExit = dir;
      skippedQuestions.delete("townExit");
      advanceFromTownExit();
    }),
  );
  host.querySelector("#dont-know-btn")!.addEventListener("click", () => {
    answers.townExit = undefined;
    skippedQuestions.add("townExit");
    advanceFromTownExit();
  });
  wireNavArrows(host, "townexit", advanceFromTownExit);
  wireCloseButton(host);
}

function advanceFromTownExit() {
  switch (currentFlow) {
    case "extras-to-l4":
      currentFlow = "initial";
      setPhase("level4");
      return;
    case "extras-to-pipeline":
      currentFlow = "pipeline";
      runPipeline("Applying town-exit filter…");
      return;
    case "pipeline":
      runPipeline("Applying town-exit filter…");
      return;
    case "initial":
      // Shouldn't happen — town exit isn't asked in the initial flow.
      setPhase("level4");
      return;
  }
}

function renderResultsView(host: HTMLElement) {
  const n = pipelineCount();
  const shown = pipelineSeeds.length;
  let header: string;
  if (n === 0)      header = "No matching seeds. Double-check your answers.";
  else if (n === 1) header = "Found your seed!";
  else              header = `${fmt(n)} candidates remain`;
  const list = pipelineSeeds.map((s) => {
    // Default-load Tower Cellar Level 1 (level id 21) — that's where the
    // player will start verifying the seed against their map.
    const url = `${MAP_VIEWER_URL}?seed=${s}&level=21`;
    return `<li><a href="${url}" target="_blank" rel="noopener">${s}</a></li>`;
  }).join("");
  // When the server truncated the list, show how many we're actually
  // displaying so "1024 candidates remain" + a 64-link list isn't
  // contradictory.
  const truncatedNote = n > shown
    ? `<div class="results-warning">Showing first ${fmt(shown)} of ${fmt(n)} — narrow further (add more L4 rooms or answer skipped tells) to drop the count.</div>`
    : "";
  const warning = lastFilterResponse?.warning
    ? `<div class="results-warning">${lastFilterResponse.warning}</div>`
    : "";
  host.innerHTML = `
    <div class="question-card results-card">
      <h2>${header}</h2>
      ${warning}
      ${truncatedNote}
      <p class="question-prompt">
        Click a seed to open it in the map viewer (a new tab).
      </p>
      <ul class="seed-link-list">${list || "<li>(none)</li>"}</ul>
      <div class="question-actions">
        <button id="edit-prefilter-btn" class="btn">Edit extra filters</button>
      </div>
      <button id="back-l4-btn" class="btn back-corner">← Back to map</button>
    </div>
  `;
  host.querySelector("#back-l4-btn")!.addEventListener("click", () => setPhase("level4"));
  host.querySelector("#edit-prefilter-btn")!.addEventListener("click", () => {
    // The "extras-to-pipeline" flow re-runs /api/filter once the user
    // finishes town exit, so the results list reflects the new answers.
    // Clearing skippedQuestions ensures L5 / town exit are surfaced again
    // even if the user had skipped them earlier.
    skippedQuestions.clear();
    currentFlow = "extras-to-pipeline";
    setPhase("stairs1");
  });
}
