#!/usr/bin/env python3
"""Structural check of an assembled Pages site (scripts/build_pages_site.sh).

Deploying a broken viewer is only visible in a browser, so this asserts the
parts a static check *can* see:

  - every posture directory carries the shell, the worker script and both
    backend runtimes (a missing bundle is a blank canvas at runtime);
  - the application posture has no sidecar — that absence *is* what selects
    application mode in `viewer.html`;
  - every publication's sidecar names an archive that exists, and that archive
    is self-describing: its root `nodehammer.toml` `[project]` config/geometry
    keys resolve to entries inside the same archive.

Usage:
    scripts/check_pages_site.py <site-dir>
"""

from __future__ import annotations

import json
import sys
import tomllib
import zipfile
from pathlib import Path

RUNTIME_FILES = (
    "viewer.html",
    "compute_worker.js",
    "nodehammer-gles3.js",
    "nodehammer-gles3.wasm",
    "nodehammer-wgpu.js",
    "nodehammer-wgpu.wasm",
    "nodehammer-compute.js",
    "nodehammer-compute.wasm",
)

APPLICATION_POSTURES = ("app",)
PUBLICATION_POSTURES = ("odd", "odd-simple")


def fail(msg: str) -> None:
    sys.exit(f"pages site check failed: {msg}")


def check_runtime(posture_dir: Path) -> None:
    for name in RUNTIME_FILES:
        if not (posture_dir / name).is_file():
            fail(f"{posture_dir}: missing {name}")


def check_publication(posture_dir: Path) -> None:
    sidecar = posture_dir / "nh_manifest.json"
    if not sidecar.is_file():
        fail(f"{posture_dir}: no nh_manifest.json — would come up as the application")
    manifest = json.loads(sidecar.read_text(encoding="utf-8"))
    archive_name = manifest.get("archive")
    if not archive_name:
        fail(f"{sidecar}: sidecar names no archive")
    archive = posture_dir / archive_name
    if not archive.is_file():
        fail(f"{sidecar}: names a missing archive ({archive_name})")

    with zipfile.ZipFile(archive) as zf:
        names = set(zf.namelist())
        if "nodehammer.toml" not in names:
            fail(f"{archive}: no root nodehammer.toml — archive is not self-describing")
        project = tomllib.loads(zf.read("nodehammer.toml").decode("utf-8")).get("project", {})
    for field in ("config", "geometry"):
        key = project.get(field)
        if not key:
            fail(f"{archive}: nodehammer.toml [project].{field} is missing")
        if key not in names:
            fail(f"{archive}: [project].{field} = {key!r} is not an entry in the archive")

    size_mb = archive.stat().st_size / 1e6
    print(f"  {posture_dir.name}: {archive_name} — {len(names)} entries, {size_mb:.1f} MB")


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit("usage: check_pages_site.py <site-dir>")
    site = Path(sys.argv[1])

    if not (site / "index.html").is_file():
        fail(f"{site}: no landing page")

    for name in APPLICATION_POSTURES + PUBLICATION_POSTURES:
        posture_dir = site / name
        if not posture_dir.is_dir():
            fail(f"{site}: missing posture directory {name}/")
        check_runtime(posture_dir)

    for name in APPLICATION_POSTURES:
        if (site / name / "nh_manifest.json").exists():
            fail(f"{site / name}: has a sidecar — it would open in viewer mode, not as the app")
        print(f"  {name}: application mode (no sidecar)")

    for name in PUBLICATION_POSTURES:
        check_publication(site / name)

    print(f"pages site ok: {site}")


if __name__ == "__main__":
    main()
