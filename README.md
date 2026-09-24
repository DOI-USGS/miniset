# Miniset

High-performance geospatial sensor modeling library with WebAssembly support for planetary science.

## Features

- **DEM Operations**: GeoTIFF elevation models with automatic spatial reference extraction
- **CSM Camera Models**: Image-to-ground transformations (C++ only)
- **WebAssembly**: Zero-config browser/Node.js support with GDAL and PROJ
- **Planetary Ready**: Mars, Moon, Earth coordinate systems work automatically

## Quick Start

### C++ Build

```bash
mamba env create -f environment.yaml -n miniset
mamba activate miniset

cd miniset
mkdir build && cd build
cmake ..
make -j$(nproc)
ctest
```

### WebAssembly Build

```bash
conda activate miniset  # with emscripten

git submodule update --init --recursive
mkdir build-wasm && cd build-wasm
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
emmake make miniset_wasm -j$(nproc)

# Test
cd ../tests/wasm
./run_all_tests.js
```

## Usage

### C++ DEM Example

```cpp
#include "surface/dem.hpp"

// Load DEM (ellipsoid extracted automatically)
surface::GdalDEM dem("terrain.tif", surface::DEMType::HEIGHT);

// Query elevation (lat/lon in degrees)
double height = dem.getHeight(4.5, 137.4);

std::cout << "Height: " << height << " m\n";
std::cout << "Ellipsoid: " << dem.getSemiMajorA() << " m\n";
```

### C++ Camera Example

```cpp
#include "csm/campt.hpp"

auto model = csm::createCsmFromISD("image.json");
csm::CamptResult result = csm::campt(model.get(), 2500, 1000, 0);

std::cout << "Lat: " << result.latitude << "°\n";
std::cout << "Lon: " << result.longitude << "°\n";
```

### JavaScript/WASM DEM Example

```javascript
import MinisetFactory from './miniset.js';
import fs from 'fs';

const Module = await MinisetFactory();

// Load DEM
const data = fs.readFileSync('mars_dem.tif');
Module.FS.writeFile('/tmp/dem.tif', data);

const dem = new Module.GdalDEM('/tmp/dem.tif', Module.DEMType.HEIGHT);
const height = dem.getHeight(-4.0, 222.5);  // lat/lon in degrees

console.log('Height:', height, 'm');
console.log('Ellipsoid:', dem.getSemiMajorA(), 'm');  // 3,396,190 for Mars

dem.delete();
```

### Browser Example

```html
<script type="module">
import MinisetFactory from './miniset.js';

const Module = await MinisetFactory();

const response = await fetch('dem.tif');
const data = new Uint8Array(await response.arrayBuffer());
Module.FS.writeFile('/tmp/dem.tif', data);

const dem = new Module.GdalDEM('/tmp/dem.tif', Module.DEMType.HEIGHT);
const height = dem.getHeight(5.73, 11.46);  // lat/lon in degrees
console.log('Elevation:', height, 'm');
dem.delete();
</script>
```

### Cloud Optimized GeoTIFF

```javascript
// No download needed - access via HTTP
const dem = new Module.GdalDEM(
  '/vsicurl/https://example.com/data/mars_dem.tif',
  Module.DEMType.HEIGHT
);
const height = dem.getHeight(latDeg, lonDeg);  // lat/lon in degrees
dem.delete();
```

## API Reference

### C++ DEM API

```cpp
surface::GdalDEM(filename, dem_type)
  // dem_type: DEMType::HEIGHT or DEMType::RADIUS
  // Ellipsoid auto-extracted from GeoTIFF

dem.getHeight(lat_deg, lon_deg)    // Height above ellipsoid (m) - lat/lon in degrees
dem.getRadius(lat_deg, lon_deg)    // Radius from center (m) - lat/lon in degrees
dem.getSemiMajorA()                // Ellipsoid semi-major axis (m)
dem.getSemiMajorB()                // Ellipsoid semi-major axis (m)
dem.getSemiMinorC()                // Ellipsoid semi-minor axis (m)
```

### JavaScript DEM API

```javascript
const dem = new Module.GdalDEM(filepath, Module.DEMType.HEIGHT)
  // Ellipsoid auto-extracted from GeoTIFF
  // Supports /vsicurl/ paths for remote COGs

dem.getHeight(lat_deg, lon_deg)    // Height above ellipsoid (m) - lat/lon in degrees
dem.getRadius(lat_deg, lon_deg)    // Radius from center (m) - lat/lon in degrees
dem.getSemiMajorA()                // Ellipsoid semi-major axis (m)
dem.getSemiMajorB()                // Ellipsoid semi-major axis (m)
dem.getSemiMinorC()                // Ellipsoid semi-minor axis (m)
dem.delete()                       // Free memory (required)
```

**Note:** All lat/lon inputs are in degrees. Internal conversions to radians happen automatically.

### C++ Camera API

```cpp
csm::createCsmFromISD(filename)           // Load CSM model
csm::campt(model, sample, line, height)   // Camera point
csm::imageToGround(model, line, sample, height)  // Image→ground
csm::groundToImage(model, x, y, z)        // Ground→image
```

**Note:** CSM functions not exposed in WASM. Uncomment lines 463-476 in `src/wasm/miniset_bindings.cpp` to enable.

## Testing

```bash
# C++ tests
cd build
ctest --output-on-failure

# WASM tests
cd tests/wasm
./run_all_tests.js

# Individual WASM tests
node test_api.js     # API structure
node test_dem.js     # DEM operations
node test_csm.js     # CSM (currently skipped)
```

See [tests/wasm/README_TESTS.md](tests/wasm/README_TESTS.md) for details.

## Dependencies

**Required:**
- C++17 compiler
- CMake 3.15+
- CSMAPI, USGSCSM

**Optional:**
- GDAL 3.8+ (DEM support)
- PROJ 9.4+ (coordinate transforms)
- OpenMP (parallelization)

**WASM:**
- Emscripten 3.1+
- PROJ and GDAL built automatically from submodules

## Build Details

**Native:**
```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**WASM:**
```bash
# First build: 10-20 min (builds PROJ + GDAL)
# Subsequent: ~30 sec
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
emmake make miniset_wasm -j$(nproc)

# Output: miniset.js (162 KB) + miniset.wasm (15 MB / 4.9 MB gzipped)
```

## References

- [GDAL](https://gdal.org/) - Geospatial Data Abstraction Library
- [PROJ](https://proj.org/) - Coordinate transformation
- [CSM API](https://github.com/sminster/csm) - Community Sensor Model
- [USGSCSM](https://github.com/DOI-USGS/usgscsm) - USGS CSM implementation

## Contributing

Bug reports, feature requests, and pull requests are welcome. See
[CONTRIBUTING.md](CONTRIBUTING.md) for how to report issues, set up a
development build, and submit changes.

## Contact / Maintainer

**Kelvin Rodriguez** — <krodriguez@usgs.gov>
U.S. Geological Survey, Astrogeology Science Center

- Issues and feature requests: <https://github.com/DOI-USGS/miniset/issues>
- Documentation: <https://DOI-USGS.github.io/miniset/>

For suspected security vulnerabilities, please email the maintainer directly
rather than opening a public issue.

## License

This software is released into the public domain under
[CC0 1.0 Universal](LICENSE.md). See [DISCLAIMER.md](DISCLAIMER.md) for the
USGS provisional software disclaimer.

---

## Generative AI Disclosure

This repository contains software code that was generated or modified with the
assistance of artificial intelligence (AI) tools, in accordance with U.S.
Geological Survey (USGS) disclosure requirements. All AI-generated or AI-assisted
code has been reviewed and validated by USGS developers. Contributors using AI tools are responsible for
reviewing and testing their code. 
