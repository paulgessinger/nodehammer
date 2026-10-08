"""`SemanticScene`, through the extension.

The Python twin of tests/public/test_public_semantic_scene.cpp. The counts are
exact rather than merely non-zero for the same reason they are there: the
synthetic importer builds one box, so a count that came back wrong is a value
that did not survive the boundary, which is a different failure from a pipeline
bug and worth telling apart.
"""

from __future__ import annotations

import pytest

import nodehammer as nh


def test_read_imports_through_the_synthetic_backend():
    result = nh.read_semantic("", format="synthetic")

    assert result.scene.valid
    assert not result.diags.has_errors

    assert result.scene.node_count == 1
    assert result.scene.log_vol_count == 1
    assert result.scene.shape_count == 1
    assert result.scene.material_count == 1


def test_result_unpacks_like_the_cpp_structured_binding():
    scene, diags = nh.read_semantic("", format="synthetic")

    assert scene.valid
    assert not diags.has_errors


def test_formats_reports_what_this_build_can_read_and_write():
    formats = nh.semantic_read_formats()

    assert isinstance(formats, list)
    assert all(isinstance(f, str) for f in formats)

    # Unconditional backends: a build that cannot report these is broken rather
    # than merely minimal.
    assert "synthetic" in formats
    assert "json" in formats
    assert "nhb" in formats
    assert "nhb" in formats  # the write side's name for it


def test_read_rejects_a_format_this_build_does_not_have():
    with pytest.raises(nh.Error) as excinfo:
        nh.read_semantic("", format="no-such-backend")

    assert excinfo.value.code == "NH0101"
    assert "semanticReadFormats()" in str(excinfo.value)


def test_read_throws_on_a_file_that_will_not_open(tmp_path):
    with pytest.raises(nh.Error):
        nh.read_semantic(tmp_path / "does-not-exist.nhb")


def test_round_trips_through_nhb_bytes():
    original = nh.read_semantic("", format="synthetic").scene

    payload = nh.to_nhb(original)
    assert isinstance(payload, bytes)
    assert len(payload) > 0

    restored = nh.from_nhb(payload).scene

    assert restored.valid
    assert restored.node_count == original.node_count
    assert restored.shape_count == original.shape_count


def test_round_trips_through_a_file(tmp_path):
    original = nh.read_semantic("", format="synthetic").scene

    path = tmp_path / "scene.nhb"
    nh.write(original, path)
    assert path.exists()

    restored = nh.read_semantic(path).scene
    assert restored.node_count == original.node_count


def test_read_accepts_os_pathlike_and_str(tmp_path):
    scene = nh.read_semantic("", format="synthetic").scene
    path = tmp_path / "scene.nhb"
    nh.write(scene, path)

    assert nh.read_semantic(path).scene.valid  # pathlib.Path
    assert nh.read_semantic(str(path)).scene.valid  # str


def test_write_rejects_a_format_this_build_does_not_have(tmp_path):
    scene = nh.read_semantic("", format="synthetic").scene

    with pytest.raises(nh.Error):
        nh.write(scene, tmp_path / "scene.out", format="no-such-writer")


def test_an_empty_scene_answers_and_raises_where_it_would_dereference():
    empty = nh.SemanticScene()

    assert not empty.valid

    # NH0800: the call cannot deliver what its signature promises, so it is
    # fatal rather than a diagnostic.
    with pytest.raises(nh.Error):
        nh.to_nhb(empty)


def test_a_scene_outlives_the_result_that_produced_it():
    # The C++ handle owns a shared_ptr<const Impl>, so the scene is not borrowed
    # from the result. If that ownership did not survive the binding, this reads
    # freed memory rather than failing an assertion — which is why it is a case.
    def make():
        return nh.read_semantic("", format="synthetic").scene

    scene = make()
    import gc

    gc.collect()

    assert scene.valid
    assert scene.node_count == 1


@pytest.mark.parametrize("level", [-1, 3, 9])
def test_compression_level_and_filename_contract(tmp_path, level):
    scene = nh.read_semantic("", format="synthetic").scene
    payload = nh.to_nhb_zstd(scene, compression_level=level)
    assert payload[:4] == bytes.fromhex("28b52ffd")
    assert nh.to_nhb(nh.from_nhb(payload).scene) == nh.to_nhb(scene)
    path = tmp_path / "scene.nhb.zst"
    nh.write(scene, path, compression_level=level)
    assert path.read_bytes() == payload
    plain = tmp_path / "scene.nhb"
    nh.write(scene, plain, compression_level=level)
    assert plain.read_bytes() == nh.to_nhb(scene)
    with pytest.raises(nh.Error):
        nh.from_nhb(payload[:8])


@pytest.mark.parametrize("options", [
    {"dd4hep.typo": True},
    {"dd4hep.useGlobalDetector": True},
    {"dd4hep.useGlobalDetector": False},
    {"dd4hep.useGlobalDetector": 1},
    {"dd4hep.useGlobalDetector": 1.0},
    {"dd4hep.useGlobalDetector": "true"},
])
def test_backend_options_are_validated_and_do_not_leak(options):
    with pytest.raises(nh.Error) as error:
        nh.read_semantic("", format="synthetic", importer_options=options)
    assert error.value.code == "NH0105"
    assert next(iter(options)) in str(error.value)
    assert nh.read_semantic("", format="synthetic").scene.node_count == 1


@pytest.mark.parametrize("options", [{1: True}, {"option": []}, {"option": {}}, {"option": None}])
def test_backend_options_reject_nonprimitive_values(options):
    with pytest.raises(TypeError):
        nh.read_semantic("", format="synthetic", importer_options=options)


@pytest.mark.skipif("dd4hep" not in nh.semantic_read_formats(), reason="requires DD4hep")
@pytest.mark.parametrize("fail_first", [False, True])
def test_global_detector_lifetime_in_a_fresh_process(tmp_path, fail_first):
    import pathlib
    import subprocess
    import sys

    simple = pathlib.Path(__file__).resolve().parents[2] / "fixtures/dd4hep/simple_box.xml"
    broken = tmp_path / "broken.xml"
    broken.write_text("<lccdd><broken")
    script = """
import sys
import nodehammer as nh
options = {"dd4hep.useGlobalDetector": True}
if sys.argv[3] == "True":
    try:
        nh.read_semantic(sys.argv[2], importer_options=options)
    except nh.Error as error:
        assert error.code == "NH0300"
    else:
        raise AssertionError("invalid compact was accepted")
else:
    assert nh.read_semantic(sys.argv[1], importer_options=options).scene.node_count == 2
try:
    nh.read_semantic(sys.argv[1], importer_options=options)
except nh.Error as error:
    assert "fresh default detector" in str(error)
else:
    raise AssertionError("global detector was reused")
assert nh.read_semantic(sys.argv[1]).scene.node_count == 2
"""
    result = subprocess.run(
        [sys.executable, "-c", script, str(simple), str(broken), str(fail_first)],
        capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "NH0302" not in result.stderr


@pytest.mark.parametrize("value", [-(2**63) - 1, 2**63])
def test_backend_option_integer_overflow(value):
    with pytest.raises(OverflowError):
        nh.read_semantic("", format="synthetic", importer_options={"option": value})


@pytest.mark.skipif("dd4hep" not in nh.semantic_read_formats(), reason="requires DD4hep")
@pytest.mark.parametrize("value,kind", [(1, "int64"), (1.0, "double"), ("true", "string")])
def test_backend_options_do_not_coerce_values_to_bool(value, kind):
    with pytest.raises(nh.Error) as error:
        nh.read_semantic("unused.xml", importer_options={"dd4hep.useGlobalDetector": value})
    assert error.value.code == "NH0105"
    assert f"expects bool, got {kind}" in str(error.value)
