# Building for WebAssembly

The docs site ships a prebuilt WASM module in
[`docs/docs/assets/wasm/`](https://github.com/DOI-USGS/miniset).
This page covers rebuilding it yourself and refreshing the copy the
[playground](playground.md) loads.

## Prerequisites

- [Emscripten](https://emscripten.org/) 3.1+
- CMake 3.15+
- The submodules (PROJ + GDAL are built from source)

```bash
git submodule update --init --recursive
```

## Build

```bash
conda activate miniset          # environment that includes emscripten

mkdir build-wasm && cd build-wasm
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
emmake make miniset_wasm -j$(nproc)
```

- **First build:** 10–20 minutes (compiles PROJ + GDAL).
- **Subsequent builds:** ~30 seconds.
- **Output:** `miniset.js` (~216 KB) + `miniset.wasm` (~15 MB, ~4.9 MB gzipped).

## Refresh the docs playground

The playground loads its module from `docs/docs/assets/wasm/`. After a rebuild,
copy the fresh artifacts over:

```bash
cp build-wasm/miniset.js   docs/docs/assets/wasm/miniset.js
cp build-wasm/miniset.wasm docs/docs/assets/wasm/miniset.wasm
```

Then rebuild the docs (see below) and the playground picks them up.

## The linking gotcha

USGSCSM is built with PROJ bundled inside it. If the WASM target *also* links
PROJ separately you'll hit duplicate-symbol errors like:

```
wasm-ld: error: duplicate symbol: proj_context_delete_cpp_context(...)
```

The fix is to **not** link PROJ separately for the Emscripten target — PROJ
already arrives via `--whole-archive USGSCSM::usgscsm`. See the notes in the
repo's `WASM_LINKING_ISSUE.md` for the exact CMake change.

## Building the docs site

The documentation itself is a [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/)
site living in `docs/`.

```bash
pip install mkdocs-material
cd docs

mkdocs serve      # live-reload preview at http://127.0.0.1:8000
mkdocs build      # static site → docs/site/
```

!!! note "Serving requires WASM MIME support"

    `mkdocs serve` sets the correct `application/wasm` content type, as do
    GitLab/GitHub Pages. If you deploy behind a custom server, make sure it
    serves `.wasm` files with `Content-Type: application/wasm` so the browser
    can stream-compile the module. The module uses **no** threads or
    `SharedArrayBuffer`, so no COOP/COEP headers are required.
