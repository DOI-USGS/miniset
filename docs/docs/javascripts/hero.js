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
  offsetX: 0.25,       // fraction of viewport to shove the globe RIGHT
                       //   (0 = centered, 0.4–0.5 packs it into the right 2/3)
  offsetY: 0.0,        // vertical nudge (fraction of viewport; + is up)
  autoRotate: true,    // slow spin so the globe reads as 3D
  rotateSpeed: 0.08,  // radians / second — also sets the load rate (see below)

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
  maxPoints: 10000000,     // cap on how many points to stream onto the GPU total
                           //   (the source file holds ~9.4M; this bounds the render)
  streamBatchSize: 60000,   // max points pulled from the net per read (short reads)
  fadeDurationMs: 1600,    // how long each point takes to ramp to full opacity
  seedPoints: 50000,       // load this many immediately at startup so the front
                           //   is populated at once (no waiting for the first read)
  loadLeadTurns: 0.0,      // load this fraction of a turn AHEAD of the front.
                           //   0 = new points appear dead-front (on camera); a
                           //   positive lead spawns them off to the side, where
                           //   the right-shoved globe can clip them off-screen.

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
  "/vsicurl/https://asc-isisdata.s3.us-west-2.amazonaws.com/cnf_test_data/largenet.stards";

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
  // These calls hit the network (StarDS ranged fetch) and therefore SUSPEND: the
  // module is built with -sASYNCIFY, so a suspending Embind call returns a Promise
  // that must be awaited. openPointsXYZ pulls the header/index; pointsCount reads
  // from that index (no fetch, but await is harmless).
  const handle = await Miniset.openPointsXYZ(STARDS_URL);
  const total = await Miniset.pointsCount(handle);
  return { Miniset, handle, total };
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
 * Initialize the Three.js scene inside `canvas`, sized to `host`, and stream the
 * point cloud in from `src` over time.
 * @param {HTMLCanvasElement} canvas
 * @param {HTMLElement} host
 * @param {{Miniset: any, handle: number, total: number}} src
 */
function initScene(canvas, host, src) {
  const renderer = new THREE.WebGLRenderer({ canvas, alpha: true, antialias: true });
  const pixelRatio = Math.min(window.devicePixelRatio || 1, 2);
  renderer.setPixelRatio(pixelRatio);
  renderer.setClearAlpha(CONFIG.clearAlpha);

  const scene = new THREE.Scene();

  // How many points we will ultimately stream in.
  const target = Math.min(src.total, CONFIG.maxPoints);

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
  scene.add(orient);

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
    // radius-D (front)]; the shader ramps opacity between these.
    const D = camera.position.z;
    material.uniforms.uNearZ.value = radius - D;
    material.uniforms.uFarZ.value = -radius - D;
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
    const rateStr = loaded >= target || rate < 1 ? "" : ` · ${numFmt.format(Math.round(rate))}/s`;
    const text = `${numFmt.format(loaded)} points${rateStr}`;
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
    if (done || inFlight || loaded >= target) { if (loaded >= target) done = true; return; }

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
    Promise.resolve(src.Miniset.pointsReadXYZ(src.handle, start, want))
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

  let raf = 0;
  function tick(now) {
    swept = CONFIG.autoRotate ? (now / 1000) * CONFIG.rotateSpeed : swept;
    // Spin about the body's pole (Z). `orient` leans this axis, so the globe
    // turns on a tilted, pole-up axis rather than with the poles on the side.
    spin.rotation.z = swept;
    pumpBatch(now);
    material.uniforms.uTime.value = now / 1000;
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
    get total() { return src.total; },
    apply() {
      material.uniforms.uSize.value = CONFIG.dotSize;
      material.uniforms.uOpacity.value = CONFIG.dotOpacity;
      material.uniforms.uColor.value.set(CONFIG.dotColor);
      material.uniforms.uFogColor.value.set(CONFIG.fogColor);
      material.uniforms.uFogStrength.value = CONFIG.fogStrength;
      material.uniforms.uFogFalloff.value = CONFIG.fogFalloff;
      material.uniforms.uFade.value = CONFIG.fadeDurationMs / 1000;
      renderer.setClearAlpha(CONFIG.clearAlpha);
      applyRenderEdge();
      applyOrientation();
      frame();
    },
    dispose() {
      cancelAnimationFrame(raf);
      ro.disconnect();
      counterEl.remove();
      geom.dispose();
      material.dispose();
      renderer.dispose();
      try { src.Miniset.closePointsXYZ(src.handle); } catch (_) {}
    },
  };
}

async function boot() {
  const host = document.querySelector("[data-ms-hero]");
  const canvas = host && host.querySelector("[data-ms-hero-canvas]");
  if (!host || !canvas) return;

  // Guard against double-init across Material's instant navigation.
  if (window.msHero) { window.msHero.dispose(); window.msHero = null; }

  try {
    const src = await openStream();
    initScene(canvas, host, src);
    host.setAttribute("data-ms-hero-ready", "true");
  } catch (err) {
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
