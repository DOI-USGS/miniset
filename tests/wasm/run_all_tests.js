#!/usr/bin/env node
/**
 * Run all Miniset WASM test suites
 */

import { spawn } from 'child_process';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const TEST_SUITES = [
    'test_api.js',
    'test_dem.js',
    'test_csm.js',
    'test_attached_spice.js'
];

async function runTest(testFile) {
    return new Promise((resolve) => {
        console.log(`\n${'='.repeat(60)}`);
        console.log(`Running: ${testFile}`);
        console.log('='.repeat(60));

        const proc = spawn('node', [testFile], {
            cwd: __dirname,
            stdio: 'inherit'
        });

        proc.on('close', (code) => {
            resolve({ testFile, passed: code === 0 });
        });
    });
}

async function main() {
    console.log('Miniset WASM Test Suite\n');

    const results = [];
    for (const testFile of TEST_SUITES) {
        const result = await runTest(testFile);
        results.push(result);
    }

    console.log(`\n${'='.repeat(60)}`);
    console.log('TEST SUMMARY');
    console.log('='.repeat(60));

    let allPassed = true;
    for (const { testFile, passed } of results) {
        const status = passed ? '✓ PASSED' : '✗ FAILED';
        console.log(`${status} - ${testFile}`);
        if (!passed) allPassed = false;
    }

    console.log('='.repeat(60));

    if (allPassed) {
        console.log('\n🎉 All test suites passed!\n');
        process.exit(0);
    } else {
        console.log('\n❌ Some test suites failed\n');
        process.exit(1);
    }
}

main().catch(err => {
    console.error('Test runner failed:', err);
    process.exit(1);
});
