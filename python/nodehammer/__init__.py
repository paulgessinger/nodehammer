"""Detector geometry tessellation and export.

The public C++ API, mirrored one-to-one with snake_case members. The pipeline
reads the same way it does in C++::

    import nodehammer as nh

    scene, d1 = nh.SemanticScene.read("odd.gdml")
    cfg, d2 = nh.Config.read("odd.toml")
    rs, d3 = nh.build(scene, cfg.scene())
    rs.write("odd.glb", cfg.output())

Failures are :class:`Error`, carrying the code, context and the diagnostics
observed before the failure. A returned ``DiagnosticList`` describes the
*quality* of a result that exists; it never reports whether it exists. See
``docs/error-model.md``.
"""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as _dist_version
from typing import Any, Mapping

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
    "load_config",
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


def load_config(
    src: Mapping[str, Any] | str,
    *,
    base_dir: str | None = None,
    **overrides: Any,
) -> Config:
    """Build a :class:`Config` from a dict, TOML text, or either plus overrides.

    A dict is serialized to TOML and handed to :meth:`Config.parse`, so it goes
    through exactly the same validator as a file the CLI reads and produces the
    same diagnostic codes. Nothing here reimplements config semantics.

    ``base_dir`` roots any ``include``. ``None`` means *this content has no
    location* — not the working directory. That is the library's rule (a config
    must not be able to pull in a file from wherever the process happened to
    start), and the choice of what "here" means belongs to the application, so
    if you want the working directory, pass it.

    Raises :class:`Error` if the document does not load.
    """
    if not isinstance(src, str):
        try:
            import tomli_w
        except ModuleNotFoundError as exc:  # pragma: no cover - depends on env
            raise ModuleNotFoundError(
                "load_config() with a dict needs tomli-w; install nodehammer[dict] "
                "or pass TOML text instead."
            ) from exc
        src = tomli_w.dumps(_deep_merge(dict(src), overrides))
    elif overrides:
        raise TypeError("overrides are only supported when src is a mapping")

    return Config.parse(src, base_dir).config


def _deep_merge(base: dict[str, Any], overlay: Mapping[str, Any]) -> dict[str, Any]:
    """Merge ``overlay`` into ``base``, recursing into nested tables."""
    for key, value in overlay.items():
        existing = base.get(key)
        if isinstance(existing, dict) and isinstance(value, Mapping):
            base[key] = _deep_merge(dict(existing), value)
        else:
            base[key] = value
    return base
