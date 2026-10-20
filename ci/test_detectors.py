from concurrent.futures import ThreadPoolExecutor
import json
import os

import pytest
from pathlib import Path
import sys
import threading

import detectors
import examples


@pytest.fixture
def thread_pool(monkeypatch):
    # Keep local mocks in process for coordinator tests; real spawn is tested below.
    monkeypatch.setattr(detectors, 'ProcessPoolExecutor',
                        lambda max_workers, **kwargs: ThreadPoolExecutor(max_workers=max_workers))


def test_shared_pool_is_bounded_and_collects_failures(tmp_path, thread_pool):
    barrier = threading.Barrier(2)
    lock = threading.Lock()
    active = 0
    peak = 0

    def work(key):
        nonlocal active, peak
        with lock:
            active += 1
            peak = max(peak, active)
        try:
            barrier.wait(timeout=5)
            if key == 'bad':
                raise RuntimeError('import failed')
            return {key: {'status': 'passed'}}
        finally:
            with lock:
                active -= 1

    tasks = [(suite, key, {key: {'title': key}}, lambda key=key: work(key))
             for suite, key in [('fccee', 'good'), ('examples', 'bad'),
                                ('fccee', 'later'), ('examples', 'last')]]
    outputs = {suite: tmp_path / suite for suite in ['fccee', 'examples']}
    assert detectors.steer(tasks, outputs, 2) == 1
    assert peak == 2
    report = json.loads((outputs['examples'] / 'summary.json').read_text())
    assert report['bad']['error'] == 'import failed'
    assert report['last']['status'] == 'passed'
    assert set(json.loads((outputs['fccee'] / 'summary.json').read_text())) == {'good', 'later'}


def test_missing_results_cannot_pass_and_stale_summary_is_cleared(tmp_path, thread_pool):
    (tmp_path / 'summary.json').write_text('{"old": {"status": "passed"}}')
    tasks = [('examples', 'input', {'a': {}, 'b': {}}, lambda: {})]
    assert detectors.steer(tasks, {'examples': tmp_path}, 1) == 1
    report = json.loads((tmp_path / 'summary.json').read_text())
    assert set(report) == {'a', 'b'}
    assert all(row['status'] == 'failed' for row in report.values())


def test_presets_share_one_import_and_continue_after_failure(tmp_path, monkeypatch):
    calls = []
    catalog = {'base_url': 'https://example.invalid/',
               'inputs': {'atlas': {'path': 'atlas.nhb', 'sha256': 'hash'}}}
    rows = [{'id': 'detail', 'title': 'Detailed'}, {'id': 'simple', 'title': 'Simplified'}]
    monkeypatch.setattr(examples, 'fetch', lambda *args: Path('/source'))

    def prepare(*args):
        calls.append('import')
        return Path('/geometry'), {'nodes': 100}

    def example(exe, row, nhb, output):
        calls.append(row['id'])
        if row['id'] == 'detail':
            raise ValueError('tessellation failed')
        return {'status': 'packaged'}

    monkeypatch.setattr(examples, 'prepare', prepare)
    monkeypatch.setattr(examples, 'example', example)
    report = examples.run_input(Path('/nh'), catalog, 'atlas', rows, tmp_path, tmp_path)
    assert calls == ['import', 'detail', 'simple']
    assert report['detail']['status'] == 'failed'
    assert report['simple']['status'] == 'packaged'


def test_catalog_is_scheduled_by_input(tmp_path, monkeypatch, thread_pool):
    monkeypatch.setattr(sys, 'argv', ['detectors.py', '--suite', 'examples',
                                    '--output', str(tmp_path), '--jobs', '2'])
    seen = []

    def run_input(exe, catalog, key, rows, cache, output):
        seen.append((key, len(rows)))
        return {row['id']: {'status': 'packaged'} for row in rows}

    monkeypatch.setattr(examples, 'run_input', run_input)
    assert detectors.main() == 0
    assert sorted(count for _, count in seen) == [1, 1, 2, 2]
    assert len({key for key, _ in seen}) == 4


def process_result(key):
    return {key: {'status': 'passed', 'pid': os.getpid()}}


def crash_worker():
    os._exit(0)


def test_spawn_recycles_each_input_worker(tmp_path):
    from functools import partial
    tasks = [('examples', key, {key: {}}, partial(process_result, key))
             for key in ['a', 'b', 'c']]
    assert detectors.steer(tasks, {'examples': tmp_path}, 2) == 0
    rows = json.loads((tmp_path / 'summary.json').read_text())
    assert len({row['pid'] for row in rows.values()}) == 3
    assert all(row['pid'] != os.getpid() for row in rows.values())


def test_native_exit_does_not_hang_or_pass(tmp_path):
    tasks = [('examples', 'bad', {'bad': {}}, crash_worker)]
    assert detectors.steer(tasks, {'examples': tmp_path}, 1) == 1
    assert json.loads((tmp_path / 'summary.json').read_text())['bad']['status'] == 'failed'
