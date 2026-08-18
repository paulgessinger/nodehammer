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
    result = nh.SemanticScene.read("", format="synthetic")

    assert result.scene.valid()
    assert not result.diags.has_errors()

    assert result.scene.node_count() == 1
    assert result.scene.log_vol_count() == 1
    assert result.scene.shape_count() == 1
    assert result.scene.material_count() == 1


def test_result_unpacks_like_the_cpp_structured_binding():
    scene, diags = nh.SemanticScene.read("", format="synthetic")

    assert scene.valid()
    assert not diags.has_errors()


def test_formats_reports_what_this_build_can_read_and_write():
    formats = nh.SemanticScene.formats()

    assert isinstance(formats, list)
    assert all(isinstance(f, str) for f in formats)

    # Unconditional backends: a build that cannot report these is broken rather
    # than merely minimal.
    assert "synthetic" in formats
    assert "json" in formats
    assert "flatbuffer" in formats
    assert "nhb" in formats  # the write side's name for it


def test_read_rejects_a_format_this_build_does_not_have():
    with pytest.raises(nh.Error) as excinfo:
        nh.SemanticScene.read("", format="no-such-backend")

    assert excinfo.value.code == "NH0101"
    assert "formats()" in str(excinfo.value)


def test_read_throws_on_a_file_that_will_not_open(tmp_path):
    with pytest.raises(nh.Error):
        nh.SemanticScene.read(tmp_path / "does-not-exist.nhb")


def test_round_trips_through_nhb_bytes():
    original = nh.SemanticScene.read("", format="synthetic").scene

    payload = original.to_nhb()
    assert isinstance(payload, bytes)
    assert len(payload) > 0

    restored = nh.SemanticScene.read(payload).scene

    assert restored.valid()
    assert restored.node_count() == original.node_count()
    assert restored.shape_count() == original.shape_count()


def test_round_trips_through_a_file(tmp_path):
    original = nh.SemanticScene.read("", format="synthetic").scene

    path = tmp_path / "scene.nhb"
    original.write(path)
    assert path.exists()

    restored = nh.SemanticScene.read(path).scene
    assert restored.node_count() == original.node_count()


def test_read_accepts_os_pathlike_and_str(tmp_path):
    scene = nh.SemanticScene.read("", format="synthetic").scene
    path = tmp_path / "scene.nhb"
    scene.write(path)

    assert nh.SemanticScene.read(path).scene.valid()  # pathlib.Path
    assert nh.SemanticScene.read(str(path)).scene.valid()  # str


def test_write_rejects_a_format_this_build_does_not_have(tmp_path):
    scene = nh.SemanticScene.read("", format="synthetic").scene

    with pytest.raises(nh.Error):
        scene.write(tmp_path / "scene.out", format="no-such-writer")


def test_an_empty_scene_answers_and_raises_where_it_would_dereference():
    empty = nh.SemanticScene()

    assert not empty.valid()

    # NH0800: the call cannot deliver what its signature promises, so it is
    # fatal rather than a diagnostic.
    with pytest.raises(nh.Error):
        empty.to_nhb()


def test_a_scene_outlives_the_result_that_produced_it():
    # The C++ handle owns a shared_ptr<const Impl>, so the scene is not borrowed
    # from the result. If that ownership did not survive the binding, this reads
    # freed memory rather than failing an assertion — which is why it is a case.
    def make():
        return nh.SemanticScene.read("", format="synthetic").scene

    scene = make()
    import gc

    gc.collect()

    assert scene.valid()
    assert scene.node_count() == 1
