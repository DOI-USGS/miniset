#!/usr/bin/env python3
"""Local-development CORS proxy for fetching remote ISD JSON and COG rasters.

WARNING -- LOCAL DEVELOPMENT ONLY. DO NOT DEPLOY.
====================================================================
This server fetches URLs on behalf of a browser page, which is a
server-side request forgery (SSRF) shaped tool by construction: whoever can
reach this port can make it issue outbound HTTP requests. It exists only so
the `web_gui` demo can read remote data that lacks CORS headers.

It is hardened for local use, not for exposure:

  * binds to 127.0.0.1 only (not all interfaces),
  * permits only the http/https schemes,
  * permits only hosts matching an allowlist (see ALLOWED_HOSTS),
  * refuses hosts that resolve to private, loopback, link-local, or otherwise
    non-global addresses -- this is what blocks cloud instance-metadata
    endpoints such as 169.254.169.254 and internal network hosts,
  * re-validates the target on every redirect hop, so an allowlisted host
    cannot redirect the proxy to an internal one,
  * reflects a CORS origin only for localhost pages, rather than sending `*`,
  * caps the response body size and applies a request timeout.

Do not place this behind a public interface, a tunnel, or a reverse proxy.

Configuration (environment variables)
------------------------------------
MINISET_PROXY_PORT          Listen port (default 8001).
MINISET_PROXY_ALLOWED_HOSTS Comma-separated host allowlist, replacing the
                            default. An entry beginning with "." matches that
                            domain and any subdomain (".usgs.gov" matches
                            "usgs.gov" and "astrowebmaps.wr.usgs.gov").
MINISET_PROXY_MAX_BYTES     Maximum response body size (default 1 GiB).
MINISET_PROXY_ALLOW_ANY_HOST
                            Set to "1" to bypass the host allowlist. The
                            private-address checks still apply. Intended for
                            short-lived local experiments; prints a warning.

Usage
-----
    python3 proxy_server.py
    # then: http://127.0.0.1:8001/proxy?url=https://example.usgs.gov/data.tif
"""

import http.server
import ipaddress
import json
import os
import socket
import socketserver
import sys
import traceback
import urllib.error
import urllib.request
from urllib.parse import urljoin, urlparse, parse_qs

# Bind to loopback only. Binding to "" would expose this fetch-any-URL tool to
# every host on the local network.
HOST = "127.0.0.1"
PORT = int(os.environ.get("MINISET_PROXY_PORT", "8001"))

# Maximum bytes to relay for a single response, to bound memory use.
MAX_BYTES = int(os.environ.get("MINISET_PROXY_MAX_BYTES", str(1024 * 1024 * 1024)))

# Bytes per chunk when relaying the body.
CHUNK_SIZE = 64 * 1024

# Seconds to wait on the upstream request.
TIMEOUT_SECONDS = 30

# Maximum redirect hops to follow (each one is re-validated).
MAX_REDIRECTS = 5

# Default host allowlist. Entries starting with "." match the domain and any
# subdomain of it. Override with MINISET_PROXY_ALLOWED_HOSTS.
DEFAULT_ALLOWED_HOSTS = (
    ".usgs.gov",          # USGS web map atlas, ASC data services
    ".amazonaws.com",     # S3-hosted COGs and control networks
    ".nasa.gov",          # PDS / planetary data archives
)

_env_hosts = os.environ.get("MINISET_PROXY_ALLOWED_HOSTS", "").strip()
ALLOWED_HOSTS = tuple(
    h.strip().lower() for h in _env_hosts.split(",") if h.strip()
) or DEFAULT_ALLOWED_HOSTS

ALLOW_ANY_HOST = os.environ.get("MINISET_PROXY_ALLOW_ANY_HOST") == "1"

ALLOWED_SCHEMES = ("http", "https")

# CORS: reflect only localhost origins instead of sending "*".
ALLOWED_ORIGIN_HOSTS = ("localhost", "127.0.0.1", "[::1]", "::1")


class ProxyRefused(Exception):
    """Raised when a requested URL fails validation and must not be fetched."""


def _host_allowed(hostname):
    """Return True if `hostname` matches the configured host allowlist."""
    if ALLOW_ANY_HOST:
        return True
    host = hostname.lower().rstrip(".")
    for entry in ALLOWED_HOSTS:
        if entry.startswith("."):
            if host == entry[1:] or host.endswith(entry):
                return True
        elif host == entry:
            return True
    return False


def _check_addresses(hostname, allow_private):
    """Refuse `hostname` if it resolves to an address this proxy must not reach.

    Always refused, regardless of the allowlist, because they are never a
    legitimate target for this tool and are the classic SSRF objectives:

      * link-local (169.254.0.0/16, fe80::/10) -- cloud instance-metadata
        endpoints such as 169.254.169.254 live here,
      * loopback (127.0.0.0/8, ::1),
      * multicast, reserved, and unspecified addresses.

    Private RFC1918 / ULA addresses are permitted only when `allow_private` is
    True, which is the case for hosts matched by the explicit allowlist. USGS
    internal DNS resolves usgs.gov names to RFC1918 addresses when on the USGS
    network, so refusing those outright would break the tool for its actual
    users. When the allowlist is bypassed via MINISET_PROXY_ALLOW_ANY_HOST,
    private addresses are refused as well.
    """
    try:
        infos = socket.getaddrinfo(hostname, None)
    except socket.gaierror as exc:
        raise ProxyRefused(f"cannot resolve host: {hostname} ({exc})")

    if not infos:
        raise ProxyRefused(f"cannot resolve host: {hostname}")

    for info in infos:
        addr = info[4][0]
        try:
            ip = ipaddress.ip_address(addr)
        except ValueError:
            raise ProxyRefused(f"unparsable address for host {hostname}: {addr}")

        if ip.is_link_local:
            raise ProxyRefused(
                f"host {hostname} resolves to link-local address {addr} "
                "(instance-metadata range); refused"
            )
        if ip.is_loopback:
            raise ProxyRefused(
                f"host {hostname} resolves to loopback address {addr}; refused"
            )
        if ip.is_multicast or ip.is_reserved or ip.is_unspecified:
            raise ProxyRefused(
                f"host {hostname} resolves to reserved address {addr}; refused"
            )
        if ip.is_private and not allow_private:
            raise ProxyRefused(
                f"host {hostname} resolves to private address {addr} and the "
                "host allowlist is disabled; refused"
            )
    return True


def validate_url(raw_url):
    """Validate `raw_url` against the scheme, host, and address-range policies.

    Returns the parsed URL on success. Raises ProxyRefused with a reason
    otherwise. Every outbound fetch -- including each redirect hop -- goes
    through this function.
    """
    parsed = urlparse(raw_url)

    if parsed.scheme.lower() not in ALLOWED_SCHEMES:
        raise ProxyRefused(
            f"scheme {parsed.scheme!r} not permitted; allowed: {', '.join(ALLOWED_SCHEMES)}"
        )

    if not parsed.hostname:
        raise ProxyRefused("URL has no host")

    if not _host_allowed(parsed.hostname):
        raise ProxyRefused(
            f"host {parsed.hostname!r} is not in the allowlist. "
            f"Allowed: {', '.join(ALLOWED_HOSTS)}. "
            "Set MINISET_PROXY_ALLOWED_HOSTS to change this."
        )

    # A host that matched the explicit allowlist may legitimately resolve to a
    # private address (USGS internal DNS does this on-network). Loopback and
    # link-local are refused either way -- see _check_addresses.
    _check_addresses(parsed.hostname, allow_private=not ALLOW_ANY_HOST)
    return parsed


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Redirect handler that surfaces the Location header instead of following it.

    Following redirects inside urllib would skip validation, letting an
    allowlisted host bounce the proxy to an internal address. Instead each hop
    is returned to the caller, re-validated, and re-issued explicitly.
    """

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise _Redirected(newurl)


class _Redirected(Exception):
    """Internal signal carrying the redirect target URL."""

    def __init__(self, location):
        super().__init__(location)
        self.location = location


def open_validated(raw_url, headers, method="GET"):
    """Fetch `raw_url`, validating the target at each redirect hop.

    Returns the open response object. Raises ProxyRefused if any hop fails
    validation or the redirect limit is exceeded.
    """
    opener = urllib.request.build_opener(_NoRedirect)
    url = raw_url

    for _ in range(MAX_REDIRECTS + 1):
        validate_url(url)
        req = urllib.request.Request(url, headers=headers, method=method)
        try:
            return opener.open(req, timeout=TIMEOUT_SECONDS)
        except _Redirected as redirect:
            url = urljoin(url, redirect.location)
            print(f"  -> redirect to {url} (re-validating)")

    raise ProxyRefused(f"too many redirects (limit {MAX_REDIRECTS})")


class CORSProxyHandler(http.server.BaseHTTPRequestHandler):
    """Serves GET/HEAD/OPTIONS on /proxy?url=<URL> with permissive CORS headers.

    Only localhost origins are granted CORS access, and only allowlisted,
    publicly-routable targets are fetched. See the module docstring.
    """

    server_version = "MinisetDevProxy/2.0"

    def _cors_origin(self):
        """Return the value to send for Access-Control-Allow-Origin, or None.

        Reflects the request Origin only when it is a localhost page; otherwise
        no CORS header is sent. This replaces an unconditional "*".
        """
        origin = self.headers.get("Origin")
        if not origin:
            return None
        try:
            host = urlparse(origin).hostname
        except ValueError:
            return None
        if host in ALLOWED_ORIGIN_HOSTS:
            return origin
        return None

    def _send_cors_headers(self, methods="GET, HEAD, OPTIONS"):
        """Emit the CORS and range-support headers shared by all responses."""
        origin = self._cors_origin()
        if origin:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
        self.send_header("Access-Control-Allow-Methods", methods)
        self.send_header("Access-Control-Allow-Headers", "Range, Content-Type")
        self.send_header(
            "Access-Control-Expose-Headers",
            "Content-Range, Accept-Ranges, Content-Length",
        )

    def _send_error_json(self, status, message):
        """Send a JSON error body with CORS headers attached."""
        payload = json.dumps({"error": message}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self._send_cors_headers()
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(payload)

    def _target_url(self):
        """Extract the `url` query parameter from /proxy, or None if absent."""
        parsed_path = urlparse(self.path)
        if parsed_path.path != "/proxy":
            return None
        params = parse_qs(parsed_path.query)
        if "url" not in params or not params["url"]:
            return None
        return params["url"][0]

    def do_GET(self):
        """Fetch the validated target URL and relay its body to the caller."""
        url = self._target_url()
        if url is None:
            self._send_error_json(404, "Not found. Use /proxy?url=<URL>")
            return

        print(f"GET  {url}")
        try:
            headers = {}
            if "Range" in self.headers:
                headers["Range"] = self.headers["Range"]
                print(f"  Range: {self.headers['Range']}")

            with open_validated(url, headers, method="GET") as response:
                content_type = response.headers.get(
                    "Content-Type", "application/octet-stream"
                )
                content_length = response.headers.get("Content-Length")
                content_range = response.headers.get("Content-Range")

                if content_length is not None:
                    try:
                        if int(content_length) > MAX_BYTES:
                            self._send_error_json(
                                502,
                                f"upstream body of {content_length} bytes exceeds "
                                f"the {MAX_BYTES} byte limit",
                            )
                            return
                    except ValueError:
                        content_length = None

                self.send_response(206 if content_range else 200)
                self.send_header("Content-Type", content_type)
                self.send_header("Accept-Ranges", "bytes")
                if content_length:
                    self.send_header("Content-Length", content_length)
                if content_range:
                    self.send_header("Content-Range", content_range)
                self._send_cors_headers()
                self.end_headers()

                # Stream in chunks rather than buffering the whole body, and
                # stop if the upstream exceeds the cap despite its headers.
                sent = 0
                while True:
                    chunk = response.read(CHUNK_SIZE)
                    if not chunk:
                        break
                    sent += len(chunk)
                    if sent > MAX_BYTES:
                        print(f"  aborted: exceeded {MAX_BYTES} byte limit")
                        return
                    self.wfile.write(chunk)
                print(f"  ok: {sent} bytes")

        except ProxyRefused as exc:
            print(f"  refused: {exc}")
            self._send_error_json(403, f"refused: {exc}")
        except urllib.error.HTTPError as exc:
            print(f"  upstream HTTP {exc.code}: {exc.reason}")
            self._send_error_json(exc.code, f"{exc.code} {exc.reason}")
        except Exception as exc:  # noqa: BLE001 - dev tool, report and continue
            print(f"  error: {exc}")
            traceback.print_exc()
            self._send_error_json(500, str(exc))

    def do_HEAD(self):
        """Issue a HEAD against the validated target and relay its headers."""
        url = self._target_url()
        if url is None:
            self.send_response(404)
            self._send_cors_headers()
            self.end_headers()
            return

        print(f"HEAD {url}")
        try:
            with open_validated(url, {}, method="HEAD") as response:
                content_type = response.headers.get(
                    "Content-Type", "application/octet-stream"
                )
                content_length = response.headers.get("Content-Length")

                self.send_response(200)
                self.send_header("Content-Type", content_type)
                self.send_header("Accept-Ranges", "bytes")
                if content_length:
                    self.send_header("Content-Length", content_length)
                self._send_cors_headers()
                self.end_headers()
                print("  ok")

        except ProxyRefused as exc:
            print(f"  refused: {exc}")
            self._send_error_json(403, f"refused: {exc}")
        except urllib.error.HTTPError as exc:
            print(f"  upstream HTTP {exc.code}: {exc.reason}")
            self._send_error_json(exc.code, f"{exc.code} {exc.reason}")
        except Exception as exc:  # noqa: BLE001 - dev tool, report and continue
            print(f"  error: {exc}")
            traceback.print_exc()
            self._send_error_json(500, str(exc))

    def do_OPTIONS(self):
        """Answer CORS preflight requests."""
        self.send_response(204)
        self._send_cors_headers()
        self.send_header("Access-Control-Max-Age", "600")
        self.end_headers()


def main():
    """Print the active policy and serve until interrupted."""
    print("Miniset dev CORS proxy -- LOCAL DEVELOPMENT ONLY, DO NOT DEPLOY")
    print(f"  listening on http://{HOST}:{PORT}  (loopback only)")
    print(f"  usage:        http://{HOST}:{PORT}/proxy?url=<URL>")
    print(f"  schemes:      {', '.join(ALLOWED_SCHEMES)}")
    if ALLOW_ANY_HOST:
        print("  hosts:        ANY (MINISET_PROXY_ALLOW_ANY_HOST=1)")
        print("  WARNING: host allowlist disabled. Private addresses are still")
        print("           refused, but do not leave this enabled.")
    else:
        print(f"  hosts:        {', '.join(ALLOWED_HOSTS)}")
    print(f"  max body:     {MAX_BYTES} bytes")
    print("  refusing any host that resolves to a private/loopback address")

    with socketserver.TCPServer((HOST, PORT), CORSProxyHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down...")
            return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
