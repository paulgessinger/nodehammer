# nodehammer-web

The WebAssembly viewer runtime for [nodehammer](https://github.com/paulgessinger/nodehammer).

This package is a payload, not a program: it carries the Emscripten build of the
nodehammer viewer — the bundles, the worker script and the page that loads them —
and one function that says where they are.

```python
import nodehammer_web

nodehammer_web.runtime_dir()  # -> .../site-packages/nodehammer_web/runtime
```

You do not normally call that. Installing this package alongside `nodehammer` is
what makes

```console
$ nodehammer viewer serve
```

open the viewer in a browser: the command stages a servable directory from this
runtime, serves it on loopback and opens a tab. Without it, that command reports
that it could not find a runtime and lists where it looked.

It is a separate distribution because it is built by a different toolchain than
the platform wheel — no native build can produce WebAssembly — and because a
headless install has no use for 7 MB of it.

Versions move together with `nodehammer`, and the two are checked against each
other at load time rather than only by the resolver: the runtime carries a schema
id, and the library refuses one it was not built against.
