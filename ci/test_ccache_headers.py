"""Installed SYSTEM headers must invalidate a warmed consumer compilation cache."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which("ccache"), "ccache is not installed")
class InstalledHeaderCacheTest(unittest.TestCase):
    def test_changed_system_header_recompiles_consumer(self):
        # Inherit the CI cache policy, but never modify its real cache contents.
        with tempfile.TemporaryDirectory(prefix="nh-ccache-headers-") as directory:
            root = Path(directory)
            include = root / "include"
            include.mkdir()
            source = root / "consumer.cpp"
            source.write_text(
                '#include <version.hpp>\n#include <cstdio>\n'
                'int main() { std::puts(VERSION); }\n'
            )
            env = dict(os.environ, CCACHE_DIR=str(root / "cache"))
            compiler = shlex.split(env.get("CXX", "c++"))
            obj = root / "consumer.o"
            exe = root / "consumer"
            for version in ("main", "pull-request"):
                (include / "version.hpp").write_text(
                    f'inline constexpr const char* VERSION = "{version}";\n'
                )
                subprocess.run(
                    ["ccache", *compiler, "-std=c++20", "-isystem", str(include),
                     "-c", str(source), "-o", str(obj)],
                    env=env, check=True, capture_output=True, text=True,
                )
                subprocess.run(
                    [*compiler, str(obj), "-o", str(exe)],
                    env=env, check=True, capture_output=True, text=True,
                )
                actual = subprocess.check_output([str(exe)], text=True).strip()
                self.assertEqual(actual, version, "ccache reused an outdated SYSTEM header")


if __name__ == "__main__":
    unittest.main()
