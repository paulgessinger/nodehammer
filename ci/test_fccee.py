from contextlib import nullcontext
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
from types import SimpleNamespace

import pytest

import fccee

TREE = {"maxDepth": 2, "nodes": [
    {"path": "/world", "depth": 0},
    {"path": "/world/det", "depth": 1},
    {"path": "/world/det/module", "depth": 2},
]}
RENDER = {"nodes": [{"meshBindings": [{"meshId": 1}]},
                     {"meshBindings": []}, {"meshBindings": []}],
          "meshAssets": [{"id": 1, "indexCount": 3, "vertexCount": 3}]}


def test_phase_captures_native_output_and_restores_descriptors(tmp_path, capfd):
    timings = {}
    with pytest.raises(ValueError, match='failed'):
        with fccee.capture_phase(tmp_path, 'convert', timings):
            os.write(1, b'plugin stdout\n')
            os.write(2, b'plugin stderr\n')
            raise ValueError('failed')
    assert (tmp_path / 'convert.stdout').read_text() == 'plugin stdout\n'
    assert (tmp_path / 'convert.log').read_text() == 'plugin stderr\n'
    assert 'convert' in timings
    os.write(1, b'restored stdout\n')
    os.write(2, b'restored stderr\n')
    captured = capfd.readouterr()
    assert captured.out == 'restored stdout\n'
    assert captured.err == 'restored stderr\n'


def test_four_models_overlap_and_failure_is_reported(tmp_path, monkeypatch):
    barrier = threading.Barrier(4)
    for model, (family, _) in fccee.MODELS.items():
        source = tmp_path / 'FCCee' / family / 'compact' / model / f'{model}.xml'
        source.parent.mkdir(parents=True)
        source.touch()

    def run_worker(source, output, minimum):
        # All four must start before any can finish.
        barrier.wait(timeout=5)
        assert output.name == source.stem
        if source.stem == 'IDEA_o1_v03':
            raise RuntimeError('import failed')
        return {'full_nodes': minimum}

    monkeypatch.setattr(sys, 'argv', [
        'fccee.py', '--geometry-root', str(tmp_path), '--output', str(tmp_path / 'results')])
    monkeypatch.setattr(fccee, 'run_worker', run_worker)
    assert fccee.main() == 1
    report = json.loads((tmp_path / 'results/summary.json').read_text())
    assert set(report) == set(fccee.MODELS)
    assert report['IDEA_o1_v03'] == {'status': 'failed', 'error': 'import failed'}
    assert sum(row['status'] == 'passed' for row in report.values()) == 3


def test_bounded_config():
    text = fccee.smoke_config(TREE)
    assert '/world/det/module/**' in text
    assert 'fallback = "fail"' in text
    assert 'merge_descendants' not in text


@pytest.mark.parametrize('tree', [
    {"maxDepth": 2, "nodes": []},
    {"maxDepth": 3, "nodes": TREE['nodes']},
    {"maxDepth": 2, "nodes": [{"depth": 0, "path": "/world*"}]},
], ids=['empty', 'wrong-depth', 'glob-path'])
def test_bad_tree_fails(tree):
    with pytest.raises(ValueError):
        fccee.smoke_config(tree)


@pytest.mark.parametrize('render', [
    {"nodes": [], "meshAssets": []},
    {**RENDER, "meshAssets": []},
    {**RENDER, "meshAssets": [{"id": 2, "indexCount": 3, "vertexCount": 3}]},
], ids=['empty', 'missing-meshes', 'dangling-binding'])
def test_empty_geometry_and_dangling_bindings_fail(render):
    with pytest.raises(ValueError):
        fccee.check_render(render, 3)


def test_valid_render_counts_triangles():
    assert fccee.check_render(RENDER, 3)['triangles'] == 1


def test_process_failure_is_not_a_pass(tmp_path, monkeypatch):
    monkeypatch.setattr(fccee.subprocess, 'run',
                        lambda *args, **kwargs: subprocess.CompletedProcess([], 7))
    with pytest.raises(RuntimeError, match='worker exited 7'):
        fccee.run_worker(Path('/det.xml'), tmp_path, 1)


def test_successful_exit_without_result_is_not_a_pass(tmp_path, monkeypatch):
    (tmp_path / 'result.json').write_text('{}')
    monkeypatch.setattr(fccee.subprocess, 'run',
                        lambda *args, **kwargs: subprocess.CompletedProcess([], 0))
    with pytest.raises(FileNotFoundError):
        fccee.run_worker(Path('/det.xml'), tmp_path, 1)


@pytest.fixture
def bindings(tmp_path, monkeypatch):
    class Diagnostics(list):
        has_errors = False

    diags = Diagnostics()
    scene = SimpleNamespace(valid=True, node_count=260_000_000)
    cfg = SimpleNamespace(scene=object(), output=object())
    calls = []
    cli_calls = []

    def read_semantic(source, **kwargs):
        calls.append((source, kwargs))
        return scene, diags

    def write(value, path, *args, **kwargs):
        if value is scene:
            path.write_bytes(b'geometry')
        else:
            path.write_text(json.dumps(RENDER))

    def inspect(args):
        cli_calls.append(args)
        if args[0] in ('convert', 'project'):
            Path(args[args.index('-o') + 1]).write_bytes(b'packaged')
            return 0
        phase = args[3]
        doc = ({'nodes': scene.node_count, 'diagnostics': {'errors': 0}}
               if phase == 'summary' else TREE)
        (tmp_path / f'{phase}.stdout').write_text(json.dumps(doc))
        return 0

    nh = SimpleNamespace(read_semantic=read_semantic, write=write,
                         cli=SimpleNamespace(run=inspect),
                         read_config=lambda path: (cfg, diags),
                         build=lambda value, config: (object(), diags))
    monkeypatch.setitem(sys.modules, 'nodehammer', nh)
    monkeypatch.setattr(fccee, 'capture_phase', lambda *args: nullcontext())
    return SimpleNamespace(diags=diags, calls=calls, cli_calls=cli_calls)


def test_full_import_and_render_are_checked(tmp_path, bindings):
    result = fccee.run_case(Path('/det.xml'), tmp_path, 200_000_000)
    assert result['render_nodes'] == 3
    assert bindings.calls[0] == (Path('/det.xml'), {
        'importer_options': {'dd4hep.useGlobalDetector': True}})
    assert result['full_nodes'] == 260_000_000


def test_reduced_geometry_fails(tmp_path, bindings):
    with pytest.raises(ValueError, match='unexpectedly reduced'):
        fccee.run_case(Path('/det.xml'), tmp_path, 300_000_000)


def test_error_diagnostics_fail(tmp_path, bindings):
    bindings.diags.has_errors = True
    with pytest.raises(ValueError, match='error diagnostics'):
        fccee.run_case(Path('/det.xml'), tmp_path)


def test_package_uses_bounded_selection(tmp_path, bindings):
    fccee.run_case(Path('/det.xml'), tmp_path, package=True)
    select, pack = bindings.cli_calls[-2:]
    assert select[0] == 'convert'
    assert select[select.index('-c') + 1] == str(tmp_path / 'smoke.toml')
    assert pack[:2] == ['project', 'pack']
    assert pack[pack.index('-i') + 1] == select[select.index('-o') + 1]
    assert (tmp_path / 'scene.nhproj').stat().st_size > 0


def test_package_failure_is_not_a_pass(tmp_path, bindings, monkeypatch):
    nh = sys.modules['nodehammer']
    original = nh.cli.run
    monkeypatch.setattr(nh.cli, 'run', lambda args: 7 if args[0] == 'project' else original(args))
    with pytest.raises(RuntimeError, match='pack exited 7'):
        fccee.run_case(Path('/det.xml'), tmp_path, package=True)
