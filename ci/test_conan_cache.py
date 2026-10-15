"""Offline integration checks for graph archives, plus cache-pruning boundaries."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "ci" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Pruning(unittest.TestCase):
    def test_preserves_refs_versions_and_newest_two(self):
        prune = load("prune-caches")
        key = "conan-native-" + "a" * 64
        entries = [dict(id=i, key=f"{key}-{i}", ref="refs/heads/main",
                        version="v1", created_at=str(i)) for i in range(1, 5)]
        entries += [dict(entries[0], id=5, ref="refs/pull/1/merge"),
                    dict(entries[0], id=6, version="v2"),
                    dict(entries[0], id=7, key="conan-v3-fixed-key")]
        self.assertEqual([e["id"] for e in prune.candidates(entries, 2)], [2, 1])


class GraphArchive(unittest.TestCase):
    def test_archive_excludes_unrelated_packages_and_restores_offline(self):
        with tempfile.TemporaryDirectory(prefix="nh-cache-test-") as directory:
            root = Path(directory)
            env = dict(os.environ, CONAN_HOME=str(root / "producer"))

            def conan(*args):
                result = subprocess.run(["conan", *map(str, args)], env=env,
                                        text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout

            conan("profile", "detect")
            recipe = root / "recipe"
            recipe.mkdir()
            (recipe / "conanfile.py").write_text('''from conan import ConanFile
from conan.tools.files import save
import os
class Package(ConanFile):
    version = "1.0"
    package_type = "header-library"
    def source(self):
        save(self, os.path.join(self.source_folder, "scratch-source"), "temporary")
    def build(self):
        save(self, os.path.join(self.build_folder, "scratch-object"), "temporary")
    def package(self):
        save(self, os.path.join(self.package_folder, "include", "probe.h"), "#pragma once")
''')
            for name in ("needed", "build-tool", "unrelated"):
                conan("create", recipe, f"--name={name}", "--no-remote")
            project = root / "project"
            project.mkdir()
            (project / "conanfile.txt").write_text("[requires]\nneeded/1.0\n[tool_requires]\nbuild-tool/1.0\n")
            conan("lock", "create", project, "--no-remote", f"--lockfile-out={project / 'conan.lock'}")
            archive = root / "dependencies.tgz"
            env["NH_CONAN_CACHE_ARCHIVE"] = str(archive)

            def install():
                result = subprocess.run(["python3", str(ROOT / "ci/conan-cache.py"),
                                         str(project), "--no-remote", "--build=never",
                                         "-of", str(root / "output")],
                                        env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout + result.stderr

            install()
            import tarfile
            with tarfile.open(archive) as tar:
                self.assertFalse(any("scratch-" in name for name in tar.getnames()))
            env["CONAN_HOME"] = str(root / "consumer")
            conan("profile", "detect")
            result = install()
            self.assertIn("Already installed!", result)
            packages = json.loads(conan("list", "*", "--format=json"))["Local Cache"]
            self.assertEqual(set(packages), {"needed/1.0", "build-tool/1.0"})
            conan("cache", "check-integrity", "*")


if __name__ == "__main__":
    unittest.main()
