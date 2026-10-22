#!/usr/bin/env python3
"""Add a Zensical dashboard and tested projects to the existing Pages site."""
import argparse
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import tomllib
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]


def readme_page(text):
    """Resolve README links without maintaining a second copy of the overview."""
    def link(url):
        if url == "docs/assets/logo.png":
            return "assets/logo.png"
        if url.startswith(("#", "/")) or urlsplit(url).scheme:
            return url
        return "https://github.com/paulgessinger/nodehammer/blob/main/" + url

    text = re.sub(r'(\]\()([^\s)]+)(\))',
                  lambda match: match[1] + link(match[2]) + match[3], text)
    text = re.sub(r'((?:src|href)=")([^"]+)(")',
                  lambda match: match[1] + link(match[2]) + match[3], text)
    return "[Explore the detector catalog](catalog.md)\n\n" + text


def source_catalog():
    catalog = tomllib.loads((ROOT / "examples/catalog.toml").read_text())
    lines = ["# Detector catalog", "",
             "Available presets and pinned inputs. Build results were not supplied;",
             "this documentation-only build does not include viewer publications or validation results.", "",
             "| Example | Source | Configuration |", "| --- | --- | --- |"]
    for row in catalog["examples"]:
        entry = catalog["inputs"][row["input"]]
        source = catalog["base_url"] + entry["path"]
        config = (f"[Lua preset](https://github.com/paulgessinger/nodehammer/blob/main/{row['config']})"
                  if "config" in row else "Generated depth-2 overview")
        lines.append(f"| {row['title']} | [Input]({source}) | {config} |")
    lines += ["", "FCC-ee examples are generated from the CI Key4hep release's k4geo installation.", ""]
    return "\n".join(lines)


def fixture_links():
    lines = ["", "## Authored FCC-ee configurations", "",
             "Simplified presets are published from the same full imports used by CI.",
             "Detailed configs are provided for authoring, not automatically published.",
             "**IDEA detailed requires the original reduced input; it does not remove fibers itself.**", "",
             "[Fixture provenance and input restrictions](https://github.com/paulgessinger/"
             "nodehammer/blob/main/fixtures/configs/fccee/README.md)", "",
             "| Model | Simplified | Detailed |", "| --- | --- | --- |"]
    for model in sorted((ROOT / 'fixtures/configs/fccee').iterdir()):
        if not model.is_dir():
            continue
        base = f"configs/fccee/{model.name}"
        lines.append(f"| {model.name} | [TOML]({base}/simplified.toml) | "
                     f"[TOML]({base}/detailed.toml) |")
    return "\n".join(lines) + "\n"


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
        "FCC-ee smoke checks and sPHENIX use bounded depth-2 overviews.",
        "Authored FCC-ee simplified presets are additional package-only publications.", "",
        "CI validates native processing, project archives, and the static site layout.",
        "Browser rendering is not automatically tested.",
        "Only successful builds deploy. Counts describe unique meshes, not instanced GPU cost.", "",
        "[Download machine-readable results](results.json)", "",
        "## Input provenance", "",
    ]
    for key, row in rows.items():
        lines += [f"### {row['title']}", ""]
        if "sha256" in row:
            lines += [f"[Source input]({row['source']})", "", f"SHA256: `{row['sha256']}`", ""]
        else:
            lines += ["From the pinned Key4hep release's k4geo installation.", ""]
        lines += [f"[Download project](viewers/{key}/{Path(row['archive']).name})", ""]
    with tempfile.TemporaryDirectory(prefix="nodehammer-docs-") as temporary:
        temp = Path(temporary)
        docs = temp / "pages"
        authored = ROOT / "docs/site/pages"
        if authored.exists():
            shutil.copytree(authored, docs)
        else:
            docs.mkdir()
        for reserved in ("index.md", "catalog.md"):
            if (docs / reserved).exists():
                raise ValueError(f"reserved generated documentation page: {reserved}")
        (docs / "index.md").write_text(readme_page((ROOT / "README.md").read_text()))
        (docs / "catalog.md").write_text(("\n".join(lines) if results else source_catalog()) + fixture_links())
        shutil.copytree(ROOT / "fixtures/configs/fccee", docs / "configs/fccee")
        (docs / "assets").mkdir(exist_ok=True)
        shutil.copy2(ROOT / "docs/assets/logo.png", docs / "assets/logo.png")
        if results:
            (docs / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
        shutil.copy2(ROOT / "docs/site/zensical.toml", temp / "zensical.toml")
        subprocess.run(["zensical", "build", "--clean", "--config-file", "zensical.toml"],
                       cwd=temp, check=True)
        shutil.copytree(temp / "site", site, dirs_exist_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--site", required=True, type=Path)
    parser.add_argument("--results", type=Path, nargs="+", default=[])
    args = parser.parse_args()
    if args.results and not args.runtime:
        parser.error("--runtime is required when publishing --results")
    build(args.runtime.resolve() if args.runtime else None, args.site.resolve(), args.results)


if __name__ == "__main__":
    main()
