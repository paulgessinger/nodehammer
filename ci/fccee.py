#!/usr/bin/env python3
"""Full DD4hep -> NHS9 conversion and bounded rendering of representative FCC-ee models."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import subprocess
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


def check_render(render, expected_nodes):
    if len(render["nodes"]) != expected_nodes:
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


def run_case(exe, source, output, min_nodes=1):
    output.mkdir(parents=True, exist_ok=True)
    timings = {}

    def run(phase, *args, document=False):
        start = time.monotonic()
        with (output / f"{phase}.log").open('w') as log:
            result = subprocess.run([str(exe), *map(str, args)], stdout=subprocess.PIPE,
                                    stderr=log, text=True, timeout=1200)
        timings[phase] = time.monotonic() - start
        (output / f"{phase}.stdout").write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f"{phase} exited {result.returncode}; see {output / (phase + '.log')}")
        if document:
            value = json.loads(result.stdout)
            (output / f"{phase}.json").write_text(json.dumps(value, indent=2) + '\n')
            return value
        return None

    nhb = output / 'geometry.nhb.zst'
    nhb.unlink(missing_ok=True)
    flags = ['--dd4hep-global'] if source.suffix == '.xml' else []
    run('convert', 'convert', '-i', source, *flags, '-o', nhb)
    if not nhb.is_file() or nhb.stat().st_size == 0:
        raise ValueError("conversion did not write geometry")
    summary = run('summary', 'inspect', '--output-format', 'json', 'summary', '-i', nhb,
                  document=True)
    if summary['nodes'] < min_nodes or summary['diagnostics']['errors']:
        raise ValueError("full geometry is empty, unexpectedly reduced, or invalid")
    tree = run('tree', 'inspect', '--output-format', 'json', 'tree', '-i', nhb,
               '--depth', 2, document=True)
    config = output / 'smoke.toml'
    config.write_text(smoke_config(tree))
    run('config', 'config', 'validate', '-c', config)
    render_file = output / 'render.json'
    render_file.unlink(missing_ok=True)
    run('tessellate', 'convert', '-i', nhb, '-c', config,
        '--output-format', 'render-json', '-o', render_file)
    counts = check_render(json.loads(render_file.read_text()), len(tree['nodes']))
    return {"source": str(source), "full_nodes": summary['nodes'],
            "nhb_bytes": nhb.stat().st_size, **counts, "seconds": timings}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nodehammer', type=Path, default=Path('build/Release/nodehammer'))
    parser.add_argument('--geometry-root', type=Path)
    parser.add_argument('--output', type=Path, default=Path('build/fccee-results'))
    parser.add_argument('--model', choices=MODELS, action='append')
    args = parser.parse_args()
    root = args.geometry_root or geometry_root()
    report = {}
    def run_model(model):
        family, minimum = MODELS[model]
        source = root / 'FCCee' / family / 'compact' / model / f'{model}.xml'
        if not source.is_file():
            raise FileNotFoundError(source)
        return run_case(args.nodehammer.resolve(), source, args.output / model, minimum)

    # Threads only orchestrate isolated CLI processes: ROOT/DD4hep imports
    # cannot share a process. Each model keeps its own sequential phases/logs.
    with ThreadPoolExecutor(max_workers=4) as pool:
        pending = {}
        for model in dict.fromkeys(args.model or MODELS):
            print(f'Starting {model}', flush=True)
            pending[pool.submit(run_model, model)] = model
        for future in as_completed(pending):
            model = pending[future]
            try:
                report[model] = {"status": "passed", **future.result()}
                print(f'Passed {model}', flush=True)
            except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.TimeoutExpired) as error:
                report[model] = {"status": "failed", "error": str(error)}
                print(f'::error::{model}: {error}', flush=True)
            finally:
                args.output.mkdir(parents=True, exist_ok=True)
                (args.output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    return int(any(row['status'] != 'passed' for row in report.values()))


if __name__ == '__main__':
    sys.exit(main())
