/**
 * Geospatial Processing Module
 * Handles coordinate transforms, footprint calculations, and projection
 */

import { validateGeoTIFF, validateISD } from './validation.js';

/**
 * Detects target body from ISD and automatically switches basemap
 * @param {Object} isd - ISD JSON object
 * @param {Function} updateStatusCallback - Callback to update UI status
 */
export function detectAndSwitchTargetBody(isd, updateStatusCallback) {
    console.log('detectAndSwitchTargetBody called with ISD');

    if (!isd) {
        console.log('No ISD provided');
        return;
    }

    let bodyCode = null;
    let targetName = null;

    // Try multiple methods to detect target body

    // Method 1: Check for BODY_CODE in body_rotation
    if (isd.body_rotation && isd.body_rotation.BODY_CODE) {
        bodyCode = isd.body_rotation.BODY_CODE;
        console.log('✓ Found BODY_CODE in body_rotation:', bodyCode);
    }

    // Method 2: Check for target_name field
    if (!bodyCode && isd.target_name) {
        targetName = isd.target_name.toUpperCase();
        console.log('✓ Found target_name:', targetName);
    }

    // Method 3: Parse from name_platform (e.g., "Mars_Reconnaissance_Orbiter" -> "MARS")
    if (!bodyCode && !targetName && isd.name_platform) {
        const platform = isd.name_platform.toLowerCase();
        console.log('Parsing target from name_platform:', isd.name_platform);

        if (platform.includes('mars')) targetName = 'MARS';
        else if (platform.includes('lunar') || platform.includes('moon')) targetName = 'MOON';
        else if (platform.includes('mercury')) targetName = 'MERCURY';
        else if (platform.includes('venus')) targetName = 'VENUS';
        else if (platform.includes('jupiter')) targetName = 'JUPITER';
        else if (platform.includes('saturn')) targetName = 'SATURN';

        if (targetName) {
            console.log('✓ Parsed target from platform name:', targetName);
        }
    }

    // Method 4: Check radii values to identify body
    if (!bodyCode && !targetName && isd.radii) {
        const avgRadius = (isd.radii.semimajor + isd.radii.semiminor) / 2;
        console.log('Trying to identify body from radius:', avgRadius);

        // Match radius to known bodies (approximate)
        if (avgRadius > 3380 && avgRadius < 3400) targetName = 'MARS';
        else if (avgRadius > 1730 && avgRadius < 1750) targetName = 'MOON';
        else if (avgRadius > 6360 && avgRadius < 6380) targetName = 'EARTH';

        if (targetName) {
            console.log('✓ Identified target from radius:', targetName);
        }
    }

    if (!bodyCode && !targetName) {
        console.log('Could not detect target body from ISD');
        return;
    }

    // If we have a bodyCode but no targetName yet, map it
    if (bodyCode && !targetName) {
        // NAIF code to target name mapping (common bodies)
        const naifToTarget = {
            '199': 'MERCURY',
            '299': 'VENUS',
            '399': 'EARTH',
            '301': 'MOON',
            '499': 'MARS',
            '401': 'PHOBOS',
            '402': 'DEIMOS',
            '599': 'JUPITER',
            '501': 'IO',
            '502': 'EUROPA',
            '503': 'GANYMEDE',
            '504': 'CALLISTO',
            '699': 'SATURN',
            '601': 'MIMAS',
            '602': 'ENCELADUS',
            '603': 'TETHYS',
            '604': 'DIONE',
            '605': 'RHEA',
            '606': 'TITAN',
            '607': 'HYPERION',
            '608': 'IAPETUS',
            '799': 'URANUS',
            '899': 'NEPTUNE',
            '901': 'TRITON',
            '999': 'PLUTO',
            '2000001': 'CERES',
            '2000004': 'VESTA',
            '2000433': 'EROS'
        };

        targetName = naifToTarget[String(bodyCode)];
        if (!targetName) {
            console.log('Unknown BODY_CODE:', bodyCode, '- keeping current basemap');
            return;
        }
    }

    const planetSelector = document.getElementById('planetary-body');
    if (!planetSelector) return;

    // Check if this target exists in the dropdown
    const option = planetSelector.querySelector(`option[value="${targetName}"]`);
    if (option) {
        console.log(`✓ Auto-switching basemap from ${planetSelector.value} to ${targetName}`);
        planetSelector.value = targetName;
        // Trigger change event to reload CartoCosmos
        const event = new Event('change', { bubbles: true });
        planetSelector.dispatchEvent(event);
        updateStatusCallback(`Switched to ${targetName} basemap`, 'success');
    } else {
        console.log(`Target ${targetName} not available in basemap list`);
    }
}

/**
 * Projects GeoTIFF to equirectangular using camproject and displays footprint/raster on map
 * Uses GDAL/PROJ to ensure both footprint and raster use the same projection algorithm
 * @async
 * @param {Object} params - Projection parameters
 * @param {Object} params.minisetV8 - Miniset v8 WASM module instance
 * @param {Object} params.isdData - ISD data object
 * @param {string} params.isdUrl - ISD URL (if data not loaded yet)
 * @param {File} params.geotiffFile - GeoTIFF file object
 * @param {string} params.geotiffUrl - GeoTIFF URL
 * @param {boolean} [params.showFootprint=true] - Whether to show footprint overlay
 * @param {boolean} [params.showRaster=true] - Whether to show raster overlay
 * @param {Function} params.updateStatusCallback - Callback to update UI status
 * @param {Function} params.fetchISDCallback - Callback to fetch ISD from URL
 * @param {Function} params.sendFootprintCallback - Callback to send footprint to CartoCosmos
 * @param {Function} params.sendRasterCallback - Callback to send raster to CartoCosmos
 * @param {Function} params.displayFootprintCallback - Callback to display footprint info
 * @param {Function} params.enableLayerControlsCallback - Callback to enable layer controls
 * @param {Function} params.checkReadyCallback - Callback to check ready state
 * @returns {Promise<Object|null>} Footprint GeoJSON or null on failure
 */
export async function projectAndDisplayGeoTIFF({
    minisetV8,
    isdData,
    isdUrl,
    geotiffFile,
    geotiffUrl,
    showFootprint = true,
    showRaster = true,
    updateStatusCallback,
    fetchISDCallback,
    sendFootprintCallback,
    sendRasterCallback,
    displayFootprintCallback,
    enableLayerControlsCallback,
    checkReadyCallback
}) {
    const hasIsd = isdData !== null || isdUrl !== null;
    const hasCog = geotiffFile !== null || geotiffUrl !== null;

    if (!hasCog) {
        updateStatusCallback('❌ Requires a GeoTIFF file', 'error');
        return null;
    }

    if (!minisetV8) {
        updateStatusCallback('❌ Miniset v8 not loaded', 'error');
        return null;
    }

    if (!hasIsd) {
        updateStatusCallback('❌ Requires ISD or GeoTIFF with embedded CSM State', 'error');
        return null;
    }

    updateStatusCallback('Projecting GeoTIFF with camproject...', 'loading');

    try {
        // Fetch ISD if needed
        if (isdUrl && !isdData) {
            const success = await fetchISDCallback(isdUrl);
            if (!success) return null;
        }

        // Read GeoTIFF
        let inputData;
        const source = geotiffFile ? geotiffFile.name : geotiffUrl;
        if (geotiffFile) {
            const arrayBuffer = await geotiffFile.arrayBuffer();
            inputData = new Uint8Array(arrayBuffer);
        } else {
            try {
                // Get abort signal for cancellation support
                let signal;
                if (typeof window.createGeoTIFFAbortController === 'function') {
                    signal = window.createGeoTIFFAbortController();
                }

                const response = await fetch(geotiffUrl, { signal });
                if (!response.ok) {
                    throw new Error(`HTTP ${response.status}: ${response.statusText}`);
                }
                const arrayBuffer = await response.arrayBuffer();
                inputData = new Uint8Array(arrayBuffer);

                // Clear controller on success
                if (typeof window.clearGeoTIFFAbortController === 'function') {
                    window.clearGeoTIFFAbortController();
                }
            } catch (fetchErr) {
                // Don't show error if request was intentionally aborted
                if (fetchErr.name === 'AbortError') {
                    console.log('  GeoTIFF fetch was canceled');
                    throw fetchErr; // Re-throw to exit processing
                }

                // Better error messages for network failures
                let errorMessage = 'Failed to fetch GeoTIFF';

                if (fetchErr.name === 'TypeError' && fetchErr.message.includes('Failed to fetch')) {
                    errorMessage = 'Network error fetching GeoTIFF - check CORS, firewall, or URL accessibility';
                } else if (fetchErr.message.includes('HTTP 404')) {
                    errorMessage = 'GeoTIFF not found (HTTP 404) - verify the URL is correct';
                } else if (fetchErr.message.includes('HTTP 403')) {
                    errorMessage = 'Access denied (HTTP 403) - check permissions or authentication';
                } else if (fetchErr.message.includes('HTTP 500') || fetchErr.message.includes('HTTP 502') || fetchErr.message.includes('HTTP 503')) {
                    errorMessage = 'Server error - the remote server is having issues, try again later';
                } else {
                    errorMessage = `Failed to fetch GeoTIFF: ${fetchErr.message}`;
                }

                // Clear controller on error
                if (typeof window.clearGeoTIFFAbortController === 'function') {
                    window.clearGeoTIFFAbortController();
                }

                throw new Error(errorMessage);
            }
        }

        console.log(`  ✓ Read input GeoTIFF: ${inputData.length} bytes`);

        // Pre-flight validation
        console.log('  → Validating GeoTIFF...');
        updateStatusCallback('Validating GeoTIFF...', 'loading');
        const validation = await validateGeoTIFF(inputData, source);

        if (!validation.valid) {
            throw new Error(validation.error);
        }

        // Log warnings if any
        if (validation.warnings && validation.warnings.length > 0) {
            console.log('  ⚠️ Validation warnings:');
            validation.warnings.forEach(w => console.log('    -', w));
        }

        // Log metadata
        if (validation.metadata) {
            console.log('  ✓ GeoTIFF validation passed:', validation.metadata);
        }

        // Parse GeoTIFF to get actual image dimensions
        const GeoTIFF = window.GeoTIFF;
        const inputTiff = await GeoTIFF.fromArrayBuffer(inputData.buffer);

        // Check how many images are in the GeoTIFF
        const imageCount = await inputTiff.getImageCount();
        console.log(`  ✓ GeoTIFF contains ${imageCount} image(s)`);

        // Get the first (main) image
        const inputImage = await inputTiff.getImage(0);
        const actualWidth = inputImage.getWidth();
        const actualHeight = inputImage.getHeight();
        console.log(`  ✓ GeoTIFF image 0 dimensions: ${actualWidth} x ${actualHeight} pixels`);

        // If first image is 1x1, check if there are other images
        if (actualWidth === 1 && actualHeight === 1 && imageCount > 1) {
            console.log('  ⚠️ First image is 1x1, checking other images...');
            for (let i = 1; i < imageCount; i++) {
                const img = await inputTiff.getImage(i);
                const w = img.getWidth();
                const h = img.getHeight();
                console.log(`  Image ${i}: ${w} x ${h} pixels`);
            }
            throw new Error('GeoTIFF first image is 1x1 pixels. This may be a thumbnail/overview image. Please use the full-resolution GeoTIFF.');
        }

        if (actualWidth === 1 && actualHeight === 1) {
            throw new Error('GeoTIFF is only 1x1 pixels. This file may be corrupted or is not a valid input image.');
        }

        // Validate ISD
        console.log('  → Validating ISD...');
        const isdValidation = validateISD(isdData, 'ISD');

        if (!isdValidation.valid) {
            throw new Error(isdValidation.error);
        }

        // Log ISD warnings if any
        if (isdValidation.warnings && isdValidation.warnings.length > 0) {
            console.log('  ⚠️ ISD validation warnings:');
            isdValidation.warnings.forEach(w => console.log('    -', w));
        }

        // Update ISD with correct image dimensions if missing or incorrect
        if (!isdData.image_samples || !isdData.image_lines ||
            isdData.image_samples !== actualWidth || isdData.image_lines !== actualHeight) {
            console.log(`  ⚠️ ISD dimensions (${isdData.image_samples} x ${isdData.image_lines}) don't match GeoTIFF - updating ISD`);
            isdData.image_samples = actualWidth;
            isdData.image_lines = actualHeight;
        }

        // Write to virtual FS
        const inputPath = '/input.tif';
        const outputPath = '/output_projected.tif';
        minisetV8.FS.writeFile(inputPath, inputData);

        // Create camera model from ISD (with corrected dimensions)
        const isdString = JSON.stringify(isdData);
        const modelId = minisetV8.createCsmFromISD(isdString);
        console.log('  ✓ Created camera model:', modelId);

        // Get image dimensions and model metadata
        const imageSize = minisetV8.getImageSize(modelId);
        console.log('  Image size:', imageSize);

        // Get planetary radii for ECEF to lat/lon conversion
        const radii = minisetV8.getRadii(modelId);
        console.log('  Radii:', radii);

        // Get model metadata (optional)
        let modelName, sensorId, platformId;
        try {
            modelName = minisetV8.getModelName ? minisetV8.getModelName(modelId) : undefined;

            // Since getSensorIdentifier and getPlatformIdentifier aren't exposed in WASM,
            // extract directly from ISD
            if (isdData) {
                console.log('  ISD keys:', Object.keys(isdData).slice(0, 20));
                sensorId = isdData.sensor_identifier || isdData.instrument_id || isdData.name_sensor || undefined;
                platformId = isdData.platform_identifier || isdData.spacecraft_name || isdData.name_platform || undefined;
                console.log('  Extracted from ISD - sensorId:', sensorId, 'platformId:', platformId);
            }
        } catch (err) {
            console.log('  Note: Some metadata functions not available:', err.message);
        }

        // Define output projection (equirectangular with body-specific radii)
        // Use radii from the CSM model (extracted from ISD)
        const outputProj = `+proj=eqc +lat_ts=0 +lat_0=0 +lon_0=0 +x_0=0 +y_0=0 +a=${radii.a} +b=${radii.c} +units=m +no_defs`;
        const groundHeight = 0.0;
        console.log('  Output projection using body radii:', `a=${radii.a} b=${radii.c}`);

        // Run camproject
        console.log('  → Running camproject...');
        console.log('    modelId:', modelId);
        console.log('    inputPath:', inputPath);
        console.log('    outputProj:', outputProj);
        console.log('    outputPath:', outputPath);
        console.log('    groundHeight:', groundHeight);

        try {
            minisetV8.camproject(modelId, inputPath, outputProj, outputPath, groundHeight);
            console.log('  ✓ camproject completed');
        } catch (camprojectErr) {
            console.error('  ✗ camproject failed:', camprojectErr);
            console.error('  Exception type:', typeof camprojectErr);

            // Try to get exception message from WASM or error object
            let errorMessage = 'Projection failed';

            // Check if it's a RuntimeError or Error object with a message
            if (camprojectErr && typeof camprojectErr === 'object' && camprojectErr.message) {
                errorMessage = camprojectErr.message;
                console.error('  Error message:', errorMessage);

                // Check for specific known errors
                if (errorMessage.includes('inflateInit_')) {
                    errorMessage = 'GeoTIFF compression not supported. The WASM module lacks zlib support. Please use an uncompressed GeoTIFF.';
                } else if (errorMessage.includes('Out of memory')) {
                    const sizeMB = (inputData.length / (1024 * 1024)).toFixed(0);
                    errorMessage = `Out of memory processing ${sizeMB}MB GeoTIFF. Try using a downsampled version (e.g., gdal_translate -outsize 50% 50% input.tif output_small.tif)`;
                }
            } else if (minisetV8.getExceptionMessage && typeof camprojectErr === 'number') {
                // Try to get WASM exception message for numeric exceptions
                try {
                    errorMessage = minisetV8.getExceptionMessage(camprojectErr);
                    console.error('  WASM Exception message:', errorMessage);
                } catch (e) {
                    console.error('  Could not extract exception message:', e);
                }
            }

            // Check if files exist in WASM filesystem
            try {
                const inputExists = minisetV8.FS.analyzePath(inputPath).exists;
                console.error('  Input file exists:', inputExists);

                // Try to get input file size
                if (inputExists) {
                    const stats = minisetV8.FS.stat(inputPath);
                    console.error('  Input file size:', stats.size, 'bytes');
                }
            } catch (fsErr) {
                console.error('  Could not check filesystem:', fsErr);
            }

            // Check model validity
            try {
                const imageSizeCheck = minisetV8.getImageSize(modelId);
                console.error('  Model image size check:', imageSizeCheck);
            } catch (modelErr) {
                console.error('  Model may be invalid:', modelErr);
            }

            throw new Error(errorMessage);
        }

        // Read projected output
        const outputData = minisetV8.FS.readFile(outputPath);
        console.log(`  ✓ Read projected output: ${outputData.length} bytes`);

        // Parse projected GeoTIFF (GeoTIFF already declared above)
        const tiff = await GeoTIFF.fromArrayBuffer(outputData.buffer);
        const image = await tiff.getImage();
        const bbox = image.getBoundingBox();
        const gdalMetadata = image.getGDALMetadata();
        const width = image.getWidth();
        const height = image.getHeight();

        // Get nodata value
        let nodataValue = 0;
        if (gdalMetadata && gdalMetadata.NODATA) {
            nodataValue = parseFloat(gdalMetadata.NODATA);
        }

        console.log('  Projected bounds (meters):', bbox);
        console.log('  Projected dimensions:', width, 'x', height);
        console.log('  Nodata value:', nodataValue);

        // Read raster once to compute both data range and corners
        console.log('  → Analyzing raster data...');
        const rasterData = await image.readRasters();
        const band = rasterData[0];

        // Compute data range for proper display stretching
        let minVal = Infinity, maxVal = -Infinity;
        for (let i = 0; i < band.length; i++) {
            const val = band[i];
            if (val !== nodataValue) {
                if (val < minVal) minVal = val;
                if (val > maxVal) maxVal = val;
            }
        }
        console.log('  Data range (excluding nodata):', { min: minVal, max: maxVal });

        // Find centroid of valid pixels
        let sumX = 0, sumY = 0, count = 0;
        const sampleStep = 10;
        for (let y = 0; y < height; y += sampleStep) {
            for (let x = 0; x < width; x += sampleStep) {
                if (band[y * width + x] !== nodataValue) {
                    sumX += x;
                    sumY += y;
                    count++;
                }
            }
        }
        const centerX = sumX / count;
        const centerY = sumY / count;
        console.log('  Centroid:', [centerX, centerY]);

        // Find furthest valid pixel from center in each quadrant
        let tlX = centerX, tlY = centerY, tlDist = 0;
        let trX = centerX, trY = centerY, trDist = 0;
        let brX = centerX, brY = centerY, brDist = 0;
        let blX = centerX, blY = centerY, blDist = 0;

        for (let y = 0; y < height; y += 5) {
            for (let x = 0; x < width; x += 5) {
                if (band[y * width + x] !== nodataValue) {
                    const dist = Math.sqrt((x - centerX) ** 2 + (y - centerY) ** 2);
                    if (x < centerX && y < centerY && dist > tlDist) {
                        tlX = x; tlY = y; tlDist = dist;
                    } else if (x >= centerX && y < centerY && dist > trDist) {
                        trX = x; trY = y; trDist = dist;
                    } else if (x >= centerX && y >= centerY && dist > brDist) {
                        brX = x; brY = y; brDist = dist;
                    } else if (x < centerX && y >= centerY && dist > blDist) {
                        blX = x; blY = y; blDist = dist;
                    }
                }
            }
        }

        console.log('  Corner pixels:', { tl: [tlX, tlY], tr: [trX, trY], br: [brX, brY], bl: [blX, blY] });

        // Convert pixel coordinates to geographic coordinates
        const pixelWidth = (bbox[2] - bbox[0]) / width;
        const pixelHeight = (bbox[3] - bbox[1]) / height;

        const cornerMeters = [
            [bbox[0] + tlX * pixelWidth, bbox[3] - tlY * pixelHeight], // top-left
            [bbox[0] + trX * pixelWidth, bbox[3] - trY * pixelHeight], // top-right
            [bbox[0] + brX * pixelWidth, bbox[3] - brY * pixelHeight], // bottom-right
            [bbox[0] + blX * pixelWidth, bbox[3] - blY * pixelHeight]  // bottom-left
        ];

        // Convert to lat/lon degrees
        // WORKAROUND: camproject always outputs in Mars equirectangular meters
        // regardless of the body radii specified in the projection string.
        // This is a bug in the WASM camproject implementation.
        // TODO: Fix camproject to respect the projection radii parameter
        const marsRadiusA = 3396190;
        const bodyRadiusA = marsRadiusA; // Always use Mars radius for coordinate conversion
        const cornersLatLon = cornerMeters.map(([x, y]) => [
            x / bodyRadiusA * (180 / Math.PI), // lon
            y / bodyRadiusA * (180 / Math.PI)  // lat
        ]);

        console.log('  Corner coordinates (lon, lat):', cornersLatLon);

        // Create footprint from projected corners
        console.log('=== CSM MODEL METADATA ===');
        console.log('  modelName:', modelName);
        console.log('  sensorId:', sensorId);
        console.log('  platformId:', platformId);

        const csmFootprint = {
            type: 'Feature',
            properties: {
                name: 'Image Footprint',
                source: 'camproject',
                samples: imageSize.samples,
                lines: imageSize.lines,
                ...(modelName && { modelName }),
                ...(sensorId && { sensorId }),
                ...(platformId && { platformId })
            },
            geometry: {
                type: 'Polygon',
                coordinates: [[
                    cornersLatLon[0],
                    cornersLatLon[1],
                    cornersLatLon[2],
                    cornersLatLon[3],
                    cornersLatLon[0]
                ]]
            }
        };

        // Display footprint info in sidebar
        displayFootprintCallback(csmFootprint);

        // Send footprint to CartoCosmos if requested
        if (showFootprint) {
            sendFootprintCallback(csmFootprint);
            enableLayerControlsCallback();
        }

        // Send raster to CartoCosmos if requested
        if (showRaster) {
            // Convert full bbox to lat/lon (equirectangular: meters = radius * radians)
            const rasterMinLon = bbox[0] / bodyRadiusA * (180 / Math.PI);
            const rasterMinLat = bbox[1] / bodyRadiusA * (180 / Math.PI);
            const rasterMaxLon = bbox[2] / bodyRadiusA * (180 / Math.PI);
            const rasterMaxLat = bbox[3] / bodyRadiusA * (180 / Math.PI);

            console.log('  Raster bounds with margin:', {
                bounds: [[rasterMinLat, rasterMinLon], [rasterMaxLat, rasterMaxLon]],
                dimensions: [width, height],
                bbox_meters: bbox
            });

            sendRasterCallback({
                data: outputData.buffer,
                nodataValue: nodataValue,
                bounds: [[rasterMinLat, rasterMinLon], [rasterMaxLat, rasterMaxLon]],
                minVal: minVal,
                maxVal: maxVal
            });

            // Show loading status - wait for CartoCosmos to confirm rendering complete
            if (showFootprint && showRaster) {
                updateStatusCallback('Loading raster tiles...', 'loading');
            } else if (showRaster) {
                updateStatusCallback('Loading raster tiles...', 'loading');
            }
        }

        // Clean up
        minisetV8.FS.unlink(inputPath);
        minisetV8.FS.unlink(outputPath);
        minisetV8.deleteModel(modelId);

        // Don't set success status here - wait for CartoCosmos confirmation
        if (showFootprint && !showRaster) {
            updateStatusCallback('✓ Footprint calculated', 'success');
        }

        // Update button states
        checkReadyCallback();

        return csmFootprint;

    } catch (err) {
        console.error('  ✗ Failed:', err);

        // Extract useful error message
        let errorMsg = 'Unknown error';
        if (err && typeof err === 'object') {
            if (err.message) {
                errorMsg = err.message;
            } else if (err.toString && err.toString() !== '[object Object]') {
                errorMsg = err.toString();
            }
        } else if (typeof err === 'string') {
            errorMsg = err;
        } else if (typeof err === 'number') {
            errorMsg = 'WASM error code: ' + err;
        }

        console.error('  Error details:', errorMsg);
        updateStatusCallback('❌ Failed to project GeoTIFF: ' + errorMsg, 'error');
        return null;
    }
}

/**
 * Sets up the target selector dropdown with Astro Web Maps data
 * Fetches planetary body list and populates dropdown organized by system
 * @async
 * @param {Function} handlePlanetChangeCallback - Callback for planet change events
 */
export async function setupTargetSelector(handlePlanetChangeCallback) {
    console.log('setupTargetSelector called');
    const planetSelector = document.getElementById('planetary-body');
    if (!planetSelector) {
        console.error('planetary-body element not found!');
        return;
    }
    console.log('Found planetSelector:', planetSelector);

    // Add change listener first (so it always works)
    planetSelector.addEventListener('change', handlePlanetChangeCallback);

    // Organize targets by system (deduplicate by name)
    function organizeTargets(astroWebMaps) {
        const targetMap = new Map();

        for (const target of astroWebMaps.targets) {
            // Skip if target name is missing
            if (!target.name) continue;

            // Only add if not already in map (deduplicates)
            if (!targetMap.has(target.name)) {
                targetMap.set(target.name, {
                    name: target.name,
                    system: target.system || 'OTHER',
                    naif: target.naif,
                    layers: target.webmap ? target.webmap.length : 0
                });
            }
        }

        // Convert map to array
        const targets = Array.from(targetMap.values());

        // Sort by system, then by name
        targets.sort((a, b) => {
            if (a.system !== b.system) {
                return (a.system || '').localeCompare(b.system || '');
            }
            return (a.name || '').localeCompare(b.name || '');
        });
        return targets;
    }

    // Fetch Astro Web Maps data
    try {
        console.log('Fetching Astro Web Maps data...');
        const response = await fetch('https://astrowebmaps.wr.usgs.gov/webmapatlas/Layers/maps.json');
        const data = await response.json();
        const targets = organizeTargets(data);

        console.log('Organizing', targets.length, 'targets...');

        // Save current value
        const currentValue = planetSelector.value || 'Mars';

        // Clear existing options
        planetSelector.innerHTML = '';

        // Add targets organized by system
        let currentSystem = null;
        let optgroup = null;

        targets.forEach(target => {
            // Create new optgroup for each system
            if (target.system !== currentSystem) {
                currentSystem = target.system;
                optgroup = document.createElement('optgroup');
                optgroup.label = currentSystem;
                planetSelector.appendChild(optgroup);
            }

            // Add option to current optgroup
            const option = document.createElement('option');
            option.value = target.name;
            option.textContent = target.name;
            optgroup.appendChild(option);
        });

        // Default to Mars (must match iframe default)
        if (planetSelector.querySelector('option[value="MARS"]')) {
            planetSelector.value = 'MARS';
        } else if (planetSelector.querySelector('option[value="Mars"]')) {
            planetSelector.value = 'Mars';
        } else if (planetSelector.querySelector(`option[value="${currentValue}"]`)) {
            planetSelector.value = currentValue;
        }

        console.log('✓ Loaded', targets.length, 'planetary targets from Astro Web Maps');
    } catch (err) {
        console.error('Failed to load Astro Web Maps:', err);
        // Keep default Mars option on error (HTML already has it)
        console.log('Using fallback Mars option');
    }
}

console.log('✓ Geospatial module loaded');
