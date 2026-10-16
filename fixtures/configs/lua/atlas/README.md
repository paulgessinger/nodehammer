# ATLAS nominal / Phase 2 scenes

Four entry configs are grouped by detector phase:

| Input snapshot | Detailed config | Overview config |
| --- | --- | --- |
| `atlas-nominal-standard.nhb.zst` (Phase 1) | `phase1/detailed.lua` | `phase1/simple.lua` |
| `atlas-phase2-05-00-00-full.nhb.zst` | `phase2/detailed.lua` | `phase2/simple.lua` |

Authored against the files in
`/Users/pagessin/cernbox_www/nodehammer/inputs/atlas/v1` on 2026-10-16.
The Phase 1 label follows the supplied nominal snapshot; the filename alone
is not an independently verified ATLAS geometry tag. The earlier Downloads
`atlas.nhb.zst` was LAr-only and is not the input for these full-detector configs.

## Shared definitions

`shared/scene.lua` contains the common selection and tessellation logic, parameterized
by phase and detail level. `shared/materials.lua` defines the detailed material
palette once for both phases. Each phase directory has a `shared -> ../shared`
symlink because Lua imports must remain under the entry directory. Project packing
reads those dependencies into the archive as ordinary files; the published viewer
does not need filesystem symlink support. Preserve the links in local checkouts.

## Detail policy

`shared/scene.lua` owns ordered selection, shared palette and tessellation settings.
Both phases include beam pipe, LAr barrel/endcaps, Tile and muon systems.
Phase 1 adds Pixel, SCT and TRT; Phase 2 adds ITk Pixel/Strip and HGTD.
InDetServMat is excluded from the Phase 1 detailed scene to omit the separate
bulk service-material model; most services within retained subsystems are also omitted.

Detailed scenes are tuned for interactive viewing: individual silicon solids
in Pixel/SCT or ITk/HGTD, real cryostat shells, the beam pipe and passive muon
assemblies (including both toroids). Silicon electronics may also match the
silicon-material selection. Calorimeter envelopes/modules and muon-station
envelopes replace their repeated internals. Sampling foils, chamber wires and
most services are omitted. Circle resolution is 24 by default, 128 for tracker
structures/silicon and the beam pipe, 256 for ring/disk/half-shell backing
structures, and eight for TRT straws; generic-trap tolerance is
0.1 mm. Geometry remains instanced and individually pickable; no global merge
is applied.

Phase 1 detailed keeps all 298,304 TRT `Straw` / `StrawM` outer cylinders with
eight segments each, sharing two base meshes. Straw gas and wires are omitted.
Its restored tracker structures are Pixel half-shells, stave/assembly rings
and frame cones; SCT cylinders, disk supports and frames; and TRT inner/outer
supports. The initial subset added 105 structural placements; the expanded
backing selection adds another 8,442: Pixel disk sectors, Omega stave pieces,
face plates and end blocks; SCT baseboards, endcap spines/sub-spines and disk
fixations. `ModuleShell*` mother solids
are excluded: they overlap the explicitly rendered straw interiors.
Phase 2 detailed retains 3,420 primary structural placements: ITk Pixel barrel/stave and
endcap-ring supports, half-shells and carbon/face-sheet parts; ITk Strip stave
face sheets, channels, hats, petal face sheets and flanges; HGTD
support plates. A further 4,672 placements restore ITk Strip stave mounts and
stave/petal closeouts. These are explicit logical-volume allowlists scoped to each
subsystem, not entire service subtrees. Supports use source-material finishes.

Overview scenes use existing solids as **visual envelope proxies**, not
material-budget equivalents: Pixel/SCT layers and disks, TRT assemblies,
LAr envelopes and FCAL absorber bodies, Tile modules and muon stations.
Phase 2 keeps silicon volumes (including any silicon electronics selected by
material) to preserve the ITk's inclined modules and endcap layout, plus HGTD
layer envelopes. The remaining sampling and service descendants are dropped,
except for the passive muon assemblies retained to show the toroids.
Beam-pipe material stays in both overviews. Curves use 48 segments.

Simplified scenes retain subsystem colors and bronze passive muon assemblies.
Detailed scenes use `shared/materials.lua`, based on the PBR palette in
`fixtures/configs/odd/materials.toml`, softened using the supplied installation
photos: dark satin silicon, matte carbon
composites, rough silver aluminum/steel/titanium/beryllium, aged copper/brass, matte
insulation and polymers, and amber polyimide TRT straws. FCAL absorber finishes
follow the imported composition tags: copper-rich Module 1 and tungsten-rich
Modules 2/3. Unknown mixtures retain a neutral matte fallback.

These are physics-plausible **visual approximations**, not measured surface
finishes or optical constants. Homogenized composites use representative
appearances. Silicon uses a softened metallic-roughness approximation; the model
is not a semiconductor optical simulation. Straws are opaque because their
selected cylinders omit their interiors. Retained LAr/muon/Tile envelopes use
matte brown chamber-panel and grey coated-housing finishes inspired by the
installation photos; they do not depict literal liquid/gas
surfaces. No additional geometry, transparency, or selection changes are
introduced by the material palette. Extended PBR properties are carried by
exports; the native viewer's shading supports a subset of these properties.

Complete passive muon assemblies remain selected, including toroid coils and
supports. Detailed toroids follow their actual source-material assignments
instead of a uniform bronze tint. Air/ether/vacuum envelope solids are hidden.
The importer supports `volume` predicates for selecting cryostat shells
without relying on anonymous placement names.

GeoModel source lengths are millimetres (confirmed by imported unit tags).
GLB/glTF and OBJ exports are scaled to metres. Viewer camera distances remain
in source millimetres. Unsupported solids use `fallback = "skip"`; review
conversion diagnostics when applying these configs to another snapshot.
All four tested conversions reported zero warnings and errors.

## Reproduce

Run from the repository root with the existing local build:

```sh
build/RelWithDebInfo/nodehammer convert \
  -i /Users/pagessin/cernbox_www/nodehammer/inputs/atlas/v1/atlas-phase2-05-00-00-full.nhb.zst \
  -c fixtures/configs/lua/atlas/phase2/simple.lua \
  -o build/atlas-authoring/phase2-simple.glb
```

Use the matching Phase 1 input/config or replace `simple` with `detailed`.
Keep the `atlas/` directory (both `scene.lua` and `shared/materials.lua`) beside the
four entry files when copying them.
The native viewer accepts these Lua entry files directly, including when the
geometry lives outside the working directory. Flattening is optional when you
want a self-contained config for sharing.

```sh
build/RelWithDebInfo/nodehammer viewer shot \
  -i /Users/pagessin/cernbox_www/nodehammer/inputs/atlas/v1/atlas-phase2-05-00-00-full.nhb.zst \
  -c fixtures/configs/lua/atlas/phase2/simple.lua \
  -o build/atlas-authoring/phase2-simple.png \
  --angle-cut --cut-start 0 --cut-end 90 \
  --camera-target-x 0 --camera-target-y 0 --camera-target-z 0 \
  --camera-distance 65000 --camera-yaw 35 --camera-pitch 20
```

## Verification

Config validation, flattening/validation of flattened TOML, semantic selection,
and full GLB conversion were exercised for all four configurations. Selected
overview trees contain every intended subsystem; Phase 1 includes all four SCT
barrel layers and eighteen endcap wheels. Full inputs have 5,136,290 and
3,652,449 occurrences respectively.

| Scene | Selected nodes | Unique exported meshes | GLB size (MiB) |
| --- | ---: | ---: | ---: |
| Phase 1 overview | 20,986 | 864 | 12.09 |
| Phase 1 detailed | 338,422 | 955 | 112.11 |
| Phase 2 overview | 49,126 | 867 | 22.13 |
| Phase 2 detailed | 82,521 | 976 | 41.16 |

The previous detailed selections contained 5,135,677 / 3,652,449 nodes and
produced 927.94 / 688.16 MiB GLBs. The new selections remove about 93.4% /
97.7% of those nodes. Generated artifacts remain in `build/atlas-authoring/`.

Both revised detailed scenes were converted and visually checked with a
90-degree viewer cutaway. Conversion and viewer tessellation reported no
warnings or errors. Tracker-only cutaways also verify the restored mechanics
and TRT straws. Previews are `phase1-supports-full.png`,
`phase2-supports-full.png` and the corresponding `*-tracker.png` files.

The viewer now packs mesh assets into shared GPU buffer batches and checks
allocation failures. A larger uniform buffer and a draw-group capacity check
also address per-frame shader-data exhaustion in large scenes. These require
a rebuilt native viewer; browser deployments require rebuilding WASM too.
The original Phase 1 full-detail selection was also rendered successfully
after both GPU fixes (12,357 base meshes / 32,862 cut meshes using 10 / 32
geometry buffers), independently of the selection reduction.

The primitive tessellator closes radial faces of partial polycones, reverses
descending-Z profiles, and triangulates whole stepped profiles to avoid
T-junctions. Regressions cover the toroid tubes and the LAr cryostat cooling /
EMEC stretcher shapes previously responsible for NH0506/NH0501 warnings.
The CLI's separate `convert --wedge-cut` path previously threw
`shared geometry requires affine transforms` on these full inputs; this
verification uses the viewer cutaway path.


Material-palette verification: both detailed GLBs retain their previous node,
mesh and triangle counts, and export without warnings/errors. Full and tracker
cutaway previews use PBR/IBL: `phase1-materials-full.png`,
`phase2-materials-full.png` and the corresponding `*-tracker.png` files.

TRT overlap / faceting check: removed the 96 `ModuleShell*` placements while
retaining every straw and the separate inner/outer supports. Curved tracker
parts and beam pipe use 128 phi segments to reduce visible faceting and
chord-induced support intersections. Both detailed exports remain warning-free;
tracker cutaways and an inner-ring close-up were checked. Diagnostic previews
`trt-smooth-contrast.png` and `rings-smooth-close.png` use subsystem colors only
to expose intersections; the shipped detailed material palette is unchanged.

The photo-inspired finish pass raises metal roughness to roughly 0.55–0.8,
removes carbon/polyimide clearcoat and softens the silicon response. Bare metals
retain metallic shading; the assembly-envelope panel/housing proxies use
predominantly dielectric finishes. Geometry, lighting and the simplified
palette are unchanged. Both detailed exports remain warning-free and retain
the preceding geometry counts. Same-camera/lighting previews are
`phase1-satin.png` and `phase2-satin.png`.

Expanded backing detail: both phases were re-exported with zero warnings/errors
and checked in tracker cutaways and close-ups (`phase*-backings-tracker.png`,
`phase*-backings-close.png`). Ring/disk resolution is 256; discrete sensor
modules and genuine polygonal parts retain their physical outlines. Increasing
phi resolution cannot remove the steps between separate modules.

### ITk support overlap correction

Detailed Phase 2 omits `StaveCore` and `PetalCore` bulk mother solids while
retaining their explicit face sheets, channels and closeouts. In the source
snapshot, the stave core and far-end closeout share faces at local z = ±2.598 mm.
The petal core encloses its face sheets and shares their radial boundaries
(380–974.95 mm). Rendering both complete mother and daughter solids causes
occlusion and coincident faces; simulation daughter material replacement does
not subtract these regions from the displayed mother mesh. This selection
correction avoids those overlaps without shifting physical placements.

### New Small Wheel chamber proxies

Both detail levels retain the sTGC honeycomb and Micromegas FR4 assembly
mothers as chamber proxies, omitting the sTGC internal layers and the
`MM_Sensitive`/`MM_Frame` daughters. Source inspection found identical polygon
outlines on sTGC mothers, gas layers and G10 boards; displaying them together
creates coincident edge faces. Detailed proxies use the same matte chamber-panel
finish as other muon envelopes. External supports and toroids remain selected.
