"""The nodehammer web viewer runtime, and where it is.

This package is one directory of WebAssembly and a function that returns its
path. Nothing here imports :mod:`nodehammer`, and nothing here runs the viewer:
serving it is the library's job, and this only answers the question the library
cannot answer for itself.

That question is worth spelling out, because it is the whole reason this package
exists as a separate distribution. ``nodehammer viewer --web`` needs a directory
of Emscripten output, and it finds one by walking a ladder --- ``--web-assets``,
then ``NODEHAMMER_WEB_ASSETS``, then whatever the calling program supplies, then
``<executable>/../share/nodehammer/web``. Under a wheel the last rung is wrong:
the running executable is the interpreter, and in a virtualenv the platform call
behind it resolves the ``bin/python`` symlink to the *base* interpreter, so the
search lands in the wrong prefix entirely. The rung above it is the one
:mod:`nodehammer.cli` fills, with :func:`runtime_dir` below.

Installing this package is therefore what makes the GUI work from Python, and
uninstalling it is what makes a headless install 7 MB smaller. Both are
supported; the library reports the absence as one rung of a search rather than
as a fault.
"""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version
from importlib.resources import files
from pathlib import Path

__all__ = ["runtime_dir"]

try:
    __version__ = version("nodehammer-web")
except PackageNotFoundError:  # running from a source checkout, never installed
    __version__ = "0.0.0"


def runtime_dir() -> Path:
    """The directory holding the viewer shell, the worker script and the bundles.

    :returns: an existing directory containing ``index.html``,
        ``compute_worker.js``, ``nh_runtime.json`` and the three
        ``nodehammer-*.{js,wasm}`` bundle pairs.

    :raises RuntimeError: if the payload is missing, which means a broken
        install rather than a supported configuration --- there is no version of
        this package whose purpose survives the runtime not being in it.

    The contents are deliberately *not* checked here beyond the directory
    existing. ``nh_runtime.json`` carries the schema id the bundles were built
    against, and comparing it to the one compiled into the library is the check
    that matters; doing it in C++ means every front door gets it, including the
    ones that never touch Python. Duplicating a weaker version of it here would
    add a second answer to a question that already has one.
    """
    # `files()` rather than `__file__` for the usual reason, but with the result
    # narrowed to a real path: the payload is 7 MB, and the Traversable API's
    # general answer for a non-filesystem package is to extract it to a
    # temporary directory that vanishes when the context manager exits --- which
    # is neither what a server wants to be handed nor something worth doing
    # silently. Wheels install unpacked, so this is the ordinary case; a zipped
    # install is refused rather than worked around.
    root = files(__package__)
    if not isinstance(root, Path):
        msg = (
            f"{__package__} must be installed unpacked, not as a zip import: "
            "the viewer runtime is served from disk"
        )
        raise RuntimeError(msg)

    runtime = root / "runtime"
    if not runtime.is_dir():
        msg = (
            f"{__package__} carries no runtime at {runtime}. "
            "The wheel is built by staging a wasm build into it, so this is a "
            "broken install rather than a configuration."
        )
        raise RuntimeError(msg)
    return runtime
