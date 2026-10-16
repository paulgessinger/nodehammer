#!/usr/bin/env python3
"""Read-only SQLite inventory and provenance; never decodes geometry itself."""
import argparse
import hashlib
import json
import sqlite3
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("database", type=Path)
parser.add_argument("--source-url")
parser.add_argument("--geometry-tag", help="Only supply a verified tag")
args = parser.parse_args()
path = args.database.resolve()
with path.open("rb") as stream:
    hasher = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        hasher.update(block)
    digest = hasher.hexdigest()
with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True) as db:
    names = [row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
    if "RootVolume" not in names or "GeoNodesTypes" not in names:
        raise SystemExit("Not a GeoModel database")
    counts = {}
    for name in names:
        quoted = '"' + name.replace('"', '""') + '"'
        counts[name] = db.execute(f"SELECT count(*) FROM {quoted}").fetchone()[0]
    report = {
        "file": str(path), "sha256": digest, "bytes": path.stat().st_size,
        "source_url": args.source_url, "geometry_tag": args.geometry_tag,
        "geometry_tag_status": "supplied" if args.geometry_tag else "unverified",
        "tables": counts,
        "opaque_shapes": db.execute("SELECT name, asciiData FROM Shapes_UnidentifiedShape").fetchall()
        if "Shapes_UnidentifiedShape" in names else [],
    }
print(json.dumps(report, indent=2))
