#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

usage() {
    echo "usage: $0 <link|copy> <odd|none> <target-dir> [src-dir]" >&2
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

bundles=("$src"/nodehammer-gles3.* "$src"/nodehammer-wgpu.*)
if [ "${#bundles[@]}" -eq 0 ]; then
    echo "no nodehammer-{gles3,wgpu} artifacts in $src - run 'just wasm-build' first." >&2
    exit 1
fi
for bundle in "${bundles[@]}"; do
    stage_file "$bundle" "$out/$(basename "$bundle")"
done

# Clean any stale manifest from a previous run; we'll re-emit below only if a
# scene was requested. Drop legacy synthetic-name symlinks from earlier recipes.
rm -f "$out/nh_manifest.json"
rm -f "$out/scene.nhb.zst" "$out/scene.toml"

case "$scene" in
    none|'')
        scene_mode="upload-only (no manifest)"
        ;;
    odd)
        stage_file "$root/odd.nhb.zst" "$out/odd.nhb.zst"
        stage_file "$root/fixtures/configs/odd.toml" "$out/odd.toml"
        mkdir -p "$out/odd"
        for f in base.toml materials.toml tracker.toml calorimeters.toml muon.toml; do
            stage_file "$root/fixtures/configs/odd/$f" "$out/odd/$f"
        done
        printf '%s\n' \
            '{' \
            '  "input": "odd.nhb.zst",' \
            '  "config": "odd.toml",' \
            '  "title": "Open Data Detector"' \
            '}' \
            > "$out/nh_manifest.json"
        scene_mode="autoload (odd)"
        ;;
    *)
        echo "unknown scene '$scene' - known: odd, none" >&2
        exit 1
        ;;
esac

printf '%s\n' "$out"
printf '%s\n' "$scene_mode"
