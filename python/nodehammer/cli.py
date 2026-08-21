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

import sys
from collections.abc import Sequence

from . import _nodehammer

__all__ = ["run"]


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

    Nothing here calls ``exit``: the interpreter, its stack and its ``finally``
    blocks all survive a failing command. That is not incidental -- it is why
    the CLI had to move into the shared library before this function could exist.
    """
    if args is None:
        args = sys.argv[1:]
    return _nodehammer.cli_run([str(a) for a in args], pager)
