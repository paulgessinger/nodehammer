#!/usr/bin/env python3
"""Render hyperfine benchmark JSONs into a Markdown table.

Reads `bench.json` from each `bench-<artifact-name>` directory under the
given root and writes a Markdown summary to stdout. Designed to be piped
into `$GITHUB_STEP_SUMMARY` from CI.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def fmt_secs(s: float) -> str:
    return f"{s * 1000:.1f} ms" if s < 1 else f"{s:.3f} s"


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <bench-root>", file=sys.stderr)
        return 2

    root = Path(sys.argv[1])
    if not root.is_dir():
        print(f"no such directory: {root}", file=sys.stderr)
        return 1

    rows: list[tuple[str, float, float, float, float]] = []
    for d in sorted(root.iterdir()):
        bench = d / "bench.json"
        if not bench.is_file():
            continue
        data = json.loads(bench.read_text())
        for r in data.get("results", []):
            label = r.get("command_name") or d.name
            label = label.removeprefix("bench-").removeprefix("nodehammer-")
            rows.append(
                (label, r["mean"], r["stddev"], r["min"], r["max"])
            )

    print("## Benchmark — `nodehammer convert -i odd.nhb.zst -c fixtures/configs/odd.toml`")
    print()
    print(
        "Wall time over 10 runs (after 2 warmups) on GitHub-hosted runners. "
        "Cross-runner numbers aren't directly comparable — different hardware, "
        "different noise floor. The `wasm32` row also includes node startup + "
        "Emscripten init."
    )
    print()
    if not rows:
        print("_No benchmark results were found._")
        return 0

    print("| Target | Mean | Stddev | Min | Max |")
    print("|---|---:|---:|---:|---:|")
    for name, mean, stddev, mn, mx in rows:
        print(
            f"| `{name}` | {fmt_secs(mean)} | ±{fmt_secs(stddev)} | "
            f"{fmt_secs(mn)} | {fmt_secs(mx)} |"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
