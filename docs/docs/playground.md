---
title: Playground
---

# Playground

This page runs the real Miniset WebAssembly module — the same `miniset.js` +
`miniset.wasm` you'd ship in your own project. Everything executes locally in
your browser; nothing is sent to a server.

**How it works**

- The module is already loaded and handed to your snippet as `Miniset`.
- Call `log(...)` to print to the output panel (it accepts numbers and objects too).
- `await` works — the code runs inside an async function.
- Press **Run** (or ++ctrl+enter++ / ++cmd+enter++) to execute; **Reset** restores the sample.
- Remember to `.delete()` any WASM objects you create.

<div class="ms-playground" data-ms-playground markdown>

<div class="ms-playground__toolbar">
  <span class="ms-playground__status" data-ms-status data-state="loading">Loading WASM…</span>
  <div class="ms-playground__buttons">
    <button type="button" class="md-button ms-playground__btn" data-ms-run disabled>
      &#9654;&nbsp;Run
    </button>
    <button type="button" class="md-button ms-playground__btn ms-playground__btn--ghost" data-ms-reset>
      Reset
    </button>
  </div>
</div>

```javascript
// Compare the mean planetary radius of Mars, the Moon, and Earth using
// EllipsoidDEM (pure geometry — no data file required).
const bodies = [
  { name: "Mars",  a: 3396190, c: 3376200 },
  { name: "Moon",  a: 1737400, c: 1737400 },
  { name: "Earth", a: 6378137, c: 6356752 },
];

log("Radius from body centre, in metres:\n");
log("body    equator (0°)      pole (90°)");
log("-----   ---------------   ---------------");

for (const b of bodies) {
  const dem = new Miniset.EllipsoidDEM(b.a, b.c);
  const eq = dem.getRadius(0, 0).toFixed(1);
  const pole = dem.getRadius(90, 0).toFixed(1);
  log(`${b.name.padEnd(6)}  ${eq.padStart(15)}   ${pole.padStart(15)}`);
  dem.delete();
}
```

<div class="ms-playground__output" markdown>
<pre data-ms-output class="ms-playground__pre"><code>Ready — press Run.</code></pre>
</div>

</div>

## Things to try

- Swap in your own body: `new Miniset.EllipsoidDEM(semiMajor, semiMinor)`.
- Inspect the enum: `log(Object.keys(Miniset.DEMType))`.
- Load a Cloud Optimized GeoTIFF over HTTP:

    ```javascript
    const dem = new Miniset.GdalDEM(
      "/vsicurl/https://example.com/mars_dem.tif",
      Miniset.DEMType.HEIGHT
    );
    log(dem.getHeight(-4.0, 222.5));
    dem.delete();
    ```

    (Remote fetches need the host to allow cross-origin requests.)

See the [API Reference](api-reference.md) for every class and method the module
exposes.
