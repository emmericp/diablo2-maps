// World-coord camera with pan + zoom. World units = "device pixels at zoom 1".
// Screen origin is the canvas center, so zoom-around-cursor stays stable.

export interface Bounds {
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
}

export class Viewport {
  cx = 0; // world coord at screen center
  cy = 0;
  scale = 1;
  minScale = 0.05;
  maxScale = 32;
  // Drawing buffer dimensions (already DPR-scaled).
  width = 0;
  height = 0;
  onChange: (() => void) | null = null;

  resize(w: number, h: number) {
    this.width = w;
    this.height = h;
  }

  worldToScreen(wx: number, wy: number): [number, number] {
    return [
      (wx - this.cx) * this.scale + this.width / 2,
      (wy - this.cy) * this.scale + this.height / 2,
    ];
  }

  screenToWorld(sx: number, sy: number): [number, number] {
    return [
      (sx - this.width / 2) / this.scale + this.cx,
      (sy - this.height / 2) / this.scale + this.cy,
    ];
  }

  panBy(dxBuf: number, dyBuf: number) {
    this.cx -= dxBuf / this.scale;
    this.cy -= dyBuf / this.scale;
    this.onChange?.();
  }

  zoomAt(sx: number, sy: number, factor: number) {
    const next = clamp(this.scale * factor, this.minScale, this.maxScale);
    if (next === this.scale) return;
    const [wxBefore, wyBefore] = this.screenToWorld(sx, sy);
    this.scale = next;
    const [wxAfter, wyAfter] = this.screenToWorld(sx, sy);
    this.cx += wxBefore - wxAfter;
    this.cy += wyBefore - wyAfter;
    this.onChange?.();
  }

  // Auto-fit caps zoom-in at this scale so very small levels (e.g. the
  // ForgottenTower entrance) don't get blown up to fill the canvas. Mouse
  // wheel can still zoom past it manually.
  maxFitScale = 4;

  fitTo(b: Bounds, marginPx = 32) {
    const w = Math.max(1, b.maxX - b.minX);
    const h = Math.max(1, b.maxY - b.minY);
    const sw = Math.max(1, this.width - marginPx * 2);
    const sh = Math.max(1, this.height - marginPx * 2);
    const fit = Math.min(sw / w, sh / h);
    const cap = Math.min(this.maxScale, this.maxFitScale);
    this.scale = clamp(fit, this.minScale, cap);
    this.cx = (b.minX + b.maxX) / 2;
    this.cy = (b.minY + b.maxY) / 2;
    this.onChange?.();
  }
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

export function attachInputs(
  canvas: HTMLCanvasElement,
  vp: Viewport,
  onPointerMove?: (bx: number, by: number) => void,
  onClick?: (bx: number, by: number) => void,
): () => void {
  // Single-finger drag state. `panPointerId` is the pointer currently driving
  // the pan; it's null while pinching (two fingers) or when nothing is down.
  let panPointerId: number | null = null;
  let lastClientX = 0;
  let lastClientY = 0;
  let downClientX = 0;
  let downClientY = 0;
  let downTime = 0;

  // Multi-touch pinch state. We don't lock into "pinch mode" — we just look at
  // the current set of active pointers each move and act accordingly.
  const active = new Map<number, { x: number; y: number }>();
  let pinchPrevDist: number | null = null;
  let pinchPrevMidX = 0;
  let pinchPrevMidY = 0;
  // True once a 2nd finger has touched down during the current gesture. Used
  // to suppress the click that would otherwise fire when the last finger lifts
  // after a pinch.
  let multiTouchUsed = false;

  const clientToBuffer = (cx: number, cy: number): [number, number] => {
    const rect = canvas.getBoundingClientRect();
    const xScale = canvas.width / rect.width;
    const yScale = canvas.height / rect.height;
    return [(cx - rect.left) * xScale, (cy - rect.top) * yScale];
  };

  const onDown = (e: PointerEvent) => {
    // Touch events report button=0; mouse left=0, middle=1. Ignore right-click etc.
    if (e.button !== 0 && e.button !== 1) return;
    canvas.setPointerCapture(e.pointerId);
    active.set(e.pointerId, { x: e.clientX, y: e.clientY });

    if (active.size === 1) {
      panPointerId = e.pointerId;
      lastClientX = e.clientX;
      lastClientY = e.clientY;
      downClientX = e.clientX;
      downClientY = e.clientY;
      downTime = performance.now();
      canvas.classList.add("dragging");
    } else if (active.size === 2) {
      // Promote single-finger drag into pinch. Stop dragging on the first
      // pointer to avoid mixing pan deltas with the pinch midpoint pan.
      multiTouchUsed = true;
      panPointerId = null;
      canvas.classList.remove("dragging");
      const pts = [...active.values()];
      const a = pts[0]!;
      const b = pts[1]!;
      pinchPrevDist = Math.hypot(a.x - b.x, a.y - b.y);
      pinchPrevMidX = (a.x + b.x) / 2;
      pinchPrevMidY = (a.y + b.y) / 2;
    }
  };

  const onMove = (e: PointerEvent) => {
    if (active.has(e.pointerId)) {
      active.set(e.pointerId, { x: e.clientX, y: e.clientY });
    }

    if (active.size >= 2 && pinchPrevDist !== null) {
      // Two-finger gesture: pan by midpoint delta, zoom by distance ratio.
      const pts = [...active.values()];
      const a = pts[0]!;
      const b = pts[1]!;
      const dist = Math.hypot(a.x - b.x, a.y - b.y);
      const midX = (a.x + b.x) / 2;
      const midY = (a.y + b.y) / 2;
      const rect = canvas.getBoundingClientRect();
      const xScale = canvas.width / rect.width;
      const yScale = canvas.height / rect.height;
      vp.panBy((midX - pinchPrevMidX) * xScale, (midY - pinchPrevMidY) * yScale);
      if (dist > 0 && pinchPrevDist > 0) {
        const [bx, by] = clientToBuffer(midX, midY);
        vp.zoomAt(bx, by, dist / pinchPrevDist);
      }
      pinchPrevDist = dist;
      pinchPrevMidX = midX;
      pinchPrevMidY = midY;
    } else if (panPointerId === e.pointerId) {
      const rect = canvas.getBoundingClientRect();
      const xScale = canvas.width / rect.width;
      const yScale = canvas.height / rect.height;
      vp.panBy((e.clientX - lastClientX) * xScale, (e.clientY - lastClientY) * yScale);
      lastClientX = e.clientX;
      lastClientY = e.clientY;
    }

    if (onPointerMove) {
      const [bx, by] = clientToBuffer(e.clientX, e.clientY);
      onPointerMove(bx, by);
    }
  };

  const onUp = (e: PointerEvent) => {
    if (!active.has(e.pointerId)) return;
    active.delete(e.pointerId);
    canvas.releasePointerCapture(e.pointerId);

    if (active.size < 2) {
      pinchPrevDist = null;
    }

    const wasPanPointer = panPointerId === e.pointerId;
    if (wasPanPointer) panPointerId = null;

    if (active.size === 0) {
      canvas.classList.remove("dragging");
      // Click detection: only the lone-finger-no-pinch case is a tap.
      if (wasPanPointer && !multiTouchUsed && onClick) {
        const moved = Math.hypot(e.clientX - downClientX, e.clientY - downClientY);
        const elapsed = performance.now() - downTime;
        if (moved < 4 && elapsed < 350) {
          const [bx, by] = clientToBuffer(e.clientX, e.clientY);
          onClick(bx, by);
        }
      }
      multiTouchUsed = false;
    }
    // If a finger comes up while another is still down, we don't promote it
    // back to drag — that path can produce spurious clicks and the user can
    // just lift fully and re-touch if they want to keep panning.
  };

  const onWheel = (e: WheelEvent) => {
    e.preventDefault();
    const [bx, by] = clientToBuffer(e.clientX, e.clientY);
    // deltaMode 1 = lines (Firefox); else pixels.
    const raw = -e.deltaY * (e.deltaMode === 1 ? 18 : 1);
    const factor = Math.exp(raw * 0.0015);
    vp.zoomAt(bx, by, factor);
  };

  canvas.addEventListener("pointerdown", onDown);
  canvas.addEventListener("pointermove", onMove);
  canvas.addEventListener("pointerup", onUp);
  canvas.addEventListener("pointercancel", onUp);
  canvas.addEventListener("wheel", onWheel, { passive: false });

  return () => {
    canvas.removeEventListener("pointerdown", onDown);
    canvas.removeEventListener("pointermove", onMove);
    canvas.removeEventListener("pointerup", onUp);
    canvas.removeEventListener("pointercancel", onUp);
    canvas.removeEventListener("wheel", onWheel);
  };
}
