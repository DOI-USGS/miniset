#!/usr/bin/env node

/**
 * Test the simplified DEM API (getHeight/getRadius with degrees only)
 * Tests that the API changes work correctly in WASM
 */

import { TestRunner, assert, assertEqual, assertClose } from './test_runner.js';
import MinisetFactory from '../../build-wasm/miniset.js';

const suite = new TestRunner('Simplified DEM API Tests');

suite.test('EllipsoidDEM has simplified API methods', async () => {
    const Module = await MinisetFactory();

    // Create Mars ellipsoid
    const dem = new Module.EllipsoidDEM(3396190, 3376200);

    // Verify the simplified methods exist
    assert(typeof dem.getHeight === 'function', 'getHeight should exist');
    assert(typeof dem.getRadius === 'function', 'getRadius should exist');
    assert(typeof dem.getSemiMajorA === 'function', 'getSemiMajorA should exist');
    assert(typeof dem.getSemiMajorB === 'function', 'getSemiMajorB should exist');
    assert(typeof dem.getSemiMinorC === 'function', 'getSemiMinorC should exist');

    // Verify old methods are removed
    assert(typeof dem.getHeightDeg === 'undefined', 'getHeightDeg should NOT exist (removed)');
    assert(typeof dem.getRadiusDeg === 'undefined', 'getRadiusDeg should NOT exist (removed)');
    assert(typeof dem.getHeightRad === 'undefined', 'getHeightRad should NOT exist (removed)');
    assert(typeof dem.getRadiusRad === 'undefined', 'getRadiusRad should NOT exist (removed)');

    dem.delete();
});

suite.test('getHeight works with degrees', async () => {
    const Module = await MinisetFactory();

    const dem = new Module.EllipsoidDEM(3396190, 3376200);

    // Pure ellipsoid has zero height everywhere
    const height1 = dem.getHeight(0.0, 0.0);    // Equator, prime meridian
    const height2 = dem.getHeight(90.0, 0.0);   // North pole
    const height3 = dem.getHeight(45.0, 180.0); // Mid-latitude

    assertEqual(height1, 0.0, 'Height at equator should be 0');
    assertEqual(height2, 0.0, 'Height at pole should be 0');
    assertEqual(height3, 0.0, 'Height at mid-latitude should be 0');

    dem.delete();
});

suite.test('getRadius works with degrees and returns correct values', async () => {
    const Module = await MinisetFactory();

    const dem = new Module.EllipsoidDEM(3396190, 3376200);

    // Test at equator (should be semi-major axis)
    const radius_equator = dem.getRadius(0.0, 0.0);
    assertClose(radius_equator, 3396190.0, 1.0, 'Radius at equator should be semi-major axis');

    // Test at pole (should be semi-minor axis)
    const radius_pole = dem.getRadius(90.0, 0.0);
    assertClose(radius_pole, 3376200.0, 1.0, 'Radius at pole should be semi-minor axis');

    // Test at mid-latitude (should be between semi-major and semi-minor)
    const radius_mid = dem.getRadius(45.0, 0.0);
    assert(radius_mid > 3376200.0 && radius_mid < 3396190.0,
           'Radius at mid-latitude should be between semi-minor and semi-major');

    dem.delete();
});

suite.test('Ellipsoid accessors work correctly', async () => {
    const Module = await MinisetFactory();

    const dem = new Module.EllipsoidDEM(3396190, 3376200);

    assertEqual(dem.getSemiMajorA(), 3396190.0, 'Semi-major A should be 3396190');
    assertEqual(dem.getSemiMajorB(), 3396190.0, 'Semi-major B should be 3396190');
    assertEqual(dem.getSemiMinorC(), 3376200.0, 'Semi-minor C should be 3376200');

    dem.delete();
});

suite.test('API accepts negative longitudes', async () => {
    const Module = await MinisetFactory();

    const dem = new Module.EllipsoidDEM(3396190, 3376200);

    // Test with negative longitude (western hemisphere)
    const radius_neg = dem.getRadius(45.0, -90.0);
    const radius_pos = dem.getRadius(45.0, 270.0);

    // Should give same result (270° = -90°)
    assertClose(radius_neg, radius_pos, 1.0,
                'Negative and positive longitudes should give same result');

    dem.delete();
});

suite.test('API works across longitude boundaries', async () => {
    const Module = await MinisetFactory();

    const dem = new Module.EllipsoidDEM(3396190, 3376200);

    // Test at 0° and 360° (should be same)
    const radius_0 = dem.getRadius(0.0, 0.0);
    const radius_360 = dem.getRadius(0.0, 360.0);

    assertClose(radius_0, radius_360, 1.0,
                '0° and 360° longitude should give same result');

    dem.delete();
});

suite.test('Triaxial ellipsoid (biaxial in this case)', async () => {
    const Module = await MinisetFactory();

    // Create Earth-like ellipsoid
    const dem = new Module.EllipsoidDEM(6378137, 6356752);

    assertEqual(dem.getSemiMajorA(), 6378137.0, 'Earth semi-major A');
    assertEqual(dem.getSemiMajorB(), 6378137.0, 'Earth semi-major B (same as A for biaxial)');
    assertEqual(dem.getSemiMinorC(), 6356752.0, 'Earth semi-minor C');

    // Test radius at equator
    const radius = dem.getRadius(0.0, 0.0);
    assertClose(radius, 6378137.0, 1.0, 'Earth radius at equator');

    dem.delete();
});

// Run tests
suite.run().then(success => {
    process.exit(success ? 0 : 1);
}).catch(error => {
    console.error('Test runner failed:', error);
    process.exit(1);
});
