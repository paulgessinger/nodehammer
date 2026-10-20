#!/usr/bin/env python3
"""Render one published project in Chromium; preserve evidence even on failure."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
import time

from PIL import Image, ImageStat
from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("build/browser-results"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "summary.json").unlink(missing_ok=True)
    (args.output / "scene.png").unlink(missing_ok=True)
    handler = partial(SimpleHTTPRequestHandler, directory=str(args.site.resolve()))
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    messages = []
    errors = []
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch(args=[
                "--use-angle=swiftshader", "--enable-unsafe-swiftshader",
            ])
            page = browser.new_page(viewport={"width": 1280, "height": 900})
            page.on("console", lambda message: messages.append(f"{message.type}: {message.text}"))
            page.on("pageerror", lambda error: errors.append(error.stack))
            try:
                page.goto(f"http://127.0.0.1:{server.server_port}/viewers/sphenix-root/"
                          "?backend=webgl2")
                deadline = time.monotonic() + 180
                status = None
                while time.monotonic() < deadline:
                    status = page.evaluate("() => window.Module?.nodehammerStatus ?? null")
                    if errors or (status and (status["ready"] or status["error"])):
                        break
                    page.wait_for_timeout(250)
                else:
                    raise RuntimeError(f"viewer did not settle within 180 seconds: {status}")
                if (not status or status["error"] or not status["drawCalls"] or
                        not status["triangles"] or errors):
                    raise RuntimeError(f"render failed: {status}, JS errors: {errors}")
                page.locator("canvas").screenshot(path=str(args.output / "scene.png"))
                image = Image.open(args.output / "scene.png").convert("RGB")
                # Draw counters alone cannot detect a blank final composite.
                if max(ImageStat.Stat(image).stddev) < 5:
                    raise RuntimeError("rendered canvas is effectively blank")
                (args.output / "summary.json").write_text(json.dumps(status, indent=2) + "\n")
            finally:
                (args.output / "console.log").write_text("\n".join(messages + errors))
                (args.output / "last-status.json").write_text(json.dumps(
                    page.evaluate("() => window.Module?.nodehammerStatus ?? null"), indent=2))
                page.screenshot(path=str(args.output / "page.png"))
                browser.close()
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
