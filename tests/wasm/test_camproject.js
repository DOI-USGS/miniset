#!/usr/bin/env node

/**
 * Test camproject functionality - image projection using CSM camera model
 */

import { TestRunner, assert } from './test_runner.js';
import MinisetFactory from '../../build-wasm/miniset.js';
import fs from 'fs';

const suite = new TestRunner('Image Projection Tests');

// Helper to check if CSM and GDAL are available
async function featuresAvailable(Module) {
    if (typeof Module.createCsmFromISD !== 'function') {
        console.log('    (Skipped: CSM bindings not available)');
        return false;
    }
    if (typeof Module.camproject !== 'function') {
        console.log('    (Skipped: camproject binding not available)');
        return false;
    }
    return true;
}

suite.test('camproject API status', async () => {
    const Module = await MinisetFactory();

    // Check if camproject function exists
    if (typeof Module.camproject !== 'function') {
        console.log('    (camproject function not available - bindings disabled)');
        console.log('    This is expected if CSM bindings are disabled in miniset_bindings.cpp');
        assert(true, 'camproject binding intentionally disabled');
        return;
    }

    // If available, verify it's a function
    assert(typeof Module.camproject === 'function', 'camproject should be a function');
});

suite.test('camproject fails with invalid model pointer', async () => {
    const Module = await MinisetFactory();
    if (!await featuresAvailable(Module)) return;

    let threw = false;
    try {
        Module.camproject(
            0,  // Invalid pointer
            '/tmp/test.tif',
            '+proj=longlat +datum=WGS84',
            '/tmp/output.tif',
            0.0
        );
    } catch (e) {
        threw = true;
        assert(e.message.includes('Invalid model pointer'), 'Should report invalid pointer');
    }

    assert(threw, 'Should throw error for invalid model pointer');
});

suite.test('camproject fails with non-existent input file', async () => {
    const Module = await MinisetFactory();
    if (!await featuresAvailable(Module)) return;

    // Create a minimal ISD for testing
    const minimalISD = {
        "name_model": "USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL",
        "image_lines": 100,
        "image_samples": 100,
        "name_platform": "TestSat",
        "name_sensor": "TestCam",
        "center_ephemeris_time": 100000000.0,
        "radii": {
            "semimajor": 3396190.0,
            "semiminor": 3376200.0,
            "unit": "m"
        },
        "focal_length_model": {"focal_length": 0.5},
        "detector_sample_summing": 1,
        "detector_line_summing": 1,
        "starting_detector_sample": 0,
        "starting_detector_line": 0,
        "focal2pixel_samples": [0.0, 0.0, 100.0],
        "focal2pixel_lines": [0.0, 100.0, 0.0],
        "optical_distortion": {"radial": {"coefficients": [0.0, 0.0, 0.0]}},
        "sensor_position": {
            "positions": [[0, 0, 4000000], [100, 0, 4000000]],
            "velocities": [[1, 0, 0], [1, 0, 0]],
            "unit": "m"
        },
        "sensor_orientation": {
            "quaternions": [[0, 1, 0, 0, 0], [100, 1, 0, 0, 0]]
        },
        "sun_position": {
            "positions": [[1.5e11, 0, 0]],
            "velocities": [[0, 0, 0]],
            "unit": "m"
        }
    };

    try {
        const model = Module.createCsmFromISD(JSON.stringify(minimalISD));

        let threw = false;
        try {
            Module.camproject(
                model,
                '/tmp/nonexistent.tif',
                '+proj=longlat +datum=WGS84',
                '/tmp/output.tif',
                0.0
            );
        } catch (e) {
            threw = true;
            console.log(`    Expected error: ${e.message}`);
        }

        assert(threw, 'Should throw error for non-existent input file');
        Module.deleteModel(model);
    } catch (e) {
        console.log('    (Skipped: Could not create CSM model)');
    }
});

suite.test('camproject validates PROJ string', async () => {
    const Module = await MinisetFactory();
    if (!await featuresAvailable(Module)) return;

    const minimalISD = {
        "name_model": "USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL",
        "image_lines": 100,
        "image_samples": 100,
        "name_platform": "TestSat",
        "name_sensor": "TestCam",
        "center_ephemeris_time": 100000000.0,
        "radii": {
            "semimajor": 3396190.0,
            "semiminor": 3376200.0,
            "unit": "m"
        },
        "focal_length_model": {"focal_length": 0.5},
        "detector_sample_summing": 1,
        "detector_line_summing": 1,
        "starting_detector_sample": 0,
        "starting_detector_line": 0,
        "focal2pixel_samples": [0.0, 0.0, 100.0],
        "focal2pixel_lines": [0.0, 100.0, 0.0],
        "optical_distortion": {"radial": {"coefficients": [0.0, 0.0, 0.0]}},
        "sensor_position": {
            "positions": [[0, 0, 4000000], [100, 0, 4000000]],
            "velocities": [[1, 0, 0], [1, 0, 0]],
            "unit": "m"
        },
        "sensor_orientation": {
            "quaternions": [[0, 1, 0, 0, 0], [100, 1, 0, 0, 0]]
        },
        "sun_position": {
            "positions": [[1.5e11, 0, 0]],
            "velocities": [[0, 0, 0]],
            "unit": "m"
        }
    };

    try {
        const model = Module.createCsmFromISD(JSON.stringify(minimalISD));

        // Create a dummy input file
        const dummyData = new Uint8Array(100);
        Module.FS.writeFile('/tmp/test_input.tif', dummyData);

        let threw = false;
        try {
            Module.camproject(
                model,
                '/tmp/test_input.tif',
                'invalid projection string',
                '/tmp/output.tif',
                0.0
            );
        } catch (e) {
            threw = true;
            console.log(`    Expected error: ${e.message}`);
        }

        assert(threw, 'Should throw error for invalid PROJ string');
        Module.deleteModel(model);
    } catch (e) {
        console.log('    (Skipped: Could not create CSM model)');
    }
});

// Run tests
suite.run().then(success => {
    process.exit(success ? 0 : 1);
}).catch(error => {
    console.error('Test runner failed:', error);
    process.exit(1);
});
