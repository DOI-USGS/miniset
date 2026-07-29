# Getting Started

Miniset ships two ways to run: a **native C++ library** and a **WebAssembly
module** for the browser and Node.js. Pick whichever fits your project.

## C++

### Build

```bash
# Create the conda/mamba environment (compilers + GDAL/PROJ/CSM/USGSCSM).
mamba env create -f environment.yaml -n miniset
mamba activate miniset

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure
```

### Query a DEM

```cpp
#include "surface/dem.hpp"

// The ellipsoid is extracted from the GeoTIFF's spatial reference automatically.
surface::GdalDEM dem("terrain.tif", surface::DEMType::HEIGHT);

double height = dem.getHeight(4.5, 137.4);   // lat/lon in degrees
std::cout << "Height: " << height << " m\n";
std::cout << "Ellipsoid A: " << dem.getSemiMajorA() << " m\n";
```

### Project a camera point

```cpp
#include "csm/campt.hpp"

auto model = csm::createCsmFromISD("image.json");
csm::CamptResult result = csm::campt(model.get(), 2500, 1000, 0);

std::cout << "Lat: " << result.latitude  << "°\n";
std::cout << "Lon: " << result.longitude << "°\n";
```

## JavaScript / WebAssembly

The WASM module is an ES module that bundles GDAL and PROJ. No build step is
needed to *use* it — just import the prebuilt `miniset.js` + `miniset.wasm`.

=== "Browser"

    ```html
    <script type="module">
      import MinisetFactory from "./miniset.js";

      const Miniset = await MinisetFactory();

      const response = await fetch("dem.tif");
      const bytes = new Uint8Array(await response.arrayBuffer());
      Miniset.FS.writeFile("/tmp/dem.tif", bytes);

      const dem = new Miniset.GdalDEM("/tmp/dem.tif", Miniset.DEMType.HEIGHT);
      console.log("Elevation:", dem.getHeight(5.73, 11.46), "m");  // degrees
      dem.delete();
    </script>
    ```

=== "Node.js"

    ```javascript
    import MinisetFactory from "./miniset.js";
    import fs from "fs";

    const Miniset = await MinisetFactory();

    const data = fs.readFileSync("mars_dem.tif");
    Miniset.FS.writeFile("/tmp/dem.tif", data);

    const dem = new Miniset.GdalDEM("/tmp/dem.tif", Miniset.DEMType.HEIGHT);
    console.log("Height:", dem.getHeight(-4.0, 222.5), "m");  // degrees
    dem.delete();
    ```

!!! warning "Free your objects"

    WASM-owned objects (`EllipsoidDEM`, `GdalDEM`, …) are **not** garbage
    collected. Call `.delete()` when you're done to release their memory.

### No file needed: EllipsoidDEM

`EllipsoidDEM` is pure geometry — give it a body's semi-major and semi-minor
radii and query radius/height directly. This is what the
[playground](playground.md) runs by default:

```javascript
const Miniset = await MinisetFactory();
const dem = new Miniset.EllipsoidDEM(3396190, 3376200);  // Mars, metres
console.log(dem.getRadius(45, 0));   // radius at 45°N, in metres
dem.delete();
```

## Next steps

- [Playground](playground.md) — run Miniset live in your browser.
- [API Reference](api-reference.md) — the full C++ and JS surface.
- [Building for WASM](building-wasm.md) — compile the module yourself.
