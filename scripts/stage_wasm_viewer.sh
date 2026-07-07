#!/usr/bin/env bash
set -euo pipefail
shopt -s nullglob

usage() {
    echo "usage: $0 <link|copy> <scene|none> <target-dir> [src-dir]" >&2
    echo "  scene names any fixtures/configs/<scene>.toml (e.g. odd, odd_simple)" >&2
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

# Clean any stale manifest from a previous run; we'll re-emit below only if a
# scene was requested. Drop legacy synthetic-name symlinks from earlier recipes.
rm -f "$out/nh_manifest.json"
rm -f "$out/scene.nhb.zst" "$out/scene.toml"

# Recursively collects the `include = [...]` (or `include = "..."`) paths of a
# fixtures/configs/*.toml file, relative to fixtures/configs/. Good enough for
# these configs' plain quoted-string includes; not a general TOML parser.
collect_includes() {
    local rel="$1"
    local path="$configs_dir/$rel"
    local reldir
    reldir="$(dirname "$rel")"
    [ -f "$path" ] || { echo "config include not found: $rel" >&2; exit 1; }
    if [[ " ${seen[*]} " == *" $rel "* ]]; then
        return 0
    fi
    seen+=("$rel")

    local include_expr
    include_expr="$(perl -0777 -ne 'print $1 if /^\s*include\s*=\s*(\[[^\]]*\]|"[^"]*")/ms' "$path")"
    [ -n "$include_expr" ] || return 0
    while IFS= read -r inc; do
        [ -n "$inc" ] || continue
        local inc_rel
        if [ "$reldir" = "." ]; then
            inc_rel="$inc"
        else
            inc_rel="$reldir/$inc"
        fi
        collect_includes "$inc_rel"
    done < <(printf '%s' "$include_expr" | grep -o '"[^"]*"' | tr -d '"')
}

case "$scene" in
    none|'')
        scene_mode="upload-only (no manifest)"
        ;;
    *)
        configs_dir="$root/fixtures/configs"
        scene_toml="$configs_dir/$scene.toml"
        if [ ! -f "$scene_toml" ]; then
            echo "unknown scene '$scene' - no fixtures/configs/$scene.toml (use 'none' for upload-only)" >&2
            exit 1
        fi

        seen=()
        collect_includes "$scene.toml"

        stage_file "$root/odd.nhb.zst" "$out/odd.nhb.zst"
        for rel in "${seen[@]}"; do
            mkdir -p "$out/$(dirname "$rel")"
            stage_file "$configs_dir/$rel" "$out/$rel"
        done

        printf '%s\n' \
            '{' \
            '  "input": "odd.nhb.zst",' \
            "  \"config\": \"$scene.toml\"," \
            "  \"title\": \"$scene\"" \
            '}' \
            > "$out/nh_manifest.json"
        scene_mode="autoload ($scene)"
        ;;
esac

printf '%s\n' "$out"
printf '%s\n' "$scene_mode"
