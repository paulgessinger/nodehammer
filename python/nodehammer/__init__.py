"""Detector geometry tessellation and export.

The public C++ API, mirrored one-to-one with snake_case members. The pipeline
reads the same way it does in C++::

    import nodehammer as nh

    scene, d1 = nh.SemanticScene.read("odd.gdml")
    cfg, d2 = nh.Config.read(Path("odd.toml"))
    rs, d3 = nh.build(scene, cfg.scene)
    rs.write("odd.glb", cfg.output)

A config source is decided by its type: a ``Path`` is a file, a ``str`` is TOML
text, a ``dict`` is serialized and parsed. Never by asking the filesystem, so a
mistyped path is a clean error rather than a document nobody wrote.

Failures are :class:`Error`, carrying the code, context and the diagnostics
observed before the failure. A returned ``DiagnosticList`` describes the
*quality* of a result that exists; it never reports whether it exists. See
``docs/error-model.md``.
"""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as _dist_version

from . import _nodehammer
from ._nodehammer import (
    VERSION,
    VERSION_MAJOR,
    VERSION_MINOR,
    VERSION_PATCH,
    Config,
    ConfigResult,
    Diagnostic,
    DiagnosticList,
    Error,
    OutputConfig,
    RenderResult,
    RenderScene,
    SceneConfig,
    SemanticResult,
    SemanticScene,
    apply_selection,
    build,
    deduplicate,
    tessellate,
    version,
)

__all__ = [
    "VERSION",
    "VERSION_MAJOR",
    "VERSION_MINOR",
    "VERSION_PATCH",
    "Config",
    "ConfigResult",
    "Diagnostic",
    "DiagnosticList",
    "Error",
    "OutputConfig",
    "RenderResult",
    "RenderScene",
    "SceneConfig",
    "SemanticResult",
    "SemanticScene",
    "apply_selection",
    "build",
    "deduplicate",
    "tessellate",
    "version",
]

#: The version pip installed.
try:
    __version__ = _dist_version("nodehammer")
except PackageNotFoundError:  # imported from a build tree rather than a wheel
    __version__ = VERSION

#: What ``libnodehammer`` reports out of line — the version *linked*, as opposed
#: to :data:`VERSION`, which is a constant compiled into the headers. They differ
#: only if the extension found a different library than it was built against,
#: which is what the wheel's rpath exists to prevent.
__cxx_version__ = version()
