FCC-ee processing configs for k4geo 00-24
========================================

Each model directory contains simplified.toml and detailed.toml. These were
written from the actual imported hierarchy and materials of the installed
Key4hep 2026-04-08 / k4geo 00-24 geometries, including historical versions.
The inventory includes ALLEGRO, CLD (including FCCee-named predecessors), IDEA,
ILD_FCCee, and the two standalone MDI models. XML fragments and empty master
templates are not separate detectors. The duplicate FCCee_o1_v04 XML under
FCCee_o2_v01 is not counted twice.

The configs are versioned here; geometry, reports and generated outputs below
are local artifacts, not supplied by this checkout.

Geometry files: analysis/fccee/geometry/<model>.nhb.zst
Source paths, checksums, exceptions: analysis/fccee/manifest.json
Comparison of retained node counts: analysis/fccee/summary.csv
Mesh-build reports: analysis/fccee/validation/<model>.<mode>.json
Compressed render scenes: analysis/fccee/renders/<model>.<mode>.nhr.zst
Deferred large-geometry work: fixtures/configs/fccee/FOLLOW_UP.txt

Simplified
----------
Retains sensitive silicon tracker placements, physical beam-line parts, and
observed envelope solids for calorimeters, gas trackers, magnets and muon
systems. Internal sampling layers, tracker supports/services and fine wire/
fiber structure are omitted from this display view. Containers remain useful
selection headings. Hoisting preserves world placement. Curved shapes use
24 segments per full circle; Boolean failures are reported, not replaced by
bounding boxes. Colors distinguish subsystem roles. Envelopes are display
proxies, not material-budget approximations for simulation.

Detailed
--------
Preserves every placement in the supplied NHB and all physical material layers.
No descendant merging, coincident-face removal or material-stack averaging.
Only non-sensitive air/vacuum, beam-vacuum volumes and synthetic assembly
boxes are hidden; their descendants remain. Sensitive gases are translucent.
Curved shapes use 64 segments per full circle. Unsupported Boolean geometry
fails explicitly rather than silently disappearing. Material colors are
chosen for display; detector dimensions and material assignments in the NHB
are not changed by these rendering colors.

IMPORTANT: IDEA_o1_v03 and IDEA_o2_v01 intentionally omit the very large
fiber/capillary calorimeter internals, as requested. Their detailed configs
show all REMAINING detector parts plus explicitly tagged calorimeter envelope
proxies. These two NHBs must not be described as complete full-detail models.
See their *.omissions.json reports for exact boundaries and counts.

Use (from the repository root)
------------------------------
source /cvmfs/sw.hsf.org/key4hep/setup.sh -r 2026-04-08
nh=build/key4hep/build/Release/nodehammer

$nh config validate -c fixtures/configs/fccee/CLD_o2_v08/simplified.toml
$nh convert -i analysis/fccee/geometry/CLD_o2_v08.nhb.zst \
  -c fixtures/configs/fccee/CLD_o2_v08/simplified.toml -o CLD-simple.glb
$nh convert -i analysis/fccee/geometry/CLD_o2_v08.nhb.zst \
  -c fixtures/configs/fccee/CLD_o2_v08/detailed.toml -o CLD-detailed.glb

Replace the model name as needed. glTF/GLB and OBJ exports use metres
(unit_scale = 0.01 for this build's ROOT/DD4hep centimetre coordinates).
NHB/NHR retain native geometry units.

Validation
----------
The original authoring exercise performed the geometry checks described below.
Restoring these configs validates and flattens each config again; it does not
repeat full geometry conversion or establish viewer acceptance.

Configs are syntax/semantics checked, flattened, and checked again. The
flattened configs are then applied to the real NHB, tessellated and written
as compressed NHR. Machine-readable reports record diagnostics and actual
node/mesh counts. Detailed selection must retain the full input node count.
The validation gate rejects warnings, errors and empty meshes. Counts of
unique triangles do not count every placement of an instanced mesh.
A representative simplified CLD GLB export is also checked. No GPU screenshot
review was performed because the session has no display server.

Import compatibility
--------------------
ALLEGRO readout plugins use DD4hep's global detector instance; the affected
models were loaded through that instance and nodehammer's existing import API.
The original XML definitions were preserved.
FCCee_o2_v03 required resolving stale DetCommon/DetFCCeeCommon include paths
against the installed FCCDetectors 0.1pre11 package. It, FCCee_o2_v04 and
IDEA_o1_v01 also required the standard DD4hep detector-type definitions.
Local XML overlays and repair reports are retained under analysis/fccee;
installed source files were not modified.
CLD_o3_v01 exposed a nodehammer bug that discarded quadrilateral facets.
The importer fix is now on main, with a closed-solid regression test, and that
model was re-imported and its detailed mesh conversion checked successfully.
