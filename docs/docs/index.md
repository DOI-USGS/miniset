---
title: Miniset
hide:
  - navigation
  - toc
---

<div class="ms-hero" data-ms-hero markdown>

<canvas class="ms-hero__canvas" data-ms-hero-canvas></canvas>

<div class="ms-hero__content" markdown>

<span class="ms-hero__eyebrow">U.S. Geological Survey · Astrogeology</span>

# Planetary sensor modeling that runs anywhere { .ms-hero__title }


[Try the playground below :material-arrow-down:](#playground){ .md-button .md-button--primary }
[Get started](getting-started.md){ .md-button }

</div>

</div>

<div class="ms-cards" markdown>

<div class="ms-card" markdown>
:material-terrain:{ .ms-card__icon }

### DEM operations

Query elevation and planetary radius straight from GeoTIFF DEMs. The ellipsoid
is extracted from the file's spatial reference **automatically** — no manual
setup per body.
</div>

<div class="ms-card" markdown>
:material-camera-iris:{ .ms-card__icon }

### CSM camera models

Image-to-ground and ground-to-image transforms built on the Community Sensor
Model and USGSCSM — the same models the planetary science community relies on.
</div>

<div class="ms-card" markdown>
:material-language-javascript:{ .ms-card__icon }

### WebAssembly, zero config

One `import` and you have GDAL + PROJ in the browser or Node.js. No server, no
install. Try it live in the [playground](#playground) — it's real WASM running
on this page.
</div>

<div class="ms-card" markdown>
:material-earth:{ .ms-card__icon }

### Planetary ready

Mars, Moon, and Earth coordinate systems work out of the box. Feed it a body's
DEM and the correct triaxial ellipsoid comes along for the ride.
</div>

<div class="ms-card" markdown>
:material-flash:{ .ms-card__icon }

### High performance

A modern C++17 core with optional OpenMP parallelization. Compiles to fast
native binaries and a compact WASM module for the web.
</div>

<div class="ms-card" markdown>
:material-earth-remove:{ .ms-card__icon }

### Cloud-native data

Stream Cloud Optimized GeoTIFFs directly over HTTP with `/vsicurl/` — query
terabyte-scale planetary DEMs without downloading a byte more than you need.
</div>

</div>

## Run Miniset in your browser { #playground }

The window below loads the real Miniset WebAssembly module — the very same one
you'd `import` in your own project — and runs your JavaScript against it. Edit
the code and hit **Run**.

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
// The Miniset module is already loaded and passed in as `Miniset`.
// `log(...)` prints to the output panel below.

// Mars' triaxial ellipsoid (semi-major, semi-minor, in metres).
const dem = new Miniset.EllipsoidDEM(3396190, 3376200);

log("Mars ellipsoid — height & radius by latitude");
log("(EllipsoidDEM needs no GeoTIFF: it's pure geometry)\n");

for (const lat of [0, 30, 60, 90]) {
  const radius = dem.getRadius(lat, 0);   // lat/lon in degrees
  const height = dem.getHeight(lat, 0);
  log(`lat ${String(lat).padStart(3)}°  radius ${radius.toFixed(1)} m  height ${height} m`);
}

log(`\nSemi-major A: ${dem.getSemiMajorA()} m`);
log(`Semi-minor C: ${dem.getSemiMinorC()} m`);

// Always free WASM-owned objects when you're done.
dem.delete();
```

<div class="ms-playground__output" markdown>
<pre data-ms-output class="ms-playground__pre"><code>Ready — press Run.</code></pre>
</div>

</div>

!!! tip "Want to load a real DEM?"

    `EllipsoidDEM` is pure geometry and needs no data file. To query a real
    GeoTIFF, write it into the WASM filesystem and use `GdalDEM`:

    ```javascript
    const bytes = new Uint8Array(await (await fetch("mars_dem.tif")).arrayBuffer());
    Miniset.FS.writeFile("/tmp/dem.tif", bytes);
    const dem = new Miniset.GdalDEM("/tmp/dem.tif", Miniset.DEMType.HEIGHT);
    log(dem.getHeight(-4.0, 222.5));   // lat/lon in degrees
    dem.delete();
    ```

    See the [API reference](api-reference.md) for the full surface.

---

<small>
This software is preliminary or provisional and is subject to revision. It is
provided to meet the need for timely best science and has not received final
approval by the U.S. Geological Survey (USGS). No warranty, expressed or
implied, is made by the USGS or the U.S. Government as to its functionality.
See the full [disclaimer](https://github.com/DOI-USGS/miniset/raw/main/DISCLAIMER.md).
</small>
