# Detector examples and CI coverage

The Pages dashboard is built with Zensical from the results of the Key4hep job.
Its viewer runtime comes from the separate WASM build in the same workflow.
Pull requests build and check the site and upload it as `pages-site`; only
successful `main` runs deploy through the existing GitHub Pages environment.

## Coverage

| Input | Check | Published view |
| --- | --- | --- |
| Key4hep k4geo DD4hep XML | Existing full FCC-ee imports and bounded tessellation | Depth-2 overviews |
| ATLAS Phase 2 SQLite | GeoModel import, detailed selection and tessellation | Detailed and simplified |
| ATLAS Phase 1 NHB | Read, select and package | Detailed and simplified |
| sPHENIX ROOT | TGeo import and bounded tessellation | Depth-2 overview |
| sPHENIX GDML | Geant4 import and bounded tessellation | Depth-2 overview |
| sPHENIX ROOT publication | Chromium/WebGL2 load, draw counters and canvas screenshot | Same project as above |

This deliberately avoids a matrix of every preset, importer entry point, and
graphics backend. The overview configs limit rendering to two placement levels;
they are useful integration examples, not polished detector visualizations.
ATLAS's detailed preset is the representative expensive check. The dashboard
labels the other presets as package-only; a successful package is not a claim
that its rendering was tested.

The Key4hep build enables DD4hep, TGeo, GeoModel and Geant4/GDML. It must provide
Geant4 with GDML support; a missing backend is a build failure, not a skipped
test. No ATLAS job is launched through CVMFS.

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
PYTHONPATH="$PWD/build/Release/python${PYTHONPATH:+:$PYTHONPATH}" python3 ci/fccee.py
python3 ci/examples.py --nodehammer build/Release/nodehammer
```

Then assemble the site with an installed WASM runtime:

```bash
python3 -m venv build/gallery-venv
source build/gallery-venv/bin/activate
pip install -r ci/gallery-requirements.txt
playwright install chromium
scripts/build_pages_site.sh stage/share/nodehammer/web build/gallery-site
python scripts/build_example_gallery.py \
  --runtime stage/share/nodehammer/web --site build/gallery-site \
  --results build/example-results build/fccee-results
scripts/check_pages_site.py build/gallery-site
python ci/browser_smoke.py --site build/gallery-site \
  --output build/gallery-site/browser-results
```

On Linux, `playwright install --with-deps chromium` also installs system
dependencies. The browser test uses software WebGL2 for runner portability;
it does not establish WebGPU performance or native/browser parity.
It saves screenshots, console output and render counts; CI includes them in
the site and uploads them separately, including on failure. Viewer readiness is exposed as
`Module.nodehammerStatus` after render submission, including terminal build or
upload errors.
