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
from pathlib import Path
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
    "check_config",
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
    src: Mapping[str, Any] | Path | str,
    *,
    base_dir: str | None = None,
    **overrides: Any,
) -> Config:
    """Build a :class:`Config` from a dict, a file, or TOML text.

    What ``src`` means is decided by its **type**, never by looking at the
    filesystem — see :func:`check_config` for the rule and the reasoning.

    Raises :class:`Error` if the document does not load. Use :func:`check_config`
    to get every problem as a report instead.
    """
    kind, value = _resolve_source(src, overrides)
    if kind == "path":
        return Config.read(value).config
    try:
        return Config.parse(value, base_dir).config
    except Error as exc:
        # A heuristic in the *error* path only: it never changes what a
        # successful call does, it just answers the question the confusing
        # message provokes. "expected '=' after key" is unhelpful when what
        # actually happened is that someone passed a filename as text.
        if isinstance(src, str) and _looks_like_a_filename(src):
            raise Error(
                exc.code,
                f"{exc} -- `src` is a str, which is read as TOML text; "
                f"pass Path({src!r}) to read it as a file",
            ) from exc
        raise


def check_config(
    src: Mapping[str, Any] | Path | str,
    *,
    base_dir: str | None = None,
    **overrides: Any,
) -> DiagnosticList:
    """Report every problem in a config, rather than raising on the first.

    The reporting half of :func:`load_config`, and the same dispatch: a
    :class:`~pathlib.Path` is a file, a :class:`str` is TOML *text*, and a
    :class:`~collections.abc.Mapping` is serialized to TOML and parsed, so a
    dict-built config produces the same diagnostic codes as a file the CLI reads.

    Deciding by type rather than by whether a file happens to exist is
    deliberate. Existence-based dispatch would make the meaning of an argument
    depend on the state of the filesystem, and a mistyped path would silently
    become a document to parse rather than a missing file to report.

    ``base_dir`` roots any ``include``. ``None`` means *this content has no
    location* — not the working directory. That is the library's rule (a config
    must not be able to pull in a file from wherever the process happened to
    start), and choosing what "here" means belongs to the application.
    """
    kind, value = _resolve_source(src, overrides)
    if kind == "path":
        return Config.check(value)
    return Config.check_string(value, base_dir)


def _resolve_source(
    src: Mapping[str, Any] | Path | str, overrides: Mapping[str, Any]
) -> tuple[str, Any]:
    """Normalize the three accepted shapes to either a path or TOML text.

    Shared by load_config and check_config so the dispatch rule is stated once.
    The two differ only in which promise they make of the result, which is the
    same split the C++ API draws between `read`/`parse` and `check`.
    """
    if isinstance(src, (Path, str)):
        if overrides:
            raise TypeError("overrides are only supported when src is a mapping")
        return ("path", src) if isinstance(src, Path) else ("text", src)

    try:
        import tomli_w
    except ModuleNotFoundError as exc:  # pragma: no cover - depends on env
        raise ModuleNotFoundError(
            "a dict config needs tomli-w; install nodehammer[dict] or pass TOML "
            "text instead."
        ) from exc

    return ("text", tomli_w.dumps(_deep_merge(dict(src), overrides)))


def _looks_like_a_filename(src: str) -> bool:
    """A one-line string ending in a config extension, i.e. almost certainly a path."""
    return "\n" not in src and src.strip().endswith((".toml", ".lua"))


def _deep_merge(base: dict[str, Any], overlay: Mapping[str, Any]) -> dict[str, Any]:
    """Merge ``overlay`` into ``base``, recursing into nested tables."""
    for key, value in overlay.items():
        existing = base.get(key)
        if isinstance(existing, dict) and isinstance(value, Mapping):
            base[key] = _deep_merge(dict(existing), value)
        else:
            base[key] = value
    return base
