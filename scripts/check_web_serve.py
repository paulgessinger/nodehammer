#!/usr/bin/env python3
"""Run `viewer --web`, fetch everything it serves, and stop it.

The staged root and the server agree or they do not, and the only cheap proof is
to ask the server for every file the runtime is supposed to contain. A browser
would prove more and costs a great deal more; this catches the failures that have
actually happened -- a payload missing the shell, a mount point that resolves
nothing, a MIME type that makes a browser refuse a bundle -- and needs nothing
but a socket.

It is also the end-to-end check for the Python pairing: point it at a console
script from a venv holding `nodehammer` and `nodehammer-web` and it exercises the
rung the wheel fills, which nothing else reaches.

Usage:
    scripts/check_web_serve.py <nodehammer-command> [args...]

Example:
    scripts/check_web_serve.py build/nodehammer
    scripts/check_web_serve.py .venv/bin/nodehammer
"""

from __future__ import annotations

import queue
import signal
import subprocess
import sys
import threading
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_pages_site import RUNTIME_FILES  # noqa: E402

# Generous, because the first run of a cold binary on a CI runner is not fast and
# a flaky check is worse than a slow one. Nothing here waits this long when it
# works.
STARTUP_TIMEOUT = 60.0
SHUTDOWN_TIMEOUT = 30.0
FETCH_TIMEOUT = 30.0


def fail(msg: str) -> None:
    sys.exit(f"web serve check failed: {msg}")


def _pump(stream, sink: queue.Queue) -> None:
    for line in stream:
        sink.put(line)
    sink.put(None)


def _await_url(lines: queue.Queue, seen: list[str]) -> str:
    """The URL the command printed, or a failure naming what it said instead."""
    while True:
        try:
            line = lines.get(timeout=STARTUP_TIMEOUT)
        except queue.Empty:
            fail(f"no URL within {STARTUP_TIMEOUT:.0f}s; output so far:\n" + "".join(seen))
        if line is None:
            fail("the command exited before serving anything; output:\n" + "".join(seen))
        seen.append(line)
        if line.startswith("serving "):
            return line.split(None, 1)[1].strip()


def _get(url: str) -> tuple[int, int]:
    """(status, body length). A refused connection is a status of 0."""
    try:
        with urllib.request.urlopen(url, timeout=FETCH_TIMEOUT) as response:  # noqa: S310
            return response.status, len(response.read())
    except urllib.error.HTTPError as exc:
        return exc.code, 0
    except OSError as exc:
        fail(f"{url}: {exc}")
    raise AssertionError("unreachable")


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    command = [*sys.argv[1:], "viewer", "--web", "--no-browser", "--port", "0"]
    print("$ " + " ".join(command), flush=True)

    # stderr is merged in rather than discarded: the ladder's explanation goes
    # there, and a run that finds no runtime is exactly the case where the
    # failure message is the useful part of the output.
    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    lines: queue.Queue = queue.Queue()
    seen: list[str] = []
    threading.Thread(target=_pump, args=(proc.stdout, lines), daemon=True).start()

    try:
        url = _await_url(lines, seen)
        print(f"serving {url}", flush=True)

        base = url.rstrip("/")
        # The shell through the mount point's index, not just by name: `/` and
        # `/index.html` are different code paths in the server, and `/` is the
        # one a browser asks for.
        for path in ("/", *(f"/{name}" for name in RUNTIME_FILES)):
            status, length = _get(base + path)
            if status != 200:
                fail(f"{path}: {status}")
            if length == 0:
                fail(f"{path}: served 200 but empty")
            print(f"  200  {length:>9}  {path}", flush=True)

        # No sidecar means application mode, and the absence *is* the signal --
        # index.html selects on it. A 200 here would mean the staging left one
        # behind and the viewer would come up locked to somebody else's project.
        status, _ = _get(base + "/nh_manifest.json")
        if status != 404:
            fail(f"/nh_manifest.json: expected 404 in application mode, got {status}")
        print("  404             /nh_manifest.json  (application mode)", flush=True)
    finally:
        # SIGINT rather than terminate: the command installs a handler so the
        # staging directory is removed, and checking that it exits 0 is what
        # proves the handler ran rather than the process being cut down.
        #
        # Nothing in here calls `fail`. This block also runs while a failure
        # above is on its way out, and a second `sys.exit` from a cleanup path
        # would replace the diagnosis with a complaint about shutdown.
        proc.send_signal(signal.SIGINT)
        try:
            code = proc.wait(timeout=SHUTDOWN_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = None

    if code is None:
        fail(f"did not stop within {SHUTDOWN_TIMEOUT:.0f}s of SIGINT")
    if code != 0:
        fail(f"exited {code} after SIGINT; output:\n" + "".join(seen))
    print("ok", flush=True)


if __name__ == "__main__":
    main()
