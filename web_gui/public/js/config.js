/**
 * Runtime configuration for the Miniset web GUI.
 *
 * Single source of truth for the local service endpoints this demo depends on.
 * Previously these were hardcoded at each call site, which meant changing a port
 * required edits in several files.
 *
 * Override order (last wins):
 *   1. the defaults below,
 *   2. `window.MINISET_CONFIG` if set before `app.js` loads,
 *   3. URL query parameters, e.g. `?proxy=http://127.0.0.1:9001`.
 *
 * All of these point at localhost by design: both the CartoCosmos basemap and
 * the CORS proxy are local development services. See web_gui/README.md.
 */

const DEFAULTS = {
    /** Base URL of the local CartoCosmos instance serving the basemap iframe. */
    cartoCosmosBaseUrl: 'http://localhost:8000',

    /** Base URL of the local CORS proxy (see web_gui/proxy_server.py). */
    proxyBaseUrl: 'http://127.0.0.1:8001',

    /** Planetary body the basemap opens on. */
    defaultTarget: 'MARS',
};

/** Read the query-parameter overrides, ignoring anything not recognized. */
function fromQueryParams() {
    const overrides = {};
    let params;
    try {
        params = new URLSearchParams(window.location.search);
    } catch {
        return overrides;
    }
    const map = {
        cartocosmos: 'cartoCosmosBaseUrl',
        proxy: 'proxyBaseUrl',
        target: 'defaultTarget',
    };
    for (const [param, key] of Object.entries(map)) {
        const value = params.get(param);
        if (value) {
            overrides[key] = value;
        }
    }
    return overrides;
}

export const config = Object.freeze({
    ...DEFAULTS,
    ...(window.MINISET_CONFIG || {}),
    ...fromQueryParams(),
});

/**
 * Build the CartoCosmos iframe URL for a planetary body.
 * @param {string} [target] - Body name, e.g. 'MARS'. Defaults to config.defaultTarget.
 * @returns {string} Fully qualified iframe URL.
 */
export function cartoCosmosUrl(target = config.defaultTarget) {
    return `${config.cartoCosmosBaseUrl}?target=${encodeURIComponent(target)}&hideControls=true`;
}

/**
 * Wrap a remote URL so it is fetched through the local CORS proxy.
 *
 * The proxy enforces a host allowlist and refuses private/loopback targets, so
 * not every URL will succeed; see web_gui/proxy_server.py.
 *
 * @param {string} url - Absolute URL of the remote resource.
 * @returns {string} Proxied URL.
 */
export function proxyUrl(url) {
    return `${config.proxyBaseUrl}/proxy?url=${encodeURIComponent(url)}`;
}

/**
 * True if `url` is remote and therefore needs the CORS proxy.
 * Local and blob URLs are fetched directly.
 * @param {string} url
 * @returns {boolean}
 */
export function needsProxy(url) {
    if (typeof url !== 'string' || !url.startsWith('http')) {
        return false;
    }
    try {
        const { hostname } = new URL(url);
        return !(hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '::1');
    } catch {
        return false;
    }
}
