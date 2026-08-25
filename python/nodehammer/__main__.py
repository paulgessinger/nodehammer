"""The console script, and ``python -m nodehammer``.

``[project.scripts]`` in pyproject.toml points at :func:`main`, so a wheel
install puts a ``nodehammer`` command on the PATH without shipping a compiled
executable: the generated script imports the extension and calls into
``libnodehammer``, which is where the CLI lives. ``uvx nodehammer`` is the same
path with the install made ephemeral.

Being importable as ``__main__`` as well is free and worth having -- it is what
lets somebody run the CLI out of a checkout, or against a specific interpreter,
without a console script existing at all.
"""

from __future__ import annotations

import sys

from .cli import run


def main() -> int:
    """Entry point for the ``nodehammer`` console script.

    Paging is on here and off in :func:`nodehammer.cli.run`, and the difference
    is the whole reason the option exists: reaching this function means a person
    typed a command at a shell, which is the one situation where taking over
    their terminal until they press q is the helpful thing to do.

    ``quiet`` is inverted for the same reason and points the same way. The
    progress a command reports on stderr is written for somebody watching it
    happen, and here somebody is. ``-q`` still turns it off, exactly as it does
    for the compiled executable -- which sets both of these identically
    (``src/cli/main.cpp``), so the two front doors behave the same.
    """
    return run(sys.argv[1:], pager=True, quiet=False)


if __name__ == "__main__":
    sys.exit(main())
