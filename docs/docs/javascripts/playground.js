/**
 * Miniset in-browser playground.
 *
 * Loads the real Miniset WebAssembly module (the same one shipped for Node.js
 * and the browser) and runs the code from the page's code block against it.
 *
 * The module is an Emscripten ES module (`export default MinisetFactory`) with
 * no threads / SharedArrayBuffer, so it works on any static host — no special
 * COOP/COEP headers required (which matters for GitLab/GitHub Pages).
 */

// Resolve the WASM asset relative to the site root so it works regardless of
// where MkDocs is deployed (subpath, custom domain, `mkdocs serve`, etc.).
const WASM_JS_URL = new URL("../assets/wasm/miniset.js", import.meta.url).href;

let modulePromise = null;

/**
 * Load the Miniset module once and cache the promise. Subsequent Run clicks
 * reuse the same instance.
 * @returns {Promise<object>} the initialized Emscripten module
 */
function loadMiniset() {
  if (modulePromise) return modulePromise;

  modulePromise = import(/* webpackIgnore: true */ WASM_JS_URL)
    .then(({ default: MinisetFactory }) =>
      MinisetFactory({
        // Point the loader at the sibling .wasm binary.
        locateFile: (path) =>
          path.endsWith(".wasm")
            ? new URL("../assets/wasm/miniset.wasm", import.meta.url).href
            : path,
        // Swallow the module's own stdout/stderr; user code prints via log().
        print: () => {},
        printErr: (text) => {
          if (/\[ERROR\]|Exception|abort/i.test(text)) {
            console.error("[Miniset WASM]", text);
          }
        },
      })
    )
    .catch((err) => {
      // Reset so a later Run can retry after a transient failure.
      modulePromise = null;
      throw err;
    });

  return modulePromise;
}

/**
 * Wire up a single [data-ms-playground] block: grab the toolbar controls, the
 * source code, and the output panel, then drive Run / Reset.
 * @param {HTMLElement} root
 */
function initPlayground(root) {
  const runBtn = root.querySelector("[data-ms-run]");
  const resetBtn = root.querySelector("[data-ms-reset]");
  const statusEl = root.querySelector("[data-ms-status]");
  const outputEl = root.querySelector("[data-ms-output]");

  // MkDocs/Material renders the fenced block into a <code> element inside a
  // highlight wrapper. Read the source once and keep the pristine copy so the
  // editor can be reset. The code element is contenteditable so people can
  // tweak the snippet inline.
  const codeEl = root.querySelector("pre code");
  if (!codeEl || !runBtn || !outputEl) return;

  const originalCode = codeEl.textContent;
  codeEl.setAttribute("contenteditable", "plaintext-only");
  codeEl.setAttribute("spellcheck", "false");

  const setStatus = (text, state) => {
    if (!statusEl) return;
    statusEl.textContent = text;
    statusEl.dataset.state = state;
  };

  const write = (line) => {
    outputEl.textContent += (outputEl.textContent ? "\n" : "") + line;
  };

  // Kick off loading immediately so the module is warm by the time someone
  // presses Run.
  loadMiniset()
    .then(() => {
      runBtn.disabled = false;
      setStatus("WASM ready", "ready");
    })
    .catch((err) => {
      setStatus("Failed to load WASM", "error");
      console.error("[Miniset playground] load failed:", err);
    });

  const run = async () => {
    runBtn.disabled = true;
    outputEl.textContent = "";
    setStatus("Running…", "running");

    let Miniset;
    try {
      Miniset = await loadMiniset();
    } catch (err) {
      write("Error: could not load the Miniset WASM module.");
      write(String(err));
      setStatus("Failed to load WASM", "error");
      runBtn.disabled = false;
      return;
    }

    const log = (...args) =>
      write(
        args
          .map((a) => (typeof a === "string" ? a : formatValue(a)))
          .join(" ")
      );

    try {
      // Wrap the user's code in an async function so `await` works, and inject
      // `Miniset` + `log` as locals. This is a docs playground running code the
      // visitor typed themselves — no untrusted third-party input involved.
      const userCode = codeEl.textContent;
      // eslint-disable-next-line no-new-func
      const fn = new Function(
        "Miniset",
        "log",
        `"use strict";\nreturn (async () => {\n${userCode}\n})();`
      );
      await fn(Miniset, log);
      if (!outputEl.textContent) write("(no output — call log(...) to print)");
      setStatus("Done", "ready");
    } catch (err) {
      write("\n⚠ " + (err && err.message ? err.message : String(err)));
      setStatus("Error", "error");
    } finally {
      runBtn.disabled = false;
    }
  };

  runBtn.addEventListener("click", run);

  if (resetBtn) {
    resetBtn.addEventListener("click", () => {
      codeEl.textContent = originalCode;
      outputEl.textContent = "Ready — press Run.";
      setStatus(modulePromise ? "WASM ready" : "Loading WASM…", "ready");
    });
  }

  // Ctrl/Cmd+Enter runs, matching common code-editor muscle memory.
  codeEl.addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
      e.preventDefault();
      if (!runBtn.disabled) run();
    }
  });
}

/**
 * Pretty-print non-string log arguments (numbers, objects, errors).
 * @param {unknown} value
 * @returns {string}
 */
function formatValue(value) {
  if (value instanceof Error) return value.stack || value.message;
  try {
    return JSON.stringify(value, null, 2) ?? String(value);
  } catch {
    return String(value);
  }
}

function boot() {
  document.querySelectorAll("[data-ms-playground]").forEach(initPlayground);
}

// Material's instant navigation swaps page content without a full reload, so
// hook both the initial load and subsequent SPA-style navigations.
if (typeof document$ !== "undefined" && document$.subscribe) {
  document$.subscribe(boot);
} else if (document.readyState !== "loading") {
  boot();
} else {
  document.addEventListener("DOMContentLoaded", boot);
}
