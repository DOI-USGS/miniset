# API Reference

All latitude/longitude inputs are in **degrees**; internal conversions to
radians happen automatically. All lengths are in **metres**.

## JavaScript / WASM

The module factory returns an Emscripten `Module`. Everything below hangs off
that object (shown here as `Miniset`).

### `Miniset.DEMType`

Enum selecting what a DEM's samples represent.

| Value                  | Meaning                          |
| ---------------------- | -------------------------------- |
| `Miniset.DEMType.HEIGHT` | Height above the ellipsoid (m) |
| `Miniset.DEMType.RADIUS` | Radius from body centre (m)    |

### `Miniset.EllipsoidDEM`

Analytic ellipsoid — pure geometry, no data file. Ideal for quick radius/height
math and for the [playground](playground.md).

```javascript
new Miniset.EllipsoidDEM(semiMajor, semiMinor)
```

| Method                       | Returns  | Description                                         |
| ---------------------------- | -------- | --------------------------------------------------- |
| `getHeight(latDeg, lonDeg)`  | `number` | Height above the ellipsoid (0 for a bare ellipsoid) |
| `getRadius(latDeg, lonDeg)`  | `number` | Radius from the body centre (m)                     |
| `getSemiMajorA()`            | `number` | Semi-major axis A (m)                               |
| `getSemiMajorB()`            | `number` | Semi-major axis B (m)                               |
| `getSemiMinorC()`            | `number` | Semi-minor axis C (m)                               |
| `delete()`                   | `void`   | Free the underlying WASM memory                     |

### `Miniset.GdalDEM`

A GeoTIFF-backed DEM. Extends `EllipsoidDEM`, so it has all of the methods
above **plus** the ones below. The ellipsoid is read from the file's spatial
reference automatically.

```javascript
new Miniset.GdalDEM(filepath)                       // defaults to HEIGHT
new Miniset.GdalDEM(filepath, Miniset.DEMType.HEIGHT)
```

`filepath` is a path inside the WASM filesystem (write the bytes with
`Miniset.FS.writeFile`), or a `/vsicurl/https://…` URL for a remote Cloud
Optimized GeoTIFF.

| Method                    | Returns  | Description                               |
| ------------------------- | -------- | ----------------------------------------- |
| `getRasterValue(lat, lon)`| `number` | Raw raster sample at the given lat/lon    |
| `setProj(wkt)`            | `void`   | Override the spatial reference (WKT)      |
| *(inherited)*             |          | `getHeight`, `getRadius`, `getSemiMajorA`, `getSemiMajorB`, `getSemiMinorC`, `delete` |

### Filesystem helpers

Emscripten's virtual filesystem is exposed as `Miniset.FS`:

```javascript
Miniset.FS.writeFile("/tmp/dem.tif", uint8Array);
Miniset.FS.readFile("/tmp/out.bin");
```

!!! note "CSM in WASM"

    CSM camera-model functions are **not** exposed in the WASM build by
    default. They're available in the native C++ library (below). To enable
    them in WASM, uncomment the bindings in `wasm/src/miniset_bindings.cpp` and
    rebuild.

## C++

### DEM

```cpp
#include "surface/dem.hpp"

surface::GdalDEM dem(filename, surface::DEMType::HEIGHT);
double h = dem.getHeight(lat_deg, lon_deg);   // height above ellipsoid (m)
double r = dem.getRadius(lat_deg, lon_deg);   // radius from centre (m)
double a = dem.getSemiMajorA();               // ellipsoid axes (m)
```

| Method                             | Description                          |
| ---------------------------------- | ------------------------------------ |
| `getHeight(lat_deg, lon_deg)`      | Height above the ellipsoid (m)       |
| `getRadius(lat_deg, lon_deg)`      | Radius from the body centre (m)      |
| `getSemiMajorA()` / `B()` / `getSemiMinorC()` | Ellipsoid radii (m)       |

### Camera (CSM)

```cpp
#include "csm/campt.hpp"

auto model = csm::createCsmFromISD("image.json");
csm::CamptResult r = csm::campt(model.get(), sample, line, height);
```

| Function                                     | Description        |
| -------------------------------------------- | ------------------ |
| `csm::createCsmFromISD(filename)`            | Load a CSM model   |
| `csm::campt(model, sample, line, height)`    | Camera point       |
| `csm::imageToGround(model, line, sample, h)` | Image → ground     |
| `csm::groundToImage(model, x, y, z)`         | Ground → image     |
