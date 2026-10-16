import json
from pathlib import Path
import subprocess
import tempfile
import threading
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
    def test_four_models_overlap_and_failure_is_reported(self):
        barrier = threading.Barrier(4)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for model, (family, _) in fccee.MODELS.items():
                source = root / 'FCCee' / family / 'compact' / model / f'{model}.xml'
                source.parent.mkdir(parents=True)
                source.touch()

            def run_case(exe, source, output, minimum):
                # All four must start before any can finish.
                barrier.wait(timeout=5)
                self.assertEqual(output.name, source.stem)
                if source.stem == 'IDEA_o1_v03':
                    raise RuntimeError('import failed')
                return {'full_nodes': minimum}

            argv = ['fccee.py', '--geometry-root', str(root), '--output', str(root / 'results')]
            with patch('sys.argv', argv), patch('fccee.run_case', side_effect=run_case):
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
            with patch('fccee.subprocess.run', return_value=subprocess.CompletedProcess([], 7, '')):
                with self.assertRaisesRegex(RuntimeError, 'convert exited 7'):
                    fccee.run_case(Path('/nh'), Path('/det.xml'), Path(directory))

    def test_successful_exit_without_geometry_is_not_a_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch('fccee.subprocess.run', return_value=subprocess.CompletedProcess([], 0, '')):
                (Path(directory) / 'geometry.nhb.zst').write_bytes(b'stale output')
                with self.assertRaisesRegex(ValueError, 'did not write geometry'):
                    fccee.run_case(Path('/nh'), Path('/det.xml'), Path(directory))

    def test_full_import_and_render_are_checked(self):
        commands = []
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)

            def execute(args, **kwargs):
                commands.append(args)
                if 'summary' in args:
                    doc = {"nodes": 260_000_000, "diagnostics": {"errors": 0}}
                elif 'tree' in args:
                    doc = TREE
                elif '--output-format' in args:
                    (output / 'render.json').write_text(json.dumps(RENDER))
                    doc = {}
                else:
                    if '-o' in args:
                        (output / 'geometry.nhb.zst').write_bytes(b'geometry')
                    doc = {}
                return subprocess.CompletedProcess(args, 0, json.dumps(doc))

            with patch('fccee.subprocess.run', side_effect=execute):
                result = fccee.run_case(Path('/nh'), Path('/det.xml'), output, 200_000_000)
            self.assertEqual(result['render_nodes'], 3)
            self.assertIn('--dd4hep-global', commands[0])
            self.assertNotIn('-c', commands[0])  # The full import is never filtered.
            self.assertEqual(result['full_nodes'], 260_000_000)
            with patch('fccee.subprocess.run', side_effect=execute):
                with self.assertRaisesRegex(ValueError, 'unexpectedly reduced'):
                    fccee.run_case(Path('/nh'), Path('/det.xml'), output, 300_000_000)


if __name__ == '__main__':
    unittest.main()
