#!/usr/bin/env python3
"""Full DD4hep -> NHS9 conversion and bounded rendering of representative FCC-ee models."""
from contextlib import contextmanager
import ctypes
import json
import os
from pathlib import Path
import sys
import time

MODELS = {
    "CLD_o2_v08": ("CLD", 1_000),
    "ALLEGRO_o1_v01": ("ALLEGRO", 1_000),
    "IDEA_o1_v03": ("IDEA", 200_000_000),
    "IDEA_o2_v01": ("IDEA", 200_000_000),
}


def geometry_root():
    candidates = [os.environ.get("K4GEO", "")]
    candidates += os.environ.get("CMAKE_PREFIX_PATH", "").split(os.pathsep)
    for candidate in filter(None, candidates):
        for root in (Path(candidate), Path(candidate) / "share/k4geo"):
            if (root / "FCCee").is_dir():
                return root
    raise RuntimeError("k4geo compact files not found; set K4GEO or --geometry-root")


def smoke_config(tree):
    """Keep the first two placement levels; explicitly drop their descendant closures."""
    nodes = tree["nodes"]
    if not nodes or len(nodes) > 20_000 or tree.get("maxDepth") != 2:
        raise ValueError("expected a nonempty, bounded depth-2 tree")
    roots = [n for n in nodes if n["depth"] == 0]
    if len(roots) != 1:
        raise ValueError("expected exactly one tree root")
    for node in nodes:
        if any(c in node["path"] for c in '*?[]'):
            raise ValueError("smoke selection requires literal paths without glob metacharacters")
    # json.dumps produces TOML-compatible basic strings, including nested predicate quotes.
    quote = json.dumps
    lines = ["hoist_orphans = false", "deduplicate_shapes = true"]
    for path in sorted({n["path"] for n in nodes if n["depth"] == 2}):
        lines += ["[[selection_rules]]", "drop_if = " + quote('path ~= ' + quote(path + '/**'))]
    lines += [
        '[materials.smoke]', 'base_color = "#8EAD88"',
        '[[rules]]', 'material = "smoke"',
        '[[rules]]', 'match = ' + quote('path ~= ' + quote(roots[0]["path"])),
        '[rules.tessellation]', 'skip_geometry = true',
        '[defaults.tessellation]', 'max_segments_circle = 24', 'fallback = "fail"',
    ]
    return '\n'.join(lines) + '\n'


def check_render(render, expected_nodes=None):
    if expected_nodes is not None and len(render["nodes"]) != expected_nodes:
        raise ValueError("render node count differs from the selected depth-2 tree")
    meshes = {m["id"]: m for m in render["meshAssets"]}
    triangles = sum(m["indexCount"] // 3 for m in meshes.values())
    bindings = [b for n in render["nodes"] for b in n["meshBindings"]]
    if not triangles or not bindings or not meshes:
        raise ValueError("tessellation produced no bound triangle geometry")
    if any(b["meshId"] not in meshes for b in bindings):
        raise ValueError("render references a missing mesh")
    if not any(meshes[b["meshId"]]["vertexCount"] > 0 and
               meshes[b["meshId"]]["indexCount"] >= 3 for b in bindings):
        raise ValueError("no bound mesh contains vertices and triangles")
    return {"render_nodes": len(render["nodes"]), "meshes": len(meshes), "triangles": triangles}


@contextmanager
def capture_phase(output, phase, timings):
    """Capture Python and native plugin output in a dedicated worker process."""
    start = time.monotonic()
    sys.stdout.flush()
    sys.stderr.flush()
    ctypes.CDLL(None).fflush(None)
    saved = [os.dup(fd) for fd in (1, 2)]
    try:
        with (output / f'{phase}.stdout').open('w') as stdout, \
                (output / f'{phase}.log').open('w') as stderr:
            os.dup2(stdout.fileno(), 1)
            os.dup2(stderr.fileno(), 2)
            try:
                yield
            finally:
                sys.stdout.flush()
                sys.stderr.flush()
                ctypes.CDLL(None).fflush(None)
    finally:
        for fd, original in zip((1, 2), saved):
            os.dup2(original, fd)
            os.close(original)
        timings[phase] = time.monotonic() - start


def check_diagnostics(diags):
    for diagnostic in diags:
        print(f'{diagnostic.code}: {diagnostic.message}', file=sys.stderr)
    if diags.has_errors:
        raise ValueError('bindings returned error diagnostics')


def run_case(source, output, min_nodes=1, package=False):
    # Imported only in the fresh worker, never in the parent orchestrator.
    import nodehammer as nh

    output.mkdir(parents=True, exist_ok=True)
    timings = {}
    nhb = output / 'geometry.nhb.zst'
    nhb.unlink(missing_ok=True)
    with capture_phase(output, 'convert', timings):
        options = {'dd4hep.useGlobalDetector': True} if source.suffix == '.xml' else {}
        scene, diags = nh.read_semantic(source, importer_options=options)
        check_diagnostics(diags)
        if not scene.valid or scene.node_count < min_nodes:
            raise ValueError('full geometry is empty, unexpectedly reduced, or invalid')
        nh.write(scene, nhb)
    if not nhb.is_file() or nhb.stat().st_size == 0:
        raise ValueError('conversion did not write geometry')

    # Inspect the serialized scene too, preserving the NHB round-trip checks.
    # Tree/summary inspection has no scene API; use the in-process CLI binding.
    documents = {}
    for phase, extra in (('summary', []), ('tree', ['--depth', '2'])):
        with capture_phase(output, phase, timings):
            code = nh.cli.run(['inspect', '--output-format', 'json', phase,
                               '-i', str(nhb), *extra])
            if code:
                raise RuntimeError(f'{phase} exited {code}')
        document = json.loads((output / f'{phase}.stdout').read_text())
        (output / f'{phase}.json').write_text(json.dumps(document, indent=2) + '\n')
        documents[phase] = document
    summary, tree = documents['summary'], documents['tree']
    if summary['nodes'] != scene.node_count or summary['diagnostics']['errors']:
        raise ValueError('serialized geometry differs from the imported scene or is invalid')
    config = output / 'smoke.toml'
    config.write_text(smoke_config(tree))
    with capture_phase(output, 'config', timings):
        cfg, diags = nh.read_config(config)
        check_diagnostics(diags)
    render_file = output / 'render.json'
    render_file.unlink(missing_ok=True)
    with capture_phase(output, 'tessellate', timings):
        render, diags = nh.build(scene, cfg.scene)
        check_diagnostics(diags)
        nh.write(render, render_file, cfg.output, format='render-json')
    counts = check_render(json.loads(render_file.read_text()), len(tree['nodes']))
    if package:
        pack_project(nhb, config, output, timings)
    return {"source": str(source), "full_nodes": scene.node_count,
            "nhb_bytes": nhb.stat().st_size, **counts, "seconds": timings}


def pack_project(nhb, config, output, timings, max_selected=None):
    import nodehammer as nh

    # Publish only the bounded selection, never the full IDEA intermediate.
    selected = output / 'selected.nhb.zst'
    archive = output / 'scene.nhproj'
    for phase, arguments, target in (
        ('select', ['convert', '-i', str(nhb), '-c', str(config), '-o', str(selected)], selected),
        ('pack', ['project', 'pack', '-i', str(selected), '-c', str(config),
                  '-o', str(archive)], archive),
    ):
        target.unlink(missing_ok=True)
        with capture_phase(output, phase, timings):
            code = nh.cli.run(arguments)
            if code:
                raise RuntimeError(f'{phase} exited {code}')
        if not target.is_file() or not target.stat().st_size:
            raise ValueError(f'{phase} did not produce output')
        if phase == 'select' and max_selected is not None:
            with capture_phase(output, 'selected-summary', timings):
                scene, diags = nh.read_semantic(selected)
                check_diagnostics(diags)
                if not scene.valid or not 0 < scene.node_count <= max_selected:
                    raise ValueError(f'authored preset selected {scene.node_count} nodes; '
                                     f'expected 1..{max_selected}')


def pack_fixture(nhb, config, output):
    """Publish an authored preset without attributing smoke-test counts to it."""
    import nodehammer as nh

    output.mkdir(parents=True, exist_ok=True)
    timings = {}
    # Validate and flatten before packaging; the result is portable.
    flat = output / 'scene.toml'
    with capture_phase(output, 'config', timings):
        code = nh.cli.run(['config', 'flatten', '-c', str(config), '-o', str(flat)])
        if code:
            raise RuntimeError(f'config flatten exited {code}')
    pack_project(nhb, flat, output, timings, max_selected=200_000)
    return {'status': 'packaged', 'seconds': timings}
