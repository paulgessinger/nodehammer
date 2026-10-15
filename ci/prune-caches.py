#!/usr/bin/env python3
"""Prune superseded main-branch CI caches; dry-run unless --apply is passed."""

import argparse
import json
import re
import subprocess
from collections import defaultdict


def family(key):
    # Strip only known per-run suffixes. Keep distinct dependency/config hashes,
    # platforms and cache formats separate; leave fixed keys and PR refs alone.
    patterns = [
        r"((?:conan|emscripten-cache)-.+-[0-9a-f]{64})-\d+",
        r"(ccache-ccache-.+)-\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+Z",
        r"(ccache-wheel-.+)-\d+",
        r"(ccache-key4hep-v2-\d+)-\d+-\d+",
    ]
    for pattern in patterns:
        match = re.fullmatch(pattern, key)
        if match:
            return match[1]
    return None


def candidates(caches, keep):
    groups = defaultdict(list)
    for cache in caches:
        group = family(cache["key"])
        if group and cache["ref"] == "refs/heads/main":
            groups[(group, cache["version"])].append(cache)
    return [cache for group in groups.values()
            for cache in sorted(group, key=lambda c: c["created_at"], reverse=True)[keep:]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="paulgessinger/nodehammer")
    parser.add_argument("--keep", type=int, default=2)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    if args.keep < 1:
        parser.error("--keep must retain at least one snapshot")
    result = subprocess.run(
        ["gh", "api", "--paginate", "--slurp", f"repos/{args.repo}/actions/caches?per_page=100"],
        check=True, capture_output=True, text=True,
    )
    caches = [c for page in json.loads(result.stdout) for c in page["actions_caches"]]
    selected = candidates(caches, args.keep)
    for cache in selected:
        print(f"{cache['id']} {cache['size_in_bytes']:,} bytes {cache['key']}", flush=True)
    size = sum(c["size_in_bytes"] for c in selected)
    print(f"{'Deleting' if args.apply else 'Would delete'} {len(selected)} snapshots ({size / 2**30:.2f} GiB)", flush=True)
    if args.apply:
        for cache in selected:
            subprocess.run(["gh", "api", "--method", "DELETE",
                            f"repos/{args.repo}/actions/caches/{cache['id']}"], check=True)


if __name__ == "__main__":
    main()
