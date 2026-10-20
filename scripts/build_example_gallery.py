#!/usr/bin/env python3
"""Add a Zensical dashboard and tested projects to the existing Pages site."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def build(runtime, site, results):
    rows = {}
    for directory in results:
        report = json.loads((directory / "summary.json").read_text())
        for key, row in report.items():
            if key in rows or row["status"] not in ("passed", "packaged"):
                raise ValueError(f"duplicate or unsuccessful example: {key}")
            archive = directory / row["archive"]
            if not archive.is_file():
                raise ValueError(f"missing project: {archive}")
            subprocess.run([str(ROOT / "scripts/stage_wasm_viewer.sh"), "copy",
                            str(archive.resolve()), str(site / "viewers" / key),
                            str(runtime)], check=True, stdout=subprocess.DEVNULL)
            rows[key] = row
    lines = [
        "# nodehammer examples", "",
        "Preconfigured detector scenes, built with the same revision as the web viewer.",
        "Choose **simplified** for a lighter ATLAS scene. Detailed scenes need more GPU memory.", "",
        "[Open the application](app/) · [ODD full](odd/) · [ODD simplified](odd-simple/)", "",
        "## Detector examples", "",
        "| Example | Validation | Render nodes | Unique triangles |", "| --- | --- | ---: | ---: |",
    ]
    for key, row in rows.items():
        status = "Import + tessellation + package" if row["status"] == "passed" else "Package only"
        lines.append(f"| [{row['title']}](viewers/{key}/) | {status} | "
                     f"{row.get('render_nodes', '—')} | {row.get('triangles', '—')} |")
    lines += [
        "", "## What the checks cover", "",
        "The Key4hep build imports the FCC-ee DD4hep models, ATLAS Phase 2 SQLite,",
        "and sPHENIX ROOT and GDML. ATLAS Phase 1 uses a pinned NHB snapshot.",
        "One ATLAS detailed preset and both sPHENIX inputs are tessellated;",
        "the other ATLAS presets are packaged without duplicating those tests.",
        "FCC-ee and sPHENIX use bounded depth-2 overviews, not full-detail selections.", "",
        "CI also opens the sPHENIX ROOT publication in Chromium/WebGL2 and checks",
        "that geometry was drawn. This is one browser smoke test, not GPU/backend parity coverage.",
        "Only successful builds deploy. Counts describe unique meshes, not instanced GPU cost.", "",
        "[Download machine-readable results](results.json) · "
        "[Browser screenshot](browser-results/scene.png) · "
        "[Browser render metrics](browser-results/summary.json)", "",
        "## Input provenance", "",
    ]
    for key, row in rows.items():
        lines += [f"### {row['title']}", ""]
        if "sha256" in row:
            lines += [f"[Source input]({row['source']})", "", f"SHA256: `{row['sha256']}`", ""]
        else:
            lines += ["From the pinned Key4hep release's k4geo installation.", ""]
        lines += [f"[Download project](viewers/{key}/{Path(row['archive']).name})", ""]
    with tempfile.TemporaryDirectory(prefix="nodehammer-gallery-") as temporary:
        temp = Path(temporary)
        docs = temp / "docs"
        docs.mkdir()
        (docs / "index.md").write_text("\n".join(lines))
        (docs / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
        config = temp / "zensical.toml"
        config.write_text(
            '[project]\nsite_name = "nodehammer examples"\n'
            'docs_dir = "docs"\nsite_dir = "site"\n'
        )
        subprocess.run(["zensical", "build", "--clean", "--config-file", "zensical.toml"],
                       cwd=temp, check=True)
        shutil.copytree(temp / "site", site, dirs_exist_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--site", required=True, type=Path)
    parser.add_argument("--results", required=True, type=Path, nargs="+")
    args = parser.parse_args()
    build(args.runtime.resolve(), args.site.resolve(), args.results)


if __name__ == "__main__":
    main()
