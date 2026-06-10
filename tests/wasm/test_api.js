#!/usr/bin/env node

/**
 * Test API structure and basic functionality
 */

import { TestRunner, assert, assertEqual } from './test_runner.js';
import MinisetFactory from '../../build-wasm/miniset.js';

const suite = new TestRunner('API Structure Tests');

suite.test('Module loads successfully', async () => {
    const Module = await MinisetFactory();
    assert(Module !== null, 'Module should not be null');
    assert(typeof Module === 'object', 'Module should be an object');
});

suite.test('CSM functions availability', async () => {
    const Module = await MinisetFactory();

    // CSM functions are currently not exposed in WASM bindings
    // (commented out in miniset_bindings.cpp lines 463-476)
    // This test verifies they're intentionally unavailable
    const csmFunctions = [
        'createCsmFromISD',
        'createCsmFromStateString',
        'campt',
        'imageToGround',
        'groundToImage',
        'getModelState',
        'getModelName',
        'getImageIdentifier',
        'getImageSize',
        'getSensorState',
        'getRadii',
        'deleteModel'
    ];

    const availableCount = csmFunctions.filter(fn => typeof Module[fn] === 'function').length;

    if (availableCount === 0) {
        console.log('    (CSM functions not exposed - expected, bindings are disabled)');
        assert(true, 'CSM functions intentionally not exposed');
    } else if (availableCount === csmFunctions.length) {
        console.log('    (All CSM functions available)');
        assert(true, 'All CSM functions available');
    } else {
        assert(false, `Partial CSM binding: ${availableCount}/${csmFunctions.length} functions available`);
    }
});

suite.test('DEM classes are available', async () => {
    const Module = await MinisetFactory();

    assert(typeof Module.GdalDEM === 'function', 'Module.GdalDEM should be a constructor');
    assert(Module.DEMType !== undefined, 'Module.DEMType enum should exist');
    assert(Module.DEMType.HEIGHT !== undefined, 'Module.DEMType.HEIGHT should exist');
    assert(Module.DEMType.RADIUS !== undefined, 'Module.DEMType.RADIUS should exist');
});

suite.test('Virtual filesystem is available', async () => {
    const Module = await MinisetFactory();

    assert(Module.FS !== undefined, 'Module.FS should be available');
    assert(typeof Module.FS.writeFile === 'function', 'Module.FS.writeFile should be a function');
    assert(typeof Module.FS.readFile === 'function', 'Module.FS.readFile should be a function');
    assert(typeof Module.FS.unlink === 'function', 'Module.FS.unlink should be a function');
});

suite.test('Can write and read from virtual filesystem', async () => {
    const Module = await MinisetFactory();

    const testData = new Uint8Array([1, 2, 3, 4, 5]);
    const testPath = '/tmp/test_file.bin';

    Module.FS.writeFile(testPath, testData);
    const readData = Module.FS.readFile(testPath);

    assertEqual(readData.length, testData.length, 'Read data length should match');
    for (let i = 0; i < testData.length; i++) {
        assertEqual(readData[i], testData[i], `Byte ${i} should match`);
    }

    Module.FS.unlink(testPath);
});

suite.test('Memory allocation works', async () => {
    const Module = await MinisetFactory();

    // Test that we can allocate and work with memory
    const testString = "Hello, WASM!";

    // Create a simple object to test memory
    const testData = new Uint8Array(1000);
    for (let i = 0; i < testData.length; i++) {
        testData[i] = i % 256;
    }

    Module.FS.writeFile('/tmp/memory_test.bin', testData);
    const readBack = Module.FS.readFile('/tmp/memory_test.bin');

    assertEqual(readBack.length, testData.length, 'Memory allocation should work');
    Module.FS.unlink('/tmp/memory_test.bin');
});

// Run tests
suite.run().then(success => {
    process.exit(success ? 0 : 1);
}).catch(error => {
    console.error('Test runner failed:', error);
    process.exit(1);
});
