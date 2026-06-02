/**
 * Pre-JS for Miniset WASM module
 * This runs BEFORE the WASM module is instantiated
 * Sets up default environment variables for GDAL/PROJ
 */

// Set up default Module configuration if not provided by user
if (typeof Module === 'undefined') {
    Module = {};
}

// Set up default preRun to configure GDAL/PROJ environment
var defaultPreRun = function(moduleInstance) {
    // Ensure ENV object exists
    if (!moduleInstance.ENV) {
        moduleInstance.ENV = {};
    }

    // Set PROJ search paths (unless user has already set them)
    if (!moduleInstance.ENV.PROJ_LIB) {
        moduleInstance.ENV.PROJ_LIB = '/usr/share/proj';
    }
    if (!moduleInstance.ENV.PROJ_DATA) {
        moduleInstance.ENV.PROJ_DATA = '/usr/share/proj';
    }

    // Set GDAL data path (unless user has already set it)
    if (!moduleInstance.ENV.GDAL_DATA) {
        moduleInstance.ENV.GDAL_DATA = '/usr/share/gdal';
    }

    // Disable GDAL threading for WASM (unless user has set it)
    if (!moduleInstance.ENV.GDAL_NUM_THREADS) {
        moduleInstance.ENV.GDAL_NUM_THREADS = '0';
    }
};

// Merge with user's preRun if provided
if (Module.preRun) {
    if (Array.isArray(Module.preRun)) {
        // User provided an array - prepend our default
        Module.preRun.unshift(defaultPreRun);
    } else {
        // User provided a single function - wrap both
        var userPreRun = Module.preRun;
        Module.preRun = [defaultPreRun, userPreRun];
    }
} else {
    // No user preRun - use our default
    Module.preRun = [defaultPreRun];
}
