#!/usr/bin/env python3
"""Static file server that disables all HTTP caching.

Drop-in for `python3 -m http.server`, but stamps every response with
`Cache-Control: no-store`. This exists specifically for the wasm viewer's
compute worker: its `.wasm` is fetched from *inside* a Web Worker
(`new Worker` -> `importScripts` -> `fetch`), and browsers don't apply a hard
reload's cache-bypass or DevTools "Disable Cache" to worker-initiated requests
(they live on a separate load group). The result is that a freshly rebuilt
worker module is silently served from the stale HTTP cache while the main-thread
viewer module updates -- a confusing split-brain. Sending `no-store` on every
response means nothing is ever cached, so each reload always fetches the
just-built files.

Usage: serve_nocache.py [port] [--directory DIR]   (default port 8000, cwd)
"""

from __future__ import annotations

import argparse
import contextlib
import functools
import http.server


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    """SimpleHTTPRequestHandler that forbids caching on every response."""

    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()


def main() -> None:
    parser = argparse.ArgumentParser(description="No-cache static file server.")
    parser.add_argument("port", nargs="?", type=int, default=8000, help="port (default 8000)")
    parser.add_argument("--directory", default=None, help="directory to serve (default cwd)")
    args = parser.parse_args()

    handler = NoCacheHandler
    if args.directory is not None:
        handler = functools.partial(NoCacheHandler, directory=args.directory)

    class Server(http.server.ThreadingHTTPServer):
        allow_reuse_address = True

    with Server(("", args.port), handler) as httpd:
        print(f"serving (no-cache) on http://localhost:{args.port}/")
        with contextlib.suppress(KeyboardInterrupt):
            httpd.serve_forever()


if __name__ == "__main__":
    main()
