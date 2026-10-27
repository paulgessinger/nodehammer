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
| IDEA_o1_v03 | `/world/DRcalo/AssemblyTube_*` | Dual-readout calorimeter internals |
| IDEA_o2_v01 | `/world/DREndcapTubes/phiER_*/towerLog_*`, `/world/DRBarrelTubes/stave_volume_*/tower_*` | Tower air, capillaries and fibres |

IDEA o1 assembly proxies use a neutral finish. IDEA o2 detailed retains 6,480
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
