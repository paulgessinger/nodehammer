import json
from contextlib import nullcontext
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import fccee

TREE = {"maxDepth": 2, "nodes": [
    {"path": "/world", "depth": 0},
    {"path": "/world/det", "depth": 1},
    {"path": "/world/det/module", "depth": 2},
]}
RENDER = {"nodes": [{"meshBindings": [{"meshId": 1}]},
                     {"meshBindings": []}, {"meshBindings": []}],
          "meshAssets": [{"id": 1, "indexCount": 3, "vertexCount": 3}]}


class FCCeeChecks(unittest.TestCase):
    def test_phase_captures_native_output_and_restores_descriptors(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            timings = {}
            with self.assertRaisesRegex(ValueError, 'failed'):
                with fccee.capture_phase(output, 'convert', timings):
                    os.write(1, b'plugin stdout\n')
                    os.write(2, b'plugin stderr\n')
                    raise ValueError('failed')
            self.assertEqual((output / 'convert.stdout').read_text(), 'plugin stdout\n')
            self.assertEqual((output / 'convert.log').read_text(), 'plugin stderr\n')
            self.assertIn('convert', timings)

    def test_four_models_overlap_and_failure_is_reported(self):
        barrier = threading.Barrier(4)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for model, (family, _) in fccee.MODELS.items():
                source = root / 'FCCee' / family / 'compact' / model / f'{model}.xml'
                source.parent.mkdir(parents=True)
                source.touch()

            def run_worker(source, output, minimum):
                # All four must start before any can finish.
                barrier.wait(timeout=5)
                self.assertEqual(output.name, source.stem)
                if source.stem == 'IDEA_o1_v03':
                    raise RuntimeError('import failed')
                return {'full_nodes': minimum}

            argv = ['fccee.py', '--geometry-root', str(root), '--output', str(root / 'results')]
            with patch('sys.argv', argv), patch('fccee.run_worker', side_effect=run_worker):
                self.assertEqual(fccee.main(), 1)
            report = json.loads((root / 'results/summary.json').read_text())
            self.assertEqual(set(report), set(fccee.MODELS))
            self.assertEqual(report['IDEA_o1_v03'], {'status': 'failed', 'error': 'import failed'})
            self.assertEqual(sum(row['status'] == 'passed' for row in report.values()), 3)

    def test_bounded_config(self):
        text = fccee.smoke_config(TREE)
        self.assertIn('/world/det/module/**', text)
        self.assertIn('fallback = "fail"', text)
        self.assertNotIn('merge_descendants', text)

    def test_bad_tree_fails(self):
        for tree in ({"maxDepth": 2, "nodes": []},
                     {"maxDepth": 3, "nodes": TREE['nodes']},
                     {"maxDepth": 2, "nodes": [{"depth": 0, "path": "/world*"}]}):
            with self.assertRaises(ValueError):
                fccee.smoke_config(tree)

    def test_empty_geometry_and_dangling_bindings_fail(self):
        for render in ({"nodes": [], "meshAssets": []},
                       {**RENDER, "meshAssets": []},
                       {**RENDER, "meshAssets": [{"id": 2, "indexCount": 3, "vertexCount": 3}]}):
            with self.assertRaises(ValueError):
                fccee.check_render(render, 3)
        self.assertEqual(fccee.check_render(RENDER, 3)['triangles'], 1)

    def test_process_failure_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch('fccee.subprocess.run', return_value=subprocess.CompletedProcess([], 7)):
                with self.assertRaisesRegex(RuntimeError, 'worker exited 7'):
                    fccee.run_worker(Path('/det.xml'), Path(directory), 1)

    def test_successful_exit_without_result_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / 'result.json').write_text('{}')
            with patch('fccee.subprocess.run', return_value=subprocess.CompletedProcess([], 0)):
                with self.assertRaises(FileNotFoundError):
                    fccee.run_worker(Path('/det.xml'), output, 1)

    def test_full_import_and_render_are_checked(self):
        class Diagnostics(list):
            has_errors = False

        diags = Diagnostics()
        scene = SimpleNamespace(valid=True, node_count=260_000_000)
        cfg = SimpleNamespace(scene=object(), output=object())
        calls = []
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)

            def read_semantic(source, **kwargs):
                calls.append((source, kwargs))
                return scene, diags

            def write(value, path, *args, **kwargs):
                if value is scene:
                    path.write_bytes(b'geometry')
                else:
                    path.write_text(json.dumps(RENDER))

            def inspect(args):
                phase = args[3]
                doc = ({'nodes': scene.node_count, 'diagnostics': {'errors': 0}}
                       if phase == 'summary' else TREE)
                (output / f'{phase}.stdout').write_text(json.dumps(doc))
                return 0

            nh = SimpleNamespace(read_semantic=read_semantic, write=write,
                                 cli=SimpleNamespace(run=inspect),
                                 read_config=lambda path: (cfg, diags),
                                 build=lambda value, config: (object(), diags))
            with patch.dict('sys.modules', nodehammer=nh), \
                    patch('fccee.capture_phase', side_effect=lambda *args: nullcontext()):
                result = fccee.run_case(Path('/det.xml'), output, 200_000_000)
                self.assertEqual(result['render_nodes'], 3)
                self.assertEqual(calls[0], (Path('/det.xml'), {
                    'importer_options': {'dd4hep.useGlobalDetector': True}}))
                self.assertEqual(result['full_nodes'], 260_000_000)
                with self.assertRaisesRegex(ValueError, 'unexpectedly reduced'):
                    fccee.run_case(Path('/det.xml'), output, 300_000_000)
                diags.has_errors = True
                with self.assertRaisesRegex(ValueError, 'error diagnostics'):
                    fccee.run_case(Path('/det.xml'), output)


if __name__ == '__main__':
    unittest.main()
