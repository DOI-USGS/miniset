# Miniset WASM Build

Complete replication of gdal3.js approach with latest GDAL.

## Architecture

**Key Innovation**: Spatial reference extraction happens in **JavaScript** using GDAL C functions via `cwrap`, avoiding C++ exception corruption in WASM.

- **C Functions** (GDAL/PROJ): Exported directly, called from JavaScript
- **C++ Classes** (Miniset): Exposed via Embind for core functionality
- **JavaScript Layer**: Integrates both, handles spatial reference extraction

## Versions

- GDAL: 3.13.0 (latest)
- PROJ: 9.5.1 (latest)
- SQLite: 3.46.1
- GEOS: 3.13.0
- TIFF: 4.7.0
- GeoTIFF: 1.7.3

## Build

```bash
cd wasm
make all
```

This will:
1. Download and build all dependencies (PROJ, GDAL, etc.) with Emscripten
2. Compile Miniset C++ code with Embind
3. Link everything into `build/dist/miniset_core.js` + `miniset_core.wasm`

## Usage

```javascript
import initMiniset from './build/dist/miniset.js';

const Miniset = await initMiniset();

// Load DEM file into WASM filesystem
const demData = await fetch('mars.tif').then(r => r.arrayBuffer());
Miniset.FS.writeFile('/tmp/mars.tif', new Uint8Array(demData));

// Create DEM with automatic spatial reference extraction
const dem = Miniset.createDEM('/tmp/mars.tif', Miniset.DEMType.HEIGHT);

// Ellipsoid is automatically extracted from DEM metadata
console.log('Semi-major:', dem.getSemiMajorA()); // Mars: 3,396,190 m
console.log('Semi-minor:', dem.getSemiMinorC()); // Mars: 3,396,190 m

// Query elevation
const height = dem.getHeight(0.1, 0.2); // lat, lon in degrees
console.log('Height:', height);
```

## How It Works

### 1. GDAL C Functions (JavaScript → WASM)

```javascript
// In gdal_functions.js
const GDALGetProjectionRef = Module.cwrap('GDALGetProjectionRef', 'string', ['number']);
const wkt = GDALGetProjectionRef(datasetPtr); // ✅ Works! No C++ exceptions
```

### 2. Ellipsoid Extraction (JavaScript)

```javascript
// Extract WKT using GDAL C API
const wkt = extractSpatialReference('/tmp/mars.tif');

// Parse ellipsoid from WKT using OSR C API
const ellipsoid = parseEllipsoidFromWKT(wkt);
// { semiMajor: 3396190, semiMinor: 3396190 }
```

### 3. Set on C++ Object

```javascript
const dem = new Module.GdalDEM('/tmp/mars.tif'); // Defaults to WGS84
dem.setProj(wkt); // Updates to Mars ellipsoid
```

## Why This Works

**Problem**: `GDALGetProjectionRef()` crashes when called from C++ in WASM due to internal C++ exception handling.

**Solution**: Call it from JavaScript using `cwrap`. The function executes entirely in WASM, returns a string pointer to JavaScript, and JavaScript reads it safely.

```
❌ C++ → GDALGetProjectionRef() → Exception → Crash
✅ JavaScript → cwrap('GDALGetProjectionRef') → String → Success
```

## File Structure

```
wasm/
├── Makefile              # Build system (downloads & compiles dependencies)
├── src/
│   ├── miniset_bindings.cpp   # Embind bindings for C++ classes
│   ├── gdal_functions.js      # GDAL C function wrappers
│   └── miniset.js             # Main module integrating everything
└── build/
    ├── src/               # Downloaded source tarballs
    ├── install/           # Built libraries & headers
    └── dist/              # Final WASM output
```

## Differences from gdal3.js

1. **Latest GDAL**: 3.13.0 vs 3.8.4
2. **Minimal Dependencies**: Only PROJ, GDAL, SQLite, TIFF, GeoTIFF (no GEOS, JPEG, etc.)
3. **Miniset Integration**: Adds Miniset C++ classes via Embind
4. **Simplified**: Focused on DEM + spatial reference support

## Testing

```bash
cd wasm
node test.js
```

Expected output:
```
✅ Mars DEM loaded
✅ Spatial reference extracted
✅ Ellipsoid: a=3396190, b=3396190
✅ All tests passed
```

## Cleaning

```bash
make clean        # Remove everything
make clean-dist   # Remove only final output (keep dependencies)
```

## Credits

Build approach based on [gdal3.js](https://github.com/bugra9/gdal3.js) by Uğur Aydın.
