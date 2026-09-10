/**
 * C2 -- the viewport. Scrubbing the bake before it is applied.
 *
 * WHY THIS EXISTS, AND WHY A CONTACT SHEET IS NOT ENOUGH
 * -----------------------------------------------------
 * A5's result: a wrapped rotation is invisible on every still. 184.88 deg and
 * -175.12 deg are the same orientation -- only the tween between them differs --
 * and the damage lives inside the single frame interval containing the
 * crossing. AE plays the tween, not the keyframes.
 *
 * That happened in this project on 2026-09-10. A layer crossed 180 deg between
 * frames 69 and 70; the readback sampled 68.5 and 85.5 and stepped straight
 * over it. Nothing was wrong, and nothing could have told us if it was.
 *
 * So the scrubber is CONTINUOUS on purpose. The slider moves in fractions of a
 * frame, not in frames, because the only thing a per-frame scrubber can show
 * is the set of values we already know are correct.
 *
 * WHAT THIS FILE IS ALLOWED TO KNOW
 * ---------------------------------
 * The transform, and nothing else. Geometry arrives already flattened into
 * polygons in layer space (`ae-physics-render/1`) because turning a comp into
 * polygons is bezier flattening, group transforms, layer scale and convex
 * decomposition -- verified Python that must not be reimplemented here. What
 * is left is:
 *
 *     world = position + R(theta) * (vertex - anchor)
 *
 * plus linear sampling between keyframes. `c2_render_model.py` section 3
 * checks exactly this arithmetic against `preview.py` at fractional frames, and
 * its controls fire at 100.0 px for an anchor sign error and 386.2 px for
 * degrees used as radians. This file is the third witness that check covers.
 *
 * One thing that check CANNOT cover: whether the canvas draws what these
 * numbers say. A correct polygon list and a wrong fill rule look identical
 * offline.
 */

/** Matches preview.py's PALETTE, so the window and the PNG agree on identity. */
const PALETTE = [
  "#e86a54", "#4e9ad0", "#7ec46c", "#eeba4a",
  "#a87cd0", "#56c6be", "#e284b2", "#969696",
];

type Poly = [number, number][];

type RenderLayer = {
  id: number;
  name: string;
  anchor: [number, number];
  parts: Poly[];
  static: boolean;
  rest: { position: [number, number]; rotation: number };
};

type RenderModel = {
  schema: string;
  comp: { width: number; height: number; fps: number; duration_frames: number };
  statics: [[number, number], [number, number]][];
  layers: RenderLayer[];
};

type Track = [number, number | [number, number]][];

type BakeLayer = {
  id: number;
  name: string;
  static: boolean;
  keyframes: { position: Track; rotation: Track };
};

/**
 * Linear interpolation, taking FRACTIONAL t.
 *
 * At integer t this returns the stored value untouched, which is the case our
 * bake makes common -- one keyframe per frame. The float is the whole point:
 * it is the only way to see what AE actually plays.
 */
function sample(track: Track, t: number, scalar: true): number;
function sample(track: Track, t: number, scalar: false): [number, number];
function sample(track: Track, t: number, scalar: boolean): any {
  if (!track || track.length === 0) return scalar ? 0 : [0, 0];
  if (t <= track[0][0]) return track[0][1];
  if (t >= track[track.length - 1][0]) return track[track.length - 1][1];

  let lo = 0;
  let hi = track.length - 1;
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1;
    if (track[mid][0] <= t) lo = mid;
    else hi = mid;
  }
  const [f0, v0] = track[lo];
  const [f1, v1] = track[hi];
  const u = f1 === f0 ? 0 : (t - f0) / (f1 - f0);
  if (scalar) return (v0 as number) + ((v1 as number) - (v0 as number)) * u;
  const a = v0 as [number, number];
  const b = v1 as [number, number];
  return [a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u];
}

/**
 * A pinned layer has no keyframes at all -- B3's rule, because writing even a
 * constant would overwrite the user's own placement. Without the resting pose
 * the render model ships, the floor would simply not be drawn and everything
 * would appear to fall through empty space.
 */
function poseOf(L: RenderLayer, baked: BakeLayer | undefined, t: number) {
  if (!baked || (!baked.keyframes.position.length && !baked.keyframes.rotation.length)) {
    return { p: L.rest.position, deg: L.rest.rotation };
  }
  return {
    p: sample(baked.keyframes.position, t, false),
    deg: sample(baked.keyframes.rotation, t, true),
  };
}

/** world = position + R(theta) * (vertex - anchor). Comp space is y-down and so
 *  is canvas space, so there is no flip anywhere -- A1's y-down convention
 *  paying out again. */
function worldParts(L: RenderLayer, baked: BakeLayer | undefined, t: number): Poly[] {
  const { p, deg } = poseOf(L, baked, t);
  const th = (deg * Math.PI) / 180;
  const c = Math.cos(th);
  const s = Math.sin(th);
  const [ax, ay] = L.anchor;
  return L.parts.map((part) =>
    part.map(([x, y]) => [
      p[0] + (x - ax) * c - (y - ay) * s,
      p[1] + (x - ax) * s + (y - ay) * c,
    ] as [number, number]),
  );
}

export class Viewport {
  private model: RenderModel | null = null;
  private baked = new Map<number, BakeLayer>();
  private t = 0;
  private playing = false;
  private raf = 0;
  private last = 0;
  private hidden = new Set<number>();

  constructor(
    private canvas: HTMLCanvasElement,
    private onTime: (t: number, max: number) => void,
  ) {}

  get frame() {
    return this.t;
  }

  get duration() {
    return this.model ? this.model.comp.duration_frames : 0;
  }

  get layers(): RenderLayer[] {
    return this.model ? this.model.layers : [];
  }

  load(renderText: string, bakeText: string) {
    this.model = JSON.parse(renderText);
    const bake = JSON.parse(bakeText);
    this.baked.clear();
    // Keyed by id, never by name: AE allows duplicate layer names and B1 found
    // Phase A silently dropping a layer for exactly this.
    for (const L of bake.layers as BakeLayer[]) this.baked.set(L.id, L);
    this.t = 0;
    this.draw();
  }

  toggle(id: number) {
    if (this.hidden.has(id)) this.hidden.delete(id);
    else this.hidden.add(id);
    this.draw();
  }

  isHidden(id: number) {
    return this.hidden.has(id);
  }

  seek(t: number) {
    if (!this.model) return;
    this.t = Math.max(0, Math.min(this.model.comp.duration_frames, t));
    this.draw();
  }

  play() {
    if (!this.model || this.playing) return;
    this.playing = true;
    this.last = performance.now();
    const step = (now: number) => {
      if (!this.playing || !this.model) return;
      const dt = (now - this.last) / 1000;
      this.last = now;
      // Advance in COMP time, not in frames per rAF tick: the point is to see
      // what AE plays, and AE plays seconds.
      this.t += dt * this.model.comp.fps;
      if (this.t >= this.model.comp.duration_frames) {
        this.t = 0;
      }
      this.draw();
      this.raf = requestAnimationFrame(step);
    };
    this.raf = requestAnimationFrame(step);
  }

  pause() {
    this.playing = false;
    if (this.raf) cancelAnimationFrame(this.raf);
    this.raf = 0;
  }

  get isPlaying() {
    return this.playing;
  }

  /** Fit the comp box into the canvas, preserving aspect. */
  private fit() {
    const m = this.model!;
    const cw = this.canvas.width;
    const ch = this.canvas.height;
    const k = Math.min(cw / m.comp.width, ch / m.comp.height);
    return { k, ox: (cw - m.comp.width * k) / 2, oy: (ch - m.comp.height * k) / 2 };
  }

  draw() {
    const cv = this.canvas;
    const ctx = cv.getContext("2d");
    if (!ctx) return;

    // Match the backing store to the CSS box so nothing is drawn blurry on a
    // scaled display.
    const rect = cv.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(rect.width * dpr));
    const h = Math.max(1, Math.round(rect.height * dpr));
    if (cv.width !== w || cv.height !== h) {
      cv.width = w;
      cv.height = h;
    }

    ctx.clearRect(0, 0, cv.width, cv.height);
    if (!this.model) return;

    const m = this.model;
    const { k, ox, oy } = this.fit();
    const X = (x: number) => ox + x * k;
    const Y = (y: number) => oy + y * k;

    // the comp box
    ctx.fillStyle = "#14161a";
    ctx.fillRect(X(0), Y(0), m.comp.width * k, m.comp.height * k);

    // The world the sim actually ran in. B2's rolling layer reached 838,591 px
    // through a floor-only default; drawing the walls means the box is visible
    // rather than inferred from where things stopped.
    ctx.strokeStyle = "#3a4048";
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (const [a, b] of m.statics) {
      ctx.moveTo(X(a[0]), Y(a[1]));
      ctx.lineTo(X(b[0]), Y(b[1]));
    }
    ctx.stroke();

    m.layers.forEach((L, i) => {
      if (this.hidden.has(L.id)) return;
      const colour = PALETTE[i % PALETTE.length];
      const parts = worldParts(L, this.baked.get(L.id), this.t);

      ctx.fillStyle = L.static ? "#2a2f36" : colour + "cc";
      ctx.strokeStyle = L.static ? "#4a525c" : colour;
      ctx.lineWidth = 1;

      for (const part of parts) {
        if (part.length < 2) continue;
        ctx.beginPath();
        ctx.moveTo(X(part[0][0]), Y(part[0][1]));
        for (let j = 1; j < part.length; j++) ctx.lineTo(X(part[j][0]), Y(part[j][1]));
        ctx.closePath();
        ctx.fill();
        ctx.stroke();
      }

      // The anchor, which is where the rotation actually happens. Worth seeing:
      // c2_render_model.py found that every dynamic anchor in a real comp sits
      // at [0, 0], so this dot is usually ON the layer's origin rather than at
      // its centroid, and a surprise there is a real finding rather than a
      // drawing bug.
      const { p } = poseOf(L, this.baked.get(L.id), this.t);
      ctx.fillStyle = "#ffffff";
      ctx.beginPath();
      ctx.arc(X(p[0]), Y(p[1]), 2.5, 0, Math.PI * 2);
      ctx.fill();
    });

    this.onTime(this.t, m.comp.duration_frames);
  }
}
