"""nodehammer — HEP geometry conversion pipeline.

The native core is a single compiled extension module (``_nodehammer``); there
is no separate executable in the wheel. The ``nodehammer`` console script is
:func:`main` below, which hands ``sys.argv`` straight to the same C++ entry
point the native binary's ``main`` calls.
"""

from __future__ import annotations

import sys
from collections.abc import Sequence

from ._nodehammer import __version__, main as _main

__all__ = ["__version__", "main"]


def main(argv: Sequence[str] | None = None) -> int:
    """Console-script entry point. Returns the exit code."""
    return _main(list(sys.argv if argv is None else argv))


def _console_script() -> None:
    """``project.scripts`` target: exits with the CLI's status code."""
    sys.exit(main())
