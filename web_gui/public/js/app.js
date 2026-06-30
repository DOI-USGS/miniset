/**
 * Miniset Web GUI - Main Application (Modular)
 * Planetary sensor model visualization with CSM integration
 * Version: 2026-05-29-v4-modular
 */

// Import modules
import { loadCSMModule, loadMinisetV8Module, getMinisetV8, getCSMModule } from './wasm-loader.js';
import {
    handleISDFile,
    handleISDUrl,
    fetchISDFromUrl,
    handleGeoTIFFFile,
    handleGeoTIFFUrl,
    clearISDFile,
    clearISDUrl,
    clearGeoTIFFFile,
    clearGeoTIFFUrl,
    getISDData,
    getISDUrl,
    getGeoTIFFUrl,
    getGeoTIFFFile,
    createGeoTIFFAbortController,
    clearGeoTIFFAbortController,
    abortGeoTIFFFetch
} from './file-handlers.js';
import {
    setupCartoCosmos,
    handlePlanetChange,
    sendGeoTIFFToCartoCosmos,
    sendGeoTIFFFileToCartoCosmos,
    updateCartoCosmoOpacity,
    sendFootprintToCartoCosmos,
    sendProjectedRasterToCartoCosmos,
    toggleFootprintVisibility,
    toggleRasterVisibility,
    zoomToFootprint,
    isCartoCosmosReady,
    getCartoCosmosFrame,
    clearAllLayers
} from './cartocosmos-bridge.js';
import {
    updateStatus,
    checkReadyToProcess,
    updateDebugInfo,
    displayFootprintInfo,
    enableLayerControls,
    getCSMFootprint,
    setCSMFootprint
} from './ui-state.js';
import {
    detectAndSwitchTargetBody,
    projectAndDisplayGeoTIFF,
    setupTargetSelector
} from './geospatial.js';

// Debug log capture (expose to window for WASM access)
window.debugLogs = [];
window.captureLogsEnabled = false;

// Expose abort controller functions globally for geospatial.js
window.createGeoTIFFAbortController = createGeoTIFFAbortController;
window.clearGeoTIFFAbortController = clearGeoTIFFAbortController;
window.abortGeoTIFFFetch = abortGeoTIFFFetch;

// Override console methods to capture logs
const originalConsoleLog = console.log;
const originalConsoleError = console.error;
const originalConsoleWarn = console.warn;

console.log = function(...args) {
    if (window.captureLogsEnabled) {
        window.debugLogs.push({ type: 'log', time: new Date().toISOString(), message: args.map(a => String(a)).join(' ') });
        return;
    }
    originalConsoleLog.apply(console, args);
};

console.error = function(...args) {
    if (window.captureLogsEnabled) {
        window.debugLogs.push({ type: 'error', time: new Date().toISOString(), message: args.map(a => String(a)).join(' ') });
        return;
    }
    originalConsoleError.apply(console, args);
};

console.warn = function(...args) {
    if (window.captureLogsEnabled) {
        window.debugLogs.push({ type: 'warn', time: new Date().toISOString(), message: args.map(a => String(a)).join(' ') });
        return;
    }
    originalConsoleWarn.apply(console, args);
};

/**
 * Downloads the extracted CSM State as a text file
 * @global
 */
window.downloadCsmState = function() {
    if (window.extractedCsmState) {
        const blob = new Blob([window.extractedCsmState], { type: 'text/plain' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'csm_state_extracted.txt';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        console.log('✓ Downloaded csm_state_extracted.txt');
    } else {
        console.error('No CSM State available. Run "Calculate Footprint" first.');
    }
};

/**
 * Starts capturing console logs for debugging
 * @global
 */
window.startLogCapture = function() {
    window.debugLogs = [];
    window.captureLogsEnabled = true;
    originalConsoleLog('✓ Log capture started - console output suppressed');
};

/**
 * Stops capturing console logs and restores normal console output
 * @global
 */
window.stopLogCapture = function() {
    window.captureLogsEnabled = false;
    originalConsoleLog('✓ Log capture stopped - console output restored');
};

/**
 * Downloads captured debug logs as a timestamped text file
 * @global
 */
window.downloadLogs = function() {
    if (window.debugLogs.length === 0) {
        originalConsoleError('No logs captured. Run startLogCapture() first.');
        return;
    }

    const logText = window.debugLogs.map(log =>
        `[${log.time}] [${log.type.toUpperCase()}] ${log.message}`
    ).join('\n');

    const blob = new Blob([logText], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `debug_logs_${new Date().toISOString().replace(/[:.]/g, '-')}.txt`;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    originalConsoleLog(`✓ Downloaded debug logs (${window.debugLogs.length} entries)`);
};

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', initializeApp);

/**
 * Initializes the application after DOM is loaded
 * Sets up event listeners, loads WASM modules, and initializes CartoCosmos
 * @async
 */
async function initializeApp() {
    console.log('→ Initializing Miniset Web GUI...');

    // Get DOM elements
    const isdInput = document.getElementById('isd-file');
    const isdUrlInput = document.getElementById('isd-url');
    const geotiffUrlInput = document.getElementById('geotiff-url');
    const geotiffFileInput = document.getElementById('geotiff-file');
    const showRasterBtn = document.getElementById('show-raster-btn');
    const cartoCosmosFrame = document.getElementById('cartocosmos-frame');

    // Set up event listeners
    setupEventListeners(isdInput, isdUrlInput, geotiffFileInput, geotiffUrlInput, showRasterBtn, cartoCosmosFrame);

    // Listen for messages from CartoCosmos
    setupCartoCosmos(cartoCosmosFrame, updateStatus, checkReadyWrapper);

    // Populate target selector from Astro Web Maps API
    setupTargetSelector((e) => handlePlanetChange(e, updateStatus)).catch(err => {
        console.error('Error in setupTargetSelector:', err);
    });

    // Load WASM modules
    await loadCSMModule(updateStatus);
    await loadMinisetV8Module(updateStatus);

    // Check if CartoCosmos is ready, otherwise show loading status
    if (isCartoCosmosReady()) {
        updateStatus('✓ CartoCosmos map ready', 'success');
    } else {
        updateStatus('Loading CartoCosmos map...', 'loading');
    }

    console.log('✓ Application initialized');
}

/**
 * Wrapper for checkReadyToProcess that gathers current state
 */
function checkReadyWrapper() {
    const hasIsd = getISDData() !== null || getISDUrl() !== null;
    const hasCog = getGeoTIFFUrl() !== null || getGeoTIFFFile() !== null;
    const hasCogFile = getGeoTIFFFile() !== null;
    const hasCogUrl = getGeoTIFFUrl() !== null && getGeoTIFFFile() === null;
    const hasCsmModule = getCSMModule() !== null;
    const cartoCosmosReady = isCartoCosmosReady();

    checkReadyToProcess({ hasIsd, hasCog, hasCogFile, hasCogUrl, hasCsmModule, cartoCosmosReady });
}

/**
 * Sets up all DOM event listeners for the application
 * @param {HTMLElement} isdInput - ISD file input element
 * @param {HTMLElement} isdUrlInput - ISD URL input element
 * @param {HTMLElement} geotiffFileInput - GeoTIFF file input element
 * @param {HTMLElement} geotiffUrlInput - GeoTIFF URL input element
 * @param {HTMLElement} showRasterBtn - Show raster button element
 * @param {HTMLElement} cartoCosmosFrame - CartoCosmos iframe element
 */
function setupEventListeners(isdInput, isdUrlInput, geotiffFileInput, geotiffUrlInput, showRasterBtn, cartoCosmosFrame) {
    // ISD file input
    if (isdInput) {
        isdInput.addEventListener('change', (e) => {
            // Clear previous layers when new ISD is loaded
            clearAllLayers();

            handleISDFile(e, updateStatus, checkReadyWrapper,
                (isd) => detectAndSwitchTargetBody(isd, updateStatus));
            // Show clear button when file is selected
            const container = isdInput.closest('.url-input-container');
            if (container) {
                if (isdInput.files.length > 0) {
                    container.classList.add('has-file');
                } else {
                    container.classList.remove('has-file');
                }
            }
        });
    }

    // ISD URL input
    if (isdUrlInput) {
        isdUrlInput.addEventListener('input', (e) => {
            // Clear previous layers when new ISD URL is entered
            const url = e.target.value.trim();
            if (url) {
                clearAllLayers();
            }

            handleISDUrl(e, updateStatus, checkReadyWrapper);
        });
    }

    // GeoTIFF file input
    if (geotiffFileInput) {
        geotiffFileInput.addEventListener('change', (e) => {
            // Clear previous layers when new GeoTIFF is loaded
            clearAllLayers();

            handleGeoTIFFFile(e, updateStatus, checkReadyWrapper);
            // Show clear button when file is selected
            const container = geotiffFileInput.closest('.url-input-container');
            if (container) {
                if (geotiffFileInput.files.length > 0) {
                    container.classList.add('has-file');
                } else {
                    container.classList.remove('has-file');
                }
            }
        });
    }

    // GeoTIFF URL input
    if (geotiffUrlInput) {
        geotiffUrlInput.addEventListener('input', (e) => {
            // Clear previous layers when new GeoTIFF URL is entered
            const url = e.target.value.trim();
            if (url) {
                clearAllLayers();
            }

            handleGeoTIFFUrl(e, updateStatus, checkReadyWrapper);
        });
    } else {
        console.error('✗ geotiff-url element not found!');
    }

    // Show raster button
    if (showRasterBtn) {
        showRasterBtn.addEventListener('click', handleShowRaster);
    }

    // Stats toggle button
    const toggleStatsBtn = document.getElementById('toggle-stats');
    const statsPanel = document.getElementById('stats-panel');
    if (toggleStatsBtn && statsPanel) {
        toggleStatsBtn.addEventListener('click', () => {
            statsPanel.classList.toggle('collapsed');
        });
    }

    // Opacity slider
    const opacitySlider = document.getElementById('opacity-slider');
    const opacityValue = document.getElementById('opacity-value');
    if (opacitySlider && opacityValue) {
        opacitySlider.addEventListener('input', (e) => {
            const opacity = e.target.value;
            opacityValue.textContent = opacity + '%';
            updateCartoCosmoOpacity(opacity / 100);
        });
    }

    // Layer visibility toggles
    setupLayerToggles(opacitySlider);

    // Zoom to layers button
    setupZoomButton();

    // Clear buttons
    setupClearButtons(geotiffFileInput, geotiffUrlInput, isdInput, isdUrlInput);
}

/**
 * Sets up layer visibility toggle event listeners
 * @param {HTMLElement} opacitySlider - Opacity slider element
 */
function setupLayerToggles(opacitySlider) {
    const toggleFootprint = document.getElementById('toggle-footprint');
    const toggleRaster = document.getElementById('toggle-raster');

    if (toggleFootprint) {
        toggleFootprint.addEventListener('change', (e) => {
            const visible = e.target.checked;

            if (visible) {
                console.log('[TOGGLE] Setting status to Loading footprint...');
                updateStatus('Loading footprint...', 'loading');

                // Force browser to render the loading state
                const statusBox = document.getElementById('status-box');
                if (statusBox) void statusBox.offsetHeight;

                requestAnimationFrame(() => {
                    toggleFootprintVisibility(visible);
                    setTimeout(() => {
                        console.log('[TOGGLE] Footprint loaded, setting status to displayed');
                        updateStatus('✓ Footprint displayed', 'success');
                    }, 500);
                });
            } else {
                console.log('[TOGGLE] Setting status to hidden');
                updateStatus('✓ Footprint hidden', 'success');
                toggleFootprintVisibility(visible);
            }
        });
    }

    if (toggleRaster) {
        toggleRaster.addEventListener('change', (e) => {
            const visible = e.target.checked;

            if (visible) {
                console.log('[TOGGLE] Setting status to Loading raster tiles...');
                updateStatus('Loading raster tiles...', 'loading');

                const statusBox = document.getElementById('status-box');
                if (statusBox) void statusBox.offsetHeight;

                requestAnimationFrame(() => {
                    toggleRasterVisibility(visible);

                    // Enable opacity slider
                    if (opacitySlider) {
                        opacitySlider.disabled = false;
                        opacitySlider.style.cursor = 'pointer';
                        opacitySlider.style.opacity = '1';
                    }

                    setTimeout(() => {
                        console.log('[TOGGLE] Minimum wait complete, setting status to displayed');
                        updateStatus('✓ Raster overlay displayed', 'success');
                    }, 2000);
                });
            } else {
                console.log('[TOGGLE] Setting status to hidden');
                updateStatus('✓ Raster overlay hidden', 'success');

                // Disable opacity slider
                if (opacitySlider) {
                    opacitySlider.disabled = true;
                    opacitySlider.style.cursor = 'not-allowed';
                    opacitySlider.style.opacity = '0.5';
                }

                toggleRasterVisibility(visible);
            }
        });
    }
}

/**
 * Sets up zoom to layers button
 */
function setupZoomButton() {
    const zoomToLayersBtn = document.getElementById('zoom-to-layers');
    if (zoomToLayersBtn) {
        zoomToLayersBtn.addEventListener('click', () => {
            if (getCartoCosmosFrame() && getCSMFootprint()) {
                updateStatus('Zooming to layers...', 'loading');
                zoomToFootprint();
                setTimeout(() => {
                    updateStatus('✓ Zoomed to layers', 'success');
                }, 500);
            }
        });
    }
}

/**
 * Sets up clear button event listeners
 */
function setupClearButtons(geotiffFileInput, geotiffUrlInput, isdInput, isdUrlInput) {
    // Clear button for COG file
    const clearGeotiffFileBtn = document.getElementById('clear-geotiff-file');
    if (clearGeotiffFileBtn && geotiffFileInput) {
        clearGeotiffFileBtn.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            geotiffFileInput.value = '';
            clearGeoTIFFFile(updateStatus, checkReadyWrapper);
            const container = geotiffFileInput.closest('.url-input-container');
            if (container) container.classList.remove('has-file');
        });
    }

    // Clear button for COG URL
    const clearGeotiffUrlBtn = document.getElementById('clear-geotiff-url');
    if (clearGeotiffUrlBtn && geotiffUrlInput) {
        clearGeotiffUrlBtn.addEventListener('click', () => {
            geotiffUrlInput.value = '';
            clearGeoTIFFUrl(updateStatus, checkReadyWrapper);
        });
    }

    // Clear button for ISD file
    const clearIsdFileBtn = document.getElementById('clear-isd-file');
    if (clearIsdFileBtn && isdInput) {
        clearIsdFileBtn.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            isdInput.value = '';
            clearISDFile(updateStatus, checkReadyWrapper);
            const container = isdInput.closest('.url-input-container');
            if (container) container.classList.remove('has-file');
        });
    }

    // Clear button for ISD URL
    const clearIsdUrlBtn = document.getElementById('clear-isd-url');
    if (clearIsdUrlBtn && isdUrlInput) {
        clearIsdUrlBtn.addEventListener('click', () => {
            isdUrlInput.value = '';
            clearISDUrl(updateStatus, checkReadyWrapper);
        });
    }
}

/**
 * Handles the Generate button click to project and display imagery
 * @async
 */
async function handleShowRaster() {
    console.log('→ Showing raster overlay...');

    const geotiffUrl = getGeoTIFFUrl();
    const geotiffFile = getGeoTIFFFile();

    if (!geotiffUrl && !geotiffFile) {
        updateStatus('No GeoTIFF loaded', 'error');
        return;
    }

    const isdData = getISDData();
    const isdUrl = getISDUrl();
    const hasIsd = isdData !== null || isdUrl !== null;
    const minisetV8 = getMinisetV8();

    if (hasIsd && minisetV8) {
        // Use camproject for both footprint and raster
        const footprint = await projectAndDisplayGeoTIFF({
            minisetV8,
            isdData,
            isdUrl,
            geotiffFile,
            geotiffUrl,
            showFootprint: true,
            showRaster: true,
            updateStatusCallback: updateStatus,
            fetchISDCallback: (url) => fetchISDFromUrl(url, updateStatus,
                (isd) => detectAndSwitchTargetBody(isd, updateStatus)),
            sendFootprintCallback: sendFootprintToCartoCosmos,
            sendRasterCallback: sendProjectedRasterToCartoCosmos,
            displayFootprintCallback: displayFootprintInfo,
            enableLayerControlsCallback: enableLayerControls,
            checkReadyCallback: checkReadyWrapper
        });

        if (footprint) {
            setCSMFootprint(footprint);
        }
    } else {
        // No ISD - just send raw GeoTIFF
        console.log('  → No ISD, sending raw GeoTIFF...');
        updateStatus('Sending raster overlay to CartoCosmos...', 'loading');

        if (geotiffFile) {
            await sendGeoTIFFFileToCartoCosmos(geotiffFile, updateStatus);
            // Status will be updated by CartoCosmos via message handler (geoTiffSuccess or geoTiffError)
            updateStatus('Processing raster overlay...', 'loading');
        } else {
            sendGeoTIFFToCartoCosmos(geotiffUrl);
            // Status will be updated by CartoCosmos via message handler
            updateStatus('Processing raster overlay...', 'loading');
        }
    }
}

console.log('✓ App module loaded (modular version)');
