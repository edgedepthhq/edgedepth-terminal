#!/usr/bin/env python3
"""
Local dev server with COOP/COEP headers for SharedArrayBuffer / WASM threading.
Usage: python3 serve_threaded.py [port] [directory]
  e.g. python3 serve_threaded.py 8000 build-threaded
"""

import http.server
import sys
import os

class COOPCOEPHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()

    def guess_type(self, path):
        if path.endswith(".wasm"):
            return "application/wasm"
        return super().guess_type(path)

    def send_range(self, path):
        """Minimal single-range GET support (bytes=a-b / bytes=a-), so local
        .edpack replay uses the same range-fetch path as R2/CDN instead of the
        whole-file fallback (a 400MB pack should not be slurped into WASM
        memory in one go). Returns True when a 206 was served."""
        spec = self.headers.get("Range", "")
        if not spec.startswith("bytes=") or "," in spec:
            return False
        try:
            size = os.path.getsize(path)
            lo_s, _, hi_s = spec[len("bytes="):].partition("-")
            lo = int(lo_s)
            hi = int(hi_s) if hi_s else size - 1
            hi = min(hi, size - 1)
            if lo > hi or lo < 0:
                return False
        except (ValueError, OSError):
            return False
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {lo}-{hi}/{size}")
        self.send_header("Content-Length", str(hi - lo + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(lo)
            remaining = hi - lo + 1
            while remaining > 0:
                chunk = f.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)
        return True

    def do_GET(self):
        # SPA fallback: serve index.html for routes that don't match a file
        # but NOT for asset requests (js, wasm, data, fonts, etc.)
        path = self.translate_path(self.path)
        if not os.path.exists(path):
            # Check if this looks like an asset request
            asset_exts = ('.js', '.wasm', '.data', '.woff', '.woff2', '.ttf',
                          '.otf', '.png', '.jpg', '.svg', '.css', '.map', '.mem',
                          '.edpack')
            if any(self.path.endswith(ext) for ext in asset_exts):
                # Strip path prefix - serve from root (e.g. /terminal/index.js -> /index.js)
                basename = '/' + os.path.basename(self.path)
                root_path = self.translate_path(basename)
                if os.path.exists(root_path):
                    self.path = basename
            else:
                self.path = "/index.html"
        # Range support for files that exist (packs range-fetch like the CDN).
        real = self.translate_path(self.path)
        if os.path.isfile(real) and "Range" in self.headers:
            if self.send_range(real):
                return
        return super().do_GET()

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    directory = sys.argv[2] if len(sys.argv) > 2 else "."

    os.chdir(directory)
    server = http.server.HTTPServer(("", port), COOPCOEPHandler)
    print(f"Serving {os.path.abspath('.')} on http://localhost:{port}")
    print("COOP/COEP headers enabled - SharedArrayBuffer available")
    print("Ctrl+C to stop")
    server.serve_forever()
