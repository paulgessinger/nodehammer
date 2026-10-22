import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import examples
from fccee import check_render


class ExampleChecks(unittest.TestCase):
    def test_download_is_verified_and_cache_is_rechecked(self):
        payload = b"geometry"
        entry = {"path": "detector/v1/input.root",
                 "sha256": hashlib.sha256(payload).hexdigest()}
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            with patch("examples.urllib.request.urlopen", return_value=io.BytesIO(payload)):
                target = examples.fetch("https://example.org/", entry, cache)
            with patch("examples.urllib.request.urlopen") as request:
                self.assertEqual(examples.fetch("https://example.org/", entry, cache), target)
                request.assert_not_called()
            target.write_bytes(b"corrupted cache")
            with patch("examples.urllib.request.urlopen", return_value=io.BytesIO(b"wrong")):
                with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
                    examples.fetch("https://example.org/", entry, cache)
            self.assertFalse(target.exists())
            self.assertFalse(list(cache.rglob("*.part")))

    def test_render_requires_bound_nonempty_geometry_without_exact_node_count(self):
        with self.assertRaises(ValueError):
            check_render({"nodes": [], "meshAssets": []})
        with self.assertRaises(ValueError):
            check_render({"nodes": [{"meshBindings": [{"meshId": 42}]}],
                          "meshAssets": [{"id": 1, "indexCount": 3, "vertexCount": 3}]})

    def test_representative_failure_cannot_be_reported_as_packaged(self):
        row = {"id": "atlas", "config": "scene.toml", "test": True}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)

            def command(exe, dest, phase, *args):
                if phase == "tessellate":
                    (dest / "render.json").write_text(json.dumps({"nodes": [], "meshAssets": []}))

            with patch("examples.command", side_effect=command):
                with self.assertRaises(ValueError):
                    examples.example(Path("/nh"), row, Path("/geometry.nhb"), output)
            self.assertFalse((output / "scene.nhproj").exists())

    def test_gallery_only_entry_does_not_repeat_tessellation(self):
        row = {"id": "atlas", "config": "scene.toml"}
        phases = []
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)

            def command(exe, dest, phase, *args):
                phases.append(phase)
                if phase == "pack":
                    (dest / "scene.nhproj").write_bytes(b"project")

            with patch("examples.command", side_effect=command):
                result = examples.example(Path("/nh"), row, Path("/geometry.nhb"), output)
            self.assertEqual(result["status"], "packaged")
            self.assertNotIn("tessellate", phases)
            self.assertIn("select", phases)


if __name__ == "__main__":
    unittest.main()
