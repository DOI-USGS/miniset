/**
 * Miniset docs — Three.js hero banner (streaming point-cloud globe).
 *
 * Renders real control-network points (POINTS, not measures) from a normalized
 * StarDS net as a rotating globe of 3D circles behind the hero text. The points
 * are PARSED BY MINISET ITSELF, streamed straight from the REMOTE net over
 * /vsicurl/: the page loads the miniset WASM module and opens a StarDS handle on
 * the S3 URL, which reads only the byte ranges it needs — the header/index on
 * open, then the covering compressed blocks per batch — so the ~2 GB file is
 * never downloaded whole. Points stream in over time (batch by batch) as the
 * globe turns; each batch fades in (rather than blinking into place) via a
 * per-point "birth time" the shader ramps to full opacity.
 *
 * The WASM module is built with -sASYNCIFY, so the StarDS calls that fetch
 * (openPointsXYZ / pointsCount / pointsReadXYZ) SUSPEND and return Promises — they
 * are awaited here. The read loop keeps one batch in flight at a time and applies
 * it when it resolves, so the render/rotation never blocks on the network.
 *
 * Requires the S3 bucket to serve CORS (Access-Control-Allow-Origin + expose
 * Range/Content-Range) so the browser fetch() succeeds from the docs origin.
 *
 * Three.js is pulled from a CDN as an ES module so the static docs need no build
 * step (same no-bundler approach as the WASM playground).
 */

import * as THREE from "https://unpkg.com/three@0.160.0/build/three.module.js";

/* ==========================================================================
   TUNING LEVERS — tweak these live; everything visual keys off this object.
   After changing one from the console, call window.msHero.apply().
   ========================================================================== */
const CONFIG = {
  // Camera / framing.
  zoom: 1.5,          // >1 zooms IN (points get bigger + denser on screen)
  offsetX: 0.1,       // fraction of viewport to shove the globe RIGHT
                       //   (0 = centered, 0.4–0.5 packs it into the right 2/3)
  offsetY: 0.0,        // vertical nudge (fraction of viewport; + is up)
  autoRotate: true,    // slow spin so the globe reads as 3D
  rotateSpeed: 0.08,  // radians / second — also sets the load rate (see below)

  // Virtual joystick (bottom-right) — lets the viewer orbit the globe by hand.
  // Deflecting the stick sets a rotation VELOCITY (hold to keep turning); the
  // stick springs back to center on release and the orbit holds its position.
  // This orbits a group ABOVE the auto-spin, so it does NOT disturb the spin that
  // paces streaming, nor the camera framing (setViewOffset right-shift is intact).
  joystick: true,          // show the orbit joystick when supported (pointer input)
  joystickYawSpeed: 1.6,   // rad/s of yaw (left/right orbit) at full deflection
  joystickPitchSpeed: 1.2, // rad/s of pitch (up/down orbit) at full deflection
  joystickPitchLimit: 80,  // clamp pitch to ±this many degrees (avoid flipping over)

  // Orientation. The body's poles are on its Z axis, so we stand the globe up
  // (pole -> screen up) and spin AROUND the pole, then lean it by `axisTilt` so
  // it rotates on a slightly tilted axis like Mars (obliquity ≈ 25°). The tilt
  // leans toward/away from the camera about the screen X axis; `axisTiltRoll`
  // spins that lean around so the pole tips left/right instead of front/back.
  axisTilt: 45,        // degrees the spin axis leans from straight-up (Mars ≈ 25°)
  axisTiltRoll: 0,     // degrees to roll the lean around vertical (0 = tips back)

  // Points.
  dotSize: 1.3,        // on-screen diameter of each circle, in device px
  dotColor: 0xe7faf3,  // circle colour
  dotOpacity: 1.0,    // opacity of a point once faded in (uniform front-to-back)

  // Depth cue — a light DISTANCE FOG (not per-point opacity). Far points blend
  // toward the fog colour so you can still faintly tell they're on the back of
  // the shell, but they stay understated and don't wash out the front.
  fogColor: 0x162b67,  // colour distant points recede toward (≈ the backdrop)
  fogStrength: 1.0,    // 0 = no fog, 1 = back points fully become fogColor.
                       //   ~0.9 keeps the back very faint but still perceptible.
  fogFalloff: 1.0,     // how quickly the fog kicks in with depth (exponent).
                       //   1 = linear; >1 keeps the front clear and only fogs
                       //   the deep back (fog "kicks in" later); <1 fogs sooner.

  // Render edge — the vertical LINE (from the left) where the cloud starts to
  // appear. The canvas is masked to transparent left of `renderEdge`, ramping to
  // fully visible over `renderEdgeSoftness`, keeping the headline area clear.
  renderEdge: 0.01,        // fraction of hero width where points begin to render
  renderEdgeSoftness: 0.01, // fraction of width the fade-in spans past the edge

  // Streaming — the cloud fills as the globe turns.
  //
  // Points are stored sorted by azimuth (a vertical wedge per file window), so we
  // tie the load cursor to rotation: whatever wedge has rotated to the FRONT is
  // loaded, so front-facing points arrive first and one full turn loads the file.
  // Slowing rotateSpeed therefore also slows the load rate — they're the same knob.
  maxPoints: 3000000,      // hard cap on REAL points streamed to the GPU: the
                           //   stream STOPS once this many have loaded (source
                           //   holds ~9.4M). Bounds memory + fetching; regions past
                           //   the cap keep the line overview instead of real points.
  // ADAPTIVE batch size: every read costs 3 fixed HTTP round-trips (one per X/Y/Z
  // array) regardless of batch size, so per-point cost falls sharply as the batch
  // grows (~134µs/pt at 30k → ~34µs/pt at 240k). But a huge first read stalls the
  // first paint. So start small (streamBatchMin) for an instant seed, then ramp
  // ×streamBatchGrow each read up to streamBatchMax — fast first frame, cheap
  // steady state. (streamBatchSize kept as the starting/back-compat value.)
  streamBatchSize: 30000,  // starting batch size (first reads, quick first paint)
  streamBatchMin: 30000,   // floor
  streamBatchMax: 240000,  // ceiling (bigger = cheaper/pt but longer per read)
  streamBatchGrow: 1.6,    // multiply the batch each read until it hits the max
  fadeDurationMs: 1600,    // how long each point takes to ramp to full opacity
  seedPoints: 50000,       // load this many immediately at startup so the front
                           //   is populated at once (no waiting for the first read)
  loadLeadTurns: 0.0,      // load this fraction of a turn AHEAD of the front.
                           //   0 = new points appear dead-front (on camera); a
                           //   positive lead spawns them off to the side, where
                           //   the right-shoved globe can clip them off-screen.
  // Fetch budget: stop streaming real points after `loadTurns` full rotations
  // (in addition to the maxPoints cap), so a user who never zooms doesn't pay to
  // fetch the whole ~9.4M cloud. 0 = unlimited (fill until maxPoints).
  loadTurns: 3.0,          // stop the point stream after this many globe turns

  // Polyline LOD (cnet/3 "lines" layer) — the preferred track overview. The net's
  // geometric filaments are drawn as ribbons IMMEDIATELY (from a compact
  // on-ellipsoid model, no real points sent yet), then each line's real points
  // stream in and the line crossfades out as its points fade in. Falls back to
  // the GMM splats, then to the plain stream, if no lines layer exists.
  showLines: true,         // draw the polyline overview when a lines layer exists
  lineColor: 0x8fb7ff,     // sampled-point tint (match dotColor for a seamless look)
  lineOpacity: 0.55,       // sampled-point brightness (additive)
  lineFadeOutMs: 900,      // how long a line fades out once its real points arrive
  // The polylines are rendered as POINTS sampled along each line (disc sprites,
  // like the real cloud) rather than as lines, so the overview reads as points.
  lineSampleSpacing: 1500, // metres between sampled points along a line
  lineMaxSamples: 100000,  // cap on total sampled points (memory/perf guard)
  lineDotSize: 1.3,        // on-screen diameter of a sampled point (device px)

  // Gaussian-splat LOD (cnet/3 "summary" layer) — fallback overview when there's
  // no "lines" layer. Reconstructs the mixture as a synthetic point cloud.
  showSplats: true,        // reconstruct + render the GMM when a summary exists
  gmmPoints: 250000,       // total synthetic points sampled from the mixture
  gmmColor: 0x8fb7ff,      // synthetic-point tint (distinct from real dotColor)
  gmmOpacity: 0.5,         // synthetic-point brightness (additive)
  gmmFadeOutMs: 900,       // how long a splat's synthetic points take to fade out
                           //   once its real points arrive

  // Backdrop.
  clearAlpha: 0.0,     // 0 = transparent (the CSS gradient shows through)
};

// The miniset WASM module (same one the playground uses) and the .stards net.
const WASM_JS_URL = new URL("../assets/wasm/miniset.js", import.meta.url).href;
// Remote control net, read over /vsicurl/ — the module fetches only the byte
// ranges it needs (header + the covering blocks per batch), never the whole
// ~2 GB file. Requires the bucket to serve CORS (Allow-Origin + Range /
// Content-Range) so the browser fetch() succeeds from the docs origin.
const STARDS_URL =
  "/vsicurl/https://asc-isisdata.s3.us-west-2.amazonaws.com/cnf_test_data/largenet_lines.stards";

let minisetPromise = null;
function loadMiniset() {
  if (minisetPromise) return minisetPromise;
  minisetPromise = import(/* webpackIgnore: true */ WASM_JS_URL)
    .then(({ default: MinisetFactory }) =>
      MinisetFactory({
        locateFile: (p) =>
          p.endsWith(".wasm")
            ? new URL("../assets/wasm/miniset.wasm", import.meta.url).href
            : p,
        print: () => {},
        printErr: () => {},
      })
    )
    .catch((err) => { minisetPromise = null; throw err; });
  return minisetPromise;
}

/**
 * Open a streaming point source over the net, PARSED BY MINISET (WASM StarDS).
 * The module opens the net BY URL over /vsicurl/ and reads only the byte ranges
 * it needs — opening pulls just the header/index (one ranged GET); each later
 * pointsReadXYZ pulls only the covering blocks. The ~2 GB file is never
 * downloaded whole. (No local copy is fetched here.)
 * @returns {Promise<{Miniset: any, handle: number, total: number}>}
 */
async function openStream() {
  const Miniset = await loadMiniset();
  // Every StarDS call over /vsicurl SUSPENDS (ASYNCIFY → returns a Promise we
  // await). Each StarDataset open() reads the file's whole header/index up front
  // (~10-15 MB for this net), so opening the net MORE THAN ONCE serially — as the
  // old order did (openPointsXYZ then openLines) — doubled the ~15s pre-render
  // wait. So: open the OVERVIEW first and return it immediately, and open the
  // point-stream handle CONCURRENTLY (its Promise resolves in the background). The
  // banner then renders the overview after ~one open instead of blocking on two.

  // --- Overview (preferred: polyline "lines" layer; fallback: GMM "summary"). --
  let lines = null, summary = null;
  try {
    if (typeof Miniset.openLines === "function") {
      const lh = await Miniset.openLines(STARDS_URL);
      const L = await Miniset.linesCount(lh);
      if (L > 0) lines = await Miniset.linesReadAll(lh);
      await Miniset.closeLines(lh);
    }
  } catch (err) {
    console.warn("[Miniset hero] lines layer unavailable:", err);
    lines = null;
  }
  if (!lines) {
    try {
      if (typeof Miniset.openSummary === "function") {
        const sh = await Miniset.openSummary(STARDS_URL);
        const k = await Miniset.summaryCount(sh);
        if (k > 0) summary = await Miniset.summaryReadSplats(sh);
        await Miniset.closeSummary(sh);
      }
    } catch (err) {
      console.warn("[Miniset hero] summary unavailable, streaming without LOD:", err);
      summary = null;
    }
  }

  // --- Point stream: open lazily (its own big index read) AFTER the overview is
  // in hand, and hand back a PROMISE so the render can start on the overview
  // before this resolves. The pump awaits `ready` before its first read.
  const ready = (async () => {
    const handle = await Miniset.openPointsXYZ(STARDS_URL);
    const total = await Miniset.pointsCount(handle);
    return { handle, total };
  })();

  return { Miniset, summary, lines, ready };
}

/**
 * Custom point shader:
 *   - circular sprite,
 *   - per-point fade-in from a birth time (streaming), and
 *   - DISTANCE FOG for the depth cue: far points blend their colour toward
 *     `uFogColor` (opacity stays uniform), so the back of the shell reads as
 *     faint/receded without the alpha-blending artefacts that per-point opacity
 *     produced. Depth is view-space z (uNearZ = front of the shell, uFarZ =
 *     back), set each frame from the camera distance and globe radius.
 */
function makePointsMaterial(pixelRatio) {
  return new THREE.ShaderMaterial({
    // OPAQUE points with real depth testing (the three.js fog model). Occlusion is
    // resolved per-pixel by the depth buffer — the nearest point at each pixel
    // wins regardless of draw order — so a back point can no longer paint over a
    // front one. Distant points are then mixed toward uFogColor; set uFogColor to
    // the page background and far points literally recede INTO the background
    // (they don't add light the way additive blending did). Front points keep
    // their full colour. See https://threejs.org/manual/#en/fog.
    transparent: false,
    depthTest: true,
    depthWrite: true,
    uniforms: {
      uTime: { value: 0 },
      uFade: { value: CONFIG.fadeDurationMs / 1000 },
      uSize: { value: CONFIG.dotSize },
      uPixelRatio: { value: pixelRatio },
      uColor: { value: new THREE.Color(CONFIG.dotColor) },
      uOpacity: { value: CONFIG.dotOpacity },
      uFogColor: { value: new THREE.Color(CONFIG.fogColor) },
      uFogStrength: { value: CONFIG.fogStrength },
      uFogFalloff: { value: CONFIG.fogFalloff },
      uNearZ: { value: 1.0 },   // view-space z of the shell front (camera side)
      uFarZ: { value: -1.0 },   // view-space z of the shell back
    },
    vertexShader: /* glsl */ `
      attribute float aBirth;          // seconds; < 0 = not yet spawned
      uniform float uTime;
      uniform float uFade;
      uniform float uSize;
      uniform float uPixelRatio;
      uniform float uNearZ;
      uniform float uFarZ;
      varying float vBorn;             // 0 = just spawned, 1 = fully faded in
      varying float vDepth;            // 0 = back of shell, 1 = front (near camera)
      void main() {
        vec4 mv = modelViewMatrix * vec4(position, 1.0);
        gl_Position = projectionMatrix * mv;
        vBorn = (aBirth < 0.0) ? 0.0 : clamp((uTime - aBirth) / uFade, 0.0, 1.0);
        // View-space z is negative in front of the camera; larger z = nearer.
        vDepth = clamp((mv.z - uFarZ) / max(uNearZ - uFarZ, 1e-3), 0.0, 1.0);
        // Unborn / not-yet-arrived points collapse to nothing; born points grow
        // in slightly as they fade, and front points render a touch larger.
        float grow = 0.55 + 0.45 * vBorn;
        float depthSize = 0.85 + 0.15 * vDepth;
        gl_PointSize = vBorn <= 0.0 ? 0.0 : uSize * uPixelRatio * grow * depthSize;
      }
    `,
    fragmentShader: /* glsl */ `
      uniform vec3 uColor;
      uniform float uOpacity;
      uniform vec3 uFogColor;
      uniform float uFogStrength;
      uniform float uFogFalloff;
      varying float vBorn;
      varying float vDepth;
      void main() {
        vec2 d = gl_PointCoord - vec2(0.5);
        if (dot(d, d) > 0.25) discard;          // clip to a circle
        if (vBorn <= 0.0) discard;              // unborn: draw nothing (no depth)
        // Everything darkens by MIXING TOWARD uFogColor (opaque, no alpha add):
        //   - distance fog: far points (vDepth→0) recede into the fog/background;
        //   - fade-in: a newborn (vBorn→0) starts at the fog colour and resolves
        //     to its true colour as it matures — a fade with no transparency;
        //   - uOpacity < 1 dims everything by holding a floor of fog mix.
        // Nearer/brighter contributions win via max(), so the front stays crisp.
        float fog = pow(1.0 - vDepth, uFogFalloff) * uFogStrength;
        float birthMix = 1.0 - vBorn;
        float dim = 1.0 - clamp(uOpacity, 0.0, 1.0);
        float m = clamp(max(max(fog, birthMix), dim), 0.0, 1.0);
        gl_FragColor = vec4(mix(uColor, uFogColor, m), 1.0);
      }
    `,
  });
}

/**
 * Lower-triangular Cholesky factor L of a symmetric 3x3 covariance given as its
 * upper triangle (xx,xy,xz,yy,yz,zz), such that Σ = L·Lᵀ. L maps a unit sphere to
 * the covariance's 1-σ ellipsoid (orientation + extent), so it is exactly the
 * per-instance transform for a splat — no eigen solver needed. Falls back to a
 * tiny isotropic scale if Σ isn't positive-definite (degenerate/empty splats).
 * Writes the 9 column-major basis entries into `out9`.
 */
function cholesky3(xx, xy, xz, yy, yz, zz, out9) {
  const l11 = Math.sqrt(Math.max(xx, 0));
  const l21 = l11 > 1e-9 ? xy / l11 : 0;
  const l31 = l11 > 1e-9 ? xz / l11 : 0;
  const l22 = Math.sqrt(Math.max(yy - l21 * l21, 0));
  const l32 = l22 > 1e-9 ? (yz - l31 * l21) / l22 : 0;
  const l33 = Math.sqrt(Math.max(zz - l31 * l31 - l32 * l32, 0));
  // Column-major 3x3 (three.js Matrix3/Matrix4 basis order): columns are the
  // images of the unit axes. L is lower-triangular: col0=(l11,l21,l31), etc.
  out9[0] = l11; out9[1] = l21; out9[2] = l31;
  out9[3] = 0;   out9[4] = l22; out9[5] = l32;
  out9[6] = 0;   out9[7] = 0;   out9[8] = l33;
}

// Deterministic per-point pseudo-random in [0,1) (mulberry32) + a Box-Muller
// standard normal, so the synthetic cloud is stable across frames/reloads.
function mulberry32(a) {
  return function () {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/**
 * Build the polyline overview as POINTS sampled along each line (disc sprites,
 * like the real cloud) — so the overview reads as a point cloud, not wireframe.
 * Each line is sampled by arc length at ~`lineSampleSpacing` metres (at least its
 * own vertices, so short lines still show). Points carry a per-point `aArrival`
 * for the per-line crossfade, and the shader applies the same fog/depth cue as
 * the real points. Sampled points are laid out in contiguous per-line blocks so
 * fade-out is an O(block) stamp.
 * @param {{positions:Float32Array, voff:Uint32Array, count:number}} lines
 * @returns {{ points: THREE.Points, mat, markLineArrived(i,tSec) }}
 */
function makeLineMesh(lines, pixelRatio) {
  const L = lines.count | 0;
  const V = lines.positions;          // V*3 floats, all lines' vertices in order
  const voff = lines.voff;            // vertex CSR (L+1)
  const spacing = Math.max(1, CONFIG.lineSampleSpacing);

  // Pass 1: count samples per line = ceil(arcLength/spacing)+1, clamped so the
  // total respects lineMaxSamples (scale spacing up if we'd blow the budget).
  const segLen = (a, b) => {
    const dx = V[b*3]-V[a*3], dy = V[b*3+1]-V[a*3+1], dz = V[b*3+2]-V[a*3+2];
    return Math.sqrt(dx*dx + dy*dy + dz*dz);
  };
  const lineArc = new Float64Array(L);
  let totalArc = 0;
  for (let i = 0; i < L; i++) {
    let arc = 0;
    for (let v = voff[i]; v + 1 < voff[i+1]; v++) arc += segLen(v, v+1);
    lineArc[i] = arc; totalArc += arc;
  }
  // Effective spacing: never finer than `spacing`, but coarsen if the budget
  // would be exceeded (≈ totalArc/budget).
  const budget = Math.max(1, CONFIG.lineMaxSamples | 0);
  const eff = Math.max(spacing, totalArc / budget);

  const start = new Uint32Array(L);
  const count = new Uint32Array(L);
  let total = 0;
  for (let i = 0; i < L; i++) {
    const nv = voff[i+1] - voff[i];
    let ns = nv <= 1 ? nv : Math.max(nv, Math.floor(lineArc[i] / eff) + 1);
    start[i] = total; count[i] = ns; total += ns;
  }

  // Pass 2: sample each line at equal arc-length steps into the point buffer.
  const pos = new Float32Array(total * 3);
  const arr = new Float32Array(total).fill(-1);   // per-point arrival time
  for (let i = 0; i < L; i++) {
    const a = voff[i], b = voff[i+1], ns = count[i], w0 = start[i];
    const nv = b - a;
    if (nv === 0) continue;
    if (nv === 1 || ns <= 1) {
      pos[w0*3] = V[a*3]; pos[w0*3+1] = V[a*3+1]; pos[w0*3+2] = V[a*3+2];
      continue;
    }
    // Walk the polyline placing ns points at arc positions [0..arc] evenly.
    const arc = lineArc[i], stepArc = arc / (ns - 1);
    let seg = a, segStartArc = 0, segL = segLen(a, a+1);
    for (let s = 0; s < ns; s++) {
      let target = s * stepArc;
      // advance to the segment containing `target`
      while (seg + 2 < b && target > segStartArc + segL) {
        segStartArc += segL; seg++; segL = segLen(seg, seg+1);
      }
      const t = segL > 0 ? Math.min(1, Math.max(0, (target - segStartArc) / segL)) : 0;
      const w = w0 + s;
      pos[w*3]   = V[seg*3]   + t * (V[(seg+1)*3]   - V[seg*3]);
      pos[w*3+1] = V[seg*3+1] + t * (V[(seg+1)*3+1] - V[seg*3+1]);
      pos[w*3+2] = V[seg*3+2] + t * (V[(seg+1)*3+2] - V[seg*3+2]);
    }
  }

  const geom = new THREE.BufferGeometry();
  geom.setAttribute("position", new THREE.BufferAttribute(pos, 3));
  const aArrival = new THREE.BufferAttribute(arr, 1).setUsage(THREE.DynamicDrawUsage);
  geom.setAttribute("aArrival", aArrival);

  const mat = new THREE.ShaderMaterial({
    // Points (disc sprites), transparent for the crossfade, with the SAME distance
    // fog + depth cue as the real cloud: far samples blend toward uFogColor and
    // dim so the back recedes into the background. uNearZ/uFarZ set per frame.
    transparent: true,
    depthWrite: false,
    depthTest: false,
    blending: THREE.AdditiveBlending,
    uniforms: {
      uTime: { value: 0 },
      uColor: { value: new THREE.Color(CONFIG.lineColor) },
      uOpacity: { value: CONFIG.lineOpacity },
      uFadeOut: { value: CONFIG.lineFadeOutMs / 1000 },
      uSize: { value: CONFIG.lineDotSize },
      uPixelRatio: { value: pixelRatio },
      uFogColor: { value: new THREE.Color(CONFIG.fogColor) },
      uFogStrength: { value: CONFIG.fogStrength },
      uFogFalloff: { value: CONFIG.fogFalloff },
      uNearZ: { value: 1.0 },
      uFarZ: { value: -1.0 },
    },
    vertexShader: /* glsl */ `
      attribute float aArrival;
      uniform float uTime, uFadeOut, uSize, uPixelRatio, uNearZ, uFarZ;
      varying float vVis;
      varying float vDepth;            // 0 = back of shell, 1 = front (near camera)
      void main() {
        vec4 mv = modelViewMatrix * vec4(position, 1.0);
        gl_Position = projectionMatrix * mv;
        float vis = 1.0;
        if (aArrival >= 0.0) vis = 1.0 - clamp((uTime - aArrival) / uFadeOut, 0.0, 1.0);
        vVis = vis;
        vDepth = clamp((mv.z - uFarZ) / max(uNearZ - uFarZ, 1e-3), 0.0, 1.0);
        float depthSize = 0.85 + 0.15 * vDepth;   // front points a touch larger
        gl_PointSize = vis <= 0.0 ? 0.0 : uSize * uPixelRatio * depthSize;
      }
    `,
    fragmentShader: /* glsl */ `
      uniform vec3 uColor, uFogColor;
      uniform float uOpacity, uFogStrength, uFogFalloff;
      varying float vVis;
      varying float vDepth;
      void main() {
        vec2 d = gl_PointCoord - vec2(0.5);
        if (dot(d, d) > 0.25) discard;          // round sprite
        if (vVis <= 0.0) discard;
        // Distance fog: far (vDepth→0) samples recede toward the fog/background
        // colour AND lose brightness, matching the point shader's depth cue.
        float fog = pow(1.0 - vDepth, uFogFalloff) * uFogStrength;
        vec3 rgb = mix(uColor, uFogColor, fog);
        float bright = uOpacity * vVis * (1.0 - 0.85 * fog);   // back dims out
        gl_FragColor = vec4(rgb * bright, 1.0);  // additive (premultiplied)
      }
    `,
  });

  const points = new THREE.Points(geom, mat);
  points.frustumCulled = false;

  function markLineArrived(i, tSeconds) {
    const s = start[i], c = count[i];
    if (c === 0 || arr[s] >= 0) return;
    for (let j = 0; j < c; j++) arr[s + j] = tSeconds;
    aArrival.addUpdateRange(s, c);
    aArrival.needsUpdate = true;
  }

  return { points, mat, markLineArrived };
}

/**
 * RECONSTRUCT the point cloud from the GMM: sample `gmmPoints` synthetic points
 * from the mixture — pick a splat with probability ∝ weight, then draw
 * p = μ + L·z (z ~ N(0,I)³, L = Cholesky(Σ)). This regenerates the tracks from
 * only the ~few-hundred-KB model, no real points fetched. Each synthetic point
 * also carries the index of the splat it came from (`aSplat`), so it can be
 * faded out per-splat once that splat's real points stream in.
 *
 * Returns { points: THREE.Points, splatArrival: Float32Array(K) } — the mesh and
 * the per-splat "real points arrived at t" array the shader reads for fade-out.
 * @param {{muX,muY,muZ,s0..s5,weight,rangeStart,rangeCount,count}} splats
 */
function makeGmmCloud(splats, pixelRatio) {
  const K = splats.count | 0;
  const N = Math.max(0, CONFIG.gmmPoints | 0);

  // Alias-free weighted pick via a CDF over splat weights (counts).
  const cdf = new Float64Array(K);
  let total = 0;
  for (let i = 0; i < K; i++) { total += splats.weight[i] || 0; cdf[i] = total; }
  if (total <= 0) return null;

  // Allot each splat a share of N proportional to its weight, laid out in
  // CONTIGUOUS blocks by splat index. That makes per-splat fade-out an O(block)
  // range stamp (not an O(N) scan) when a splat's real points arrive, and it lets
  // the sampler write points grouped by splat. gmmStart[k]..gmmStart[k]+gmmCount[k].
  const gmmStart = new Uint32Array(K);
  const gmmCount = new Uint32Array(K);
  {
    let acc = 0, assigned = 0;
    for (let k = 0; k < K; k++) {
      acc += splats.weight[k] || 0;
      const upto = Math.round((acc / total) * N);
      gmmStart[k] = assigned;
      gmmCount[k] = Math.max(0, upto - assigned);
      assigned = upto;
    }
  }

  const positions = new Float32Array(N * 3);
  const rand = mulberry32(0x9e3779b9);
  const L = new Float64Array(9);
  const gauss = () => {
    let u = 0, v = 0;
    while (u === 0) u = rand();
    while (v === 0) v = rand();
    return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
  };

  // Sample each splat's block: p = μ + L·z (L column-major col j=(L[3j..3j+2])).
  for (let k = 0; k < K; k++) {
    cholesky3(splats.s0[k], splats.s1[k], splats.s2[k],
              splats.s3[k], splats.s4[k], splats.s5[k], L);
    const start = gmmStart[k], cnt = gmmCount[k];
    const mx = splats.muX[k], my = splats.muY[k], mz = splats.muZ[k];
    for (let j = 0; j < cnt; j++) {
      const z0 = gauss(), z1 = gauss(), z2 = gauss();
      const n = (start + j) * 3;
      positions[n]     = mx + L[0] * z0 + L[3] * z1 + L[6] * z2;
      positions[n + 1] = my + L[1] * z0 + L[4] * z1 + L[7] * z2;
      positions[n + 2] = mz + L[2] * z0 + L[5] * z1 + L[8] * z2;
    }
  }

  // Per-point arrival time (seconds); -1 = this splat's real points not loaded.
  // The shader fades a synthetic point out over gmmFadeOutMs from its arrival.
  const arrivalAttr = new Float32Array(N).fill(-1);

  const geom = new THREE.BufferGeometry();
  geom.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  const aArrival = new THREE.BufferAttribute(arrivalAttr, 1).setUsage(THREE.DynamicDrawUsage);
  geom.setAttribute("aArrival", aArrival);

  const mat = new THREE.ShaderMaterial({
    transparent: true,
    depthWrite: false,
    depthTest: false,
    blending: THREE.AdditiveBlending,
    uniforms: {
      uTime: { value: 0 },
      uSize: { value: CONFIG.dotSize },
      uPixelRatio: { value: pixelRatio },
      uColor: { value: new THREE.Color(CONFIG.gmmColor) },
      uOpacity: { value: CONFIG.gmmOpacity },
      uFadeOut: { value: CONFIG.gmmFadeOutMs / 1000 },
    },
    vertexShader: /* glsl */ `
      attribute float aArrival;             // seconds real points arrived; <0 = not yet
      uniform float uTime, uSize, uPixelRatio, uFadeOut;
      varying float vVis;
      void main() {
        gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
        // Full brightness until this splat's real points arrive, then fade out.
        float vis = 1.0;
        if (aArrival >= 0.0) vis = 1.0 - clamp((uTime - aArrival) / uFadeOut, 0.0, 1.0);
        vVis = vis;
        gl_PointSize = vis <= 0.0 ? 0.0 : uSize * uPixelRatio;
      }
    `,
    fragmentShader: /* glsl */ `
      uniform vec3 uColor; uniform float uOpacity;
      varying float vVis;
      void main() {
        vec2 d = gl_PointCoord - vec2(0.5);
        if (dot(d, d) > 0.25) discard;      // round sprite
        if (vVis <= 0.0) discard;
        gl_FragColor = vec4(uColor * (uOpacity * vVis), 1.0);  // additive
      }
    `,
  });

  const points = new THREE.Points(geom, mat);
  points.frustumCulled = false;

  // Called when splat `k`'s real points arrive: stamp its contiguous synthetic
  // block with the arrival time so the shader fades exactly those points out.
  // O(block) per splat, each splat once → O(N) total across the run.
  function markSplatArrived(k, tSeconds) {
    const start = gmmStart[k], cnt = gmmCount[k];
    if (cnt === 0 || arrivalAttr[start] >= 0) return;
    for (let j = 0; j < cnt; j++) arrivalAttr[start + j] = tSeconds;
    aArrival.addUpdateRange(start, cnt);
    aArrival.needsUpdate = true;
  }

  return { points, mat, markSplatArrived };
}

/**
 * Initialize the Three.js scene inside `canvas`, sized to `host`, and stream the
 * point cloud in from `src` over time.
 * @param {HTMLCanvasElement} canvas
 * @param {HTMLElement} host
 * @param {{Miniset: any, handle: number, total: number, summary: object|null}} src
 */
function initScene(canvas, host, src) {
  const renderer = new THREE.WebGLRenderer({ canvas, alpha: true, antialias: true });
  const pixelRatio = Math.min(window.devicePixelRatio || 1, 2);
  renderer.setPixelRatio(pixelRatio);
  renderer.setClearAlpha(CONFIG.clearAlpha);

  const scene = new THREE.Scene();

  // The point-stream handle opens in the background (src.ready); until it lands
  // `pointHandle` is null and the pump no-ops, but the OVERVIEW renders right
  // away. `total` is the real point count once known; `target` is bounded by
  // maxPoints. We size buffers to the overview's covered points if available
  // (upper-bounds the stream) else to maxPoints.
  let pointHandle = null;
  let total = CONFIG.maxPoints;
  if (src.lines && src.lines.count) {
    // rangeStart[last]+rangeCount[last] bounds the points the lines cover.
    const L = src.lines.count | 0;
    total = (src.lines.rangeStart[L - 1] >>> 0) + (src.lines.rangeCount[L - 1] >>> 0);
  }
  // How many points we will ultimately stream in.
  const target = Math.min(total, CONFIG.maxPoints);

  // Preallocate the GPU buffers once; batches fill regions of them over time.
  const positions = new Float32Array(target * 3);
  const births = new Float32Array(target).fill(-1); // -1 = not yet spawned
  const posAttr = new THREE.BufferAttribute(positions, 3).setUsage(THREE.DynamicDrawUsage);
  const birthAttr = new THREE.BufferAttribute(births, 1).setUsage(THREE.DynamicDrawUsage);

  const geom = new THREE.BufferGeometry();
  geom.setAttribute("position", posAttr);
  geom.setAttribute("aBirth", birthAttr);
  geom.setDrawRange(0, 0); // nothing loaded yet

  const material = makePointsMaterial(pixelRatio);

  const cloud = new THREE.Points(geom, material);
  // Disable frustum culling. The geometry streams in over time, so at the first
  // render the position buffer is still all-zeros and three.js would compute a
  // radius-0 bounding sphere at the origin — then never recompute it, culling the
  // whole (multi-million-metre) cloud once real points arrive. The cloud always
  // fills the view when present, so skip culling entirely.
  cloud.frustumCulled = false;

  // Scene graph for a tilted, pole-up globe:
  //   orient (stands the pole up + leans it by axisTilt)
  //     └─ spin (rotates the body about its pole over time)
  //          └─ cloud
  // The body's poles are on its Z axis, so `spin` rotates about Z; `orient` first
  // maps Z→up (screen +Y) then applies the tilt, so the whole thing turns on a
  // slightly leaned axis like Mars instead of spinning with the poles on the side.
  const spin = new THREE.Group();
  spin.add(cloud);
  const orient = new THREE.Group();
  orient.add(spin);
  // userOrbit sits ABOVE orient so the joystick can orbit the whole globe (yaw +
  // pitch) on top of the fixed pole-up tilt and the time-based auto-spin. Keeping
  // it outside `orient`/`spin` means hand-orbit never disturbs the spin that
  // paces streaming, and the camera stays put (framing/right-shift preserved).
  const userOrbit = new THREE.Group();
  userOrbit.add(orient);
  scene.add(userOrbit);
  let orbitYaw = 0;    // radians, accumulated from the joystick (about screen up)
  let orbitPitch = 0;  // radians, accumulated from the joystick (about screen right)

  // LOD-0 overview, added to the `spin` group so it rotates with the globe.
  // Preferred: the polyline ("lines") model — geometric filaments drawn as
  // ribbons immediately, each fading out as its real points stream in. Fallback:
  // the GMM synthetic cloud. Both present only if the corresponding layer exists.
  const hasLines = CONFIG.showLines && src.lines && (src.lines.count | 0) > 0;
  const hasSummary = !hasLines && CONFIG.showSplats && src.summary && (src.summary.count | 0) > 0;
  let lineOverlay = null;   // { points, mat, markLineArrived }
  let gmm = null;           // { points, mat, markSplatArrived }
  if (hasLines) {
    lineOverlay = makeLineMesh(src.lines, pixelRatio);
    lineOverlay.points.renderOrder = -1;   // draw before the real points
    spin.add(lineOverlay.points);
  } else if (hasSummary) {
    gmm = makeGmmCloud(src.summary, pixelRatio);
    if (gmm) { gmm.points.renderOrder = -1; spin.add(gmm.points); }
  }

  /** Point the spin axis (body Z) up and lean it by CONFIG.axisTilt. Rebuilt from
   *  CONFIG so the tilt is tunable live via msHero.apply(). */
  function applyOrientation() {
    const tilt = THREE.MathUtils.degToRad(CONFIG.axisTilt || 0);
    const roll = THREE.MathUtils.degToRad(CONFIG.axisTiltRoll || 0);
    // Base: rotate -90° about X so the body's +Z pole points to screen +Y (up).
    // Tilt: lean that up-axis by `tilt`; `roll` spins the lean around vertical so
    // it can tip back/front (roll 0) or left/right (roll 90).
    const e = new THREE.Euler(-Math.PI / 2 + tilt, 0, 0, "XYZ");
    orient.quaternion.setFromEuler(e);
    if (roll) {
      const rollQ = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 1, 0), roll);
      orient.quaternion.premultiply(rollQ);
    }
  }
  applyOrientation();

  // Body-centered coords: the globe is centered on the origin, so frame from the
  // body radius. Seeded with a Mars-ish radius; refined from the first batch.
  let radius = 3.4e6;
  const camera = new THREE.PerspectiveCamera(45, 1, radius * 0.01, radius * 100);
  let framedFromData = false;

  /** Mask the canvas so points only start rendering past a vertical line from the
   *  left (keeps the headline area clear). `renderEdge` is where the cloud begins
   *  to appear; it ramps to fully visible over `renderEdgeSoftness`. Driven from
   *  CONFIG here (not static CSS) so both are tunable live via msHero.apply(). */
  function applyRenderEdge() {
    const a = Math.max(0, Math.min(1, CONFIG.renderEdge)) * 100;
    const b = Math.min(100, a + Math.max(0, CONFIG.renderEdgeSoftness) * 100);
    const grad = `linear-gradient(to right, transparent 0%, transparent ${a.toFixed(1)}%, #000 ${b.toFixed(1)}%)`;
    canvas.style.webkitMaskImage = grad;
    canvas.style.maskImage = grad;
  }
  applyRenderEdge();

  /** Place the camera: distance from `zoom`, and shift the globe toward the
   *  right of the frame via the projection's view offset. */
  function frame() {
    const w = host.clientWidth || 1;
    const h = host.clientHeight || 1;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    camera.near = radius * 0.01;
    camera.far = radius * 100;

    // Distance so the globe's bounding sphere fits, then divide by zoom to push in.
    const fitDist = radius / Math.tan((camera.fov * Math.PI) / 180 / 2);
    camera.position.set(0, 0, fitDist / CONFIG.zoom);
    camera.lookAt(0, 0, 0);

    // setViewOffset renders a shifted sub-window of the full frame, sliding the
    // (centered) globe toward the right / vertically without moving the camera
    // off-axis. Offsets are in pixels of the full frame.
    const dx = -CONFIG.offsetX * w;   // negative full-x => content moves right
    const dy = -CONFIG.offsetY * h;
    camera.setViewOffset(w, h, dx, dy, w, h);
    camera.updateProjectionMatrix();

    // Depth-contrast bounds: the cloud is a shell of ~`radius` centered at the
    // origin, camera at distance D on +Z. View-space z spans [-radius-D (back),
    // radius-D (front)]; the shaders ramp the fog/depth cue between these. The
    // line overlay uses the same bounds so its fog matches the points.
    const D = camera.position.z;
    material.uniforms.uNearZ.value = radius - D;
    material.uniforms.uFarZ.value = -radius - D;
    if (lineOverlay) {
      lineOverlay.mat.uniforms.uNearZ.value = radius - D;
      lineOverlay.mat.uniforms.uFarZ.value = -radius - D;
    }
  }
  frame();

  const ro = new ResizeObserver(frame);
  ro.observe(host);

  // --- Subtle "N points loaded" counter --------------------------------------
  // Created in JS so the markup stays clean; styled via .ms-hero__counter.
  const counterEl = document.createElement("div");
  counterEl.className = "ms-hero__counter";
  counterEl.setAttribute("aria-hidden", "true");
  host.appendChild(counterEl);

  // Static caption above the counter, sharing its exact styling (see the shared
  // .ms-hero__counter, .ms-hero__caption rule in extra.css).
  const captionEl = document.createElement("div");
  captionEl.className = "ms-hero__caption";
  captionEl.setAttribute("aria-hidden", "true");
  captionEl.textContent = "Real-time loading of MRO CTX control network via Miniset";
  host.appendChild(captionEl);

  // --- Subtle zoom slider (right edge) ---------------------------------------
  // A native range input (styled + rotated to vertical in extra.css) that drives
  // CONFIG.zoom live via frame(). Dragging up zooms IN. Bounds bracket the default
  // (CONFIG.zoom = 1.5): out to ~0.6 (whole globe with margin), in to ~4.
  const ZOOM_MIN = 0.6, ZOOM_MAX = 4.0;
  const zoomWrap = document.createElement("div");
  zoomWrap.className = "ms-hero__zoom";
  const zoomInput = document.createElement("input");
  zoomInput.type = "range";
  zoomInput.min = String(ZOOM_MIN);
  zoomInput.max = String(ZOOM_MAX);
  zoomInput.step = "0.01";
  zoomInput.value = String(Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, CONFIG.zoom)));
  zoomInput.setAttribute("aria-label", "Zoom the globe");
  zoomInput.setAttribute("title", "Zoom");
  const onZoom = () => {
    CONFIG.zoom = parseFloat(zoomInput.value) || CONFIG.zoom;
    frame();
  };
  zoomInput.addEventListener("input", onZoom);
  zoomWrap.appendChild(zoomInput);
  host.appendChild(zoomWrap);

  // --- Virtual orbit joystick (bottom-right) ---------------------------------
  // A base disc + a draggable knob. Dragging the knob deflects it (clamped to the
  // base radius); the normalized deflection [jx,jy] ∈ [-1,1]² becomes a rotation
  // VELOCITY applied each frame (see tick): x → yaw, y → pitch. On release the
  // knob springs back to center and deflection goes to zero, so the orbit holds.
  // Pointer Events cover mouse + touch + pen with one code path.
  let jx = 0, jy = 0;             // current normalized deflection, read by tick()
  let jActive = false;            // a drag is in progress
  const joyWrap = document.createElement("div");
  joyWrap.className = "ms-hero__joy";
  joyWrap.setAttribute("aria-hidden", "true");
  const joyKnob = document.createElement("div");
  joyKnob.className = "ms-hero__joy-knob";
  joyWrap.appendChild(joyKnob);
  host.appendChild(joyWrap);
  if (!CONFIG.joystick) joyWrap.style.display = "none";

  // Deflect the knob to a pointer position (client coords), clamped to the base
  // radius, and update [jx,jy]. y is inverted so pushing UP pitches the top of the
  // globe toward the viewer (natural "look up") rather than away.
  function joySet(clientX, clientY) {
    const r = joyWrap.getBoundingClientRect();
    const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    const maxR = r.width / 2 - 10;                    // knob travel radius (px)
    let dx = clientX - cx, dy = clientY - cy;
    const d = Math.hypot(dx, dy);
    if (d > maxR && d > 0) { dx = (dx / d) * maxR; dy = (dy / d) * maxR; }
    joyKnob.style.transform = `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px))`;
    jx = maxR > 0 ? dx / maxR : 0;
    jy = maxR > 0 ? dy / maxR : 0;
  }
  function joyReset() {
    jActive = false; jx = 0; jy = 0;
    joyKnob.style.transform = "translate(-50%, -50%)";
    joyWrap.classList.remove("is-active");
  }
  const onJoyDown = (e) => {
    jActive = true;
    joyWrap.classList.add("is-active");
    try { joyWrap.setPointerCapture(e.pointerId); } catch (_) {}
    joySet(e.clientX, e.clientY);
    e.preventDefault();
  };
  const onJoyMove = (e) => { if (jActive) { joySet(e.clientX, e.clientY); e.preventDefault(); } };
  const onJoyUp = (e) => {
    if (!jActive) return;
    try { joyWrap.releasePointerCapture(e.pointerId); } catch (_) {}
    joyReset();
  };
  joyWrap.addEventListener("pointerdown", onJoyDown);
  joyWrap.addEventListener("pointermove", onJoyMove);
  joyWrap.addEventListener("pointerup", onJoyUp);
  joyWrap.addEventListener("pointercancel", onJoyUp);
  // Never scroll/gesture the page while dragging on touch.
  joyWrap.style.touchAction = "none";

  const numFmt = new Intl.NumberFormat();
  // Sample the load rate on a fixed interval and smooth it (EMA) so the readout
  // shows a steady points/second rather than per-frame jitter.
  let rate = 0;               // smoothed points/second
  let rateSampleAt = 0;       // ms of the last rate sample
  let rateSampleLoaded = 0;   // `loaded` at the last rate sample
  let lastText = "";
  function updateCounter(nowMs) {
    if (rateSampleAt === 0) { rateSampleAt = nowMs; rateSampleLoaded = loaded; }
    const dt = nowMs - rateSampleAt;
    if (dt >= 250) {
      const inst = ((loaded - rateSampleLoaded) * 1000) / dt;   // pts/sec this window
      rate = rate === 0 ? inst : rate * 0.7 + inst * 0.3;       // EMA
      rateSampleAt = nowMs;
      rateSampleLoaded = loaded;
    }
    // Once fully loaded, the rate is meaningless — drop it.
    const rateStr = loaded >= target || rate < 1 ? "" : ` · ${numFmt.format(Math.round(rate))}p/s`;
    const text = `${numFmt.format(loaded)} points (capped to 3m) ${rateStr}`;
    if (text === lastText) return;   // only touch the DOM when it changes
    lastText = text;
    counterEl.textContent = text;
  }

  // --- Streaming, paced by rotation ------------------------------------------
  // Points load in file order, contiguous from 0, with the target count tied to
  // how far the globe has rotated (`swept`): one full turn loads the whole file.
  // Because the load target is a function of `swept` (= time × rotateSpeed),
  // slowing the rotation slows the load rate — one knob controls both. `swept`
  // also drives the spin (about the tilted body pole; see tick), so newly loaded
  // points keep arriving as the globe turns.
  let loaded = 0;              // points streamed in so far (contiguous from 0)
  let swept = 0;               // radians rotated since start
  let done = false;
  let inFlight = false;        // a batch read is awaiting the network

  // Kick off a batch read if one is due and none is in flight. The read is a
  // network fetch (StarDS ranged GET) and, under -sASYNCIFY, pointsReadXYZ returns
  // a Promise — so we do NOT block the render loop: the globe keeps rotating while
  // the batch is in flight, and the results are applied (and the points born, so
  // they fade in) whenever the fetch resolves.
  function pumpBatch(nowMs) {
    if (done || inFlight || !pointHandle || loaded >= target) { if (pointHandle && loaded >= target) done = true; return; }

    // How far the front (plus lead) has swept => how many points should exist.
    // Also seed a chunk immediately so the front is populated from the first
    // frames instead of waiting for the rotation to sweep them in.
    const leadAngle = CONFIG.loadLeadTurns * 2 * Math.PI;
    const wantFrac = Math.min(1, (swept + leadAngle) / (2 * Math.PI));
    const targetLoaded = Math.min(
      target,
      Math.max(CONFIG.seedPoints, Math.ceil(wantFrac * target))
    );
    if (targetLoaded <= loaded) return;

    const start = loaded;
    const want = Math.min(CONFIG.streamBatchSize, targetLoaded - loaded);
    inFlight = true;
    Promise.resolve(src.Miniset.pointsReadXYZ(pointHandle, start, want))
      .then((batch) => {
        const got = batch.count | 0;
        if (got <= 0) { done = true; return; }

        positions.set(batch.positions.subarray(0, got * 3), start * 3);

        // Refine framing once we have real data (globe radius is ~constant).
        if (!framedFromData && batch.radius > 0) {
          radius = batch.radius;
          framedFromData = true;
          frame();
        }

        // Points are born on arrival (they fade in from here).
        const now = performance.now() / 1000;
        births.fill(now, start, start + got);

        // Upload only the touched slices (not the whole multi-MB buffer). r159+
        // uses addUpdateRange(start, count); the old `updateRange =` setter was
        // removed and assigning to it throws in a module, which would kill the loop.
        posAttr.addUpdateRange(start * 3, got * 3);
        posAttr.needsUpdate = true;
        birthAttr.addUpdateRange(start, got);
        birthAttr.needsUpdate = true;

        loaded = start + got;
        geom.setDrawRange(0, loaded);
        if (loaded >= target) done = true;
      })
      .catch((err) => { done = true; console.warn("[Miniset hero] stream read failed:", err); })
      .finally(() => { inFlight = false; });
  }

  // --- Overview-driven streaming (cnet/3) ------------------------------------
  // With a "lines" or "summary" overview, the stream follows the overview's
  // per-item [rangeStart,rangeCount) windows in file order (front-first as the
  // globe turns) — the LOD-0 → LOD-1 drill-down. Rotation paces it (how many
  // items are "due" scales with `swept`); as each item's real points arrive we
  // crossfade its overview primitive (line/splat) out.
  //
  // THROUGHPUT: items are tiny (a line ≈ 40 points), so reading one item per call
  // would pay a full get_slice/block-decompress per ~40 points — ~1000x fewer
  // points/read than the fixed-batch path. To keep the old throughput we COALESCE
  // consecutive items into ONE read that spans a contiguous point window up to
  // ~streamBatchSize points, then mark every covered item arrived. Items are
  // contiguous in file order, so a run of them is a single window (any gap points
  // in between are real points too and simply stream in with the run).
  const overview = hasLines ? src.lines : (hasSummary ? src.summary : null);
  const markArrived = hasLines
    ? (lineOverlay ? (i, t) => lineOverlay.markLineArrived(i, t) : null)
    : (gmm ? (i, t) => gmm.markSplatArrived(i, t) : null);
  let itemCursor = 0;    // index of the next overview item to stream
  let drawHigh = 0;      // furthest point index written (items stream out of order)
  let curBatch = CONFIG.streamBatchMin || CONFIG.streamBatchSize;  // adaptive batch
  function pumpOverview(nowMs) {
    if (done || inFlight || !overview || !pointHandle) return;  // wait for the handle
    const K = overview.count | 0;
    if (itemCursor >= K) { done = true; return; }

    // Real-point cap: once maxPoints have streamed in, STOP. `target` is already
    // min(total, maxPoints), and filament windows tile [0,total) in ascending
    // rangeStart order, so once we've filled [0,target) no later line adds real
    // points — halt now instead of spinning the skip-loop over beyond-cap items
    // every frame. Regions past the cap keep showing the line overview.
    if (loaded >= target) { done = true; return; }

    // Fetch budget: stop after `loadTurns` full rotations (0 = unlimited). A user
    // who never zooms then never pays to fetch the whole cloud.
    if (CONFIG.loadTurns > 0 && swept >= CONFIG.loadTurns * 2 * Math.PI) { done = true; return; }

    // How many items should be loaded by now (rotation-paced, + immediate seed).
    const wantFrac = Math.min(1, swept / (2 * Math.PI));
    const seedItems = Math.max(1, Math.ceil((CONFIG.seedPoints / Math.max(target, 1)) * K));
    const dueItems = Math.max(seedItems, Math.ceil(wantFrac * K));
    if (itemCursor >= dueItems) return;

    // Skip leading items whose window is out of the GPU buffer or empty.
    while (itemCursor < dueItems) {
      const rc0 = overview.rangeCount[itemCursor] >>> 0;
      const rs0 = overview.rangeStart[itemCursor] >>> 0;
      if (rc0 !== 0 && rs0 < target) break;
      itemCursor++;
    }
    if (itemCursor >= dueItems) return;

    // Coalesce a run of due items into one contiguous point window, bounded by
    // streamBatchSize. `iEnd` is the exclusive item index the run stops at.
    const runStart = overview.rangeStart[itemCursor] >>> 0;
    let iEnd = itemCursor;
    let windowEnd = runStart;
    while (iEnd < dueItems) {
      const rs = overview.rangeStart[iEnd] >>> 0;
      const rc = overview.rangeCount[iEnd] >>> 0;
      if (rs >= target) break;                       // beyond the buffer
      const end = Math.min(rs + rc, target);
      // stop if adding this item would overflow the (adaptive) batch (unless first)
      if (iEnd > itemCursor && (end - runStart) > curBatch) break;
      windowEnd = Math.max(windowEnd, end);
      iEnd++;
    }
    const start = runStart;
    const want = windowEnd - runStart;
    const firstItem = itemCursor, lastItem = iEnd;   // [firstItem, lastItem)
    if (want <= 0) { itemCursor = iEnd; return; }

    inFlight = true;
    Promise.resolve(src.Miniset.pointsReadXYZ(pointHandle, start, want))
      .then((batch) => {
        const got = batch.count | 0;
        if (got > 0) {
          positions.set(batch.positions.subarray(0, got * 3), start * 3);
          if (!framedFromData && batch.radius > 0) {
            radius = batch.radius; framedFromData = true; frame();
          }
          const now = performance.now() / 1000;
          births.fill(now, start, start + got);
          posAttr.addUpdateRange(start * 3, got * 3); posAttr.needsUpdate = true;
          birthAttr.addUpdateRange(start, got); birthAttr.needsUpdate = true;
          const end = start + got;
          if (end > drawHigh) { drawHigh = end; geom.setDrawRange(0, drawHigh); }
          loaded += got;
          // Crossfade every item covered by this run.
          if (markArrived)
            for (let i = firstItem; i < lastItem; ++i) markArrived(i, now);
        }
        itemCursor = lastItem;
        if (itemCursor >= K) done = true;
        // Ramp the batch up so steady-state reads are fat (fewer amortized
        // round-trips); the small first reads already gave a quick first paint.
        curBatch = Math.min(CONFIG.streamBatchMax, Math.ceil(curBatch * CONFIG.streamBatchGrow));
      })
      .catch((err) => { done = true; console.warn("[Miniset hero] overview stream failed:", err); })
      .finally(() => { inFlight = false; });
  }

  const hasOverview = !!overview;

  // Resolve the background point-stream open. Until this lands, pumps no-op and
  // only the overview renders — so the cloud appears after ONE net open, not two.
  let realTotal = total;
  src.ready
    .then((r) => {
      pointHandle = r.handle;
      realTotal = r.total || realTotal;
      // If we had no overview to bound `target` (plain-stream fallback), the
      // buffers were sized to maxPoints, which still covers realTotal's stream.
    })
    .catch((err) => { console.warn("[Miniset hero] point stream open failed:", err); });

  let raf = 0;
  let lastFrameMs = 0;
  function tick(now) {
    const dt = lastFrameMs ? Math.min(0.05, (now - lastFrameMs) / 1000) : 0;  // clamp tab-switch jumps
    lastFrameMs = now;
    swept = CONFIG.autoRotate ? (now / 1000) * CONFIG.rotateSpeed : swept;
    // Spin about the body's pole (Z). `orient` leans this axis, so the globe
    // turns on a tilted, pole-up axis rather than with the poles on the side.
    spin.rotation.z = swept;

    // Hand-orbit: integrate the joystick deflection into yaw/pitch (velocity ×
    // dt), then set the userOrbit group's rotation (yaw about screen-up Y, pitch
    // about screen-right X). Pitch is clamped so you can't tumble past the poles.
    if (jx !== 0 || jy !== 0) {
      orbitYaw += jx * CONFIG.joystickYawSpeed * dt;
      orbitPitch += -jy * CONFIG.joystickPitchSpeed * dt;
      const lim = THREE.MathUtils.degToRad(CONFIG.joystickPitchLimit);
      orbitPitch = Math.max(-lim, Math.min(lim, orbitPitch));
    }
    userOrbit.rotation.set(orbitPitch, orbitYaw, 0, "YXZ");

    if (hasOverview) pumpOverview(now); else pumpBatch(now);
    material.uniforms.uTime.value = now / 1000;
    if (gmm) gmm.mat.uniforms.uTime.value = now / 1000;          // GMM fade-out
    if (lineOverlay) lineOverlay.mat.uniforms.uTime.value = now / 1000;  // line fade-out
    renderer.render(scene, camera);
    updateCounter(now);
    raf = requestAnimationFrame(tick);
  }
  raf = requestAnimationFrame(tick);

  // Expose the live handle so the levers can be tweaked from the console:
  //   window.msHero.CONFIG.zoom = 3; window.msHero.apply();
  window.msHero = {
    CONFIG,
    get loaded() { return loaded; },
    get total() { return realTotal; },
    apply() {
      material.uniforms.uSize.value = CONFIG.dotSize;
      material.uniforms.uOpacity.value = CONFIG.dotOpacity;
      material.uniforms.uColor.value.set(CONFIG.dotColor);
      material.uniforms.uFogColor.value.set(CONFIG.fogColor);
      material.uniforms.uFogStrength.value = CONFIG.fogStrength;
      material.uniforms.uFogFalloff.value = CONFIG.fogFalloff;
      material.uniforms.uFade.value = CONFIG.fadeDurationMs / 1000;
      renderer.setClearAlpha(CONFIG.clearAlpha);
      if (gmm) {
        gmm.mat.uniforms.uColor.value.set(CONFIG.gmmColor);
        gmm.mat.uniforms.uOpacity.value = CONFIG.gmmOpacity;
        gmm.mat.uniforms.uFadeOut.value = CONFIG.gmmFadeOutMs / 1000;
        gmm.points.visible = CONFIG.showSplats;
      }
      if (lineOverlay) {
        lineOverlay.mat.uniforms.uColor.value.set(CONFIG.lineColor);
        lineOverlay.mat.uniforms.uOpacity.value = CONFIG.lineOpacity;
        lineOverlay.mat.uniforms.uFadeOut.value = CONFIG.lineFadeOutMs / 1000;
        lineOverlay.mat.uniforms.uFogColor.value.set(CONFIG.fogColor);
        lineOverlay.mat.uniforms.uFogStrength.value = CONFIG.fogStrength;
        lineOverlay.mat.uniforms.uFogFalloff.value = CONFIG.fogFalloff;
        lineOverlay.points.visible = CONFIG.showLines;
      }
      applyRenderEdge();
      applyOrientation();
      // Keep the slider in sync if zoom was changed from the console.
      zoomInput.value = String(Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, CONFIG.zoom)));
      joyWrap.style.display = CONFIG.joystick ? "" : "none";
      frame();
    },
    dispose() {
      cancelAnimationFrame(raf);
      ro.disconnect();
      counterEl.remove();
      captionEl.remove();
      zoomInput.removeEventListener("input", onZoom);
      zoomWrap.remove();
      joyWrap.removeEventListener("pointerdown", onJoyDown);
      joyWrap.removeEventListener("pointermove", onJoyMove);
      joyWrap.removeEventListener("pointerup", onJoyUp);
      joyWrap.removeEventListener("pointercancel", onJoyUp);
      joyWrap.remove();
      geom.dispose();
      material.dispose();
      if (gmm) { gmm.points.geometry.dispose(); gmm.mat.dispose(); }
      if (lineOverlay) { lineOverlay.points.geometry.dispose(); lineOverlay.mat.dispose(); }
      renderer.dispose();
      // Close the point handle if it (or its pending open) resolved.
      if (pointHandle) { try { src.Miniset.closePointsXYZ(pointHandle); } catch (_) {} }
      else { src.ready.then((r) => { try { src.Miniset.closePointsXYZ(r.handle); } catch (_) {} }).catch(() => {}); }
    },
  };
}

// The canvas node we've initialized (or are mid-initializing). Material's instant
// navigation re-emits document$ — and thus re-runs boot() — on every navigation,
// INCLUDING in-page anchor clicks like the "Try the playground below" (#playground)
// button. Those don't replace the DOM, so the hero canvas is the SAME node; we must
// NOT tear the live render down and rebuild (that's what blanked it and reset the
// stream to 0). We only rebuild when the canvas is a genuinely new element (a real
// page content swap). Claimed synchronously below so a re-fire during the initial
// multi-second openStream() can't start a second stream.
let heroCanvas = null;

async function boot() {
  const host = document.querySelector("[data-ms-hero]");
  const canvas = host && host.querySelector("[data-ms-hero-canvas]");

  // Navigated to a page with no hero: tear down any live render so it doesn't keep
  // rendering to a now-detached canvas (and holding the point handle open).
  if (!host || !canvas) {
    if (window.msHero) { window.msHero.dispose(); window.msHero = null; }
    heroCanvas = null;
    return;
  }

  // Same canvas we already own (or are initializing) → leave the running render
  // alone. This is the fix: instant-nav / same-page clicks no longer blank it.
  if (heroCanvas === canvas) return;

  // A genuinely new canvas (real content swap): dispose the old render first.
  if (window.msHero) { window.msHero.dispose(); window.msHero = null; }
  heroCanvas = canvas;   // claim BEFORE the await so a re-entrant boot() no-ops

  try {
    const src = await openStream();
    if (heroCanvas !== canvas) return;   // superseded while awaiting — abandon
    initScene(canvas, host, src);
    host.setAttribute("data-ms-hero-ready", "true");
  } catch (err) {
    if (heroCanvas === canvas) heroCanvas = null;   // let a later emit retry
    // Non-fatal: the hero still shows its gradient + text without the render.
    console.warn("[Miniset hero] point render unavailable:", err);
  }
}

if (typeof document$ !== "undefined" && document$.subscribe) {
  document$.subscribe(boot);
} else if (document.readyState !== "loading") {
  boot();
} else {
  document.addEventListener("DOMContentLoaded", boot);
}
