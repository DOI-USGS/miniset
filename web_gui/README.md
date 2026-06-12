# Miniset Web GUI

Interactive browser-based GUI for Miniset

## Quick Start

### Step 1: Run local instance of CartoCosmos

Must be running a local instance of CartoCosmos [PR#18](https://code.chs.usgs.gov/asc/CartoCosmos/-/merge_requests/18) to be compatible with Miniset.

```bash
# In CartoCosmos repo
python -m http.server 8000
```

**Note:** CartoCosmos **must** run on port 8000 (hardcoded in `index.html`).

### Step 2: Start Miniset Web GUI Server

```bash
cd web_gui/public
python -m http.server 8002
```

### Step 3: (Optional) Start Proxy Server

For loading GeoTIFFs/ISDs from external URLs:

```bash
cd web_gui
python proxy_server.py
# Runs on port 8001
```

### Step 4: Open in Browser

Navigate to: **http://localhost:8002**

## Usage

### Basic Workflow

1. **Load GeoTIFF**
   - Click file input under "COG (Cloud-Optimized GeoTIFF)"
   - Select a planetary GeoTIFF file OR enter a URL

2. **Load ISD**
   - Click file input under "ISD (Image Support Data)"
   - Select corresponding `.json` ISD file OR enter a URL

3. **Generate**
   - Click the "Generate" button
   - Watch status updates in the sidebar
   - Footprint and raster overlay appear on map

4. **Interact**
   - **Toggle layers** - Show/hide footprint or raster
   - **Adjust opacity** - Slider for raster transparency
   - **Zoom** - Fit all layers in view
   - **Switch bodies** - Dropdown auto-switches based on ISD

### Supported Formats

- **GeoTIFF:** Uncompressed or LZW/Deflate (compression may fail due to WASM limitations)
- **ISD:** JSON format with CSM camera model parameters
- **Bodies:** Mars, Moon, Mercury, Venus, and other planetary bodies

## Project Structure

```
web_gui/
├── README.md               ← This file
│
├── public/                 ← Web application (deploy this folder)
│   ├── index.html          ← Main HTML page
│   │
│   ├── css/
│   │   └── theme-material-design.css    ← Material Design 3 styles
│   │
│   └── js/
│       ├── app.js                       ← Main application logic & initialization
│       ├── geospatial.js                ← GeoTIFF projection, footprint calculation
│       ├── cartocosmos-bridge.js        ← Map communication (postMessage API)
│       ├── file-handlers.js             ← File/URL input handling
│       ├── ui-state.js                  ← UI state & button management
│       ├── validation.js                ← Pre-flight validation
│       ├── wasm-loader.js               ← WASM module loading
│       ├── miniset_v8.js + .wasm        ← Miniset WASM module
│       └── usgscsm_wasm.js + .wasm      ← USGSCSM WASM module
│
├── proxy_server.py         ← CORS proxy for URL loading
└── src/                    ← Source files for WASM build
```

## Known Limitations & Workarounds

### 1. **Compression Support**
**Issue:** Compressed GeoTIFFs may fail (zlib not in WASM)  
**Workaround:** Convert to uncompressed:
```bash
gdal_translate -co COMPRESS=NONE input.tif output.tif
```

### 2. **Body Radii Bug**
**Issue:** `camproject` ignores body radii for non-Mars bodies  
**Workaround:** Implemented - forces Mars projection system  
**Status:** Do not hardcode Mars radii values, fix pending

### 3. **CSM State Strings**
**Issue:** Large embedded CSM states (>13MB) cause out-of-memory  
**Workaround:** Use sidecar ISD JSON file instead  
**Status:** Requires WASM rebuild with increased heap size
