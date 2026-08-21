#!/usr/bin/env python3
"""Check that a built `nodehammer` and `nodehammer-web` are a matched pair.

The constraint in the platform wheel is generated from the version, so it cannot
drift by being mistyped. What it *cannot* catch is a stale artifact: a re-run
that picks up one half from an earlier commit produces two dists whose metadata
is individually correct and jointly wrong. Nothing in packaging notices that, so
this does --- against the two files actually in hand, before either is uploaded.

Needs `packaging` (`uv run --no-project --with packaging`).

Usage:
    scripts/check_wheel_pair.py <dist-dir>
"""

from __future__ import annotations

import email
import sys
import zipfile
from pathlib import Path

from packaging.requirements import Requirement
from packaging.utils import canonicalize_name
from packaging.version import Version

PLATFORM = "nodehammer"
WEB = "nodehammer-web"


def fail(msg: str) -> None:
    sys.exit(f"wheel pair check failed: {msg}")


def metadata(wheel: Path) -> email.message.Message:
    with zipfile.ZipFile(wheel) as zf:
        names = [n for n in zf.namelist() if n.endswith(".dist-info/METADATA")]
        if len(names) != 1:
            fail(f"{wheel.name}: expected one METADATA, found {len(names)}")
        return email.message_from_bytes(zf.read(names[0]))


def sole(wheels: list[Path], name: str) -> Path:
    if not wheels:
        fail(f"no {name} wheel found")
    if len(wheels) > 1:
        # Two versions of one distribution is exactly the stale-artifact case
        # this exists to catch, so it is a failure rather than a "take the
        # newest" -- picking one would hide it.
        listed = ", ".join(sorted(w.name for w in wheels))
        fail(f"more than one {name} wheel: {listed}")
    return wheels[0]


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dist = Path(sys.argv[1])

    by_name: dict[str, list[Path]] = {}
    for wheel in sorted(dist.glob("*.whl")):
        by_name.setdefault(canonicalize_name(metadata(wheel)["Name"]), []).append(wheel)

    platform_whl = sole(by_name.get(canonicalize_name(PLATFORM), []), PLATFORM)
    web_whl = sole(by_name.get(canonicalize_name(WEB), []), WEB)

    platform_meta = metadata(platform_whl)
    web_version = Version(metadata(web_whl)["Version"])
    platform_version = Version(platform_meta["Version"])

    print(f"{PLATFORM:16} {platform_version}  ({platform_whl.name})")
    print(f"{WEB:16} {web_version}  ({web_whl.name})")

    # Same commit, therefore same version. Both are derived by setuptools_scm
    # from one git state, so a difference means one of them came from somewhere
    # else -- a re-run, a cached artifact, a leg that failed and left an old
    # upload behind.
    if platform_version != web_version:
        fail(f"different versions: {platform_version} vs {web_version}")

    requirements = [
        Requirement(r) for r in platform_meta.get_all("Requires-Dist") or []
    ]
    pins = [r for r in requirements if canonicalize_name(r.name) == canonicalize_name(WEB)]
    if not pins:
        listed = ", ".join(str(r) for r in requirements) or "(none)"
        fail(f"{platform_whl.name} does not require {WEB}; it requires {listed}")
    if len(pins) > 1:
        fail(f"{platform_whl.name} requires {WEB} more than once")
    pin = pins[0]

    # A generated constraint can still be generated wrongly. `contains` with
    # prereleases allowed on purpose: a pre-release version pair is a normal
    # state here (v0.2.0rc1 published both halves), and the resolver's own
    # fallback would accept it, so refusing it would be stricter than reality.
    if not pin.specifier.contains(web_version, prereleases=True):
        fail(f"{pin} is not satisfied by the {WEB} wheel present ({web_version})")

    # The extra that stays declared and empty, so `pip install "nodehammer[web]"`
    # keeps resolving for anyone who wrote it down while it still meant
    # something.
    extras = platform_meta.get_all("Provides-Extra") or []
    if "web" not in extras:
        fail(f"{platform_whl.name} no longer declares the 'web' extra (has {extras})")

    print(f"ok: {pin} is satisfied by {web_version}, and the 'web' extra is declared")


if __name__ == "__main__":
    main()
