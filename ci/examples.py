#!/usr/bin/env python3
"""Pinned WebEOS inputs -> representative checks and portable gallery projects."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib
import urllib.request

from fccee import check_render, smoke_config

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fetch(base_url, entry, cache):
    """Verify cache hits too; a partial or corrupt download is never reusable."""
    target = cache / entry["sha256"] / Path(entry["path"]).name
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_file() and digest(target) == entry["sha256"]:
        return target
    target.unlink(missing_ok=True)
    temporary = target.with_name(target.name + ".part")
    try:
        with urllib.request.urlopen(base_url + entry["path"], timeout=120) as response:
            with temporary.open("wb") as stream:
                shutil.copyfileobj(response, stream)
        if digest(temporary) != entry["sha256"]:
            raise ValueError(f"SHA256 mismatch: {entry['path']}")
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def command(exe, output, phase, *args, document=False):
    with (output / f"{phase}.log").open("w") as log:
        result = subprocess.run([str(exe), *map(str, args)], stdout=subprocess.PIPE,
                                stderr=log, text=True, timeout=1200)
    (output / f"{phase}.stdout").write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"{phase} exited {result.returncode}; see {output}")
    return json.loads(result.stdout) if document else None


def prepare(exe, source, output):
    output.mkdir(parents=True, exist_ok=True)
    nhb = output / "geometry.nhb.zst"
    nhb.unlink(missing_ok=True)
    command(exe, output, "import", "convert", "-i", source, "-o", nhb)
    summary = command(exe, output, "summary", "inspect", "--output-format", "json",
                      "summary", "-i", nhb, document=True)
    if summary["nodes"] < 100 or summary["diagnostics"]["errors"]:
        raise ValueError("import produced unexpectedly small or invalid geometry")
    return nhb, summary


def example(exe, row, nhb, output):
    output.mkdir(parents=True, exist_ok=True)
    if "config" in row:
        config = ROOT / row["config"]
    else:
        tree = command(exe, output, "tree", "inspect", "--output-format", "json",
                       "tree", "-i", nhb, "--depth", 2, document=True)
        config = output / "overview.toml"
        config.write_text(smoke_config(tree))
    # Flatten to make the archive portable without relying on checkout symlinks.
    flat = output / "scene.toml"
    command(exe, output, "config", "config", "flatten", "-c", config, "-o", flat)
    counts = {}
    if row.get("test"):
        render = output / "render.json"
        render.unlink(missing_ok=True)
        command(exe, output, "tessellate", "convert", "-i", nhb, "-c", flat,
                "--output-format", "render-json", "-o", render)
        counts = check_render(json.loads(render.read_text()))
        # Counts and logs are enough for artifacts; do not upload giant mesh JSON.
        render.unlink()
    # Package selected semantic geometry. Browsers still exercise tessellation,
    # but don't download irrelevant full-detector placements.
    selected = output / "selected.nhb.zst"
    command(exe, output, "select", "convert", "-i", nhb, "-c", flat, "-o", selected)
    archive = output / "scene.nhproj"
    archive.unlink(missing_ok=True)
    command(exe, output, "pack", "project", "pack", "-i", selected, "-c", flat,
            "-o", archive)
    command(exe, output, "info", "project", "info", archive)
    if not archive.is_file() or not archive.stat().st_size:
        raise ValueError("pack did not produce a project")
    return {"status": "passed" if row.get("test") else "packaged",
            "archive": f"{row['id']}/scene.nhproj", **counts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nodehammer", type=Path, default=Path("build/Release/nodehammer"))
    parser.add_argument("--catalog", type=Path, default=ROOT / "examples/catalog.toml")
    parser.add_argument("--cache", type=Path, default=Path(".cache/example-inputs"))
    parser.add_argument("--output", type=Path, default=Path("build/example-results"))
    args = parser.parse_args()
    catalog = tomllib.loads(args.catalog.read_text())
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    exe = args.nodehammer.resolve()
    prepared = {}
    report = {}
    for row in catalog["examples"]:
        key = row["input"]
        entry = catalog["inputs"][key]
        print(f"::group::{row['id']}", flush=True)
        result = {"title": row["title"], "source": catalog["base_url"] + entry["path"],
                  "sha256": entry["sha256"]}
        try:
            if key not in prepared:
                source = fetch(catalog["base_url"], entry, args.cache)
                prepared[key] = prepare(exe, source, args.output / "_inputs" / key)
            nhb, summary = prepared[key]
            result.update(example(exe, row, nhb, args.output / row["id"]))
            result["full_nodes"] = summary["nodes"]
        except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
            result.update(status="failed", error=str(error))
            print(f"::error::{row['id']}: {error}", flush=True)
        report[row["id"]] = result
        (args.output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
        print("::endgroup::", flush=True)
    return int(any(row["status"] == "failed" for row in report.values()))


if __name__ == "__main__":
    sys.exit(main())
