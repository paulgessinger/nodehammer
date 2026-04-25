#!/usr/bin/env bash
# Run hyperfine against `nodehammer convert` with the ODD fixture.
#
# Usage: ci/bench.sh <kind> <command-name>
# Kinds: native, wasm
#
# Reads RUNNER_OS (set automatically by GitHub Actions) to pick the .exe
# suffix on Windows. Writes bench.json in the working directory; out.glb
# is the conversion output (unused after the run).

set -euo pipefail

kind=${1:?kind required: native|wasm}
command_name=${2:?command name required}

case "$kind" in
  native)
    suffix=""
    [ "${RUNNER_OS:-}" = "Windows" ] && suffix=".exe"
    cmd="build/Release/nodehammer${suffix} convert -i odd.nhb.zst -c fixtures/configs/odd.toml -o out.glb"
    ;;
  wasm)
    cmd="node build/emscripten/Release/nodehammer.js convert -i odd.nhb.zst -c fixtures/configs/odd.toml -o out.glb"
    ;;
  *)
    echo "unknown kind: $kind" >&2
    exit 1
    ;;
esac

hyperfine --warmup 2 --runs 10 \
  --command-name "$command_name" \
  --export-json bench.json \
  "$cmd"
