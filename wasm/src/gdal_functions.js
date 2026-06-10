/**
 * GDAL C function wrappers using cwrap
 * Based on gdal3.js approach - calls GDAL C API directly from JavaScript
 */

export const GDALFunctions = {
    Module: null,
};

/**
 * Initialize GDAL C function wrappers
 * Must be called after Module is loaded
 */
export function initGDALFunctions(Module) {
    if (GDALFunctions.GDALOpen) return; // Already initialized

    GDALFunctions.Module = Module;

    // Register GDAL drivers
    Module.ccall('GDALAllRegister', null, [], []);

    // Configure GDAL environment
    Module.ENV.GDAL_DATA = '/usr/share/gdal';
    Module.ENV.GDAL_NUM_THREADS = '0'; // Disable threading
    Module.ENV.PROJ_LIB = '/usr/share/proj';

    console.log('GDAL/PROJ environment configured');
    console.log('  GDAL_DATA:', Module.ENV.GDAL_DATA);
    console.log('  PROJ_LIB:', Module.ENV.PROJ_LIB);

    // Wrap GDAL C functions
    GDALFunctions.GDALOpen = Module.cwrap('GDALOpen', 'number', ['string', 'number']);
    GDALFunctions.GDALClose = Module.cwrap('GDALClose', null, ['number']);
    GDALFunctions.GDALGetRasterXSize = Module.cwrap('GDALGetRasterXSize', 'number', ['number']);
    GDALFunctions.GDALGetRasterYSize = Module.cwrap('GDALGetRasterYSize', 'number', ['number']);
    GDALFunctions.GDALGetRasterCount = Module.cwrap('GDALGetRasterCount', 'number', ['number']);
    GDALFunctions.GDALGetRasterBand = Module.cwrap('GDALGetRasterBand', 'number', ['number', 'number']);

    // Spatial reference functions (the key ones that work in JavaScript but not C++)
    GDALFunctions.GDALGetProjectionRef = Module.cwrap('GDALGetProjectionRef', 'string', ['number']);
    GDALFunctions.GDALSetProjection = Module.cwrap('GDALSetProjection', 'number', ['number', 'string']);
    GDALFunctions.GDALGetGeoTransform = Module.cwrap('GDALGetGeoTransform', 'number', ['number', 'number']);
    GDALFunctions.GDALSetGeoTransform = Module.cwrap('GDALSetGeoTransform', 'number', ['number', 'number']);

    // OSR (OGR Spatial Reference) functions
    GDALFunctions.OSRNewSpatialReference = Module.cwrap('OSRNewSpatialReference', 'number', ['string']);
    GDALFunctions.OSRDestroySpatialReference = Module.cwrap('OSRDestroySpatialReference', null, ['number']);
    GDALFunctions.OSRSetFromUserInput = Module.cwrap('OSRSetFromUserInput', 'number', ['number', 'string']);
    GDALFunctions.OSRExportToWkt = Module.cwrap('OSRExportToWkt', 'number', ['number', 'number']);
    GDALFunctions.OSRGetSemiMajor = Module.cwrap('OSRGetSemiMajor', 'number', ['number', 'number']);
    GDALFunctions.OSRGetSemiMinor = Module.cwrap('OSRGetSemiMinor', 'number', ['number', 'number']);

    // CPL (Common Portability Library) functions
    GDALFunctions.CPLSetConfigOption = Module.cwrap('CPLSetConfigOption', null, ['string', 'string']);
    GDALFunctions.CPLGetLastErrorMsg = Module.cwrap('CPLGetLastErrorMsg', 'string', []);
    GDALFunctions.CPLGetLastErrorNo = Module.cwrap('CPLGetLastErrorNo', 'number', []);
    GDALFunctions.CPLErrorReset = Module.cwrap('CPLErrorReset', null, []);

    console.log('GDAL C functions initialized');
}

/**
 * Extract spatial reference from a DEM file as WKT string
 * This is the key function - it works in JavaScript but crashes in C++
 */
export function extractSpatialReference(filepath) {
    if (!GDALFunctions.GDALOpen) {
        throw new Error('GDAL functions not initialized. Call initGDALFunctions first.');
    }

    const Module = GDALFunctions.Module;
    const GA_ReadOnly = 0;

    // Open dataset
    const dataset = GDALFunctions.GDALOpen(filepath, GA_ReadOnly);
    if (!dataset) {
        const error = GDALFunctions.CPLGetLastErrorMsg();
        throw new Error(`Failed to open ${filepath}: ${error}`);
    }

    try {
        // Get projection as WKT string (this works in JavaScript!)
        const wkt = GDALFunctions.GDALGetProjectionRef(dataset);

        if (!wkt || wkt.length === 0) {
            console.warn(`No projection found in ${filepath}`);
            return null;
        }

        console.log(`Extracted WKT from ${filepath}: ${wkt.substring(0, 100)}...`);
        return wkt;
    } finally {
        // Always close the dataset
        GDALFunctions.GDALClose(dataset);
    }
}

/**
 * Parse ellipsoid parameters from WKT string using OSR
 */
export function parseEllipsoidFromWKT(wkt) {
    if (!GDALFunctions.OSRNewSpatialReference) {
        throw new Error('GDAL functions not initialized');
    }

    const Module = GDALFunctions.Module;

    // Create spatial reference from WKT
    const srs = GDALFunctions.OSRNewSpatialReference(wkt);
    if (!srs) {
        throw new Error('Failed to create spatial reference from WKT');
    }

    try {
        // Extract ellipsoid parameters
        const errPtr = Module._malloc(4); // int* for error code

        const semiMajor = GDALFunctions.OSRGetSemiMajor(srs, errPtr);
        const majorErr = Module.getValue(errPtr, 'i32');

        const semiMinor = GDALFunctions.OSRGetSemiMinor(srs, errPtr);
        const minorErr = Module.getValue(errPtr, 'i32');

        Module._free(errPtr);

        if (majorErr !== 0 || minorErr !== 0) {
            throw new Error('Failed to extract ellipsoid parameters');
        }

        console.log(`Ellipsoid: a=${semiMajor}, b=${semiMinor}`);

        return {
            semiMajor,
            semiMinor,
            isValid: semiMajor > 0 && semiMinor > 0,
        };
    } finally {
        GDALFunctions.OSRDestroySpatialReference(srs);
    }
}

/**
 * Extract ellipsoid directly from a DEM file
 */
export function extractEllipsoidFromDEM(filepath) {
    const wkt = extractSpatialReference(filepath);

    if (!wkt) {
        // Return default WGS84
        console.warn('No spatial reference, using WGS84');
        return {
            semiMajor: 6378137.0,
            semiMinor: 6356752.314245,
            isValid: false,
        };
    }

    return parseEllipsoidFromWKT(wkt);
}
