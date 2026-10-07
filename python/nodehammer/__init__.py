"""Detector geometry tessellation and export.

The public C++ API, mirrored one-to-one with snake_case members. The pipeline
reads the same way it does in C++::

    import nodehammer as nh

    scene, d1 = nh.read_semantic("odd.gdml")
    cfg, d2 = nh.read_config(Path("odd.toml"))
    rs, d3 = nh.build(scene, cfg.scene)
    nh.write(rs, "odd.glb", cfg.output)

Use ``read_config`` for files, ``from_toml`` for text, and ``from_dict`` for
Python dictionaries. Names distinguish file IO from in-memory conversion.

Failures are :class:`Error`, carrying the code, context and the diagnostics
observed before the failure. A returned ``DiagnosticList`` describes the
*quality* of a result that exists; it never reports whether it exists. See
``docs/error-model.md``.
"""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as _dist_version

from . import _nodehammer, cli
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
    read_config,
    from_toml,
    from_dict,
    check_config,
    check_config_string,
    check_config_dict,
    config_formats,
    from_nhb,
    to_nhb,
    to_nhb_zstd,
    from_nhr,
    to_nhr,
    read_semantic,
    read_render,
    semantic_read_formats,
    semantic_write_formats,
    render_read_formats,
    render_write_formats,
    write,
    apply_selection,
    build,
    deduplicate,
    tessellate,
    version,
)

__all__ = [
    "VERSION",
    "cli",
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
    "read_config",
    "from_toml",
    "from_dict",
    "check_config",
    "check_config_string",
    "check_config_dict",
    "config_formats",
    "from_nhb",
    "to_nhb",
    "to_nhb_zstd",
    "from_nhr",
    "to_nhr",
    "read_semantic",
    "read_render",
    "semantic_read_formats",
    "semantic_write_formats",
    "render_read_formats",
    "render_write_formats",
    "write",
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
