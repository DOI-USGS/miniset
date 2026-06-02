#!/usr/bin/env node

/**
 * Test createCsmFromAttachedSpice functionality
 * Tests loading CSM camera models from ISIS cubes processed with csminit
 */

import { TestRunner, assert, assertEqual, assertClose } from './test_runner.js';
import MinisetFactory from '../../build-wasm/miniset.js';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const suite = new TestRunner('Attached SPICE Tests');

// Helper to check if function is available
async function functionAvailable(Module) {
    return typeof Module.createCsmFromAttachedSpice === 'function';
}

suite.test('createCsmFromAttachedSpice binding exists', async () => {
    const Module = await MinisetFactory();

    assert(
        typeof Module.createCsmFromAttachedSpice === 'function',
        'createCsmFromAttachedSpice should be a function'
    );
});

suite.test('createCsmFromAttachedSpice fails with non-existent file', async () => {
    const Module = await MinisetFactory();
    if (!await functionAvailable(Module)) return;

    let threw = false;
    try {
        const model = Module.createCsmFromAttachedSpice("/nonexistent/path.tiff");
        Module.deleteModel(model);
    } catch (e) {
        threw = true;
        // Error thrown - exact message format varies in WASM
    }

    assert(threw, 'Should throw error for non-existent file');
});

suite.test('createCsmFromAttachedSpice fails with file without CSM metadata', async () => {
    const Module = await MinisetFactory();
    if (!await functionAvailable(Module)) return;

    // test_earth.tif doesn't have CSM metadata
    const testPath = path.resolve(__dirname, 'test_earth.tif');

    if (!fs.existsSync(testPath)) return;

    // Copy test file to VFS
    const fileData = fs.readFileSync(testPath);
    Module.FS.writeFile('/test_earth.tif', fileData);

    let threw = false;
    try {
        const model = Module.createCsmFromAttachedSpice("/test_earth.tif");
        Module.deleteModel(model);
    } catch (e) {
        threw = true;
        // Error thrown - exact message format varies in WASM
    }

    assert(threw, 'Should throw error for file without CSM metadata');
});

suite.test('createCsmFromAttachedSpice works with cropped test image', async () => {
    const Module = await MinisetFactory();
    if (!await functionAvailable(Module)) return;

    // Use the same cropped test image as C++ tests
    // Path is relative to tests/wasm/ directory
    const testPath = path.resolve(__dirname, '../data/W02_089524_2073_XN_27N269W.cropped.tiff');

    if (!fs.existsSync(testPath)) return;

        // Copy test file to VFS
        const fileData = fs.readFileSync(testPath);
        Module.FS.writeFile('/test_cropped.tiff', fileData);

        // Create CSM model from attached SPICE
        const model = Module.createCsmFromAttachedSpice("/test_cropped.tiff");

        assert(typeof model === 'number', 'Model should be a number (pointer)');
        assert(model !== 0, 'Model pointer should not be null');

        // Verify it's the correct model type
        const modelName = Module.getModelName(model);
        assertEqual(
            modelName,
            'USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL',
            'Model name should be USGS_ASTRO_LINE_SCANNER_SENSOR_MODEL'
        );

        // Verify image size matches cropped image (100x100)
        const size = Module.getImageSize(model);
        assertEqual(size.lines, 100, 'Image should have 100 lines');
        assertEqual(size.samples, 100, 'Image should have 100 samples');

        Module.deleteModel(model);
});

suite.test('createCsmFromAttachedSpice produces same result as createCsmFromISD', async () => {
    const Module = await MinisetFactory();
    if (!await functionAvailable(Module)) return;

    // Same test data as C++ tests in tests/data/
    // Paths are relative to tests/wasm/ directory
    const imagePath = path.resolve(__dirname, '../data/W02_089524_2073_XN_27N269W.cropped.tiff');
    const isdPath = path.resolve(__dirname, '../data/W02_089524_2073_XN_27N269W.cropped.json');

    if (!fs.existsSync(imagePath) || !fs.existsSync(isdPath)) return;

        // Load from attached SPICE
        const imageData = fs.readFileSync(imagePath);
        Module.FS.writeFile('/test_image.tiff', imageData);
        const modelFromSpice = Module.createCsmFromAttachedSpice("/test_image.tiff");

        // Load from ISD JSON
        const isdJson = fs.readFileSync(isdPath, 'utf8');
        const modelFromIsd = Module.createCsmFromISD(isdJson);

        // Both models should have same image size
        const sizeSpice = Module.getImageSize(modelFromSpice);
        const sizeIsd = Module.getImageSize(modelFromIsd);
        assertEqual(sizeSpice.lines, sizeIsd.lines, 'Lines should match');
        assertEqual(sizeSpice.samples, sizeIsd.samples, 'Samples should match');

        // Test same image point with both models
        const testLine = 50;
        const testSample = 50;
        const height = 0;

        const groundSpice = Module.imageToGround(modelFromSpice, testLine, testSample, height);
        const groundIsd = Module.imageToGround(modelFromIsd, testLine, testSample, height);

        // Ground coordinates should match closely (within 10 meters)
        assertClose(groundSpice.x, groundIsd.x, 10.0, 'Ground X coordinates should match');
        assertClose(groundSpice.y, groundIsd.y, 10.0, 'Ground Y coordinates should match');
        assertClose(groundSpice.z, groundIsd.z, 10.0, 'Ground Z coordinates should match');

        Module.deleteModel(modelFromSpice);
        Module.deleteModel(modelFromIsd);
});

suite.test('Round-trip projection works with attached SPICE model', async () => {
    const Module = await MinisetFactory();
    if (!await functionAvailable(Module)) return;

    // Same test image as C++ tests
    // Path is relative to tests/wasm/ directory
    const imagePath = path.resolve(__dirname, '../data/W02_089524_2073_XN_27N269W.cropped.tiff');

    if (!fs.existsSync(imagePath)) return;

        // Load model from attached SPICE
        const imageData = fs.readFileSync(imagePath);
        Module.FS.writeFile('/test_image.tiff', imageData);
        const model = Module.createCsmFromAttachedSpice("/test_image.tiff");

        // Test round-trip: image -> ground -> image
        const origLine = 50;
        const origSample = 50;
        const height = 0;

        const ground = Module.imageToGround(model, origLine, origSample, height);
        const image = Module.groundToImage(model, ground.x, ground.y, ground.z);

        // Round-trip error should be sub-pixel
        const lineError = Math.abs(image.line - origLine);
        const sampleError = Math.abs(image.sample - origSample);

        assert(lineError < 0.1, `Line round-trip error should be < 0.1 pixel (was ${lineError})`);
        assert(sampleError < 0.1, `Sample round-trip error should be < 0.1 pixel (was ${sampleError})`);

        Module.deleteModel(model);

});

// Run tests
suite.run().then(success => {
    process.exit(success ? 0 : 1);
}).catch(error => {
    console.error('Test runner failed:', error);
    process.exit(1);
});
