"""The command line, as a function.

``nodehammer convert ...`` typed at a shell and :func:`run` called from Python
are the same code: the executable is a shim over ``nodehammer::cli::run``, and so
is this. There is no second option table to drift, and no subprocess to spawn.

The console script this package installs is :mod:`nodehammer.__main__`, so
``uvx nodehammer`` and ``pip install nodehammer && nodehammer`` both arrive
here::

    import nodehammer as nh

    code = nh.cli.run(["convert", "--input", "odd.gdml", "--output", "odd.glb"])
    if code != 0:
        ...

Output goes to the process's stdout and stderr at the file-descriptor level,
not through ``sys.stdout`` -- the commands print from C++. In a notebook that
means it lands in the terminal the kernel was started from rather than in the
cell. Capture it with ``os.dup2`` or pytest's ``capfd``, not with
``contextlib.redirect_stdout``.
"""

from __future__ import annotations

import functools
import sys
from collections.abc import Sequence

from . import _nodehammer

__all__ = ["run"]


@functools.cache
def _web_runtime_dir() -> str:
    """Where the ``nodehammer-web`` package put the wasm viewer runtime.

    ``viewer --web`` needs a directory of Emscripten output that this wheel does
    not carry -- it is a separate ``py3-none-any`` distribution, so a headless
    install can drop it and a GUI one can have it without either being a
    different build. C++ cannot find it: under a wheel the running executable is
    the interpreter, and in a virtualenv ``/proc/self/exe`` resolves the symlink
    to the *base* interpreter, so looking beside it lands in the wrong prefix.
    Python knows exactly where an installed package is, so Python answers.

    ``""`` when the package is absent, which is not an error here -- it costs one
    rung of the search the library does, and the message that search produces
    names the package. Raising would break every non-viewer command in an install
    that deliberately went without it.

    A ``str`` and not a ``Path``, even though the C++ side takes a path and
    nanobind would convert either: ``Path("")`` is ``PosixPath(".")``, so the
    absent case would arrive as *the current directory* and the library would
    dutifully consider it a candidate runtime. The empty string converts to an
    empty path, which is the "not given" the ladder is looking for.

    Cached because :func:`run` is called per command and a *failed* import is not
    cached by the interpreter, so an install without the package would repeat the
    whole search on every call.
    """
    try:
        from nodehammer_web import runtime_dir
    except ImportError:
        return ""
    try:
        return str(runtime_dir())
    except Exception:  # noqa: BLE001 - a broken sibling must not break the CLI
        # The package is installed but its payload is not where it should be.
        # Returning empty leaves the library to report the whole ladder, which
        # says more than a traceback from an import would.
        return ""


def run(args: Sequence[str] | None = None, *, pager: bool = False) -> int:
    """Run one command line and return the exit code the executable would give.

    :param args: the arguments *after* the program name, as
        ``["convert", "--input", "a.gdml"]``. ``None`` means ``sys.argv[1:]``,
        which is what the console script passes. An empty sequence prints the
        help and returns 0 -- never a window, and never a default subcommand:
        what a bare invocation should do belongs to the front door, and the
        executable answers it differently.
    :param pager: page long output through ``$PAGER`` when stdout is a terminal.
        Off by default, and that default is the point: a terminal proves a
        terminal, not a reader, and an interactive interpreter has one. Paging
        replaces this process's file descriptor 1 and then blocks until someone
        quits ``less``. The console script turns it on, because there a person
        really did type the command.

    :returns: 0 on success, non-zero otherwise. A command that could not do its
        job reports the reason on stderr and answers with a code, exactly as the
        executable does -- it does not raise.

    :raises nodehammer.Error: only for a failure that escaped a command body,
        which is a defect rather than a diagnosis.

    ``viewer --web`` is told where the wasm runtime is, if the ``nodehammer-web``
    package is installed alongside this one -- see :func:`_web_runtime_dir`. That
    is a default and not an override: ``--web-assets`` and
    ``NODEHAMMER_WEB_ASSETS`` still win, so a locally built runtime can be tried
    without uninstalling anything.

    Nothing here calls ``exit``: the interpreter, its stack and its ``finally``
    blocks all survive a failing command. That is not incidental -- it is why
    the CLI had to move into the shared library before this function could exist.
    """
    if args is None:
        args = sys.argv[1:]
    return _nodehammer.cli_run([str(a) for a in args], pager, _web_runtime_dir())
