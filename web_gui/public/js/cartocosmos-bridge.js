/**
 * CartoCosmos Bridge Module
 * Handles iframe communication with the CartoCosmos mapping interface
 */

// Bridge state
let cartoCosmosReady = false;
let cartoCosmosFrame = null;
let hasLoadedOnce = false; // Track if map has loaded at least once

// Store last sent layers to re-send after basemap changes
let lastFootprintData = null;
let lastRasterData = null;
let lastRasterType = null; // 'url', 'file', or 'projected'

/**
 * Sets up CartoCosmos iframe communication and readiness detection
 * Uses polling to detect when CartoCosmos is ready to receive messages
 * @param {HTMLIFrameElement} frameElement - CartoCosmos iframe element
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 */
export function setupCartoCosmos(frameElement, updateStatusCallback, checkReadyCallback) {
    cartoCosmosFrame = frameElement;

    window.addEventListener('message', (e) => {
        console.log('→ Message from CartoCosmos:', typeof e.data, e.data);

        if (typeof e.data === 'string') {
            if (e.data === 'mapLoad Complete' || e.data === 'pong') {
                if (!cartoCosmosReady) {
                    console.log('✓ CartoCosmos map loaded and ready');
                    cartoCosmosReady = true;
                    updateStatusCallback('✓ CartoCosmos map ready', 'success');

                    // Re-check button states now that CartoCosmos is ready
                    checkReadyCallback();
                }

                // If this is not the first load and we get mapLoad Complete,
                // it means the map was reloaded (e.g., basemap change or planet change)
                // Re-send stored layers if we have any
                if (e.data === 'mapLoad Complete' && hasLoadedOnce && (lastFootprintData || lastRasterData)) {
                    console.log('[CARTOCOSMOS] Map reloaded detected, re-sending layers...');
                    setTimeout(() => resendStoredLayers(), 500); // Small delay to ensure map is fully ready
                }

                // Mark that we've loaded at least once
                if (e.data === 'mapLoad Complete') {
                    hasLoadedOnce = true;
                }
            }
            else if (e.data.includes('setWkt')) {
                const receivedWkt = e.data.replace(/^setWkt\s/i, '');
                console.log('← Received WKT from CartoCosmos:', receivedWkt);
            }
        }
        // Handle object messages from CartoCosmos
        else if (typeof e.data === 'object' && e.data !== null) {
            console.log('CartoCosmos object message:', e.data);

            if (e.data.type === 'geoTiffSuccess') {
                console.log('[CARTOCOSMOS] Received geoTiffSuccess message at', Date.now());
                const statusEl = document.getElementById('status-text');
                const currentStatus = statusEl?.textContent || '';

                // Check if we're in a loading state for raster
                if (currentStatus.includes('Loading raster tiles') ||
                    currentStatus.includes('Processing raster')) {
                    // Check if footprint is also visible
                    const toggleFootprint = document.getElementById('toggle-footprint');
                    const footprintVisible = toggleFootprint && toggleFootprint.checked;

                    if (footprintVisible) {
                        console.log('[CARTOCOSMOS] Updating status to footprint and raster displayed');
                        updateStatusCallback('✓ Footprint and raster displayed', 'success');
                    } else {
                        console.log('[CARTOCOSMOS] Updating status to raster displayed');
                        updateStatusCallback('✓ Raster overlay displayed', 'success');
                    }
                } else {
                    console.log('[CARTOCOSMOS] Skipping status update, current status:', currentStatus);
                }
            }
            else if (e.data.type === 'geoTiffError') {
                console.error('✗ CartoCosmos failed to load raster:', e.data.error);
                const errorMsg = e.data.error || 'Unknown error';
                updateStatusCallback(`✗ Raster overlay failed: ${errorMsg}`, 'error');
            }
            else if (e.data.type === 'basemapChanged') {
                console.log('[CARTOCOSMOS] Basemap changed, re-sending layers...');
                // Re-send stored layers after basemap change
                resendStoredLayers();
            }
        }
    }, false);

    // Polling to detect CartoCosmos readiness
    // This handles race conditions where CartoCosmos sends 'mapLoad Complete' before we're listening
    let pollAttempts = 0;
    const maxPollAttempts = 6; // 6 attempts * 500ms = 3 seconds max

    const pollCartoCosmos = setInterval(() => {
        pollAttempts++;

        if (cartoCosmosReady) {
            // Already ready, stop polling
            clearInterval(pollCartoCosmos);
            console.log('✓ CartoCosmos ready');
            return;
        }

        // Try to ping CartoCosmos
        if (cartoCosmosFrame && cartoCosmosFrame.contentWindow) {
            try {
                cartoCosmosFrame.contentWindow.postMessage({ type: 'ping' }, '*');
                // Only log every 3rd attempt to reduce noise
                if (pollAttempts % 3 === 0) {
                    console.log(`Waiting for CartoCosmos... (${pollAttempts * 0.5}s)`);
                }
            } catch (err) {
                console.warn('Failed to ping CartoCosmos:', err);
            }
        }

        // After max attempts, assume ready
        if (pollAttempts >= maxPollAttempts) {
            clearInterval(pollCartoCosmos);
            if (!cartoCosmosReady) {
                console.log('CartoCosmos ready (timeout)');
                cartoCosmosReady = true;
                updateStatusCallback('✓ Ready to load data', 'success');
                checkReadyCallback();
            }
        }
    }, 500); // Poll every 500ms

    // Listen for iframe load
    cartoCosmosFrame.addEventListener('load', () => {
        console.log('✓ CartoCosmos iframe loaded');
        console.log('  Current iframe src:', cartoCosmosFrame.src);
        updateStatusCallback('Waiting for CartoCosmos map...', 'loading');
    });

    cartoCosmosFrame.addEventListener('error', (e) => {
        console.error('✗ CartoCosmos iframe failed to load:', e);
        updateStatusCallback('Error loading CartoCosmos', 'error');
    });
}

/**
 * Handles planetary body selection change
 * @param {Event} e - Select change event
 * @param {Function} updateStatusCallback - Callback to update UI status
 */
export function handlePlanetChange(e, updateStatusCallback) {
    const target = e.target.value;
    console.log('Changing planetary body to:', target);

    // Reset ready flag when changing planets
    cartoCosmosReady = false;

    // Change CartoCosmos iframe URL with target parameter (local instance)
    const baseUrl = 'http://localhost:8000';
    const newSrc = `${baseUrl}?target=${target}&hideControls=true`;
    console.log('Setting iframe src to:', newSrc);
    cartoCosmosFrame.src = newSrc;

    updateStatusCallback(`Loading ${target} basemap...`, 'loading');
}

/**
 * Sends a GeoTIFF URL to CartoCosmos for raster overlay display
 * @param {string} url - URL or blob URL of the GeoTIFF
 */
export function sendGeoTIFFToCartoCosmos(url) {
    if (!cartoCosmosFrame) {
        console.error('CartoCosmos frame not found');
        return;
    }

    console.log('→ Sending COG to CartoCosmos...');
    console.log('  Original URL:', url);

    // Store for re-sending after basemap changes
    lastRasterType = 'url';
    lastRasterData = url;

    // Route through proxy if it's an external S3 URL to bypass CORS
    let proxyUrl = url;
    if (url.startsWith('http') && !url.includes('localhost')) {
        proxyUrl = `http://localhost:8001/proxy?url=${encodeURIComponent(url)}`;
        console.log('  Using proxy URL:', proxyUrl);
    }

    const planetaryBody = document.getElementById('planetary-body')?.value || 'Mars';
    const opacitySlider = document.getElementById('opacity-slider');
    const opacity = opacitySlider ? (opacitySlider.value / 100) : 1.0;

    const message = {
        type: 'addRaster',
        url: proxyUrl,
        opacity: opacity,
        planetaryBody: planetaryBody,
        maxWidth: 2048,
        nodataTransparent: true
    };

    console.log('  Message:', JSON.stringify(message, null, 2));

    cartoCosmosFrame.contentWindow.postMessage(message, '*');

    console.log('✓ COG overlay message sent to CartoCosmos');
}

/**
 * Sends a GeoTIFF file as ArrayBuffer to CartoCosmos
 * @async
 * @param {File} file - GeoTIFF file object
 * @param {Function} updateStatusCallback - Callback to update UI status
 */
export async function sendGeoTIFFFileToCartoCosmos(file, updateStatusCallback) {
    if (!cartoCosmosFrame) {
        console.error('CartoCosmos frame not found');
        return;
    }

    console.log('→ Sending COG file to CartoCosmos as ArrayBuffer...');
    console.log('  File:', file.name, `(${(file.size / 1024 / 1024).toFixed(2)} MB)`);

    try {
        // Read file as ArrayBuffer
        const arrayBuffer = await file.arrayBuffer();

        // Store for re-sending after basemap changes (keep a copy since ArrayBuffer is transferred)
        lastRasterType = 'file';
        lastRasterData = file;

        const planetaryBody = document.getElementById('planetary-body')?.value || 'Mars';
        const opacitySlider = document.getElementById('opacity-slider');
        const opacity = opacitySlider ? (opacitySlider.value / 100) : 1.0;

        // Send ArrayBuffer to CartoCosmos iframe
        cartoCosmosFrame.contentWindow.postMessage({
            type: 'addRaster',
            data: arrayBuffer,
            opacity: opacity,
            planetaryBody: planetaryBody,
            maxWidth: 2048,
            nodataTransparent: true
        }, '*', [arrayBuffer]);  // Transfer ArrayBuffer

        console.log('✓ COG file sent to CartoCosmos');
    } catch (err) {
        console.error('✗ Failed to send COG file:', err);
        updateStatusCallback(`✗ Error sending COG file: ${err.message}`, 'error');
    }
}

/**
 * Updates the opacity of the raster overlay in CartoCosmos
 * @param {number} opacity - Opacity value between 0 and 1
 */
export function updateCartoCosmoOpacity(opacity) {
    if (!cartoCosmosFrame) {
        console.error('CartoCosmos frame not found');
        return;
    }

    console.log('→ Updating CartoCosmos opacity:', opacity);

    cartoCosmosFrame.contentWindow.postMessage({
        type: 'setRasterOpacity',
        opacity: opacity
    }, '*');
}

/**
 * Sends footprint to CartoCosmos as GeoJSON
 * @param {Object} footprintGeoJSON - GeoJSON footprint object
 */
export function sendFootprintToCartoCosmos(footprintGeoJSON) {
    if (!cartoCosmosFrame || !cartoCosmosFrame.contentWindow) {
        console.error('✗ CartoCosmos iframe not available');
        return;
    }

    // Store for re-sending after basemap changes
    lastFootprintData = footprintGeoJSON;

    cartoCosmosFrame.contentWindow.postMessage({
        type: 'addFootprint',
        footprint: footprintGeoJSON
    }, '*');
    console.log('  ✓ Sent footprint to CartoCosmos');
}

/**
 * Sends projected raster to CartoCosmos as ArrayBuffer
 * @param {Object} params - Raster parameters
 * @param {ArrayBuffer} params.data - Raster data as ArrayBuffer
 * @param {number} params.nodataValue - Nodata value
 * @param {Array<Array<number>>} params.bounds - [[minLat, minLon], [maxLat, maxLon]]
 * @param {number} params.minVal - Minimum pixel value
 * @param {number} params.maxVal - Maximum pixel value
 */
export function sendProjectedRasterToCartoCosmos({ data, nodataValue, bounds, minVal, maxVal }) {
    if (!cartoCosmosFrame || !cartoCosmosFrame.contentWindow) {
        console.error('✗ CartoCosmos iframe not available');
        return;
    }

    // Store for re-sending after basemap changes (clone the ArrayBuffer since it will be transferred)
    lastRasterType = 'projected';
    lastRasterData = {
        data: data.slice(0), // Clone the ArrayBuffer
        nodataValue,
        bounds,
        minVal,
        maxVal
    };

    const planetaryBody = document.getElementById('planetary-body')?.value || 'Mars';
    const opacitySlider = document.getElementById('opacity-slider');
    const opacity = opacitySlider ? (opacitySlider.value / 100) : 1.0;

    // WORKAROUND: camproject always outputs in Mars equirectangular coordinates
    // regardless of body radii, so we tell CartoCosmos to use MARS projection
    // even for non-Mars bodies to get correct coordinate alignment
    const projectionBody = 'MARS';
    console.log(`  Note: Using ${projectionBody} projection for coordinates (actual body: ${planetaryBody})`);

    cartoCosmosFrame.contentWindow.postMessage({
        type: 'addRaster',
        data: data,
        nodataValue: nodataValue,
        isProjected: true,
        bounds: bounds,
        opacity: opacity,
        planetaryBody: projectionBody, // Use MARS for coordinate system
        nodataTransparent: true,
        pixelValMin: minVal,
        pixelValMax: maxVal
    }, '*', [data]); // Transfer ArrayBuffer
    console.log('  ✓ Sent projected raster to CartoCosmos');
}

/**
 * Sends WKT (Well-Known Text) geometry to CartoCosmos for footprint display
 * @param {string} wkt - WKT geometry string
 */
export function sendWKTToCartoCosmos(wkt) {
    if (!cartoCosmosFrame || !cartoCosmosFrame.contentWindow) {
        console.error('✗ CartoCosmos iframe not available');
        return;
    }

    const message = 'drawWkt ' + wkt;
    console.log('→ Sending to CartoCosmos:', message);
    cartoCosmosFrame.contentWindow.postMessage(message, '*');
    console.log('✓ WKT sent to CartoCosmos for drawing');
}

/**
 * Requests CartoCosmos to toggle footprint visibility
 * @param {boolean} visible - Whether footprint should be visible
 */
export function toggleFootprintVisibility(visible) {
    if (cartoCosmosFrame && cartoCosmosFrame.contentWindow) {
        cartoCosmosFrame.contentWindow.postMessage({
            type: 'toggleFootprint',
            visible: visible
        }, '*');
        console.log(`Footprint visibility: ${visible}`);
    }
}

/**
 * Requests CartoCosmos to toggle raster visibility
 * @param {boolean} visible - Whether raster should be visible
 */
export function toggleRasterVisibility(visible) {
    if (cartoCosmosFrame && cartoCosmosFrame.contentWindow) {
        cartoCosmosFrame.contentWindow.postMessage({
            type: 'toggleRaster',
            visible: visible
        }, '*');
        console.log(`Raster visibility: ${visible}`);
    }
}

/**
 * Requests CartoCosmos to zoom to footprint bounds
 */
export function zoomToFootprint() {
    if (cartoCosmosFrame && cartoCosmosFrame.contentWindow) {
        cartoCosmosFrame.contentWindow.postMessage({
            type: 'zoomToFootprint'
        }, '*');
        console.log('Zoom to layers requested');
    }
}

/**
 * Re-sends stored layers after basemap change
 * @private
 */
function resendStoredLayers() {
    console.log('[CARTOCOSMOS] Re-sending stored layers...');
    console.log('[CARTOCOSMOS] Stored layer state:', {
        hasFootprint: !!lastFootprintData,
        hasRaster: !!lastRasterData,
        rasterType: lastRasterType
    });

    // Check if raster toggle is enabled
    const rasterToggle = document.getElementById('toggle-raster');
    const rasterVisible = rasterToggle ? rasterToggle.checked : true;
    console.log('[CARTOCOSMOS] Raster toggle checked:', rasterVisible);

    // Re-send footprint if available
    if (lastFootprintData) {
        console.log('  → Re-sending footprint');
        const footprintToggle = document.getElementById('toggle-footprint');
        const footprintVisible = footprintToggle ? footprintToggle.checked : true;

        if (footprintVisible) {
            sendFootprintToCartoCosmos(lastFootprintData);
        } else {
            console.log('  ⊗ Footprint toggle is off, skipping');
        }
    } else {
        console.log('  ⊗ No footprint data stored');
    }

    // Re-send raster if available and visible
    if (lastRasterData && lastRasterType) {
        console.log(`  → Re-sending raster (type: ${lastRasterType})`);

        if (!rasterVisible) {
            console.log('  ⊗ Raster toggle is off, skipping');
            return;
        }

        if (lastRasterType === 'url') {
            console.log('  → Calling sendGeoTIFFToCartoCosmos with URL:', lastRasterData);
            sendGeoTIFFToCartoCosmos(lastRasterData);
        } else if (lastRasterType === 'file') {
            console.log('  → Calling sendGeoTIFFFileToCartoCosmos with file:', lastRasterData?.name);
            // For files, we need to re-read and send
            sendGeoTIFFFileToCartoCosmos(lastRasterData, () => {});
        } else if (lastRasterType === 'projected') {
            console.log('  → Calling sendProjectedRasterToCartoCosmos with projected data');
            // For projected rasters, re-send with stored parameters
            const { data, nodataValue, bounds, minVal, maxVal } = lastRasterData;
            console.log('  → Projected raster params:', {
                dataSize: data?.byteLength,
                nodataValue,
                bounds,
                minVal,
                maxVal
            });
            sendProjectedRasterToCartoCosmos({
                data: data.slice(0), // Clone the ArrayBuffer again
                nodataValue,
                bounds,
                minVal,
                maxVal
            });
        }
    } else {
        console.log('  ⊗ No raster data stored');
    }

    console.log('[CARTOCOSMOS] ✓ Finished re-sending layers');
}

// State getters
export function isCartoCosmosReady() { return cartoCosmosReady; }
export function getCartoCosmosFrame() { return cartoCosmosFrame; }

// State setters (for internal use)
export function setCartoCosmosReady(ready) { cartoCosmosReady = ready; }

// Clear stored layers (useful when user wants to reset)
export function clearStoredLayers() {
    lastFootprintData = null;
    lastRasterData = null;
    lastRasterType = null;
    console.log('✓ Cleared stored layers');
}

/**
 * Clears all layers from the map and resets UI
 */
export function clearAllLayers() {
    // Clear stored layer data
    clearStoredLayers();

    // Send clear message to CartoCosmos
    const frame = getCartoCosmosFrame();
    if (frame && frame.contentWindow) {
        frame.contentWindow.postMessage({ type: 'clearLayers' }, '*');
        console.log('✓ Sent clearLayers message to CartoCosmos');
    }

    // Reset raster info panel
    const debugContent = document.getElementById('debug-content');
    if (debugContent) {
        debugContent.innerHTML = 'No data loaded yet';
    }

    // Disable layer controls
    const toggleFootprint = document.getElementById('toggle-footprint');
    const toggleRaster = document.getElementById('toggle-raster');
    const opacitySlider = document.getElementById('opacity-slider');
    const zoomToLayersBtn = document.getElementById('zoom-to-layers');

    if (toggleFootprint) {
        toggleFootprint.disabled = true;
        toggleFootprint.checked = true;
        toggleFootprint.parentElement.classList.add('disabled');
    }

    if (toggleRaster) {
        toggleRaster.disabled = true;
        toggleRaster.checked = true;
        toggleRaster.parentElement.classList.add('disabled');
    }

    if (opacitySlider) {
        opacitySlider.disabled = true;
        opacitySlider.value = 100;
    }

    if (zoomToLayersBtn) {
        zoomToLayersBtn.disabled = true;
    }

    // Reset opacity value display
    const opacityValue = document.getElementById('opacity-value');
    if (opacityValue) {
        opacityValue.textContent = '100%';
    }

    console.log('✓ Cleared all layers and reset UI');
}

console.log('✓ CartoCosmos bridge module loaded');
