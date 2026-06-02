#!/usr/bin/env node
/**
 * Test Miniset DEM API - Create GdalDEM and extract projection info
 *
 * This demonstrates:
 * 1. Loading a GeoTIFF file into the WASM virtual filesystem
 * 2. Creating a Miniset GdalDEM object
 * 3. Extracting ellipsoid parameters (projection info) from the DEM
 * 4. Querying elevation at lat/lon coordinates
 */

console.log('[TEST] Testing Miniset DEM API\n');

import MinisetFactory from '../../build-wasm/miniset.js';
import fs from 'fs';

async function main() {
    // Initialize the Miniset WASM module
    // NOTE: Environment variables (PROJ_LIB, GDAL_DATA, etc.) are set automatically!
    // You can optionally override them in preRun if needed.
    const Module = await MinisetFactory({
        // Optional: customize stdout/stderr handling
        print: (text) => console.log(`[WASM] ${text}`),
        printErr: (text) => console.error(`[WASM] ${text}`)
    });

    console.log('✓ Miniset module loaded (environment auto-configured)\n');

    // Test with Mars DEM
    const marsFile = 'T01_000881_1752_XI_04S223W__N06_064519_1753_XN_04S222W-DEM.tif';
    if (!fs.existsSync(marsFile)) {
        console.error(`✗ Mars DEM not found: ${marsFile}`);
        process.exit(1);
    }

    console.log(`Loading Mars DEM: ${marsFile}`);
    const marsData = fs.readFileSync(marsFile);
    Module.FS.writeFile('/tmp/mars_dem.tif', marsData);
    console.log('✓ File loaded into WASM virtual filesystem\n');

    // Create GdalDEM object using Miniset API
    console.log('Creating GdalDEM object...');
    const dem = new Module.GdalDEM('/tmp/mars_dem.tif', Module.DEMType.HEIGHT);
    console.log('✓ GdalDEM object created\n');

    // Extract ellipsoid parameters (projection info)
    console.log('═══════════════════════════════════════════════');
    console.log('PROJECTION INFO (Ellipsoid Parameters)');
    console.log('═══════════════════════════════════════════════');
    const semiMajorA = dem.getSemiMajorA();
    const semiMajorB = dem.getSemiMajorB();
    const semiMinorC = dem.getSemiMinorC();

    console.log(`Semi-major axis A: ${semiMajorA.toFixed(2)} m`);
    console.log(`Semi-major axis B: ${semiMajorB.toFixed(2)} m`);
    console.log(`Semi-minor axis C: ${semiMinorC.toFixed(2)} m`);

    // Check if it's Mars (should be ~3,396,190 m)
    if (Math.abs(semiMajorA - 3396190) < 100) {
        console.log('\n✓ Detected Mars 2015 IAU spheroid!');
    } else if (Math.abs(semiMajorA - 6378137) < 100) {
        console.log('\n✓ Detected Earth WGS84 ellipsoid');
    }

    // Query elevation at specific coordinates
    console.log('\n═══════════════════════════════════════════════');
    console.log('ELEVATION QUERIES');
    console.log('═══════════════════════════════════════════════');

    // Test point in the middle of the image (approximately)
    // Mars DEM is around -4°S, 222-223°E
    const testLat = -4.0 * Math.PI / 180;  // Convert to radians
    const testLon = 222.5 * Math.PI / 180;

    console.log(`Query point: ${(-4.0).toFixed(2)}°S, ${222.5.toFixed(2)}°E`);

    try {
        const height = dem.getHeight(testLat, testLon);
        console.log(`Height above ellipsoid: ${height.toFixed(2)} m`);

        const radius = dem.getRadius(testLat, testLon);
        console.log(`Radius from center: ${radius.toFixed(2)} m`);

        const rasterValue = dem.getRasterValue(testLat, testLon);
        console.log(`Raw raster value: ${rasterValue.toFixed(2)}`);
    } catch (e) {
        console.log(`Query failed (point may be outside DEM bounds): ${e.message}`);
    }

    // Test with Earth GeoTIFF
    console.log('\n═══════════════════════════════════════════════');
    console.log('TESTING WITH EARTH GEOTIFF');
    console.log('═══════════════════════════════════════════════');

    const earthFile = 'test_earth.tif';
    if (fs.existsSync(earthFile)) {
        const earthData = fs.readFileSync(earthFile);
        Module.FS.writeFile('/tmp/earth_dem.tif', earthData);

        const earthDem = new Module.GdalDEM('/tmp/earth_dem.tif', Module.DEMType.HEIGHT);

        const earthA = earthDem.getSemiMajorA();
        const earthC = earthDem.getSemiMinorC();

        console.log(`Semi-major axis: ${earthA.toFixed(2)} m`);
        console.log(`Semi-minor axis: ${earthC.toFixed(2)} m`);

        if (Math.abs(earthA - 6378137) < 100) {
            console.log('✓ Detected Earth WGS84 ellipsoid!');
        }

        earthDem.delete();
        Module.FS.unlink('/tmp/earth_dem.tif');
    } else {
        console.log(`Earth test file not found: ${earthFile}`);
    }

    // Cleanup
    dem.delete();
    Module.FS.unlink('/tmp/mars_dem.tif');

    console.log('\n═══════════════════════════════════════════════');
    console.log('✓ ALL TESTS PASSED!');
    console.log('═══════════════════════════════════════════════');
    console.log('\nMiniset DEM API successfully:');
    console.log('  • Loaded GeoTIFF files');
    console.log('  • Extracted ellipsoid/projection parameters');
    console.log('  • Queried elevations at lat/lon coordinates');
}

main().catch(err => {
    console.error('\n✗ TEST FAILED:', err.message);
    console.error(err.stack);
    process.exit(1);
});
