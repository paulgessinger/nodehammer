"""`Config`, `DiagnosticList` and the error model, through the extension.

The Python twin of tests/public/test_public_config.cpp and
test_public_diagnostics.cpp, plus the dict sugar, which has no C++ counterpart.
"""

from __future__ import annotations

import pytest

import nodehammer as nh


# ── Config ──────────────────────────────────────────────────────────────────


def test_parse_accepts_toml_text():
    result = nh.Config.parse("deduplicate_shapes = true\n")

    assert result.config.valid
    assert not result.diags.has_errors
    assert result.config.scene.valid
    assert result.config.output.valid


def test_config_result_unpacks():
    config, diags = nh.Config.parse("deduplicate_shapes = true\n")

    assert config.valid
    assert not diags.has_errors


def test_read_loads_a_file(tmp_path):
    path = tmp_path / "cfg.toml"
    path.write_text("deduplicate_shapes = true\n")

    assert nh.Config.read(path).config.valid


def test_formats_is_constant_and_includes_lua():
    # Lua ships everywhere now, wasm included, so this is build-independent.
    assert nh.Config.formats() == ["toml", "lua"]


def test_read_raises_on_a_document_that_does_not_load(tmp_path):
    path = tmp_path / "broken.toml"
    path.write_text("this is not = = toml\n")

    with pytest.raises(nh.Error):
        nh.Config.read(path)


def test_check_reports_rather_than_raising():
    # Two promises, two channels, one implementation: `parse` promises a config
    # and raises; `check_string` promises a report and returns one.
    diags = nh.Config.check_string("this is not = = toml\n")

    assert diags.has_errors
    assert len(diags) > 0


def test_an_unset_base_dir_means_no_location(tmp_path, monkeypatch):
    # A base-less load resolves nothing rather than reaching into the working
    # directory — planting the include target where a cwd-rooted loader would
    # find it is the point of the case.
    monkeypatch.chdir(tmp_path)
    (tmp_path / "included.toml").write_text("deduplicate_shapes = true\n")

    diags = nh.Config.check_string('include = "included.toml"\n')

    assert diags.has_errors
    assert any("not found" in d.message for d in diags)


def test_a_base_dir_resolves_includes(tmp_path):
    (tmp_path / "included.toml").write_text("deduplicate_shapes = true\n")

    diags = nh.Config.check_string('include = "included.toml"\n', base_dir=str(tmp_path))

    assert not diags.has_errors


# ── diagnostics and the error model ─────────────────────────────────────────


def test_diagnostic_list_is_iterable_and_sized():
    diags = nh.Config.check_string("this is not = = toml\n")

    items = list(diags)
    assert len(items) == len(diags)
    assert all(isinstance(d, nh.Diagnostic) for d in items)
    assert all(d.code for d in items)


def test_no_returned_list_contains_fatal():
    # docs/error-model.md's one-line invariant: Fatal is reserved for
    # Error.diagnostic() and never appears in a list the library returns.
    diags = nh.Config.check_string("this is not = = toml\n")

    assert all(d.severity is not nh.Diagnostic.Severity.Fatal for d in diags)


def test_error_carries_code_context_and_what_was_observed():
    with pytest.raises(nh.Error) as excinfo:
        nh.SemanticScene.read("", format="no-such-backend")

    err = excinfo.value
    assert err.code == "NH0101"
    assert isinstance(err.context, str)
    assert isinstance(err.observed, list)
    # `observed` is copied out of the C++ exception: the span it comes from dies
    # with the throw, so a borrowed view would dangle by the time we read it.
    assert all(isinstance(d, nh.Diagnostic) for d in err.observed)
    assert err.diagnostic.severity is nh.Diagnostic.Severity.Fatal
    assert err.diagnostic.code == err.code


def test_error_is_a_runtime_error():
    # So `except RuntimeError` keeps working for callers who do not care which
    # library raised.
    assert issubclass(nh.Error, RuntimeError)


# ── the dict sugar ──────────────────────────────────────────────────────────


def test_load_config_accepts_toml_text():
    assert nh.load_config("deduplicate_shapes = true\n").valid


def test_load_config_accepts_a_dict():
    pytest.importorskip("tomli_w")

    config = nh.load_config({"deduplicate_shapes": True})

    assert config.valid
    assert config.scene.valid


def test_load_config_applies_overrides():
    pytest.importorskip("tomli_w")

    config = nh.load_config(
        {"export": {"gltf": {"unit_scale": 1.0}}},
        export={"gltf": {"unit_scale": 0.01}},
    )

    assert config.valid


def test_load_config_routes_through_the_same_validator():
    pytest.importorskip("tomli_w")

    # A dict-built config gets the CLI's diagnostic codes, because it is the
    # same parser and the same validator — nothing here reimplements semantics.
    # NH0003 is what the CLI reports for the same document
    # (fixtures/configs/invalid_bad_tolerance.toml).
    with pytest.raises(nh.Error) as excinfo:
        nh.load_config({"rules": [{"tessellation": {"max_segments_circle": -1}}]})

    assert excinfo.value.code == "NH0003"


def test_load_config_reads_a_path_and_parses_a_str(tmp_path):
    # What `src` means is decided by its type, never by whether a file happens
    # to exist: a Path is a file, a str is TOML text.
    path = tmp_path / "cfg.toml"
    path.write_text("deduplicate_shapes = true\n")

    assert nh.load_config(path).valid  # Path -> read the file
    assert nh.load_config("deduplicate_shapes = true\n").valid  # str -> TOML text


def test_a_filename_passed_as_a_str_says_so(tmp_path):
    # The heuristic lives in the error path only, so it cannot change what a
    # successful call does — it just answers the question that "expected '='
    # after key" provokes when someone passed a filename as text.
    path = tmp_path / "cfg.toml"
    path.write_text("deduplicate_shapes = true\n")

    with pytest.raises(nh.Error) as excinfo:
        nh.load_config(str(path))

    assert "pass Path(" in str(excinfo.value)


def test_overrides_are_rejected_for_non_mappings(tmp_path):
    with pytest.raises(TypeError):
        nh.load_config("deduplicate_shapes = true\n", hoist_orphans=True)


def test_the_cheap_accessors_are_properties_not_methods():
    # Mirroring the C++ API means its names and semantics, not its punctuation.
    # Calling one is a clear TypeError rather than something that silently works.
    scene = nh.SemanticScene.read("", format="synthetic").scene

    assert scene.valid is True
    assert isinstance(scene.node_count, int)
    with pytest.raises(TypeError):
        scene.valid()


def test_work_and_io_stay_methods():
    # The line is cost, not arity: anything that serializes, does I/O or builds
    # a container is still a call, so a property never hides work.
    scene = nh.SemanticScene.read("", format="synthetic").scene

    assert isinstance(scene.to_nhb(), bytes)
    assert isinstance(nh.SemanticScene.formats(), list)
    assert isinstance(nh.Config.parse("").diags.items(), list)
