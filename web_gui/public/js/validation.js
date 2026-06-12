/**
 * Pre-flight Validation Module
 * Validates files and inputs before processing to catch errors early
 */

/**
 * Validates a GeoTIFF file before processing
 * @async
 * @param {Uint8Array} data - GeoTIFF file data
 * @param {string} source - Source description (filename or URL)
 * @returns {Promise<{valid: boolean, error?: string, warnings?: string[], metadata?: Object}>}
 */
export async function validateGeoTIFF(data, source = 'GeoTIFF') {
    const result = {
        valid: false,
        warnings: [],
        metadata: {}
    };

    try {
        const GeoTIFF = window.GeoTIFF;
        if (!GeoTIFF) {
            result.error = 'GeoTIFF.js library not loaded';
            return result;
        }

        // Check minimum file size (a valid TIFF header is at least 8 bytes)
        if (data.length < 8) {
            result.error = `File too small (${data.length} bytes). Not a valid GeoTIFF.`;
            return result;
        }

        // Check TIFF magic number (first 4 bytes)
        // Little-endian TIFF: 0x49 0x49 0x2A 0x00 ("II" + 42 in little-endian)
        // Big-endian TIFF:    0x4D 0x4D 0x00 0x2A ("MM" + 42 in big-endian)
        const byte0 = data[0];
        const byte1 = data[1];
        const byte2 = data[2];
        const byte3 = data[3];

        const isLittleEndianTIFF = (byte0 === 0x49 && byte1 === 0x49 && byte2 === 0x2A && byte3 === 0x00);
        const isBigEndianTIFF = (byte0 === 0x4D && byte1 === 0x4D && byte2 === 0x00 && byte3 === 0x2A);

        if (!isLittleEndianTIFF && !isBigEndianTIFF) {
            result.error = 'Not a valid TIFF file (invalid magic number). Expected TIFF header, got invalid signature.';
            return result;
        }

        // Parse GeoTIFF
        let tiff, image, imageCount;
        try {
            tiff = await GeoTIFF.fromArrayBuffer(data.buffer);
            imageCount = await tiff.getImageCount();
            image = await tiff.getImage(0);
        } catch (parseErr) {
            result.error = `Failed to parse GeoTIFF: ${parseErr.message}`;
            return result;
        }

        const width = image.getWidth();
        const height = image.getHeight();
        const fileSize = data.length;
        const fileSizeMB = (fileSize / (1024 * 1024)).toFixed(1);

        result.metadata = {
            width,
            height,
            fileSize,
            fileSizeMB,
            imageCount
        };

        // Validation checks

        // 1. Check for 1x1 placeholder images
        if (width === 1 && height === 1) {
            if (imageCount > 1) {
                result.error = 'GeoTIFF first image is 1x1 pixels (likely a thumbnail). This tool requires the full-resolution image as the first/primary image in the file.';
            } else {
                result.error = 'GeoTIFF is only 1x1 pixel. This file may be a placeholder, corrupted, or not a valid input image.';
            }
            return result;
        }

        // 2. Check for suspiciously small images
        if (width < 10 || height < 10) {
            result.warnings.push(`Very small image dimensions: ${width}×${height} pixels. Verify this is the correct file.`);
        }

        // 3. Check file size vs. expected size (rough heuristic)
        const bitsPerSample = image.getBitsPerSample ? image.getBitsPerSample()[0] : 8;
        const samplesPerPixel = image.getSamplesPerPixel();
        const compression = image.getCompression ? image.getCompression() : 1;

        result.metadata.bitsPerSample = bitsPerSample;
        result.metadata.samplesPerPixel = samplesPerPixel;
        result.metadata.compression = compression;

        // Expected uncompressed size
        const expectedUncompressedSize = width * height * samplesPerPixel * (bitsPerSample / 8);
        const compressionRatio = expectedUncompressedSize / fileSize;

        // 4. Check for compression (WASM module might not support all types)
        if (compression !== 1) { // 1 = no compression
            const compressionNames = {
                1: 'None',
                2: 'CCITT Group 3',
                5: 'LZW',
                7: 'JPEG',
                8: 'Deflate',
                32773: 'PackBits'
            };
            const compressionName = compressionNames[compression] || `Unknown (${compression})`;
            result.warnings.push(`GeoTIFF uses ${compressionName} compression. If processing fails, create an uncompressed version: gdal_translate -co COMPRESS=NONE input.tif output.tif`);
        }

        // 5. Check file size warnings
        if (fileSizeMB > 500) {
            result.warnings.push(`Large file (${fileSizeMB}MB). Processing may fail due to WASM memory limits (recommended: <200MB). Consider downsampling: gdal_translate -outsize 50% 50% input.tif output_small.tif`);
        } else if (fileSizeMB > 200) {
            result.warnings.push(`File size is ${fileSizeMB}MB. May approach WASM memory limits. If processing fails, try downsampling.`);
        }

        // 6. Check for multiple images (COG structure)
        if (imageCount > 1) {
            result.metadata.hasPyramids = true;
            result.warnings.push(`GeoTIFF contains ${imageCount} images (likely COG with overview pyramids). Only the first full-resolution image will be processed.`);
        }

        // 7. Check for extreme aspect ratios (pushbroom line scanners)
        const aspectRatio = Math.max(width, height) / Math.min(width, height);
        if (aspectRatio > 100) {
            result.metadata.aspectRatio = aspectRatio.toFixed(1);
            result.warnings.push(`Extreme aspect ratio (${width}×${height}, ratio: ${aspectRatio.toFixed(1)}:1). This appears to be a pushbroom line scanner image, which is supported but may be slow to process.`);
        }

        // All checks passed
        result.valid = true;
        return result;

    } catch (err) {
        result.error = `Validation error: ${err.message}`;
        return result;
    }
}

/**
 * Validates an ISD JSON file before processing
 * @param {Object} isdData - Parsed ISD JSON object
 * @param {string} source - Source description (filename or URL)
 * @returns {{valid: boolean, error?: string, warnings?: string[], metadata?: Object}}
 */
export function validateISD(isdData, source = 'ISD') {
    const result = {
        valid: false,
        warnings: [],
        metadata: {}
    };

    // Check if isdData is an object
    if (!isdData || typeof isdData !== 'object') {
        result.error = 'ISD must be a valid JSON object';
        return result;
    }

    // Check for required fields (depends on CSM model type)
    const requiredFields = ['image_samples', 'image_lines'];
    const missingFields = [];

    for (const field of requiredFields) {
        if (!(field in isdData)) {
            missingFields.push(field);
        }
    }

    // image_samples/image_lines are optional if embedded in GeoTIFF
    // but warn if missing
    if (missingFields.length > 0) {
        result.warnings.push(`ISD missing recommended fields: ${missingFields.join(', ')}. These will be auto-detected from the GeoTIFF if not provided.`);
    }

    // Check image dimensions if present
    if (isdData.image_samples && isdData.image_lines) {
        result.metadata.image_samples = isdData.image_samples;
        result.metadata.image_lines = isdData.image_lines;

        if (isdData.image_samples < 1 || isdData.image_lines < 1) {
            result.error = `Invalid image dimensions in ISD: ${isdData.image_samples}×${isdData.image_lines}. Must be positive integers.`;
            return result;
        }

        // Check for suspicious 1x1 dimensions
        if (isdData.image_samples === 1 && isdData.image_lines === 1) {
            result.warnings.push('ISD specifies 1×1 pixel image. This is unusual - verify the ISD matches your GeoTIFF.');
        }
    }

    // Check for body/target information
    const hasBodyInfo = isdData.body_rotation?.BODY_CODE ||
                        isdData.target_name ||
                        isdData.name_platform ||
                        isdData.radii;

    if (!hasBodyInfo) {
        result.warnings.push('ISD missing target body information (BODY_CODE, target_name, or radii). Basemap may not auto-switch.');
    }

    // Check for radii
    if (isdData.radii) {
        result.metadata.radii = isdData.radii;
        if (!isdData.radii.semimajor || !isdData.radii.semiminor) {
            result.warnings.push('ISD radii incomplete (missing semimajor or semiminor).');
        }
    } else {
        result.warnings.push('ISD missing radii information. Projection may fail.');
    }

    // Check for sensor/platform metadata (optional but helpful)
    if (!isdData.name_sensor && !isdData.sensor_identifier && !isdData.instrument_id) {
        result.warnings.push('ISD missing sensor/instrument identification.');
    }

    if (!isdData.name_platform && !isdData.platform_identifier && !isdData.spacecraft_name) {
        result.warnings.push('ISD missing spacecraft/platform identification.');
    }

    // All checks passed
    result.valid = true;
    return result;
}

/**
 * Validates a URL for fetching remote files
 * @param {string} url - URL to validate
 * @param {string[]} allowedExtensions - Allowed file extensions (e.g., ['.tif', '.tiff'])
 * @returns {{valid: boolean, error?: string, warnings?: string[]}}
 */
export function validateURL(url, allowedExtensions = []) {
    const result = {
        valid: false,
        warnings: []
    };

    // Check if URL is empty
    if (!url || url.trim() === '') {
        result.error = 'URL cannot be empty';
        return result;
    }

    // Check if URL is valid
    let urlObj;
    try {
        urlObj = new URL(url);
    } catch (err) {
        result.error = 'Invalid URL format';
        return result;
    }

    // Check protocol (must be http or https)
    if (!['http:', 'https:'].includes(urlObj.protocol)) {
        result.error = `Unsupported protocol: ${urlObj.protocol}. Only HTTP and HTTPS are supported.`;
        return result;
    }

    // Check hostname validity
    const hasValidHostname = urlObj.hostname.includes('.') || urlObj.hostname === 'localhost';
    if (!hasValidHostname) {
        result.error = 'Invalid hostname. Must be a valid domain or "localhost".';
        return result;
    }

    // Check file extension if required
    if (allowedExtensions.length > 0) {
        const pathname = urlObj.pathname.toLowerCase();
        const hasValidExtension = allowedExtensions.some(ext => pathname.endsWith(ext.toLowerCase()));

        if (!hasValidExtension) {
            result.error = `URL must end with one of: ${allowedExtensions.join(', ')}`;
            return result;
        }
    }

    // Warn about localhost (CORS issues possible)
    if (urlObj.hostname === 'localhost' || urlObj.hostname === '127.0.0.1') {
        result.warnings.push('Using localhost URL. Ensure CORS is properly configured on the server.');
    }

    // Warn about HTTP (not HTTPS)
    if (urlObj.protocol === 'http:') {
        result.warnings.push('Using unencrypted HTTP connection. HTTPS is recommended for security.');
    }

    result.valid = true;
    return result;
}

/**
 * Formats validation results for display
 * @param {{valid: boolean, error?: string, warnings?: string[], metadata?: Object}} validation
 * @returns {string} Formatted message for user display
 */
export function formatValidationMessage(validation) {
    if (!validation.valid && validation.error) {
        return `❌ ${validation.error}`;
    }

    const parts = [];

    if (validation.warnings && validation.warnings.length > 0) {
        parts.push('⚠️ ' + validation.warnings.join('\n⚠️ '));
    }

    if (validation.metadata) {
        const meta = validation.metadata;
        if (meta.width && meta.height) {
            parts.push(`📐 Dimensions: ${meta.width}×${meta.height} pixels`);
        }
        if (meta.fileSizeMB) {
            parts.push(`💾 Size: ${meta.fileSizeMB}MB`);
        }
    }

    return parts.join('\n');
}

console.log('✓ Validation module loaded');
