#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "typer>=0.12",
#     "diskcache>=5.6",
#     "requests>=2.32",
# ]
# ///
"""Harvest upstream license texts for the third-party deps declared in
cmake/Dependencies.cmake.

Grepping CMake — not ideal, but the alternative is duplicating the dep list in
a separate file, and keeping them in sync is its own failure mode. The URLs
and versions live next to the FetchContent_Declare calls that use them.

Run with ``just licenses`` (or ``uv run scripts/harvest_licenses.py``). Pass
``--check`` in CI to fail when the committed files drift from upstream.
"""
from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Annotated

import diskcache
import requests
import typer

RAW_GH_RE = re.compile(
    r"^https://raw\.githubusercontent\.com/([^/]+)/([^/]+)/([^/]+)/(.+)$"
)

REPO = Path(__file__).resolve().parent.parent
DEPS_CMAKE = REPO / "cmake" / "Dependencies.cmake"
OUTPUT_ROOT = REPO / "third_party_licenses"
CACHE_DIR = REPO / ".cache" / "license-harvester"

app = typer.Typer(add_completion=False, help=__doc__)


def parse_cmake_vars(path: Path) -> tuple[list[str], dict[str, str]]:
    """Extract NH_DEP_* variables and the NH_THIRD_PARTY list. ``${VAR}``
    references inside string values are expanded using earlier assignments.
    """
    # Strip CMake line comments so tokens inside `set(...)` blocks aren't
    # polluted by explanatory text between entries.
    text = re.sub(r"#[^\n]*", "", path.read_text())

    names: list[str] = []
    list_match = re.search(
        r"set\s*\(\s*NH_THIRD_PARTY\s+(.*?)\)", text, flags=re.DOTALL
    )
    if list_match:
        names = [tok.strip() for tok in list_match.group(1).split() if tok.strip()]

    values: dict[str, str] = {}
    var_pattern = re.compile(
        r'set\s*\(\s*(NH_DEP_[A-Z0-9_]+)\s+("([^"]*)"|([^\s)]+))\s*\)'
    )
    for m in var_pattern.finditer(text):
        name = m.group(1)
        raw = m.group(3) if m.group(3) is not None else m.group(4)
        values[name] = expand(raw, values)
    return names, values


def expand(value: str, env: dict[str, str]) -> str:
    """Expand ``${VAR}`` references using already-seen assignments. Unknown
    references are left untouched so the caller sees the problem."""
    def replace(m: re.Match[str]) -> str:
        return env.get(m.group(1), m.group(0))

    prev = None
    while prev != value:
        prev = value
        value = re.sub(r"\$\{([A-Z0-9_]+)\}", replace, value)
    return value


def dep_fields(slug: str, env: dict[str, str]) -> dict[str, str]:
    key = slug.upper()
    try:
        return {
            "slug": slug,
            "version": env.get(f"NH_DEP_{key}_VERSION", ""),
            "license": env[f"NH_DEP_{key}_LICENSE"],
            "url": env[f"NH_DEP_{key}_LICENSE_URL"],
        }
    except KeyError as missing:
        raise typer.BadParameter(
            f"{slug}: missing variable {missing.args[0]} in {DEPS_CMAKE.name}"
        ) from None


def fetch(url: str, cache: diskcache.Cache, force: bool) -> bytes:
    if not force:
        cached = cache.get(url)
        if cached is not None:
            return cached

    # Route raw.githubusercontent.com through the authenticated Contents API
    # when a token is available (GITHUB_TOKEN in Actions, GH_TOKEN locally)
    # so CI doesn't hit unauthenticated rate limits.
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    fetch_url = url
    headers: dict[str, str] = {}
    m = RAW_GH_RE.match(url)
    if token and m:
        owner, repo, ref, path = m.groups()
        fetch_url = (
            f"https://api.github.com/repos/{owner}/{repo}/contents/{path}?ref={ref}"
        )
        headers = {
            "Accept": "application/vnd.github.raw",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
        }

    resp = requests.get(fetch_url, headers=headers, timeout=30)
    resp.raise_for_status()
    cache.set(url, resp.content)  # cache keyed on the declared URL
    return resp.content


@app.command()
def main(
    check: Annotated[
        bool,
        typer.Option(
            "--check",
            help="Fail if any committed file differs from upstream.",
        ),
    ] = False,
    force: Annotated[
        bool,
        typer.Option(
            "--force",
            help="Bypass the download cache and re-fetch every URL.",
        ),
    ] = False,
) -> None:
    names, env = parse_cmake_vars(DEPS_CMAKE)
    if not names:
        typer.secho(
            f"no NH_THIRD_PARTY list found in {DEPS_CMAKE}", fg="red", err=True
        )
        raise typer.Exit(code=1)

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cache = diskcache.Cache(str(CACHE_DIR))
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)

    drift: list[str] = []
    for slug in names:
        fields = dep_fields(slug, env)
        dest = OUTPUT_ROOT / slug / "LICENSE"
        dest.parent.mkdir(parents=True, exist_ok=True)
        upstream = fetch(fields["url"], cache, force)

        existing = dest.read_bytes() if dest.exists() else None
        if existing == upstream:
            typer.echo(
                f"  ok  {slug:<20} {fields['version']:<14} {fields['license']}"
            )
            continue

        if check:
            drift.append(slug)
            typer.secho(
                f"drift {slug:<20} {fields['version']:<14} {fields['license']}",
                fg="yellow",
            )
            continue

        dest.write_bytes(upstream)
        action = "write" if existing is None else "update"
        typer.secho(
            f"{action:>5} {slug:<20} {fields['version']:<14} {fields['license']}",
            fg="green",
        )

    if check and drift:
        typer.secho(
            f"\n{len(drift)} file(s) out of date: {', '.join(drift)}\n"
            "Run `just licenses` and commit the result.",
            fg="red",
            err=True,
        )
        raise typer.Exit(code=1)


if __name__ == "__main__":
    app()
