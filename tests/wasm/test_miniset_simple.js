#!/usr/bin/env node
/**
 * Simple Miniset DEM API test - NO manual environment configuration needed!
 *
 * This demonstrates that users can simply import and use Miniset
 * without having to worry about PROJ/GDAL environment setup.
 */

console.log('[TEST] Simple Miniset DEM API test (no manual config)\n');

import MinisetFactory from '../../build-wasm/miniset.js';
import fs from 'fs';

async function main() {
    // Initialize Miniset - NO preRun configuration needed!
    // The module automatically sets up PROJ_LIB, PROJ_DATA, GDAL_DATA, etc.
    const Module = await MinisetFactory();

    console.log('✓ Miniset module loaded (environment auto-configured)\n');

    // Load Mars DEM
    const marsFile = 'T01_000881_1752_XI_04S223W__N06_064519_1753_XN_04S222W-DEM.tif';
    const marsData = fs.readFileSync(marsFile);
    Module.FS.writeFile('/tmp/mars.tif', marsData);

    // Create DEM - projection info extracted automatically!
    console.log('Creating DEM from Mars GeoTIFF...');
    const dem = new Module.GdalDEM('/tmp/mars.tif', Module.DEMType.HEIGHT);
    console.log('✓ DEM created\n');

    // Get projection info (ellipsoid parameters)
    console.log('═══════════════════════════════════════════════');
    console.log('PROJECTION INFO');
    console.log('═══════════════════════════════════════════════');
    console.log(`Semi-major axis: ${dem.getSemiMajorA().toFixed(2)} m`);
    console.log(`Semi-minor axis: ${dem.getSemiMinorC().toFixed(2)} m`);

    // Verify it's Mars
    if (Math.abs(dem.getSemiMajorA() - 3396190) < 100) {
        console.log('\n✓ Correctly detected Mars 2015 IAU spheroid!');
    } else {
        console.error('\n✗ Failed to detect Mars ellipsoid');
        process.exit(1);
    }

    // Query elevation
    console.log('\n═══════════════════════════════════════════════');
    console.log('ELEVATION QUERY');
    console.log('═══════════════════════════════════════════════');
    const lat = -4.0 * Math.PI / 180;
    const lon = 222.5 * Math.PI / 180;
    console.log(`Location: ${(-4.0).toFixed(1)}°S, ${222.5.toFixed(1)}°E`);

    try {
        const height = dem.getHeight(lat, lon);
        const radius = dem.getRadius(lat, lon);
        console.log(`Height: ${height.toFixed(2)} m`);
        console.log(`Radius: ${radius.toFixed(2)} m`);
    } catch (e) {
        console.log(`Outside DEM bounds: ${e.message}`);
    }

    // Cleanup
    dem.delete();
    Module.FS.unlink('/tmp/mars.tif');

    console.log('\n═══════════════════════════════════════════════');
    console.log('✓ TEST PASSED!');
    console.log('═══════════════════════════════════════════════');
    console.log('\nMiniset automatically handled:');
    console.log('  • Environment configuration (PROJ_LIB, GDAL_DATA)');
    console.log('  • GeoTIFF loading');
    console.log('  • Projection/ellipsoid extraction');
    console.log('  • Elevation queries');
    console.log('\nNo manual setup required! 🎉');
}

main().catch(err => {
    console.error('\n✗ TEST FAILED:', err.message);
    console.error(err.stack);
    process.exit(1);
});
