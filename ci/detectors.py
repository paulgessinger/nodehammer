#!/usr/bin/env python3
"""Run detector coverage with one bounded pool of independent geometry inputs."""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
from functools import partial
import json
import multiprocessing
from pathlib import Path
import sys
import tomllib

import examples
import fccee


def steer(tasks, outputs, jobs):
    """Only the coordinator writes summaries; workers own disjoint geometry folders."""
    reports = {suite: {} for suite in outputs}
    for output in outputs.values():
        output.mkdir(parents=True, exist_ok=True)
        (output / 'summary.json').write_text('{}\n')
    with ProcessPoolExecutor(max_workers=jobs, mp_context=multiprocessing.get_context('spawn'),
                             max_tasks_per_child=1) as pool:
        pending = {pool.submit(work): (suite, label, metadata)
                   for suite, label, metadata, work in tasks}
        for future in as_completed(pending):
            suite, label, metadata = pending[future]
            try:
                rows = future.result()
                if set(rows) != set(metadata):
                    raise ValueError('worker result does not cover the expected examples')
            except Exception as error:
                rows = {key: {**meta, 'status': 'failed', 'error': str(error)}
                        for key, meta in metadata.items()}
            reports[suite].update(rows)
            (outputs[suite] / 'summary.json').write_text(
                json.dumps(reports[suite], indent=2) + '\n')
            failed = [key for key, row in rows.items() if row['status'] == 'failed']
            print(f'{"::error::Failed" if failed else "Completed"} {label}'
                  + (f': {", ".join(failed)}' if failed else ''), flush=True)
    return int(any(row['status'] == 'failed' for report in reports.values()
                   for row in report.values()))


def fcc_case(model, root, output):
    family, minimum = fccee.MODELS[model]
    source = root / 'FCCee' / family / 'compact' / model / f'{model}.xml'
    if not source.is_file():
        raise FileNotFoundError(source)
    smoke = fccee.run_case(source, output / model, minimum, package=False)
    report = {}
    for variant in ('simplified', 'detailed'):
        key = model + '-' + variant
        preset = examples.ROOT / f'fixtures/configs/fccee/{model}/{variant}.toml'
        report[key] = {'title': f'FCC-ee {model} — {variant}',
                       'source': str(source), 'archive': f'{key}/scene.nhproj',
                       'smoke_check': smoke}
        try:
            report[key].update(fccee.pack_fixture(output / model / 'geometry.nhb.zst',
                                                  preset, output / key))
        except Exception as error:
            report[key].update(status='failed', error=str(error))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4, help='maximum concurrent inputs (default: 4)')
    parser.add_argument('--suite', choices=['fccee', 'examples'], action='append',
                        help='run only this suite; default: both')
    parser.add_argument('--geometry-root', type=Path)
    parser.add_argument('--nodehammer', type=Path, default=Path('build/Release/nodehammer'))
    parser.add_argument('--catalog', type=Path, default=examples.ROOT / 'examples/catalog.toml')
    parser.add_argument('--cache', type=Path, default=Path('.cache/example-inputs'))
    parser.add_argument('--output', type=Path, default=Path('build'),
                        help='parent of fccee-results and example-results')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    suites = set(args.suite or ['fccee', 'examples'])
    outputs = {suite: args.output.resolve() / ('fccee-results' if suite == 'fccee' else 'example-results')
               for suite in suites}
    tasks = []
    if 'fccee' in suites:
        root = args.geometry_root or fccee.geometry_root()
        for model in fccee.MODELS:
            metadata = {model + '-' + variant: {'title': f'FCC-ee {model} — {variant}'}
                        for variant in ('simplified', 'detailed')}
            tasks.append(('fccee', model, metadata,
                          partial(fcc_case, model, root, outputs['fccee'])))
    if 'examples' in suites:
        catalog = tomllib.loads(args.catalog.read_text())
        groups = {}
        for row in catalog['examples']:
            groups.setdefault(row['input'], []).append(row)
        for key, rows in groups.items():
            metadata = examples.metadata(catalog, key, rows)
            tasks.append(('examples', key, metadata,
                          partial(examples.run_input, args.nodehammer.resolve(), catalog, key,
                                  rows, args.cache.resolve(), outputs['examples'])))
    return steer(tasks, outputs, args.jobs)


if __name__ == '__main__':
    sys.exit(main())
