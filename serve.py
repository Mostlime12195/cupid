#!/usr/bin/env python3
"""
Serve dist/ for local development.

Unlike `python3 -m http.server`, this disables HTTP caching (so rebuilt
engine files are always picked up) and serves .wasm as application/wasm so
browsers can compile it while it downloads.

    python3 serve.py            # http://localhost:8765
    python3 serve.py 9000
"""
import functools
import http.server
import sys
from pathlib import Path

DIST = Path(__file__).resolve().parent / "dist"


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".mjs": "text/javascript",
        ".js": "text/javascript",
        ".love": "application/octet-stream",
    }

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    handler = functools.partial(Handler, directory=str(DIST))
    with http.server.ThreadingHTTPServer(("", port), handler) as httpd:
        print(f"Serving {DIST} at http://localhost:{port}/")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
