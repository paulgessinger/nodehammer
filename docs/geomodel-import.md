# GeoModel SQLite import

Build with `NODEHAMMER_WITH_GEOMODEL=ON` and an installed GeoModelCore/GeoModelIO.
The current prototype requires GeoModel 6.29.0 or later and is validated against 6.29.0, with GCC 14/15 and C++20
for ingestion. It reuses the experiment's GeoModel libraries; it does not fetch
or build another GeoModel. GeoModelRead currently accompanies this optional backend
in both the full and ingestion builds. Athena integration and a public in-memory
adapter header are separate follow-up work.

```sh
nodehammer convert -i detector.db -o detector.nhb
nodehammer inspect summary -i detector.nhb
```

The format name is `geomodel`; extensions are `.db`, `.sqlite`, `.sqlite3`.
The loader checks the SQLite header and GeoModel tables, then delegates graph
reconstruction to `GeoModelIO::ReadGeoModel`. The shared internal conversion
entry point is `ir::importGeoModel(const GeoVPhysVol&, source)`.

## CI coverage

The Key4hep job also tests GeoModel. GeoModel is not supplied by that stack:
`ci/build-geomodel.sh` builds GeoModelCore and GeoModelIO from the pinned 6.29.0
commit with Key4hep's compiler and dependencies. The job enables all three
backends (TGeo, DD4hep and GeoModel) in the full build and in the separate
C++20 ingestion-library build, including installed and embedded consumers.

The GeoModel tests create small SQLite fixtures during the run. They cover
conversion, opaque-shape policy, NHB round-trips and deferred trap tessellation;
CI does not download an ATLAS detector snapshot or require Athena. The existing
FCC-ee conversion remains the TGeo/DD4hep integration test.

## Fidelity

- Owning NHB output contains no GeoModel pointers. Distinct physical volume
  definitions stay distinct even when their logical volume is shared; repeated
  placements share definitions. Shapes and materials are cached by source identity.
- `GeoVolumeCursor` resolves serial placements, names, identifiers, and composed
  transforms. Serial functions become their evaluated placements, rather than
  executable functions. No occurrence tree is expanded in memory.
- Geometry is a **nominal snapshot**: alignment deltas are not applied. Alignable
  placements are tagged. Dynamic conditions and alignment histories are not exported.
- Lengths are in mm, angles in radians, density in g/cm³. Material elemental names,
  symbols, Z, A in g/mol, and mass fractions are retained as namespaced volume tags;
  NHB's ordinary material catalog retains name and density.
- Box, Tube/Tubs, Cons, Trd, Para, Pcon, Pgon, and Torus remain analytical.
  Union, intersection, and subtraction retain analytical operands.
- Shifted shapes are normalized into a shape-local frame. Both boolean operands
  can be shifted. Volume placements include that frame and daughter transforms
  compensate for it. Non-identity original shape frames are recorded as
  `geomodel.shape_frame.column_major`, allowing recovery of the source frames.
- Trap and SimplePolygonBrep become double-precision triangles describing their
  planar boundaries. This preserves geometry, but not their original parametric
  representation. Concave polygon caps use ear clipping, with either input winding.
- GenericTrap remains parametric: its half-length and eight XY vertices are
  stored as 17 doubles. Import and NHB serialization do not generate a mesh.
- `GeoUnidentifiedShape` preserves its type, extension name, and ASCII payload in
  an `UnknownShape` description. Their volumes carry `geomodel.renderable=false`,
  including boolean shapes that depend on them. These objects are **not renderable**. Their source
  computed volume is not preserved in the NHB shape catalog.
- Unsupported other shapes report NH1302; virtual surfaces reject conversion.
  Published-node tables and other arbitrary auxiliary SQLite tables are not imported.

Opaque extension shapes report NH1301 at error severity by default, stopping the
normal CLI before output. To explicitly save a scene containing their opaque
representations, use:

```sh
nodehammer convert -i geometry_atlas.db -o atlas.nhb \
  --geomodel-allow-opaque-shapes
```

This downgrades only NH1301 to warnings. It neither creates meshes for these shapes
nor suppresses other unsupported-shape errors. `--strict` still rejects warnings.
The equivalent library option is `geomodel.allowOpaqueShapes=true`.

## Twisted generic traps

Import is independent of rendering quality:

```sh
nodehammer convert -i lar-standard.db -o lar-parametric.nhb.zst
nodehammer convert -i lar-parametric.nhb.zst -o lar.glb -c render.toml
```

Set rendering tolerance in `render.toml`:

```toml
[defaults.tessellation]
generic_trap_tolerance = 0.01 # mm; default
```

This replaces `max_segments_circle` as the resolution control for generic traps;
that setting continues to control circular primitives. The new setting is available
per rule in `[rules.tessellation]` and in Lua
configuration. It must be finite and positive. In C++ use
`TessellationParams::genericTrapTolerance`. Mesh-cache keys include the tolerance,
including boolean operands and merged descendants. Changing it affects generated
render meshes, never the imported scene or NHB. Both SQLite and in-memory import
preserve exactly the parameters exposed by GeoModel. The earlier importer option
`geomodel.tessellationTolerance` / `--geomodel-tessellation-tolerance` is removed.

This importer reuses the existing NHS9 GenericTrap representation and its required
feature bit. Existing NHS9 files remain readable; parametric files require a
reader supporting that representation. Legacy NHS8 export explicitly rejects this
shape rather than dropping parameters or tessellating it. JSON uses `generic_trap`.

For a side connecting lower vertices A,B to upper vertices C,D respectively,
let `delta = (D-B)-(C-A)`. With n equal strips along z, the two triangles per
strip deviate from the bilinear surface by at most `length(delta)/(4*n)`.
The implementation chooses a common strip count for all four faces from the
largest bound. Corresponding surface/mesh points provide this boundary-distance
bound in exact arithmetic; ordinary double rounding and subsequent rendering,
float conversion, or boolean operations are outside that guarantee. This is
conservative: some planar tapered faces may be subdivided unnecessarily.

Cap coordinates and shared side vertices are retained consistently. Both input
windings and collapsed cap vertices are supported; zero-area triangles are
omitted. Nonfinite inputs, nonconvex/inverted sections, and internal pinches
are rejected. A 16,384-strip limit per shape fails explicitly rather than silently
relaxing the requested tolerance. This validation and mesh generation happen
during tessellation, not import.

## Fixtures and validation

GeoModel's public example is downloadable from
<https://geomodel.web.cern.ch/examples/geometry_atlas.db>. Its filename does not
identify a geometry tag: do not label it as a particular Run-3/Run-4 configuration
without independent provenance. Use Athena DumpGeo for a tag-controlled snapshot.

Record a read-only table inventory and SHA-256:

```sh
python3 tools/geomodel_inventory.py geometry_atlas.db \
  --source-url https://geomodel.web.cern.ch/examples/geometry_atlas.db > inventory.json
```

With tests enabled, `ingestion_geomodel` exercises nested rotations, source sharing,
serial placement/identifiers, shifted boolean operands, child-frame compensation,
concave polygon orientation/volume, nominal alignment, units, opaque diagnostics,
invalid input, and SQLite/memory/NHB equivalence. SQLite transform comparison allows 1e-12 absolute
rounding from GeoModel's writer; NHB read/write must be byte-exact. It generates its
own small DB. The full build also tests CLI default/opt-in/strict policies and clean
JSON stdout. Native CLI reader messages go to stderr; SDK calls retain upstream logging.

`nodehammer_geomodel_check input.db output.nhb` is a developer validation tool:
it writes even when the returned diagnostic list contains errors, printing them
explicitly, and checks byte-identical NHB read/write. Use the normal CLI for policy
checks. Large external fixture data is not committed.

## Prototype result (2026-10-16)

Public fixture SHA-256:
`1bb11d8a520ab34c38c6d0e0e4353fb289c2a29ca3d3e5a5626d1686b33917e7`.
The 20,992,000-byte SQLite file reconstructs 3,295,074 physical occurrences from
49,131 physical volume definitions, 46,686 converted shape definitions (shifts
are factored into frames), and 478 materials. The raw NHB round-trip is byte-exact.
The normal CLI additionally deduplicates definitions, retaining the occurrence
count and 16 opaque LAr EMEC extension shapes. Compressed output is about 1.08 MiB.

A beam-pipe selection exports to glTF with 229 render nodes, 107 meshes, and
54,016 triangles, with no tessellation errors. Its 16 warnings are default-grey
material styling, not missing geometry. This is a rendering pipeline smoke test;
full-detector interactive visual verification remains outstanding.

### Athena standard EMEC experiment

An additional LAr-only simulation-geometry dump with
`GeoModel.EMECStandard=True`, nominal alignment, and tag
`ATLAS-R3S-2021-03-00-03` succeeded using Athena 25.0.75 and the geometry
parameter database from CVMFS DBRelease 400.2.442. This prototype explicitly
sets `GeoModelSvc.IgnoreTagSupport=True`: the release rejects this older tag
without that override. It is not a validated production configuration.

The resulting SQLite file is 25,841,664 bytes, SHA-256
`ba3281e012eb2810c9a482017326429d0c7af3d903c3542f32185b6cc5f0ac4d`.
It has zero `Shapes_UnidentifiedShape` rows and 256 `Shapes_GenericTrap` rows.
All 256 generic traps have nonplanar side faces (corner-to-opposite-triangle-plane
distances exceed 1e-8 mm). Connecting their eight corners with twelve triangles
would therefore not preserve the original boundary. The tessellator now approximates these surfaces with an explicit tolerance.

The developer check reconstructs 790,962 occurrences,
1,058 volume definitions, 1,273 shapes, and 61 materials. All 256 traps remain
parametric in NHB. When tessellated at 0.01 mm, they produce 1,524,576 triangles in total. Each
real trap passed closed-edge/orientation and positive-volume checks. The NHB
round-trip is byte-identical. Normal CLI import succeeds in strict mode without approximation warnings.
The parametric NHB is about 2.82 MiB compressed (20,977,120 bytes raw before
deduplication), compared with 8.82 MiB compressed for the earlier eager-tessellation
prototype. The 256 trap definitions themselves carry only 34 KiB of doubles.

Automated tests cover sampled bilinear-surface error bounds, refinement,
closed topology, winding, volume, collapsed caps, invalid inputs and excessive
refinement, SQLite/in-memory equivalence (allowing writer rounding), and exact
NHB round-trips. CLI tests cover strict parametric import, compact NHB output, deferred rendering,
and invalid rendering tolerances. A synthetic twisted-trap fixture also exports successfully
to GLB (324 triangles at 0.01 mm). Full LAr visual inspection remains outstanding.

A larger dump enabling Bpipe, BCM, Pixel, SCT, TRT, LAr, Tile, and Muon failed
during initialization because the local conditions database could not open
`COOLOFL_INDET/OFLP200`. No full-detector standard-EMEC snapshot was produced.
The scripts, logs, manifest, inventory, and LAr fixture live in the workspace's
`geomodel-fixtures/emec-standard` directory, outside this repository.
