# Miniset Web GUI

Interactive browser-based GUI for Miniset.

> **Local development only.** This GUI and its companion CORS proxy are
> developer tools. Neither is hardened for deployment — see
> [Development proxy](#development-proxy) below.

## Quick Start

### Step 1: Run local instance of CartoCosmos

Must be running a local instance of CartoCosmos [PR#18](https://code.chs.usgs.gov/asc/CartoCosmos/-/merge_requests/18) to be compatible with Miniset.

```bash
# In CartoCosmos repo
python -m http.server 8000
```

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
# Runs on 127.0.0.1:8001
```

### Step 4: Open in Browser

Navigate to: **http://localhost:8002**

## Configuration

Service endpoints live in [`public/js/config.js`](public/js/config.js) — they are
not hardcoded at the call sites or in `index.html`. Defaults:

| Setting | Default | Purpose |
| --- | --- | --- |
| `cartoCosmosBaseUrl` | `http://localhost:8000` | CartoCosmos basemap iframe |
| `proxyBaseUrl` | `http://127.0.0.1:8001` | CORS proxy |
| `defaultTarget` | `MARS` | Body the map opens on |

Override without editing files, either with a query parameter:

```
http://localhost:8002/?cartocosmos=http://localhost:9000&proxy=http://127.0.0.1:9001&target=MOON
```

or by setting `window.MINISET_CONFIG` before `app.js` loads:

```html
<script>window.MINISET_CONFIG = { cartoCosmosBaseUrl: 'http://localhost:9000' };</script>
```

## Development proxy

`proxy_server.py` fetches URLs on the browser's behalf so the GUI can read
remote data that lacks CORS headers. Because it makes outbound requests on
request, it is a server-side request forgery (SSRF) shaped tool by construction
and **must not be deployed or exposed beyond loopback.**

It is hardened for local use:

- binds to `127.0.0.1` only, never all interfaces;
- allows only the `http` and `https` schemes;
- allows only hosts matching an allowlist (default: `.usgs.gov`,
  `.amazonaws.com`, `.nasa.gov`);
- **always** refuses hosts resolving to link-local addresses (this blocks cloud
  instance-metadata endpoints such as `169.254.169.254`), loopback, multicast,
  and reserved ranges — even for allowlisted hosts whose DNS is subverted;
- permits RFC1918 private addresses only for explicitly allowlisted hosts, since
  USGS internal DNS resolves `usgs.gov` names to private addresses on-network;
- re-validates the target on every redirect hop, so an allowlisted host cannot
  redirect the proxy inward;
- reflects a CORS origin only for localhost pages instead of sending `*`;
- caps the relayed body size and applies a 30 s timeout.

Configure with environment variables:

| Variable | Default | Effect |
| --- | --- | --- |
| `MINISET_PROXY_PORT` | `8001` | Listen port |
| `MINISET_PROXY_ALLOWED_HOSTS` | see above | Comma-separated allowlist, replacing the default. A leading `.` matches the domain and its subdomains. |
| `MINISET_PROXY_MAX_BYTES` | 1 GiB | Maximum relayed body size |
| `MINISET_PROXY_ALLOW_ANY_HOST` | unset | `1` bypasses the host allowlist. Private addresses are then refused too. For short local experiments only. |

```bash
# Allow an additional data host for one session
MINISET_PROXY_ALLOWED_HOSTS=".usgs.gov,.amazonaws.com,data.example.org" python proxy_server.py
```

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
│       ├── config.js                    ← Service endpoints (single source of truth)
│       ├── geospatial.js                ← GeoTIFF projection, footprint calculation
│       ├── cartocosmos-bridge.js        ← Map communication (postMessage API)
│       ├── file-handlers.js             ← File/URL input handling
│       ├── ui-state.js                  ← UI state & button management
│       ├── validation.js                ← Pre-flight validation
│       ├── wasm-loader.js               ← WASM module loading
│       ├── miniset_v8.js + .wasm        ← Miniset WASM module
│       └── usgscsm_wasm.js + .wasm      ← USGSCSM WASM module
│
├── proxy_server.py         ← CORS proxy for URL loading (dev only, see above)
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
