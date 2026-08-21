"""Emit `nodehammer`'s dependency on `nodehammer-web`, from the version.

A constraint that is typed by hand is wrong from the first minor bump, and this
one names a sibling distribution that is built from the same commit by a
different job --- so nothing about it is worth policing by review. Both versions
come from setuptools_scm reading one git state, which means the constraint can be
computed by the build that computes the version.

Registered as the *second* `[[tool.dynamic-metadata]]` entry in the root
pyproject.toml. Entries run in list order and each provider is handed the project
as resolved so far, so this reads `project["version"]` from the setuptools_scm
entry above it. That ordering is the one thing here that can silently rot, which
is why it is checked rather than assumed.

Why same-minor and not an exact pin
-----------------------------------
`nodehammer-web == 0.2.*`, not `== 0.2.0.post1.dev9`. The exact pin looks tighter
and is unusable:

- A locally built `nodehammer` wheel could not be installed at all without its
  same-second sibling published somewhere, or sitting in the same directory.
- It would make the two dists inseparable for good: a patch release of the
  runtime alone --- a wasm rebuild against a fixed toolchain, say --- could not
  be installed against a `nodehammer` that is otherwise still current.

What it would *not* do any more is break CI. cibuildwheel installs the platform
wheel with resolution and no way to turn that off, which used to mean an exact
pin could never be satisfied on a non-release commit; ci.yml now hands that step
the web wheel from the same run, so the exact version is present. The reasons
above are what still rule it out.

Same-minor is also the policy this repository already applies to its own CMake
package (`COMPATIBILITY SameMinorVersion`), chosen for the reason spelled out
there: pre-1.0, the minor is where compatibility actually lives. What is really
version-coupled is the runtime's schema id --- and that is checked at load time
by the library, which is the only check that survives the rungs pip never sees.
Resolution keeps the *wheel pair* plausible; the stamp keeps the *running pair*
correct.
"""

from __future__ import annotations

from typing import Any

__all__ = ["Provider"]

# The sibling distribution. Kept here rather than in a plugin setting because
# there is exactly one, and a setting would only move the string.
DISTRIBUTION = "nodehammer-web"


def _same_minor(version: str) -> str:
    """`nodehammer-web == X.Y.*` for the X.Y of a PEP 440 version."""
    # Imported here, not at module scope: `get_requires_for_dynamic_metadata`
    # below is called *before* build requirements are installed, and the module
    # has to import for that call to happen at all.
    from packaging.version import Version

    release = Version(version).release
    # `release` is at least one component, so a bare "1" has no minor to read.
    major, minor = (*release, 0, 0)[:2]
    return f"{DISTRIBUTION}=={major}.{minor}.*"


class Provider:
    """The dynamic-metadata 0.3 plugin interface, as two static methods."""

    @staticmethod
    def get_requires_for_dynamic_metadata(_settings: dict[str, Any]) -> list[str]:
        # scikit-build-core already depends on packaging, so this is a statement
        # of what is used rather than something that changes the environment.
        # It matters if that ever stops being true.
        return ["packaging>=23.2"]

    @staticmethod
    def dynamic_metadata(settings: dict[str, Any], project: Any) -> dict[str, Any]:
        if settings:
            msg = f"{DISTRIBUTION} pin provider takes no settings, got {sorted(settings)}"
            raise RuntimeError(msg)

        version = project.get("version")
        if not isinstance(version, str):
            # The ordering contract, stated as an error rather than an
            # AttributeError three frames down. Only a provider listed *after*
            # the one that fills `version` can read it.
            msg = (
                "the nodehammer-web pin must be resolved after the version: list "
                "its [[tool.dynamic-metadata]] entry below the setuptools_scm one"
            )
            raise RuntimeError(msg)

        return {"dependencies": [_same_minor(version)]}
