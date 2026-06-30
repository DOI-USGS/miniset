/**
 * File Handlers Module
 * Handles ISD and GeoTIFF file/URL input processing
 */

import { validateURL } from './validation.js';

// File state
let isdData = null;
let isdUrl = null;
let geotiffUrl = null;
let geotiffFile = null;

// AbortControllers for canceling in-flight requests
let isdFetchController = null;
let geotiffFetchController = null;

/**
 * Handles ISD (Image Support Data) file upload
 * @param {Event} e - File input change event
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 * @param {Function} detectTargetCallback - Callback to detect and switch target body
 */
export function handleISDFile(e, updateStatusCallback, checkReadyCallback, detectTargetCallback) {
    const file = e.target.files[0];
    if (!file) return;

    // Cancel any in-flight ISD URL fetch
    if (isdFetchController) {
        console.log('✓ Canceling in-flight ISD URL fetch');
        isdFetchController.abort();
        isdFetchController = null;
    }

    // Clear URL input when file is uploaded
    const isdUrlInput = document.getElementById('isd-url');
    if (isdUrlInput) {
        isdUrlInput.value = '';
    }

    const reader = new FileReader();
    reader.onload = (event) => {
        try {
            const content = event.target.result;

            // Parse as JSON ISD only
            isdData = JSON.parse(content);
            console.log('✓ ISD loaded from file:', Object.keys(isdData).length, 'keys');

            // Auto-detect and switch to target body
            detectTargetCallback(isdData);

            updateStatusCallback('ISD file loaded - ready to calculate footprint', 'success');
            checkReadyCallback();
        } catch (err) {
            console.error('✗ Failed to parse ISD:', err);
            updateStatusCallback('Error parsing ISD file', 'error');
            isdData = null;
        }
    };
    reader.readAsText(file);
}

/**
 * Handles ISD URL input changes (validates but doesn't fetch until Generate is clicked)
 * @param {Event} e - Input change event
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 */
export function handleISDUrl(e, updateStatusCallback, checkReadyCallback) {
    const url = e.target.value.trim();

    // Cancel any in-flight ISD fetch when URL changes
    if (isdFetchController) {
        console.log('✓ Canceling in-flight ISD URL fetch');
        isdFetchController.abort();
        isdFetchController = null;
    }

    if (!url) {
        isdUrl = null;
        isdData = null;
        checkReadyCallback(); // Will set appropriate status
        return;
    }

    // URL validation using validation module
    const validation = validateURL(url, ['.json', '.txt']);

    if (!validation.valid) {
        updateStatusCallback(`❌ ${validation.error}`, 'error');
        isdUrl = null;
        isdData = null;
        checkReadyCallback();
        return;
    }

    // Log warnings if any
    if (validation.warnings && validation.warnings.length > 0) {
        console.warn('⚠️ ISD URL warnings:', validation.warnings);
    }

    isdUrl = url;
    isdData = null;  // Clear any previously loaded data
    console.log('✓ ISD URL validated:', url);
    updateStatusCallback('ISD URL ready - click "Generate" to load', 'success');

    // Clear file input when URL is entered
    const isdInput = document.getElementById('isd-file');
    if (isdInput) {
        isdInput.value = '';
    }

    checkReadyCallback();
}

/**
 * Fetches ISD from a URL and stores it globally
 * @async
 * @param {string} url - URL to fetch ISD from
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} detectTargetCallback - Callback to detect and switch target body
 * @returns {Promise<boolean>} True if successful, false otherwise
 */
export async function fetchISDFromUrl(url, updateStatusCallback, detectTargetCallback) {
    console.log('→ Fetching ISD from URL:', url);
    updateStatusCallback('Fetching ISD from URL...', 'loading');

    // Cancel any existing ISD fetch
    if (isdFetchController) {
        console.log('✓ Canceling previous ISD URL fetch');
        isdFetchController.abort();
    }

    // Create new AbortController for this request
    isdFetchController = new AbortController();
    const signal = isdFetchController.signal;

    try {
        // Use local proxy to bypass CORS
        const proxyUrl = `http://localhost:8001/proxy?url=${encodeURIComponent(url)}`;
        console.log('  Fetching via proxy:', proxyUrl);

        const response = await fetch(proxyUrl, { signal });

        console.log('  Response status:', response.status);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        const text = await response.text();
        console.log('  Response received, length:', text.length);

        const json = JSON.parse(text);
        isdData = json;
        console.log('✓ ISD loaded from URL:', Object.keys(isdData).length, 'keys');

        // Auto-detect and switch to target body
        detectTargetCallback(isdData);

        updateStatusCallback('ISD loaded from URL', 'success');
        isdFetchController = null; // Clear controller on success
        return true;
    } catch (err) {
        console.error('✗ Failed to fetch ISD:', err);

        // Don't show error if request was intentionally aborted
        if (err.name === 'AbortError') {
            console.log('  ISD fetch was canceled');
            return false;
        }

        // Better error messages for common failure modes
        let errorMessage = 'Error fetching ISD';

        if (err.name === 'TypeError' && err.message.includes('Failed to fetch')) {
            errorMessage = 'Network error - check CORS, firewall, or URL accessibility';
        } else if (err.message.includes('HTTP 404')) {
            errorMessage = 'ISD not found (HTTP 404) - verify the URL is correct';
        } else if (err.message.includes('HTTP 403')) {
            errorMessage = 'Access denied (HTTP 403) - check permissions';
        } else if (err.message.includes('HTTP 500') || err.message.includes('HTTP 502') || err.message.includes('HTTP 503')) {
            errorMessage = 'Server error - try again later';
        } else if (err.message.includes('JSON')) {
            errorMessage = 'Invalid JSON response - file may not be valid ISD';
        } else {
            errorMessage = `Error fetching ISD: ${err.message}`;
        }

        updateStatusCallback(`❌ ${errorMessage}`, 'error');
        isdData = null;
        isdFetchController = null; // Clear controller on error
        return false;
    }
}

/**
 * Handles GeoTIFF file upload
 * Can process COG with embedded CSM State and footprint metadata
 * @param {Event} event - File input change event
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 */
export function handleGeoTIFFFile(event, updateStatusCallback, checkReadyCallback) {
    const file = event.target.files[0];

    // Cancel any in-flight GeoTIFF URL fetch
    if (geotiffFetchController) {
        console.log('✓ Canceling in-flight GeoTIFF URL fetch');
        geotiffFetchController.abort();
        geotiffFetchController = null;
    }

    if (!file) {
        geotiffUrl = null;
        geotiffFile = null;
        checkReadyCallback();
        return;
    }

    console.log('→ GeoTIFF file selected:', file.name);

    // Check file size (warn if > 100 MB)
    const fileSizeMB = file.size / (1024 * 1024);
    console.log(`  File size: ${fileSizeMB.toFixed(2)} MB`);

    if (fileSizeMB > 100) {
        updateStatusCallback(`⚠️ Large file (${fileSizeMB.toFixed(0)} MB) may fail to load. Consider using a URL or smaller file.`, 'error');
        console.warn(`⚠️ File is very large (${fileSizeMB.toFixed(2)} MB) and may cause browser memory issues`);
    }

    // Store the file object for later use
    geotiffFile = file;
    geotiffUrl = URL.createObjectURL(file);  // Still create blob URL for local reference
    console.log('✓ COG file loaded:', file.name);

    if (fileSizeMB <= 100) {
        updateStatusCallback('✓ COG file ready to display', 'success');
    }

    checkReadyCallback();
}

/**
 * Handles GeoTIFF/COG URL input changes
 * @param {Event} e - Input change event
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 */
export function handleGeoTIFFUrl(e, updateStatusCallback, checkReadyCallback) {
    const url = e.target.value.trim();

    // Cancel any in-flight GeoTIFF fetch when URL changes
    if (geotiffFetchController) {
        console.log('✓ Canceling in-flight GeoTIFF URL fetch');
        geotiffFetchController.abort();
        geotiffFetchController = null;
    }

    if (!url) {
        geotiffUrl = null;
        checkReadyCallback(); // Will set appropriate status
        return;
    }

    // URL validation using validation module
    const validation = validateURL(url, ['.tif', '.tiff']);

    if (!validation.valid) {
        updateStatusCallback(`❌ ${validation.error}`, 'error');
        geotiffUrl = null;
        checkReadyCallback();
        return;
    }

    // Log warnings if any
    if (validation.warnings && validation.warnings.length > 0) {
        console.warn('⚠️ GeoTIFF URL warnings:', validation.warnings);
    }

    geotiffUrl = url;
    console.log('✓ COG URL set:', url);
    updateStatusCallback('✓ COG URL ready', 'success');
    checkReadyCallback();
}

/**
 * Clears ISD file state
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 */
export function clearISDFile(updateStatusCallback, checkReadyCallback) {
    // Cancel any in-flight ISD fetch
    if (isdFetchController) {
        console.log('✓ Canceling in-flight ISD URL fetch');
        isdFetchController.abort();
        isdFetchController = null;
    }

    isdData = null;
    checkReadyCallback(); // Will set appropriate status
    console.log('✓ ISD file cleared');
}

/**
 * Clears ISD URL state
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 */
export function clearISDUrl(updateStatusCallback, checkReadyCallback) {
    // Cancel any in-flight ISD fetch
    if (isdFetchController) {
        console.log('✓ Canceling in-flight ISD URL fetch');
        isdFetchController.abort();
        isdFetchController = null;
    }

    isdUrl = null;
    isdData = null;
    checkReadyCallback(); // Will set appropriate status
    console.log('✓ ISD URL cleared');
}

/**
 * Clears GeoTIFF file state
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 */
export function clearGeoTIFFFile(updateStatusCallback, checkReadyCallback) {
    // Cancel any in-flight GeoTIFF fetch
    if (geotiffFetchController) {
        console.log('✓ Canceling in-flight GeoTIFF URL fetch');
        geotiffFetchController.abort();
        geotiffFetchController = null;
    }

    geotiffUrl = null;
    geotiffFile = null;
    checkReadyCallback(); // Will set appropriate status
    console.log('✓ COG file cleared');
}

/**
 * Clears GeoTIFF URL state
 * @param {Function} updateStatusCallback - Callback to update UI status
 * @param {Function} checkReadyCallback - Callback to check if ready to process
 */
export function clearGeoTIFFUrl(updateStatusCallback, checkReadyCallback) {
    // Cancel any in-flight GeoTIFF fetch
    if (geotiffFetchController) {
        console.log('✓ Canceling in-flight GeoTIFF URL fetch');
        geotiffFetchController.abort();
        geotiffFetchController = null;
    }

    geotiffUrl = null;
    geotiffFile = null;
    checkReadyCallback(); // Will set appropriate status
    console.log('✓ COG URL cleared');
}

// State getters
export function getISDData() { return isdData; }
export function getISDUrl() { return isdUrl; }
export function getGeoTIFFUrl() { return geotiffUrl; }
export function getGeoTIFFFile() { return geotiffFile; }

// State setters (for internal use by fetch functions)
export function setISDData(data) { isdData = data; }
export function setISDUrl(url) { isdUrl = url; }
export function setGeoTIFFUrl(url) { geotiffUrl = url; }
export function setGeoTIFFFile(file) { geotiffFile = file; }

// AbortController management
export function createGeoTIFFAbortController() {
    // Cancel any existing fetch
    if (geotiffFetchController) {
        console.log('✓ Canceling previous GeoTIFF URL fetch');
        geotiffFetchController.abort();
    }

    geotiffFetchController = new AbortController();
    return geotiffFetchController.signal;
}

export function clearGeoTIFFAbortController() {
    geotiffFetchController = null;
}

export function abortGeoTIFFFetch() {
    if (geotiffFetchController) {
        console.log('✓ Aborting GeoTIFF fetch');
        geotiffFetchController.abort();
        geotiffFetchController = null;
    }
}

console.log('✓ File handlers module loaded');
