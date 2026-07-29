#!/usr/bin/env node
/**
 * WASM test: Gaussian-splat LOD summary (cnet/3) reader + drill-down.
 *
 * Exercises the readSummary Embind family (openSummary / summaryCount /
 * summaryReadSplats / closeSummary) and the LOD → full-point drill-down: read
 * the splat overview, then stream one splat's [rangeStart, rangeCount) point
 * window with the openPointsXYZ/pointsReadXYZ API and confirm the points match
 * the splat's stored mean.
 *
 * Fixture `small3.stards` is a committed cnet/3 net (4 anisotropic ribbon
 * clusters × 500 adjusted points = 2000 points, 4 splats) produced natively by
 * `cnet_convert <net> small3.stards --summarize 4`. The WASM module only READS
 * summaries (fitting is native-only), so the fixture is prebuilt.
 */

import { TestRunner, assert, assertEqual, assertClose } from './test_runner.js';
import MinisetFactory from '../../build-wasm/miniset.js';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const FIXTURE = path.join(__dirname, 'small3.stards');
const EXPECT_POINTS = 2000;
const EXPECT_SPLATS = 4;

// Load the module once and stage the fixture into its in-memory FS.
async function load() {
    const M = await MinisetFactory({ print: () => {}, printErr: () => {} });
    const bytes = new Uint8Array(fs.readFileSync(FIXTURE));
    try { M.FS.unlink('/small3.stards'); } catch (_) { /* first time */ }
    M.FS.writeFile('/small3.stards', bytes);
    return M;
}

const suite = new TestRunner('Gaussian-splat summary (cnet/3) Tests');

suite.test('openSummary reports the splat count', async () => {
    const M = await load();
    const h = M.openSummary('/small3.stards');
    assertEqual(M.summaryCount(h), EXPECT_SPLATS, 'summaryCount');
    M.closeSummary(h);
});

suite.test('summaryReadSplats returns all splat arrays sized K', async () => {
    const M = await load();
    const h = M.openSummary('/small3.stards');
    const s = M.summaryReadSplats(h);
    const K = EXPECT_SPLATS;
    for (const k of ['muX', 'muY', 'muZ', 's0', 's1', 's2', 's3', 's4', 's5']) {
        assert(s[k] instanceof Float64Array, `${k} is Float64Array`);
        assertEqual(s[k].length, K, `${k}.length`);
    }
    for (const k of ['weight', 'rangeStart', 'rangeCount']) {
        assert(s[k] instanceof Uint32Array, `${k} is Uint32Array`);
        assertEqual(s[k].length, K, `${k}.length`);
    }
    M.closeSummary(h);
});

suite.test('splat ranges are a contiguous partition of the points', async () => {
    const M = await load();
    const h = M.openSummary('/small3.stards');
    const s = M.summaryReadSplats(h);
    let expectStart = 0, wsum = 0, rcsum = 0;
    for (let i = 0; i < EXPECT_SPLATS; i++) {
        assertEqual(s.rangeStart[i], expectStart, `rangeStart[${i}] contiguous`);
        expectStart += s.rangeCount[i];
        wsum += s.weight[i];
        rcsum += s.rangeCount[i];
    }
    assertEqual(rcsum, EXPECT_POINTS, 'sum(rangeCount) == points');
    assertEqual(wsum, EXPECT_POINTS, 'sum(weight) == points');
    M.closeSummary(h);
});

suite.test('drill-down: a splat window streams its exact points (mean ≈ mu)', async () => {
    const M = await load();
    const h = M.openSummary('/small3.stards');
    const s = M.summaryReadSplats(h);

    const ph = M.openPointsXYZ('/small3.stards');
    assertEqual(M.pointsCount(ph), EXPECT_POINTS, 'pointsCount');

    for (let i = 0; i < EXPECT_SPLATS; i++) {
        const b = M.pointsReadXYZ(ph, s.rangeStart[i], s.rangeCount[i]);
        assertEqual(b.count, s.rangeCount[i], `splat ${i} window size`);
        let mx = 0, my = 0, mz = 0;
        for (let p = 0; p < b.count; p++) {
            mx += b.positions[p * 3]; my += b.positions[p * 3 + 1]; mz += b.positions[p * 3 + 2];
        }
        mx /= b.count; my /= b.count; mz /= b.count;
        // float32 readback at globe scale (~3.4e6 m) → ~metre tolerance.
        assertClose(mx, s.muX[i], 2.0, `splat ${i} mean X vs mu`);
        assertClose(my, s.muY[i], 2.0, `splat ${i} mean Y vs mu`);
        assertClose(mz, s.muZ[i], 2.0, `splat ${i} mean Z vs mu`);
    }
    M.closePointsXYZ(ph);
    M.closeSummary(h);
});

suite.test('ranges stay within the point count', async () => {
    const M = await load();
    const h = M.openSummary('/small3.stards');
    const s = M.summaryReadSplats(h);
    for (let i = 0; i < EXPECT_SPLATS; i++) {
        assert(s.rangeStart[i] + s.rangeCount[i] <= EXPECT_POINTS,
            `splat ${i} range within points`);
    }
    M.closeSummary(h);
});

const ok = await suite.run();
process.exit(ok ? 0 : 1);
