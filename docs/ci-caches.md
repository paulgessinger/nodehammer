# CI dependency caches

CI uses Conan 2.30.0 and the root `conan.lock`. The lock pins recipe versions and
revisions for Linux, macOS, Windows and WebAssembly, with and without the viewer.
Native jobs, Key4hep and wheels pass the lockfile explicitly. Local Conan commands
also discover the root lockfile automatically.

After changing a dependency or a vendored recipe, regenerate and review the lock:

```sh
uv run --no-project --with conan==2.30.0 python ci/update-conan-lock.py
```

This exports the vendored recipes and merges eight platform/viewer graphs. It
resolves dependencies without compiling them, and needs a detected default Conan
profile (`conan profile detect`). Add `--update` to refresh upstream recipe
revisions too. Review `conan.lock` along with the recipe changes. Recipe locks do
not pin compiler installations, container images, or package binary revisions.

`ci/conan-cache.py` wraps the install. When `NH_CONAN_CACHE_ARCHIVE` is set, it
restores that archive before installing. On a cache miss it captures the install's
JSON graph, converts it into a Conan package list, and saves only that graph's
recipes and package binaries, including build tools. Extracted sources, build
trees and downloads are excluded. Without the environment variable it only
performs the locked install.

Actions caches the resulting `.cache/conan/dependencies.tgz`, rather than the
entire Conan package store. Exact keys include the lock and relevant build inputs;
only main saves archives. There is no fallback across dependency configurations,
so obsolete packages cannot accumulate in the next snapshot. Conan is pinned in
both the workflow and wheel configuration because its archive format can change
between versions. Update those pins and the cache key version together.

The first run using the new archive format is cold. Subsequent runs restore one
archive per configuration. Key4hep's compiler cache remains incremental, with a
separate directory and per-run keys to avoid the container action's daily cache.
Linux wheel logs print compiler-cache statistics before running tests.

## Pruning old GitHub snapshots

Preview and then prune redundant snapshots:

```sh
python3 ci/prune-caches.py
python3 ci/prune-caches.py --apply
```

This requires `gh` authentication with permission to manage Actions caches. It
retains the newest two snapshots per recognized main-branch cache family and
cache format. Dependency hashes remain separate. PR caches, fixed keys, and
unrecognized keys are untouched. `--keep N` changes the retention count (minimum
one). Run this after bursts of CI activity; it is not scheduled automatically.
After the archive caches have populated on main, legacy fixed configuration
caches can be reviewed for removal separately.

## Validation

```sh
python3 -m unittest discover -s ci -p test_conan_cache.py -v
```

The offline integration test checks that host/build dependencies survive archive
restore into a fresh cache, unrelated packages and scratch files are excluded,
and restored packages pass Conan's integrity check. Pruning tests protect recent
snapshots, PR refs, cache formats and fixed keys. CI runs these on Linux x86_64.
