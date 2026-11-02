#!/usr/bin/env python3
"""
Development server for the test games in tools/.

Serves dist/ like serve.py, plus:

    /tools/<name>.love       tools/<name>/ zipped on the fly, e.g.
                             player.html?game=tools/https.love
    /test/echo               any method: echoes the request body; the method
                             and request headers come back as X-Echo-* headers
    /test/bytes?n=N          N bytes of 0, 1, ..., 255, 0, 1, ... (binary check)
    /test/sha256?path=P      SHA-256 of dist/P
    /test/status/<code>      responds with that status
    /test/redirect           302 to /test/echo
    /test/origin             this server's origin; ?other=1 swaps localhost
                             and 127.0.0.1 (a different origin, for CORS)
    /test/cors               allows any origin (Access-Control-Allow-Origin: *)

    python3 tools/test_server.py 8772
"""
import functools
import hashlib
import http.server
import io
import sys
import zipfile
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
TOOLS = ROOT / "tools"


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

    def reply(self, code, body=b"", headers=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def route(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        path = url.path

        if path.startswith("/tools/") and path.endswith(".love"):
            src = TOOLS / path[len("/tools/"):-len(".love")]
            if not src.is_dir():
                return self.reply(404, b"no such test game")
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
                for f in sorted(src.rglob("*")):
                    if f.is_file():
                        z.write(f, f.relative_to(src).as_posix())
            return self.reply(200, buf.getvalue(), {"Content-Type": "application/octet-stream"})

        if not path.startswith("/test/"):
            return False

        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        host = self.headers.get("Host", "localhost")

        if path == "/test/echo":
            headers = {"Content-Type": "application/octet-stream", "X-Echo-Method": self.command}
            for k, v in self.headers.items():
                headers["X-Echo-" + k] = v
            return self.reply(200, body, headers)
        if path == "/test/bytes":
            n = int(q.get("n", ["65536"])[0])
            return self.reply(200, bytes(i % 256 for i in range(n)), {"Content-Type": "application/octet-stream"})
        if path == "/test/sha256":
            f = (DIST / q["path"][0]).resolve()
            if DIST not in f.parents or not f.is_file():
                return self.reply(404, b"not found")
            return self.reply(200, hashlib.sha256(f.read_bytes()).hexdigest().encode())
        if path.startswith("/test/status/"):
            code = int(path.rsplit("/", 1)[1])
            return self.reply(code, f"status {code}".encode())
        if path == "/test/redirect":
            return self.reply(302, b"", {"Location": "/test/echo"})
        if path == "/test/origin":
            if q.get("other"):
                host = host.replace("localhost", "\0").replace("127.0.0.1", "localhost").replace("\0", "127.0.0.1")
            return self.reply(200, f"http://{host}".encode())
        if path == "/test/cors":
            return self.reply(200, b"cors ok", {"Access-Control-Allow-Origin": "*"})
        return self.reply(404, b"unknown test route")

    def do_GET(self):
        if self.route() is False:
            super().do_GET()

    def do_HEAD(self):
        if self.route() is False:
            super().do_HEAD()

    def do_POST(self):
        if self.route() is False:
            self.reply(405)

    do_PUT = do_DELETE = do_PATCH = do_POST


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8772
    handler = functools.partial(Handler, directory=str(DIST))
    with http.server.ThreadingHTTPServer(("", port), handler) as httpd:
        print(f"Serving {DIST} and test routes at http://localhost:{port}/", flush=True)
        httpd.serve_forever()


if __name__ == "__main__":
    main()
