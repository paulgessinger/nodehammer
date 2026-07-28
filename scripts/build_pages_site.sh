#!/usr/bin/env bash
# Assemble the GitHub Pages site from a built wasm viewer runtime.
#
# The site shows both web postures the viewer supports (docs/viewer-project-
# strategy.md §"Postures"):
#
#   /app/         application mode — no sidecar, so the viewer comes up empty,
#                 restores the last project from IndexedDB, and accepts dropped
#                 .nhproj / loose files.
#   /odd/         viewer mode — a sidecar names a locked .nhproj holding the
#   /odd-simple/  full / simplified ODD scene; opening the page just builds it.
#
# Each posture is a self-contained directory (shell + runtime + payload), which
# is exactly the layout the viewer's own "Publish package" emits — so the site
# doubles as a check that a published package is servable as-is. The runtime is
# ~7 MB per copy; three copies keep every posture independently droppable.
#
# Usage:
#   scripts/build_pages_site.sh <runtime-dir> <out-dir>
#
# `runtime-dir` holds the built nodehammer-{gles3,wgpu,compute}.{js,wasm}
# bundles (an install tree's bin/, or build/emscripten/Release locally).

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <runtime-dir> <out-dir>" >&2
    exit 2
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Absolute, so a relative runtime dir keeps working once we stage per posture.
runtime="$(cd "$1" && pwd)"
out="$2"

case "$out" in
    /*) ;;
    *) out="$root/$out" ;;
esac

rm -rf "$out"
mkdir -p "$out"

# posture dir : scene (none = application mode)
stage() {
    "$root/scripts/stage_wasm_viewer.sh" copy "$2" "$out/$1" "$runtime" >/dev/null
    echo "  staged $1/ ($2)"
}

stage app none
stage odd odd
stage odd-simple odd_simple

# Landing page. Deliberately dependency-free and served from the site root so
# the deployment explains itself; each card links into one posture directory.
commit="${GITHUB_SHA:-$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)}"
short_commit="${commit:0:7}"
built="$(date -u '+%Y-%m-%d %H:%M UTC')"

cat > "$out/index.html" <<HTML
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>nodehammer — web viewer</title>
<style>
    :root { color-scheme: dark; }
    body {
        margin: 0; padding: 4rem 1.5rem;
        background: #1a1f25; color: #c5cdd9;
        font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
        line-height: 1.55;
    }
    main { max-width: 46rem; margin: 0 auto; }
    h1 { font-size: 1.6rem; margin: 0 0 .35rem; color: #e8edf3; }
    p.lede { margin: 0 0 2.5rem; opacity: .7; }
    a.card {
        display: block; padding: 1.1rem 1.3rem; margin-bottom: .9rem;
        border: 1px solid #2c3540; border-radius: 8px;
        background: #212831; color: inherit; text-decoration: none;
        transition: border-color .15s, background .15s;
    }
    a.card:hover { border-color: #4a5b6e; background: #263039; }
    a.card strong { display: block; color: #e8edf3; font-size: 1.05rem; }
    a.card span { opacity: .68; font-size: .92rem; }
    footer { margin-top: 3rem; font-size: .82rem; opacity: .5; }
    code { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
</style>
</head>
<body>
<main>
    <h1>nodehammer web viewer</h1>
    <p class="lede">
        The same wasm viewer in its two web postures — an open application you
        feed your own project, and preconfigured publications that open a
        locked scene straight away.
    </p>

    <a class="card" href="app/viewer.html">
        <strong>Application</strong>
        <span>
            Comes up empty and restores your last project from browser storage.
            Drop a <code>.nhproj</code> archive or loose config/geometry files
            onto the canvas to build a scene.
        </span>
    </a>

    <a class="card" href="odd/viewer.html">
        <strong>Open Data Detector — full</strong>
        <span>
            Publication of <code>fixtures/configs/odd.toml</code>: tracker,
            calorimeters and muon spectrometer, content-locked.
        </span>
    </a>

    <a class="card" href="odd-simple/viewer.html">
        <strong>Open Data Detector — simplified</strong>
        <span>
            Publication of <code>fixtures/configs/odd_simple.toml</code>:
            sensitive tracker volumes and coarse calorimeter / muon envelopes.
            Lighter to build — start here on a modest machine.
        </span>
    </a>

    <footer>
        Built from <code>${short_commit}</code> on ${built}. WebGPU is used when
        the browser exposes it, WebGL2 otherwise; append
        <code>?backend=webgl2</code> to force the fallback.
    </footer>
</main>
</body>
</html>
HTML

echo "site assembled in $out"
