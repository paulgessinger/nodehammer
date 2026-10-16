# FCC-ee landing page: plan and prototype

A public page that shows what nodehammer is through a live 3D view of the
FCC-ee ring with a detector at each interaction point, where clicking a
detector opens the full nodehammer viewer on that geometry. This document
records the design decisions and the state of the prototype in
`examples/fcc-landing/`.

## Why not embed the viewer directly

The wasm viewer assumes it owns the page: it is built without `-sMODULARIZE`
and boots off a global `Module`, the canvas is hard-wired to `#canvas`, it
rewrites `window.location` for URL state, and sokol_app keeps one global app
state per module. Running several copies on one page would also mean one wasm
heap (64 MB initial, growing), one tessellation worker and one WebGL/WebGPU
context per copy. A grid of live viewers is therefore the wrong shape.

The design instead has two tiers:

1. **Preview tier (three.js, always live).** Each detector is a drastically
   simplified GLB produced by nodehammer itself, rendered by one three.js
   scene for the whole page. Cheap enough to animate continuously.
2. **Full tier (nodehammer, on click).** The real viewer runs in an `<iframe>`
   in embed mode (`docs/viewer-embed.md`) and is cross-faded in once it has
   drawn its first frame from the same camera as the preview.

## Preview GLB recipe

The preview is an ordinary `convert` run with a selection-heavy config and a
baked wedge cut, then packed with `gltfpack`:

```
nodehammer convert -i odd.nhb.zst -c fixtures/configs/odd_simple.toml \
    --wedge-cut 0 90 -o odd_simple_cut.glb
gltfpack -i odd_simple_cut.glb -o odd.glb -mi -cc
```

For the Open Data Detector this gives 1,029 nodes / 132 meshes / 16k
triangles, 1.46 MiB from nodehammer and **151 KB** after `gltfpack`
(quantised, meshopt-compressed, `EXT_mesh_gpu_instancing`). three.js loads it
as 53 meshes, 45 of them instanced, through `GLTFLoader` + `MeshoptDecoder`.

What does the work, in order of effect: the selection rules (silhouette
volumes only), `max_segments_circle` (10 in `odd_simple.toml`), then
`gltfpack`'s compression. nodehammer's glTF exporter already shares mesh
geometry between nodes, so file size is not the reason to add in-tree mesh
optimisation; `EXT_mesh_gpu_instancing` would be a cheap addition to the
exporter if draw-call count ever matters for a full-detail preview.

The `.glb`'s origin is the detector's interaction point and its beam axis is
+z (`unit_scale = 0.1`, baked), so the preview places the model untranslated
at the IP and rotates it so +z follows the ring tangent.

## Ring geometry and facts

Point positions are the WGS84 "theoretical beam interaction points" from the
FCC Feasibility Study Report Vol. 3 (Tables 2.2/2.3), projected onto a local
tangent plane in km around their centroid. The 8 points sit 14.2–14.4 km from
the centroid at 45° spacing, so the footprint is drawn as a closed Catmull-Rom
curve through them rather than as the rounded octagon of the schematics (a
2 km straight on a 14.4 km circle has a 36 m sagitta). Numbers on the page
(90.7 km, 4 IPs, 11,200 bunches per beam, 45.6 GeV per beam at the Z, 240 m
average depth, 30 mrad crossing, cavern and shaft sizes) are from FSR Vol. 2
and Vol. 3. The LHC (26.659 km, centre ≈ 46.27 N 6.066 E) and the CERN Meyrin
site are drawn for scale; the LHC centre is approximate.

Detectors and beams are enlarged roughly 100× relative to the ring; the page
says so. The four concepts (ALLEGRO, CLD, IDEA, ILD) are distributed over the
IPs for illustration only; the FSR does not assign them. The k4geo model ids
shown in the dossier (`ALLEGRO_o1_v01`, `CLD_o2_v08`, `IDEA_o2_v01`,
`ILD_FCCee_v02`) are the ones `ci/fccee.py` and the FCC-ee configs target.

## The click handoff

1. Click: the three.js camera flies to the detector. The host inserts an
   iframe over the canvas at `opacity: 0`, `pointer-events: none`, with
   `?embed=1`, the archive, and the camera converted into the viewer's
   convention (`docs/viewer-embed.md`: Y-up, `theta = yaw`,
   `phi = 90° − pitch`, unscaled world units, so divide the preview's
   `unit_scale`d coordinates back out).
2. The viewer boots silently: no ImGui, no toasts, no URL or storage writes.
3. On `{type: 'nh:ready'}` (first frame that drew the uploaded scene) the host
   fades the iframe in and the canvas out over ~400 ms, stops the preview
   loop and enables pointer events on the iframe. Both sides describe the
   same view, so only detail appears.
4. Back: `nh:getCamera` → set the three.js camera from the reply, fade the
   iframe out, remove it.

Keep the preview camera frozen during the fade, preload the `.wasm` and the
`.nhproj` on hover, and show a loading indicator if `nh:ready` has not arrived
after ~15 s. The viewer's background is its IBL dome through the tonemap
chain, not a flat colour, so the fade runs over the page's dark background
rather than trying to match colours.

## Prototype status (`examples/fcc-landing/`)

Done: the three.js page with the ring, both beams as 40 bunch trains each,
bloom, collision flashes cycling round the IPs, the ODD stand-in at all four
IPs, hover highlight, fly-in with a dossier, and an **Open in nodehammer**
link to the published ODD viewer. Serve the directory statically (the page
loads three.js 0.160 from jsDelivr and fetches `odd.glb` next to it).

Not done, in order:

- Wire the click to the embed-mode iframe cross-fade once a build with
  `?embed=1` is deployed somewhere the page can reach.
- Replace the stand-in with per-concept previews from the k4geo models via
  the recipe above (needs the Key4hep/LCG view for the compacts).
- Terrain and lake outline under the ring; the LHC as its own stand-in.
- Origin pinning for the postMessage bridge before public hosting.
