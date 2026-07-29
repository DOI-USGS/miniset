// Control-network readers for the WASM build.
//
// Parquet is now parsed entirely in C++ by the vendored header-only reader
// (external/miniparquet.hpp), exposed via the Embind `readControlNetParquet`
// entry. There is no JS Parquet dependency (hyparquet is gone): the file bytes
// are handed to the module and decoded natively — the same C++ code path used
// in the native build. Legacy protobuf `.net` files are likewise parsed in C++.
//
// This removes the JS<->wasm per-column marshaling that the previous hyparquet
// path required; a single Uint8Array crosses the boundary and the SoA ControlNet
// is assembled in C++.

/**
 * Read a control-network Parquet file into the WASM module (parsed in C++).
 * @param {object} Module  the initialized Miniset WASM module
 * @param {ArrayBuffer|Uint8Array} data  the .parquet file bytes
 * @returns {number} opaque ControlNet handle (free with Module.cnetDelete)
 */
export function readControlNetParquet(Module, data) {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  return Module.readControlNetParquet(bytes);
}

/**
 * Read a legacy protobuf .net file entirely in C++ (no JS Parquet lib).
 * @param {object} Module  the initialized Miniset WASM module
 * @param {Uint8Array} bytes  the .net file bytes
 * @returns {number} opaque ControlNet handle (free with Module.cnetDelete)
 */
export function readControlNetProtobuf(Module, bytes) {
  return Module.readNetProtobuf(bytes);
}
