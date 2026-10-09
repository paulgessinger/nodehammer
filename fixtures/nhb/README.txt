Frozen legacy NHS8 compatibility corpus
=======================================
legacy-nhs8-mdi.nhb.zst is an existing FCC-ee MDI_o1_ShapeBased_v01 artifact,
written on 2026-10-07 before the shared-format migration, using the NHS8 writer.
It contains 99 stored nodes. SHA-256:
27d2ff3a1ba3fac5c080c4f7152c4c18033a9f179825d3b814513be05457afd0

Source: k4geo v00-24, FCCee/MDI/compact/MDI_o1_ShapeBased_v01 (see source_file
inside the fixture for the exact entry path). This binary is a regression input;
do not regenerate it with a newer writer to make a failing compatibility test pass.
Synthetic adapter tests separately cover stale prototype daughters, differing
occurrence topology, duplicate names, rotated transforms, preserved hoisted paths,
provenance, degradation flags, tags and rejection of malformed trees.
