"""The four pipeline verbs and `version()`, through the extension.

The Python twin of tests/public/test_public_build.cpp.
"""

from __future__ import annotations

import nodehammer as nh


def synthetic():
    return nh.SemanticScene.read("", format="synthetic").scene


def test_version_reports_what_is_linked_not_what_was_compiled_against():
    # `VERSION` is a constant in the header; `version()` is a symbol in the
    # library. A mismatch means headers from one install and a library from
    # another — the failure the wheel's rpath exists to prevent.
    linked = nh.version()

    assert linked
    assert linked == nh.VERSION
    assert nh.__cxx_version__ == linked
    assert "." in nh.VERSION
    assert nh.VERSION_MAJOR >= 0


def test_the_verbs_run_against_a_default_config():
    # A default-constructed slice is usable and means the built-in defaults, so
    # the whole pipeline runs without a document anywhere in sight.
    scene = synthetic()
    defaults = nh.SceneConfig()

    selected = nh.apply_selection(scene, defaults)
    assert selected.scene.valid()
    assert selected.scene.node_count() == scene.node_count()  # no rules is a no-op

    deduped = nh.deduplicate(selected.scene, defaults)
    assert deduped.scene.valid()

    lowered = nh.tessellate(deduped.scene, defaults)
    assert lowered.scene.valid()
    assert lowered.scene.triangle_count() > 0


def test_build_is_the_three_verbs_in_order():
    # #41 §8's claim, from Python: `build` is not a fourth implementation.
    scene = synthetic()
    config = nh.Config.parse("deduplicate_shapes = true\n").config.scene()

    decomposed = nh.tessellate(
        nh.deduplicate(nh.apply_selection(scene, config).scene, config).scene,
        config,
    )
    whole = nh.build(scene, config)

    assert whole.scene.triangle_count() == decomposed.scene.triangle_count()
    assert whole.scene.node_count() == decomposed.scene.node_count()
    assert whole.scene.mesh_count() == decomposed.scene.mesh_count()


def test_the_verbs_reject_an_empty_scene():
    import pytest

    with pytest.raises(nh.Error):
        nh.build(nh.SemanticScene(), nh.SceneConfig())


def test_render_scene_round_trips_through_nhr_bytes():
    built = nh.build(synthetic(), nh.SceneConfig()).scene

    payload = built.to_nhr()
    assert isinstance(payload, bytes)

    restored = nh.RenderScene.read(payload)
    assert restored.valid()
    assert restored.triangle_count() == built.triangle_count()


def test_render_scene_writes_through_the_output_slice(tmp_path):
    cfg = nh.Config.parse("[export.gltf]\nunit_scale = 0.01\n").config
    built = nh.build(synthetic(), cfg.scene()).scene

    out = tmp_path / "scene.glb"
    built.write(out, cfg.output())

    assert out.exists()
    assert out.stat().st_size > 0
