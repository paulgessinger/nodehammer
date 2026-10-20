# Zensical documentation site

`zensical.toml` owns the site settings and navigation. The build stages sources
in a temporary directory, then invokes Zensical without modifying this checkout:

- `index.md` is generated from the repository `README.md`, keeping one source of
  truth. Its logo is copied locally; other repository-relative links point to
  GitHub until those documents are migrated into the site.
- `catalog.md` is generated from `examples/catalog.toml`. When CI results are
  supplied, it includes the published viewers, validation results, and provenance.
- Add future hand-written pages under `docs/site/pages/` and register them in
  `zensical.toml`. `index.md` and `catalog.md` are reserved generated pages.
  Existing design notes elsewhere in `docs/` are not automatically published.

Build documentation without detector inputs or a WASM build:

```bash
uv run --no-project --with zensical==0.0.71 python scripts/build_example_gallery.py \
  --site build/docs-site
python3 -m http.server --directory build/docs-site 8000
```

The catalog in this mode lists available presets and inputs, without claiming
validation or linking to missing local viewer publications. Use a separate
output directory from the complete gallery.

For the complete CI site, first stage the viewer application and ODD publications
with `scripts/build_pages_site.sh`, then pass `--runtime` and `--results` to the
same builder. See `examples/README.md` for the full invocation. Zensical output
is overlaid onto the site so the generated documentation cannot delete viewer
runtime files. The existing `app/`, `odd/`, `odd-simple/`, and `viewers/` URLs
remain unchanged; the catalog lives at `catalog.html` and results at `results.json`.
