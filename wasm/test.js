#!/usr/bin/env node
/**
 * Test Miniset WASM with Mars DEM
 */

import initMiniset from './build/dist/miniset.js';
import fs from 'fs';

async function main() {
    console.log('='.repeat(60));
    console.log('  Miniset WASM Test with Mars DEM');
    console.log('='.repeat(60));
    console.log('');

    // Initialize Miniset
    console.log('1. Initializing Miniset WASM module...');
    const Miniset = await initMiniset();
    console.log('   ✅ Module loaded\n');

    // Load Mars DEM
    const marsFile = '../tests/wasm/T01_000881_1752_XI_04S223W__N06_064519_1753_XN_04S222W-DEM.tif';

    if (!fs.existsSync(marsFile)) {
        console.error(`   ❌ Mars DEM not found: ${marsFile}`);
        console.log('   Please ensure the Mars DEM file exists');
        process.exit(1);
    }

    console.log(`2. Loading Mars DEM: ${marsFile}`);
    const demData = fs.readFileSync(marsFile);
    console.log(`   File size: ${(demData.length / 1024 / 1024).toFixed(2)} MB`);

    Miniset.FS.writeFile('/tmp/mars.tif', demData);
    console.log('   ✅ File loaded into WASM FS\n');

    // Extract spatial reference
    console.log('3. Extracting spatial reference...');
    const wkt = Miniset.extractSpatialReference('/tmp/mars.tif');

    if (!wkt) {
        console.error('   ❌ No spatial reference found');
        process.exit(1);
    }

    console.log(`   WKT: ${wkt.substring(0, 80)}...`);
    console.log('   ✅ Spatial reference extracted\n');

    // Create DEM with automatic ellipsoid extraction
    console.log('4. Creating DEM with automatic ellipsoid extraction...');
    const dem = Miniset.createDEM('/tmp/mars.tif', Miniset.DEMType.HEIGHT);
    console.log('   ✅ DEM created\n');

    // Check ellipsoid
    console.log('5. Verifying ellipsoid parameters...');
    const a = dem.getSemiMajorA();
    const b = dem.getSemiMajorB();
    const c = dem.getSemiMinorC();

    console.log(`   Semi-major axis (a): ${a} m`);
    console.log(`   Semi-major axis (b): ${b} m`);
    console.log(`   Semi-minor axis (c): ${c} m`);

    // Mars 2015 sphere: 3,396,190 m
    const expected = 3396190;
    const tolerance = 1.0;

    if (Math.abs(a - expected) < tolerance && Math.abs(c - expected) < tolerance) {
        console.log('   ✅ Mars ellipsoid correctly extracted!\n');
    } else {
        console.error(`   ❌ Wrong ellipsoid! Expected ~${expected} m`);
        process.exit(1);
    }

    // Test elevation query
    console.log('6. Testing elevation query...');
    const lat = 0.1; // radians
    const lon = 0.2; // radians
    const height = dem.getHeight(lat, lon);
    console.log(`   Height at (${lat}, ${lon}): ${height} m`);
    console.log('   ✅ Elevation query works\n');

    // Cleanup
    Miniset.FS.unlink('/tmp/mars.tif');

    console.log('='.repeat(60));
    console.log('  🎉 All tests passed!');
    console.log('='.repeat(60));
}

main().catch(err => {
    console.error('\n❌ Test failed:', err.message);
    console.error(err.stack);
    process.exit(1);
});
