/**
 * WASM Module Loader
 * Handles initialization of WebAssembly modules (Miniset v8 + USGSCSM)
 */

import MinisetFactory from './miniset_v8.js';

// Module state
let minisetV8Instance = null;
let csmModuleInstance = null;

/**
 * Loads the CSM (Community Sensor Model) WASM module
 * @async
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @returns {Promise<Object|null>} CSM module instance or null on failure
 */
export async function loadCSMModule(updateStatusCallback) {
    updateStatusCallback('Loading CSM camera models...', 'loading');

    try {
        console.log('→ Loading USGSCSM WASM module...');
        const USGSCSM = await import('./usgscsm_wasm.js');
        csmModuleInstance = await USGSCSM.default();
        console.log('✓ USGSCSM loaded successfully');
        console.log('  Module type:', typeof csmModuleInstance);
        console.log('  Available model types:', 'FRAME, LINE_SCANNER, PUSH_FRAME');

        if (csmModuleInstance.USGSCSMModel) {
            console.log('  ✓ USGSCSMModel constructor available');
        } else {
            console.warn('  ⚠ USGSCSMModel not found in module');
        }

        return csmModuleInstance;
    } catch (err) {
        console.error('✗ Failed to load USGSCSM:', err);
        console.error('  Stack:', err.stack);
        updateStatusCallback('CSM module failed to load', 'error');
        return null;
    }
}

/**
 * Loads the Miniset v8 WASM module with GDAL/PROJ support
 * Configures environment variables and logging handlers
 * @async
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @returns {Promise<Object|null>} Miniset v8 module instance or null on failure
 */
export async function loadMinisetV8Module(updateStatusCallback) {
    updateStatusCallback('Loading Miniset v8 (with SPICE support)...', 'loading');
    try {
        console.log('→ Loading Miniset v8 WASM module...');

        // Initialize WASM module with configuration
        minisetV8Instance = await MinisetFactory({
            locateFile: (path) => {
                if (path.endsWith('.wasm')) {
                    return 'js/miniset_v8.wasm';
                }
                return path;
            },
            // WASM output - suppress verbose logging
            print: (text) => {
                if (window.captureLogsEnabled) {
                    window.debugLogs.push({ type: 'wasm', time: new Date().toISOString(), message: text });
                }
                // Suppress normal WASM stdout to reduce console noise
            },
            printErr: (text) => {
                if (window.captureLogsEnabled) {
                    window.debugLogs.push({ type: 'wasm-error', time: new Date().toISOString(), message: text });
                }
                // Only log actual errors, not INFO messages
                if (text.includes('[ERROR]') || text.includes('Exception')) {
                    console.error('[WASM ERROR]', text);
                }
            },
            // Pre-run setup for GDAL/PROJ data paths
            preRun: [(moduleInstance) => {
                if (!moduleInstance.ENV) {
                    moduleInstance.ENV = {};
                }
                if (!moduleInstance.ENV.PROJ_LIB) {
                    moduleInstance.ENV.PROJ_LIB = '/usr/share/proj';
                }
                if (!moduleInstance.ENV.PROJ_DATA) {
                    moduleInstance.ENV.PROJ_DATA = '/usr/share/proj';
                }
                if (!moduleInstance.ENV.GDAL_DATA) {
                    moduleInstance.ENV.GDAL_DATA = '/usr/share/gdal';
                }
            }]
        });

        console.log('✓ Miniset v8 loaded successfully');
        return minisetV8Instance;
    } catch (err) {
        console.error('✗ Failed to load Miniset v8:', err);
        console.error('  Stack:', err.stack);
        // Don't show error status - v8 is optional for now
        return null;
    }
}

/**
 * Gets the current Miniset v8 module instance
 * @returns {Object|null} Miniset v8 module or null if not loaded
 */
export function getMinisetV8() {
    return minisetV8Instance;
}

/**
 * Gets the current CSM module instance
 * @returns {Object|null} CSM module or null if not loaded
 */
export function getCSMModule() {
    return csmModuleInstance;
}

console.log('✓ WASM loader module loaded');
