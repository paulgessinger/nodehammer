#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

usage() {
    echo "usage: $0 <link|copy> <scene|archive.nhproj|none> <target-dir> [src-dir]" >&2
    echo "  scene names any fixtures/configs/<scene>.toml (e.g. odd, odd_simple)," >&2
    echo "  packed into a .nhproj + nh_manifest.json sidecar -> viewer mode." >&2
    echo "  a path ending in .nhproj is staged as-is (however it was built --" >&2
    echo "  e.g. native File > Create archive from scene > Save As) -> viewer mode." >&2
    echo "  'none' omits the sidecar -> the viewer comes up as the application." >&2
}

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    usage
    exit 2
fi

mode="$1"
scene="$2"
target="$3"
src_override="${4:-}"

case "$mode" in
    link|copy) ;;
    *)
        usage
        exit 2
        ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -n "$src_override" ]; then
    src="$src_override"
else
    src="$root/build/emscripten/RelWithDebInfo"
fi
out="$target"
case "$out" in
    /*) ;;
    *) out="$root/$out" ;;
esac

# A scene ending in .nhproj is an already-built archive (any provenance --
# make_nhproj.py, native Save As, a web-published download) staged as-is
# instead of one we pack ourselves. Resolved up front so the stale-sidecar
# cleanup below can avoid deleting it out from under itself when the caller
# points at a file already inside $out (e.g. re-running against last run's
# output).
archive_src=""
case "$scene" in
    *.nhproj)
        archive_src="$scene"
        case "$archive_src" in
            /*) ;;
            *) archive_src="$root/$archive_src" ;;
        esac
        if [ ! -f "$archive_src" ]; then
            echo "no such archive: $scene" >&2
            exit 1
        fi
        ;;
esac

if [ ! -f "$src/nodehammer-gles3.js" ] && [ ! -f "$src/nodehammer-wgpu.js" ]; then
    echo "no nodehammer-{gles3,wgpu}.js bundle in $src - run 'just wasm-build' first." >&2
    exit 1
fi

stage_file() {
    local from="$1"
    local to="$2"
    if [ "$mode" = copy ]; then
        cp -f "$from" "$to"
    elif [ "$from" != "$to" ]; then
        ln -sf "$from" "$to"
    fi
}

mkdir -p "$out"
stage_file "$root/web/viewer.html" "$out/viewer.html"
# Worker script that hosts the headless compute module (off-main-thread
# tessellation/wedge cut). Served alongside the viewer; loaded as a classic
# Worker by the viewer when one is available.
stage_file "$root/src/web/compute_worker.js" "$out/compute_worker.js"

# Compute module bundle (nodehammer-compute.{js,wasm}) is staged alongside the
# per-backend viewer bundles.
bundles=("$src"/nodehammer-gles3.* "$src"/nodehammer-wgpu.* "$src"/nodehammer-compute.*)
if [ "${#bundles[@]}" -eq 0 ]; then
    echo "no nodehammer-{gles3,wgpu} artifacts in $src - run 'just wasm-build' first." >&2
    exit 1
fi
for bundle in "${bundles[@]}"; do
    stage_file "$bundle" "$out/$(basename "$bundle")"
done

# Clean any stale manifest/archive from a previous run; we'll re-emit below only
# if a scene was requested. Also drop the loose scene files earlier recipes
# staged (pre-archive sidecar schema) and legacy synthetic-name symlinks.
rm -f "$out/nh_manifest.json"
for stale_archive in "$out"/project.*.nhproj; do
    if [ -n "$archive_src" ] && [ "$stale_archive" -ef "$archive_src" ]; then
        continue
    fi
    rm -f "$stale_archive"
done
rm -f "$out/scene.nhb.zst" "$out/scene.toml" "$out/odd.nhb.zst"
rm -rf "$out/odd"
# Entry configs were staged at the root under their fixture name; only remove a
# .toml that is one of ours, never something else living in the target dir.
for stale in "$out"/*.toml; do
    if [ -e "$root/fixtures/configs/$(basename "$stale")" ]; then
        rm -f "$stale"
    fi
done

case "$scene" in
    none|'')
        scene_mode="application mode (no sidecar)"
        ;;
    *.nhproj)
        archive="$(basename "$archive_src")"
        stage_file "$archive_src" "$out/$archive"

        printf '%s\n' \
            '{' \
            "  \"archive\": \"$archive\"," \
            '  "lock": true,' \
            "  \"title\": \"nodehammer — $archive\"" \
            '}' \
            > "$out/nh_manifest.json"
        scene_mode="viewer mode (archive $scene -> $archive)"
        ;;
    *)
        if [ ! -f "$root/fixtures/configs/$scene.toml" ]; then
            echo "unknown scene '$scene' - no fixtures/configs/$scene.toml (use 'none' for application mode)" >&2
            exit 1
        fi

        # Viewer mode is archive-driven: pack the scene (entry config + its
        # include chain + geometry + project manifest) into a content-hashed
        # `.nhproj`, then point the sidecar at it. The archive is always a real
        # file — `link` mode only shortcuts the shell/runtime files.
        archive="$("$root/scripts/make_nhproj.py" "$scene" "$out")"

        printf '%s\n' \
            '{' \
            "  \"archive\": \"$archive\"," \
            '  "lock": true,' \
            "  \"title\": \"nodehammer — $scene\"" \
            '}' \
            > "$out/nh_manifest.json"
        scene_mode="viewer mode ($scene -> $archive)"
        ;;
esac

printf '%s\n' "$out"
printf '%s\n' "$scene_mode"
