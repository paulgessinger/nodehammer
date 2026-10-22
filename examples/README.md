# Detector examples and CI coverage

The Pages documentation site is configured in `docs/site/zensical.toml`. Its
overview renders the repository README, and its detector catalog is built with
Zensical from the results of the Key4hep job. See `docs/site/README.md` for
adding pages or building the documentation without geometry inputs.
Its viewer runtime comes from the separate WASM build in the same workflow.
Pull requests build and check the site and upload it as `pages-site`; only
successful `main` runs deploy through the existing GitHub Pages environment.

## Coverage

| Input | Check | Published view |
| --- | --- | --- |
| Key4hep k4geo DD4hep XML | Existing full FCC-ee imports and bounded tessellation | Depth-2 overviews and authored simplified presets |
| ATLAS Phase 2 SQLite | GeoModel import, detailed selection and tessellation | Detailed and simplified |
| ATLAS Phase 1 NHB | Read, select and package | Detailed and simplified |
| sPHENIX ROOT | TGeo import and bounded tessellation | Depth-2 overview |
| sPHENIX GDML | Geant4 import and bounded tessellation | Depth-2 overview |

This deliberately avoids a matrix of every preset, importer entry point, and
graphics backend. The overview configs limit rendering to two placement levels;
they are useful integration examples, not polished detector visualizations.
FCC-ee also packages the existing simplified fixtures under
`fixtures/configs/fccee/`, reusing the full import within each model's worker.
These are package-only entries; detailed fixture downloads are provided with
explicit IDEA reduced-input restrictions. We retain the depth-2 smoke checks.
ATLAS's detailed preset is the representative expensive check. The dashboard
labels the other presets as package-only; a successful package is not a claim
that its rendering was tested.

The Key4hep build enables DD4hep, TGeo, GeoModel and Geant4/GDML. It must provide
Geant4 with GDML support; a missing backend is a build failure, not a skipped
test. No ATLAS job is launched through CVMFS.

One coordinator schedules all inputs in a shared pool (`--jobs 4` by default).
A spawn-based process pool gives each input a fresh Python worker, recycled
after one task. FCC-ee uses the bindings directly; WebEOS stages invoke CLI
processes. Presets sharing an input run sequentially after one import.
There are no nested worker pools. Lower `--jobs` on memory-constrained machines;
`--jobs 1` runs serially. Use `--suite fccee` or `--suite examples` for a subset.
Ordinary failures are collected without cancelling unrelated inputs. A native
worker crash breaks the pool and marks unfinished inputs failed rather than
hanging or reporting success. The command returns nonzero if any check fails. Per-suite summary paths remain unchanged.

## Inputs and updates

`catalog.toml` pins WebEOS URLs and SHA256 hashes copied from the input
directory's `files.sha256`. CI does not trust a changing remote checksum list.
To update an input, review the new file and update its path and hash here.
The download cache uses the content hash and verifies cache hits. Partial or
incorrect downloads fail the job.

The suite writes `build/example-results/summary.json`, per-phase logs, generated
configs, and portable `.nhproj` archives. FCC-ee results are under
`build/fccee-results`. Only selected semantic geometry is packaged; full
intermediates are not uploaded as projects. Browsers tessellate those selections
using the matching WASM build.

## Run locally

With a native build containing all four importers, Python bindings, and the
Key4hep environment (use the same Python interpreter as the build):

```bash
python3 -m pytest ci -q
PYTHONPATH="$PWD/build/Release/python${PYTHONPATH:+:$PYTHONPATH}" python3 ci/detectors.py --jobs 4 --nodehammer build/Release/nodehammer
```

Then assemble the site with an installed WASM runtime:

```bash
uv venv --python 3.12 build/gallery-venv
uv pip install --python build/gallery-venv -r ci/gallery-requirements.txt
source build/gallery-venv/bin/activate
scripts/build_pages_site.sh stage/share/nodehammer/web build/gallery-site
python scripts/build_example_gallery.py \
  --runtime stage/share/nodehammer/web --site build/gallery-site \
  --results build/example-results build/fccee-results
scripts/check_pages_site.py build/gallery-site
```

CI checks native geometry processing, project packaging, and the static site
layout. Browser rendering is not automatically tested; inspect the uploaded
`pages-site` artifact or the deployed site for visual validation.
