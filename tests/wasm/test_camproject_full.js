#!/usr/bin/env node
/**
 * Full test of camproject (image reprojection) with CSM camera model
 * Uses W02_089524_2073_XN_27N269W MRO CTX image
 */

import MinisetFactory from '../../build-wasm/miniset.js';
import fs from 'fs';
import path from 'path';

const TEST_DIR = '/Users/krodriguez/work/cubes';
const IMAGE_FILE = 'W02_089524_2073_XN_27N269W.tiff';
const ISD_FILE = 'W02_089524_2073_XN_27N269W.json';
const OUTPUT_FILE = '/tmp/mroctx_projected_wasm.tif';

async function main() {
    console.log('=== Camproject WASM Test ===\n');

    // Load Miniset WASM module
    console.log('Loading Miniset WASM module...');
    const Module = await MinisetFactory();
    console.log('✓ Module loaded\n');

    // Wait for embedded files to be fully extracted
    console.log('Waiting for filesystem initialization...');
    await new Promise(resolve => setTimeout(resolve, 2000));

    // Verify proj.db exists
    try {
        const stat = Module.FS.stat('/usr/share/proj/proj.db');
        console.log(`✓ PROJ database ready (${(stat.size / 1024 / 1024).toFixed(2)} MB)\n`);
    } catch (e) {
        throw new Error('PROJ database not found in virtual filesystem');
    }

    try {
        // Load ISD file
        const isdPath = path.join(TEST_DIR, ISD_FILE);
        const isdJson = fs.readFileSync(isdPath, 'utf8');
        console.log(`Loading CSM model from: ${isdPath}`);

        // Create CSM model
        const model = Module.createCsmFromISD(isdJson);
        console.log('✓ CSM model created');

        // Get model info
        const modelName = Module.getModelName(model);
        const imageSize = Module.getImageSize(model);
        console.log(`Model: ${modelName}`);
        console.log(`Image size: ${imageSize.lines} x ${imageSize.samples}\n`);

        // Load input image into WASM virtual filesystem
        const imagePath = path.join(TEST_DIR, IMAGE_FILE);
        console.log(`Loading image: ${imagePath}`);
        const imageData = fs.readFileSync(imagePath);
        Module.FS.writeFile('/tmp/input.tiff', imageData);
        console.log('✓ Image loaded into WASM filesystem\n');

        // Define output projection (Mars equirectangular)
        const outputProj = '+proj=eqc +lat_ts=0 +lat_0=0 +lon_0=0 +x_0=0 +y_0=0 ' +
                          '+a=3396190 +b=3376200 +units=m +no_defs';

        console.log('Projecting image...');
        console.log(`  Output projection: Mars equirectangular`);
        console.log(`  Ground height: 0.0 m\n`);

        // Project the image
        Module.camproject(
            model,
            '/tmp/input.tiff',
            outputProj,
            '/tmp/output.tif',
            0.0  // ground height
        );

        console.log('✓ Projection completed!\n');

        // Read output from WASM filesystem and save to disk
        const outputData = Module.FS.readFile('/tmp/output.tif');
        fs.writeFileSync(OUTPUT_FILE, outputData);
        console.log(`✓ Output saved to: ${OUTPUT_FILE}`);
        console.log(`  File size: ${(outputData.length / 1024 / 1024).toFixed(2)} MB\n`);

        // Cleanup
        Module.deleteModel(model);
        console.log('=== TEST PASSED ===');

    } catch (error) {
        console.error('ERROR:', error.message);
        if (error.stack) {
            console.error(error.stack);
        }
        process.exit(1);
    }
}

main().catch(err => {
    console.error('Unhandled error:', err);
    process.exit(1);
});
