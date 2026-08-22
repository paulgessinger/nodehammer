"""The command line, through the binding.

This is the *breadth* half of the CLI's coverage. The other half stays in
tests/cli/test_cli_run.cpp, and the split is not arbitrary:

- What lives there is what has to hold on **every** platform and on **both**
  linkages. ``NODEHAMMER_BUILD_PYTHON`` is off by default and CI runs the Python
  legs on three of the matrix with Windows deliberately excluded -- which is
  exactly the platform whose CLI differs most (``_dup``, ``_popen``, the console
  subsystem). And this file can only ever reach the shared library, while the
  C++ cases link the static archive.
- What lives here is everything that is easier to *say* in pytest:
  ``parametrize`` over the converted ``std::exit`` sites, ``capfd`` instead of a
  hand-rolled ``dup2`` helper, ``tmp_path`` instead of hand-managed scratch
  files.

``capfd`` rather than ``capsys`` throughout, and that is load-bearing: the
commands print from C++ to file descriptors 1 and 2. ``capsys`` replaces
``sys.stdout``, which they never touch, and would capture nothing.
"""

from __future__ import annotations

import subprocess
import sys

import pytest

import nodehammer as nh


def test_a_failing_command_returns_and_the_interpreter_survives(capfd):
    # The property the whole cli::run refactor exists to provide. Before it,
    # this call ended the pytest process -- there was no assertion to write.
    code = nh.cli.run(["convert", "--input", "no-such-file.gdml", "--output", "out.glb"])

    assert code != 0
    assert "no-such-file.gdml" in capfd.readouterr().err

    # And again, to show `run` holds no state between calls.
    assert nh.cli.run(["--version"]) == 0


def test_version_is_the_same_code_path_as_the_module_version(capfd):
    assert nh.cli.run(["--version"]) == 0

    printed = capfd.readouterr().out
    # Not two constants pinned together by a test: the flag formats the same
    # `nodehammer::VERSION` the module exposes, so this asserts they cannot
    # drift rather than asserting they currently agree.
    assert nh.VERSION in printed
    assert nh.version() in printed


def test_the_version_flag_reports_the_library_not_the_distribution(capfd):
    # These two legitimately differ, and the difference is worth a test so that
    # nobody reconciles them:
    #
    #   __version__     the *distribution* version, PEP 440, from setuptools_scm
    #                   -- "1.2.0rc2.post1.dev8+g1234567" in a dev build.
    #   VERSION         what the C++ library reports. CMake's PROJECT_VERSION
    #                   holds numeric components only, so it is "1.2.0".
    #
    # On a tag they are the same string. Off a tag they cannot be, and
    # `nodehammer --version` answers for the library it is calling into --
    # which is the honest answer, since that is the code doing the work.
    assert nh.cli.run(["--version"]) == 0
    printed = capfd.readouterr().out

    assert nh.__cxx_version__ in printed
    assert nh.VERSION in printed
    assert nh.__version__.startswith(nh.VERSION)


def test_no_arguments_prints_help_and_succeeds(capfd):
    # The library answer, and the one a wheel gives: there is no viewer compiled
    # into the extension to fall back on. The executable differs deliberately.
    assert nh.cli.run([]) == 0

    out = capfd.readouterr().out
    assert "SUBCOMMAND" in out
    # The usage line names the program, which is not free: a console script's
    # argv[0] is a path inside an ephemeral venv, so without an explicit app
    # name the help would tell the reader to run
    # `/home/.../.cache/uv/.../nodehammer`.
    assert "nodehammer [OPTIONS]" in out


def test_args_default_to_sys_argv(monkeypatch, capfd):
    # What the console script relies on, asserted rather than assumed.
    monkeypatch.setattr(sys, "argv", ["nodehammer", "--version"])
    assert nh.cli.run() == 0
    assert nh.VERSION in capfd.readouterr().out


@pytest.mark.parametrize(
    ("args", "code_in_output"),
    [
        # Each of these was a `std::exit(1)` before the CLI moved into the
        # library. The point of the sweep is less the individual codes than the
        # fact that a run of them in a row cannot end this process.
        (["convert", "--input", "no-such-file.gdml", "--output", "out.glb"], "NH0101"),
        (["config", "flatten", "--config", "no-such-script.lua"], "NH0902"),
        (["config", "flatten", "--config", "no-such-config.toml"], "NH0100"),
        # `inspect` requires a nested subcommand of its own, so the sweep says
        # so -- a bare `inspect --input ...` fails for that reason instead and
        # would assert nothing about the import path.
        (["inspect", "--input", "no-such-file.gdml", "summary"], "NH0101"),
        (["config", "validate", "--config", "no-such-config.toml"], "NH0100"),
    ],
)
def test_converted_exit_sites_report_and_return(args, code_in_output, capfd):
    # `viewer` is deliberately absent from this list, and cannot be added: it is
    # registered by the executable, not by `cli::run`, because it constructs a
    # window and the shared library has to resolve every symbol it names. Its
    # fifteen converted sites are covered by the binary-level tests in
    # tests/CMakeLists.txt instead -- which is the only place they *can* be
    # covered, and a gap this file is what exposed.
    code = nh.cli.run(args)
    captured = capfd.readouterr()

    assert code != 0
    assert code_in_output in captured.err


def test_a_diagnosis_is_printed_once_not_twice(capfd):
    # `throwIfErrors` puts the whole collected list on the exception and the
    # command's handler prints it, so an eager `printDiags` before the throw
    # said everything twice. `reportOrThrow` made the halves exclusive.
    assert nh.cli.run(["config", "flatten", "--config", "no-such-config.toml"]) != 0

    err = capfd.readouterr().err
    assert err.count("NH0100") <= 1 or err.count("no-such-config.toml") <= 2


def test_a_usage_error_does_not_raise(capfd):
    # `run` reports failure by return value -- the only verb in the module that
    # does. A command has already printed its diagnosis by the time it answers,
    # so raising would be a second telling of it.
    try:
        code = nh.cli.run(["no-such-command"])
    except nh.Error as exc:  # pragma: no cover - the assertion is that we do not get here
        pytest.fail(f"an unknown subcommand raised instead of returning: {exc}")
    assert code != 0


def test_the_pager_is_off_unless_asked(capfd):
    # Not observable directly: the pager only engages on a TTY and a test runner
    # has none. What is asserted is that the option exists, is keyword-only, and
    # does not change the answer -- if paging ever leaked on by default here, it
    # would take fd 1 and block until somebody pressed q.
    assert nh.cli.run(["--version"], pager=False) == 0
    assert nh.cli.run(["--version"], pager=True) == 0

    with pytest.raises(TypeError):
        nh.cli.run(["--version"], True)  # type: ignore[misc]


def test_python_m_nodehammer_runs_the_cli():
    # The console script's twin, and testable without installing a wheel: the
    # same `main()` that `[project.scripts]` points at.
    result = subprocess.run(
        [sys.executable, "-m", "nodehammer", "--version"],
        capture_output=True,
        text=True,
        check=False,
    )

    assert result.returncode == 0
    assert nh.VERSION in result.stdout


def test_a_failing_command_gives_the_shell_a_nonzero_status():
    result = subprocess.run(
        [sys.executable, "-m", "nodehammer", "convert", "--input", "no-such.gdml",
         "--output", "out.glb"],
        capture_output=True,
        text=True,
        check=False,
    )

    # `main()` sys.exit()s the int, so a shell sees what the executable would
    # have given it. A traceback here would mean the code escaped as an
    # exception instead.
    assert result.returncode == 1
    assert "Traceback" not in result.stderr


# ── the web runtime, and the rung this package fills ─────────────────────────
#
# `viewer serve` serves a directory of Emscripten output that no native build
# produces, so the wheel gets it from a sibling distribution, `nodehammer-web`.
# C++ cannot find that directory on its own: under a wheel the running
# executable is the interpreter, and in a virtualenv the platform call behind
# `<exe>/../share` resolves `bin/python` to the *base* interpreter. So Python
# looks the package up and passes the path down.
#
# The serving half is not testable here -- it needs a 7 MB runtime this suite
# does not have, and the C++ cases already cover the ladder's precedence. What
# is testable is the wiring: that the path is passed at all, that a missing
# sibling is a supported state rather than an error, and that the failure names
# the package.


def test_the_web_runtime_is_optional(monkeypatch):
    # The headless install: no `nodehammer_web`, and every other command still
    # works. An eager import here would have made this package a hard dependency
    # of the whole CLI rather than of one flag on one subcommand.
    monkeypatch.setattr(nh.cli, "_web_runtime_dir", lambda: "")

    assert nh.cli.run(["--version"]) == 0


def test_the_runtime_directory_reaches_the_library(monkeypatch, tmp_path, capfd):
    # The wiring itself, and observable only through the refusal: the library
    # reports every rung it walked, so a path that arrived is a path that gets
    # named. Standing in for the sibling package with a directory that is *not*
    # a runtime is what makes it show up -- a real one would be served, and say
    # nothing about where it came from.
    pretend = tmp_path / "site-packages" / "nodehammer_web" / "runtime"
    pretend.mkdir(parents=True)
    monkeypatch.setattr(nh.cli, "_web_runtime_dir", lambda: str(pretend))
    monkeypatch.delenv("NODEHAMMER_WEB_ASSETS", raising=False)

    code = nh.cli.run(["viewer", "serve", "--no-browser"])

    assert code != 0
    err = capfd.readouterr().err
    assert str(pretend) in err
    # The rung it was attributed to. Reporting it as the environment variable or
    # as `--web-assets` would send the reader to fix something they never set.
    assert "host program" in err


def test_the_failure_names_the_package_that_supplies_a_runtime(monkeypatch, capfd):
    # The message a person actually hits: `pip install nodehammer`, no
    # `nodehammer-web`, `nodehammer viewer serve`. Naming the package is the
    # whole remedy, and it is the one thing the ladder's explanation cannot
    # derive from what it found -- it has to say it.
    monkeypatch.setattr(nh.cli, "_web_runtime_dir", lambda: "")
    monkeypatch.delenv("NODEHAMMER_WEB_ASSETS", raising=False)

    code = nh.cli.run(["viewer", "serve", "--no-browser"])

    assert code != 0
    assert "nodehammer-web" in capfd.readouterr().err


def test_cli_run_takes_the_runtime_directory_as_an_argument():
    # The binding's signature, asserted directly: the wrapper is what fills this
    # in, and a default of "" is what makes the sibling package optional rather
    # than the wrapper needing to know whether the argument exists.
    assert nh._nodehammer.cli_run(["--version"], False, "") == 0
    assert nh._nodehammer.cli_run(["--version"], pager=False, web_assets="") == 0
