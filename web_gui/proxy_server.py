#!/usr/bin/env python3
"""
Simple CORS proxy server for fetching ISD JSON files
"""
import http.server
import socketserver
import urllib.request
import json
import traceback
from urllib.parse import urlparse, parse_qs

PORT = 8001

class CORSProxyHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        # Parse query parameters
        parsed_path = urlparse(self.path)
        params = parse_qs(parsed_path.query)

        if parsed_path.path == '/proxy' and 'url' in params:
            url = params['url'][0]
            print(f"Proxying request to: {url}")

            try:
                # Build request with range header if provided
                headers = {}
                if 'Range' in self.headers:
                    headers['Range'] = self.headers['Range']
                    print(f"  Range request: {self.headers['Range']}")

                req = urllib.request.Request(url, headers=headers)

                # Fetch the URL
                with urllib.request.urlopen(req, timeout=30) as response:
                    # Get response headers
                    content_type = response.headers.get('Content-Type', 'application/octet-stream')
                    content_length = response.headers.get('Content-Length')
                    content_range = response.headers.get('Content-Range')

                    # Read data
                    data = response.read()

                    # Determine status code
                    status_code = 206 if content_range else 200

                    # Send response with CORS headers
                    self.send_response(status_code)
                    self.send_header('Content-Type', content_type)
                    self.send_header('Access-Control-Allow-Origin', '*')
                    self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
                    self.send_header('Access-Control-Allow-Headers', 'Range, Content-Type')
                    self.send_header('Access-Control-Expose-Headers', 'Content-Range, Accept-Ranges, Content-Length')
                    self.send_header('Accept-Ranges', 'bytes')

                    if content_length:
                        self.send_header('Content-Length', content_length)
                    if content_range:
                        self.send_header('Content-Range', content_range)

                    self.end_headers()
                    self.wfile.write(data)
                    print(f"✓ Successfully proxied {len(data)} bytes (status {status_code})")

            except urllib.error.HTTPError as e:
                # Return the same status code from S3, but with CORS headers
                print(f"✗ HTTP {e.code} from upstream: {e.reason}")
                self.send_response(e.code)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Access-Control-Allow-Origin', '*')
                self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
                self.send_header('Access-Control-Allow-Headers', 'Range, Content-Type')
                self.end_headers()
                error_msg = json.dumps({'error': f'{e.code} {e.reason}'})
                self.wfile.write(error_msg.encode())
            except Exception as e:
                print(f"✗ Error fetching URL: {e}")
                traceback.print_exc()
                self.send_response(500)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Access-Control-Allow-Origin', '*')
                self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
                self.send_header('Access-Control-Allow-Headers', 'Range, Content-Type')
                self.end_headers()
                error_msg = json.dumps({'error': str(e)})
                self.wfile.write(error_msg.encode())
        else:
            # Return 404 for other paths
            self.send_response(404)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(b'Not found. Use /proxy?url=<URL>')

    def do_HEAD(self):
        # Handle HEAD requests (same as GET but no body)
        parsed_path = urlparse(self.path)
        params = parse_qs(parsed_path.query)
        print(f"HEAD request path: {parsed_path.path}, has url param: {'url' in params}")

        if parsed_path.path == '/proxy' and 'url' in params:
            url = params['url'][0]
            print(f"HEAD request to: {url}")

            try:
                req = urllib.request.Request(url, method='HEAD')
                with urllib.request.urlopen(req, timeout=30) as response:
                    content_type = response.headers.get('Content-Type', 'application/octet-stream')
                    content_length = response.headers.get('Content-Length')

                    self.send_response(200)
                    self.send_header('Content-Type', content_type)
                    self.send_header('Access-Control-Allow-Origin', '*')
                    self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS')
                    self.send_header('Access-Control-Allow-Headers', 'Range, Content-Type')
                    self.send_header('Access-Control-Expose-Headers', 'Content-Range, Accept-Ranges, Content-Length')
                    self.send_header('Accept-Ranges', 'bytes')
                    if content_length:
                        self.send_header('Content-Length', content_length)
                    self.end_headers()
                    print(f"✓ HEAD successful")

            except urllib.error.HTTPError as e:
                print(f"✗ HTTP {e.code} from upstream: {e.reason}")
                self.send_response(e.code)
                self.send_header('Access-Control-Allow-Origin', '*')
                self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS')
                self.send_header('Access-Control-Allow-Headers', 'Range, Content-Type')
                self.end_headers()
            except Exception as e:
                print(f"✗ Error with HEAD request: {e}")
                traceback.print_exc()
                self.send_response(500)
                self.send_header('Access-Control-Allow-Origin', '*')
                self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS')
                self.send_header('Access-Control-Allow-Headers', 'Range, Content-Type')
                self.end_headers()
        else:
            # Return 404 with CORS headers
            self.send_response(404)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()

    def do_OPTIONS(self):
        # Handle preflight requests
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, HEAD, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Range, Content-Type')
        self.send_header('Access-Control-Expose-Headers', 'Content-Range, Accept-Ranges, Content-Length')
        self.end_headers()

if __name__ == '__main__':
    with socketserver.TCPServer(("", PORT), CORSProxyHandler) as httpd:
        print(f"CORS Proxy server running on http://localhost:{PORT}")
        print(f"Usage: http://localhost:{PORT}/proxy?url=<URL>")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down...")
