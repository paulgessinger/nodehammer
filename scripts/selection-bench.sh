#!/usr/bin/env bash
# Native selection-stage baseline; emits JSON to stdout for archival/comparison.
# Build nodehammer_selection_bench in Release with the same compiler/settings
# for both revisions. This is a relative WASM proxy, not a WASM latency claim.
#
# Usage: scripts/selection-bench.sh <binary> [modules=256] [fibers=1024] [runs=5] [warmups=1]
# Example: scripts/selection-bench.sh build/key4hep/build/Release/tests/nodehammer_selection_bench > selection.json
# Each invocation is a fresh process; process_peak_rss_bytes is cumulative across
# scenarios and includes scene copying and correctness checks, not just selection.
set -euo pipefail
binary=${1:?path to nodehammer_selection_bench required}
shift
exec "$binary" "$@"
