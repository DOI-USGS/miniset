/**
 * UI State Management Module
 * Handles status updates, button states, and UI feedback
 */

// UI state
let csmFootprint = null;

/**
 * Updates the status box with a message and visual state
 * @param {string} message - Status message to display
 * @param {string} [type=''] - Status type: 'loading', 'success', 'error', or ''
 */
export function updateStatus(message, type = '') {
    const statusText = document.getElementById('status-text');
    const statusSpinner = document.getElementById('status-spinner');
    const statusEl = document.getElementById('status');

    // Update text
    if (statusText) {
        statusText.textContent = message;
    } else {
        // Fallback if status-text element doesn't exist
        console.warn('status-text element not found, using fallback');
        if (statusEl) statusEl.textContent = message;
    }

    if (statusEl) statusEl.className = type;

    // Show/hide spinner and update color
    if (statusSpinner) {
        if (type === 'loading') {
            statusSpinner.style.display = 'inline-block';
            statusSpinner.style.width = '18px';
            statusSpinner.style.height = '18px';
            statusSpinner.style.backgroundColor = 'transparent';
            statusSpinner.style.borderWidth = '3px';
            statusSpinner.style.borderStyle = 'solid';
            statusSpinner.style.borderColor = '#e65100';
            statusSpinner.style.borderTopColor = 'transparent';
            statusSpinner.style.borderRadius = '50%';
            statusSpinner.style.animation = 'spin 0.8s linear infinite';
            statusSpinner.textContent = '';
        } else {
            statusSpinner.style.display = 'none';
        }
    }

    // Update status box background color
    const statusBox = document.getElementById('status-box');
    if (statusBox) {
        statusBox.className = 'status-box ' + type;
        // Set background color based on type
        if (type === 'loading') {
            statusBox.style.background = '#fff3e0';
            statusBox.style.border = '';
            if (statusText) statusText.style.color = '#e65100';
        } else if (type === 'success') {
            statusBox.style.background = '#e8f5e9';
            statusBox.style.border = '';
            if (statusText) statusText.style.color = '#2e7d32';
        } else if (type === 'error') {
            statusBox.style.background = '#ffebee';
            statusBox.style.border = '';
            if (statusText) statusText.style.color = '#c62828';
        } else {
            statusBox.style.background = '#e3f2fd';
            statusBox.style.border = '';
            if (statusText) statusText.style.color = '#1976d2';
        }
    }
}

/**
 * Checks if required inputs are ready and enables/disables the Generate button
 * Requires both GeoTIFF and ISD to be loaded
 * @param {Object} deps - Dependencies
 * @param {boolean} deps.hasIsd - Whether ISD is loaded
 * @param {boolean} deps.hasCog - Whether GeoTIFF is loaded
 * @param {boolean} deps.hasCogFile - Whether GeoTIFF file is loaded
 * @param {boolean} deps.hasCogUrl - Whether GeoTIFF URL is entered
 * @param {boolean} deps.hasCsmModule - Whether CSM module is loaded
 * @param {boolean} deps.cartoCosmosReady - Whether CartoCosmos is ready
 */
export function checkReadyToProcess({ hasIsd, hasCog, hasCogFile, hasCogUrl, hasCsmModule, cartoCosmosReady }) {
    const showRasterBtn = document.getElementById('show-raster-btn');
    const statusEl = document.getElementById('status');

    console.log('checkReadyToProcess:', { hasIsd, hasCog, hasCsmModule, cartoCosmosReady });

    // All buttons require CartoCosmos to be ready
    if (!cartoCosmosReady) {
        if (showRasterBtn) showRasterBtn.disabled = true;
        console.log('✗ All buttons disabled - CartoCosmos not ready');
    }

    // Show raster button - enabled if we have COG
    if (hasCog && cartoCosmosReady && showRasterBtn) {
        showRasterBtn.disabled = false;
        console.log('✓ Show raster button enabled');
    } else if (showRasterBtn) {
        showRasterBtn.disabled = true;
    }

    // Update status message only if not in a more specific state
    // Don't overwrite status if a footprint was just calculated or other operation is in progress
    const currentStatus = statusEl?.textContent || '';

    const isErrorState = currentStatus.includes('Invalid') || currentStatus.includes('Error');
    const isEmptyState = currentStatus.includes('Ready to load');

    const shouldUpdateStatus = (!currentStatus.includes('Footprint calculated') &&
                                !currentStatus.includes('Orthorectifying') &&
                                !currentStatus.includes('Raster overlay sent') &&
                                !currentStatus.includes('Loading'));

    if (shouldUpdateStatus) {
        // If currently in error state, only update if we have valid data to show
        if (isErrorState && !hasIsd && !hasCog) {
            // Stay in error state, don't overwrite with "Ready to load data"
            return;
        }

        // Allow status updates even if footprint exists when in error or empty state
        if (csmFootprint && !isErrorState && !isEmptyState) {
            // Don't change status if footprint already exists (unless we're in an error/empty state)
        } else if (hasIsd && hasCog && hasCsmModule) {
            updateStatus('✓ Ready to generate footprint and project image', 'success');
        } else if (hasIsd && !hasCog) {
            updateStatus('⚠️ ISD loaded - GeoTIFF required to proceed', '');
        } else if (hasCogFile && !hasIsd) {
            updateStatus('✓ GeoTIFF loaded - Load ISD or use GeoTIFF with embedded CSM data', 'success');
        } else if (hasCogUrl && !hasIsd) {
            // Don't override the "Valid URL string" message from the file handler
            // Only update if currently in a generic state
            if (isEmptyState) {
                updateStatus('✓ Valid URL string entered', 'success');
            }
        } else if (!hasCsmModule) {
            updateStatus('Loading CSM module...', 'loading');
        } else if (!hasIsd && !hasCog && !isErrorState) {
            // Only set to "Ready to load data" if not currently showing an error
            updateStatus('💡 Start by loading a GeoTIFF and ISD file', '');
        }
    }
}

/**
 * Updates the debug information panel
 * @param {string} info - Debug information HTML
 */
export function updateDebugInfo(info) {
    const debugContent = document.getElementById('debug-content');
    if (debugContent) {
        debugContent.innerHTML = info;
    }
}

/**
 * Displays footprint information in the stats panel
 * @param {Object} footprintGeoJSON - GeoJSON footprint object with metadata
 */
export function displayFootprintInfo(footprintGeoJSON) {
    window.currentFootprint = footprintGeoJSON;
    const coords = footprintGeoJSON.geometry.coordinates[0];

    // Calculate bounds
    const lats = coords.map(c => c[1]);
    const lons = coords.map(c => c[0]);
    const minLat = Math.min(...lats);
    const maxLat = Math.max(...lats);
    const minLon = Math.min(...lons);
    const maxLon = Math.max(...lons);

    // Generate WKT
    const wktCoords = coords.map(c => `${c[0].toFixed(6)} ${c[1].toFixed(6)}`).join(', ');
    const wkt = `POLYGON((${wktCoords}))`;

    // Get metadata from properties (minisetV8) or model (minisetV5)
    let modelName = footprintGeoJSON.properties.modelName || footprintGeoJSON.properties.modelType || 'Unknown';
    let sensorId = footprintGeoJSON.properties.sensorId || 'Unknown';
    let platformId = footprintGeoJSON.properties.platformId || 'Unknown';

    console.log('=== FOOTPRINT METADATA ===');
    console.log('  modelName:', modelName);
    console.log('  sensorId:', sensorId);
    console.log('  platformId:', platformId);
    console.log('  All properties:', JSON.stringify(footprintGeoJSON.properties, null, 2));

    // Fallback: try to get from CSM model object (minisetV5)
    const model = footprintGeoJSON.model;
    if (model && (sensorId === 'Unknown' || platformId === 'Unknown')) {
        try {
            if (typeof model.getSensorIdentifier === 'function' && sensorId === 'Unknown') {
                sensorId = model.getSensorIdentifier();
            }
            if (typeof model.getPlatformIdentifier === 'function' && platformId === 'Unknown') {
                platformId = model.getPlatformIdentifier();
            }
        } catch (err) {
            console.warn('Could not get model identifiers:', err);
        }
    }

    // Display in stats panel (directly in debug-content, no sub-box)
    const info = `
        <strong>Platform:</strong> ${platformId}<br>
        <strong>Sensor:</strong> ${sensorId}<br>
        <strong>CSM Model:</strong> ${modelName}<br>
        <strong>Image Size:</strong> ${footprintGeoJSON.properties.samples} × ${footprintGeoJSON.properties.lines} pixels<br>
        <strong>Latitude:</strong> ${minLat.toFixed(4)}° to ${maxLat.toFixed(4)}°<br>
        <strong>Longitude:</strong> ${minLon.toFixed(4)}° to ${maxLon.toFixed(4)}°<br>
        <br>
        <strong>Corner Coordinates:</strong><br>
        ${coords.slice(0, 4).map((c, i) =>
            `Corner ${i+1}: (${c[1].toFixed(4)}°, ${c[0].toFixed(4)}°)`
        ).join('<br>')}
    `;

    updateDebugInfo(info);
    console.log('✓ Footprint information displayed in stats panel');

    // Store WKT globally for reference
    window.currentWKT = wkt;

    // Note: Footprint is now sent via addFootprint message (GeoJSON format)
    // The old WKT method is deprecated
}

/**
 * Enables layer visibility controls after successful generation
 * Activates footprint/raster toggles, opacity slider, and zoom button
 */
export function enableLayerControls() {
    const layerControls = document.getElementById('layer-controls');
    const toggleFootprint = document.getElementById('toggle-footprint');
    const toggleRaster = document.getElementById('toggle-raster');
    const opacitySlider = document.getElementById('opacity-slider');
    const opacityValue = document.getElementById('opacity-value');

    if (layerControls) {
        layerControls.style.opacity = '1';
    }

    if (toggleFootprint) {
        toggleFootprint.disabled = false;
        toggleFootprint.style.cursor = 'pointer';
        toggleFootprint.parentElement.style.cursor = 'pointer';
        toggleFootprint.parentElement.classList.remove('disabled');
        const span = toggleFootprint.nextElementSibling;
        if (span) span.style.color = ''; // Remove inline style, let CSS handle it
    }

    if (toggleRaster) {
        toggleRaster.disabled = false;
        toggleRaster.style.cursor = 'pointer';
        toggleRaster.parentElement.style.cursor = 'pointer';
        toggleRaster.parentElement.classList.remove('disabled');
        const span = toggleRaster.nextElementSibling;
        if (span) span.style.color = ''; // Remove inline style, let CSS handle it
    }

    if (opacitySlider) {
        opacitySlider.disabled = false;
        opacitySlider.style.cursor = 'pointer';
        opacitySlider.style.opacity = '1';
    }

    if (opacityValue) {
        opacityValue.style.color = '#333';
    }

    // Enable opacity help text
    const helpText = layerControls?.querySelector('p');
    if (helpText) {
        helpText.style.color = '#666';
    }

    // Enable zoom to layers button
    const zoomToLayersBtn = document.getElementById('zoom-to-layers');
    if (zoomToLayersBtn) {
        zoomToLayersBtn.disabled = false;
        zoomToLayersBtn.style.cursor = 'pointer';
        zoomToLayersBtn.style.opacity = '1';
    }

    console.log('✓ Layer controls enabled');
}

// State getters and setters
export function getCSMFootprint() { return csmFootprint; }
export function setCSMFootprint(footprint) { csmFootprint = footprint; }

console.log('✓ UI state module loaded');
