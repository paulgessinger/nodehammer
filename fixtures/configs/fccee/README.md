# Authored FCC-ee display presets

The four representative gallery models were originally authored against
Key4hep 2026-04-08 / k4geo 00-24. The original analysis lives at
`acts-dev-rd-et:/home/pagessin/dev/nodehammer/analysis/fccee/`.

Each model publishes `simplified.toml` and `detailed.toml` from the same full
NHB import. Simplified uses a matte conceptual palette: gold vertex sensors,
teal tracking/drift chambers, red ECAL, purple HCAL, blue muons and gray magnets
and beam-line parts. It retains silicon sensors and observed envelope proxies.
IDEA o2 crystal endcaps share the ECAL color with the barrel and timing layer.

Detailed retains supports and material layers, using source-material mappings
from `../physical-materials.toml`. Metals have distinct finishes; silicon is
dark, carbon matte, polymers and foams dielectric. ALLEGRO maps its liquid argon,
lead, steel and glue separately. IDEA maps silver-coated field wires,
gold-coated sense wires, and the o2 LYSO/PbWO crystals. Unmapped materials have
a neutral dielectric fallback. These are approximate visible appearances,
not measured optical properties or simulation material-budget models.

## IDEA calorimeter reduction

Detailed configs explicitly omit descendants below the retained display
boundaries, so they can also be applied to full CI imports:

| Model | Retained proxy boundaries | Omitted detail |
| --- | --- | --- |
| IDEA_o1_v03 | `/world/DRcalo/AssemblyTube_*/tower_*`, `/world/DRcalo/AssemblyTube_*/sipmWafer_*` | Tower fibres and readout internals |
| IDEA_o2_v01 | `/world/DREndcapTubes/phiER_*/towerLog_*`, `/world/DRBarrelTubes/stave_volume_*/tower_*` | Tower air, capillaries and fibres |

IDEA o1 detailed retains 21,600 copper tower proxies and 21,600 silicon readout
wafers, using their source-material mappings. Its two assembly envelopes stay
hidden. IDEA o2 detailed retains 6,480
barrel brass towers and 5,040 positioned endcap tower containers, with a brass
finish representing their omitted contents. These are bulk display proxies;
the air and fibre channels are not represented. The larger stave and `phiER_*`
assembly boxes remain hidden because they span empty space and overlap adjacent
sections. Tower boxes can still approximate boundaries imperfectly.
IDEA o2 simplified continues to hide the endcap boxes without adding towers.

Other subsystems retain their material layers; IDEA o2 crystal sectors use
fitted hulls as described below. Selection drops tower descendants before
tessellation and packaging, so the browser never receives their capillaries or
fibres. Previously reduced inputs cannot recover the towers; regenerate from
the full CI import when switching to this preset.
No external `tag.omitted_calorimeter_internals` annotation is required. These are detailed
views with a calorimeter approximation, not full fiber-level scenes.

## CI and downloads

CI still imports complete geometries and tessellates a bounded depth-2 smoke
scene. It then selects and packages both authored presets; only the authored
presets appear in the gallery. They are labelled **package only**, and smoke
counts are recorded separately rather than attributed to their rendering.
Packaging rejects empty selections and more than 200,000 simplified or
5,000,000 detailed placements before constructing an archive.

Keep `physical-materials.toml` in the parent directory when downloading a
detailed TOML; IDEA also requires `idea-drift.toml` there. Project archives contain a flattened, self-contained config.
Historical configs for other model versions remain outside this gallery scope.

## Input provenance and baseline verification

Before adding the detailed IDEA rendering optimizations below, all eight presets were validated, selected, tessellated, packaged and inspected
using the full `.nhb.zst` inputs and matching macOS runtime from successful
main CI run `39827315248` (commit `5cfa3c892e330b5def998bd26ab5794eb7e2a6f1`).
Those baseline conversions reported zero warnings and errors. Baseline placement counts (before crystal hull replacement):

| Model | Full input | Simplified | Detailed |
| --- | ---: | ---: | ---: |
| CLD_o2_v08 | 2,034,249 | 97,690 | 2,034,249 |
| ALLEGRO_o1_v01 | 94,998 | 528 | 94,998 |
| IDEA_o1_v03 | 260,245,099 | 16,242 | 598,411 |
| IDEA_o2_v01 | 265,922,033 | 16,390 | 3,006,113 |

This is local verification evidence; CI continues to distinguish its bounded
smoke tessellation from authored-preset packaging.

## IDEA detailed rendering

Both IDEA detailed presets include `idea-drift.toml` alongside
`physical-materials.toml`. Internal gas-cell surfaces are suppressed, while the
wire instances remain. The gas envelope is hidden to expose the chamber. Foam
daughters are subtracted from carbon vessel mothers before viewing cuts, then
rendered with their original material-based appearance.

Wire cylinders use the opt-in centerline cut approximation with 12 sides.
Viewer LOD uses a 1.5-pixel thickness threshold and stepped sampling at
100/200/400 mm: 100%, 30%, 10%, then a stable 0.5% distant hint. Hints have a
half-pixel minimum display width. The distance factors, fractions, and hint
width are editable in the shared config. These are display approximations,
not material-budget or wire-density visualizations; exports do not apply LOD.
IDEA o2 also replaces dense crystal-cell sectors with fitted convex hulls.
Simplified presets retain the conceptual subsystem palette and omit wires.

The IDEA presets before the tower update were selected from those original CI imports under a
30-second per-process timeout: o1 completed in 2.6 s (598,411 placements), o2 in
20.6 s (592,289 placements). Both selected scenes were packaged successfully.
Headless tessellation with and without the viewing wedge completed in 3.7–4.7 s
for o1 and 14.6–16.2 s for o2 using the CI-derived selected inputs. These are
local wall times, not browser frame-rate or CI performance guarantees.

With both barrel and endcap towers enabled, IDEA o2 selection from the original
CI import completed in 27.9 s (603,809 placements). Rebuilding that selected NHB
with the viewing wedge took 6.9 s; packaging and visual inspection passed.
Inspection confirmed 6,480 barrel and 5,040 endcap towers, all leaves. Geometry
checks used the same 30-second per-process limit.

The gallery packages semantic NHB with the flattened preset, so the viewer
retains wire LOD metadata when rebuilding. NHR also preserves that
metadata, including when transferring scenes from the wasm compute worker.
Geometry optimizations baked into NHB are idempotent; use original input when
changing the baked geometry policy.

## Detailed preset maintenance (2026-10-29)

IDEA o1 now follows o2's positioned-tower policy, with its own copper/silicon
materials rather than o2's brass. It selects 641,611 placements from the full
260,245,099-placement CI input. Both versions continue to share drift-wire LOD
and vessel subtraction through `idea-drift.toml`. The crystal hull rules remain
o2-specific: o1 does not contain that crystal calorimeter.

CLD merges the physical components of each repeated inner/outer tracker module
and removes their coincident interior faces. Sensor/support material layers
remain; picking now stops at the module. On the compact CI input this reduces
the render tree from 2,034,249 to 111,489 nodes and the uncompressed NHR from
476 MiB to about 37 MiB. The original preset exhausted the wasm worker's 2 GiB
heap during finalization; the revised preset completed using a 331 MiB heap.
A 90-degree cut also completes (397 MiB heap). These measurements use a
Node-hosted wasm compute build, not a browser frame-rate benchmark. Older expanded imports can still exhaust memory during
preparation; rebuild packages from the compact CI import.

ALLEGRO hides gas-only `CDCHlayer*` shells and the `LAr_bath` mother while
retaining the physical cryostat walls and liquid-argon sampling layers. Endcap
`layerEnvelope` solids are replaced by merged daughter surfaces. Repeated HCAL
sampling sequences and barrel ECAL passive plates also merge with
`drop_coincident_faces`; HCAL sequences use material-stack prefiltering for
subpixel bands. That initial pass removed 60,428 interior triangles and produced
33,176 render nodes. Dense bands and partially coincident interfaces can still
alias in cut views: this is not a general boolean union of the calorimeter.

Merging now honors descendant `skip_geometry` rules while still traversing
those nodes' daughters. Rebuild the runtime as well as repacking the presets to
pick up this fix. Native cut previews were checked for IDEA o1 and ALLEGRO;
all eight preset configs validate, and the tessellation suite passes (62 cases).

### ALLEGRO follow-up: interaction cost and the central cavity

The source CDCH contains 112 gas-only cylindrical layers (radii 34.4–168.8 cm,
length 570 cm), with no wire/cell daughters. Suppressing those shells exposes
the central cavity; this input cannot supply the missing wire detail. The
preset does not invent wire geometry or fill the cavity with a solid proxy.

The detailed preset also merges the twelve liquid-argon slices within each
barrel ECAL active cell. HCAL sampling cylinders use 32 circle segments instead
of 64; ECAL and cryostat resolution remain unchanged. Liquid argon uses a local
opaque, matte blue display material to avoid overlapping translucent sampling
surfaces. This is a display convention, not an optical model of liquid argon.

At the same native 90-degree-cut camera, submitted triangles fell from about
13.1 million to 7.7 million; this measures geometry workload, not frame rate.
The bright transparent appearance is reduced, but residual HCAL band aliasing
and partially coincident radial interfaces remain. Merging whole HCAL layers
was tested and rejected because it inflated the uncompressed render payload
to 362 MiB. The smaller repeated-unit merges preserve instancing.

### Cut-volume rule matching

Viewing cuts create logical-volume names ending in `_wedgecut`. ALLEGRO's
volume-based envelope suppression and merge rules use globs that also match
those generated names. Exact-name rules previously stopped matching after a
cut, re-exposing `LAr_bath` and `layerEnvelope` solids and disabling repeated
sampling-unit merges. In an isolated positive-endcap 90-degree cut, correcting
these matches reduced render nodes from 557 to 21 and removed 15,936 interior
triangles. The full cut scene now has 13,571 render nodes and removes 38,764
interior triangles.

Inspection of the positive endcap's 535 axial leaf-material intervals found no
volume overlap between those layers. Residual fine banding remains in isolated
renders after envelope removal, including the liquid-only view; it should not
be attributed entirely to overlapping envelopes. Material-stack averaging was
trialled on the endcap but did not resolve this, and was not added there.
