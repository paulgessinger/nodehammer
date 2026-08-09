# The error model

How this project reports that something went wrong — one rule, three tiers, and
the consequences that follow from them. It governs the public API and the
internal library alike; the whole point is that there is no translation layer
between the two.

## The rule

> **A failure is fatal iff the call cannot deliver what its signature promises.**

Fatality is a property of *the call*, not of the badness of what happened. The
same broken TOML file is fatal to `Config::read`, which promised a config, and
is merely the answer for `Config::check`, which promised a report. Nothing else
needs to be adjudicated by taste: name the promise, and the channel follows.

## Three tiers

### Tier 1 — Diagnostics

In band. Non-fatal. **Always accompanied by the promised result.** A diagnostic
describes the *quality* of a result that exists; it never reports whether it
exists. A caller that ignores the whole list still holds something valid.

### Tier 2 — `Error`

Out of band, fatal, and **named**: an NH code, a message, a context, and the
diagnostics observed before the failure. The library could not deliver, for a
reason it understands and a caller can branch on. This is the only exception
type the library *raises*.

### Tier 3 — Escaping exceptions

`std::bad_alloc`, and anything else the environment raises. Fatal, unnamed,
**not wrapped**, not caught. Two reasons, the second decisive:

- No NH code would be true of them. `Error{NH0900, "out of memory"}` is a
  fiction, and a caller who catches it can do nothing with the code.
- **`Error`'s constructor allocates** — three `std::string`s. Converting
  `bad_alloc` into an `Error` is the one conversion guaranteed to fail exactly
  when it is needed.

So the promise is not "one exception type crosses this API" — which a
`catch (...)` can only pretend to keep — but:

> **Every failure this library can name is an `Error`. Anything else propagates
> unchanged.**

That one is kept by construction.

## The severity ladder

| severity  | means                                                        |
| --------- | ------------------------------------------------------------ |
| `Debug`   | trace; no action, ever                                        |
| `Info`    | something happened worth recording; the result is exactly what was asked for |
| `Warning` | the result is complete, but something was assumed or substituted |
| `Error`   | **the result exists and part of it is missing or wrong**      |
| `Fatal`   | **never appears in a returned list**                          |

`Error` severity means *partial result* and nothing else. It is the reason a
level above `Warning` exists at all: "this node has no mesh" is categorically
different from "this node got the default material", and a caller needs to tell
them apart without parsing messages.

`Fatal` is reserved: it appears only on a `Diagnostic` produced by
`Error::diagnostic()`, for a caller funnelling both channels into one report.
Hence a one-line invariant — *no `DiagnosticList` this library returns ever
contains `Fatal`* — that a test can check.

## Codes

A code names *what went wrong*. It does not name a channel, because the channel
belongs to the **call**:

- `kFatal…` — failures **no** call can observe non-fatally. Always thrown, never
  in a list. A caller matching one against a diagnostic is writing dead code.
- `kErr…` / `kWarn…` / `kInfo…` / `kDebug…` — reported at that severity. A
  `kErr…` code may *also* be thrown, by a call that promised the very result the
  error makes partial — this is the config family, where `check` reports what
  `read` throws.

`diagnostic_codes.hpp` is grouped by that distinction, so the table is the
header rather than a document that drifts from it.

## The decision is made at the point of failure

Not at a boundary. The importer that cannot open a file throws; it does not
report an error diagnostic beside an empty scene for someone else to
re-interpret. Internal code throws `nodehammer::Error`, the same type the public
API documents — the same move already made for `Diagnostic`, which is one type
across both layers rather than two identical structs and a converter.

Consequences:

- No `adopt`, no `throwReportedErrors`, no `rethrowAsError`. The API's error
  layer is *nothing*; the boundary carries no error semantics of its own.
- **No result type can represent failure.** `ImportResult` holds a scene that is
  always valid plus observations about it. There is no state to check and no way
  to forget to.
- `hasErrors()` answers exactly one question — *is the result partial?* — at
  both layers, instead of meaning "did it work" internally and "is it degraded"
  publicly.
- Third-party throws (manifold, toml++, zstd, tinygltf, FlatBuffers) are caught
  where they are called, by the code that knows what the failure means, and
  rethrown as `Error` with a code. Every `catch` names what it caught; there are
  no bare `catch (...)`.

### Two carve-outs, both named

#### The collector

A parser that promises *all the problems in a document* cannot fail on a bad
document — collecting is its job. So `ConfigLoader` keeps an internal
diagnostics-collecting core and exposes two faces over it:

- `load…` promises a config: it runs the collector and throws if the collector
  reported errors.
- `check…` promises a report: it hands the collector's list back, errors and
  all.

This is not a hole in the doctrine, it is the doctrine: two promises, two
channels, one implementation. `nodehammer validate-config` needs the second, and
without it the public API could not express a command the CLI already ships.

`Config::check(path)` and `Config::checkString(toml, baseDir)` are the public
faces. `checkString` is named rather than overloaded because a string literal
converts to both `path` and `string_view` — `check("cfg.toml")` would compile
and check a *filename* as though it were a document.

#### The asynchronous boundary

A build driven from a frame loop or delivered over `postMessage` has no caller
to unwind to: by the time it fails, the stack that asked for it is gone. There
the fatal channel is *materialised* rather than abandoned —
`SceneBuildResult::failure` is an `std::optional<Error>` holding what would have
been thrown, and `scene` is non-null exactly when it is empty.

The two channels stay as distinct here as anywhere else. What matters is that
the failure does not get folded into `diags`, because then "the build could not
run" and "the build has a hole in it" would again be one bit apart.

## Policies the library does not have

**Strictness.** "Treat warnings as errors" is a decision about a returned list.
It belongs to the caller, after the call — never to a flag threaded into library
code. `--strict` inspects the diagnostics `convert` was handed and chooses to
fail.

**Thresholds.** One unmeshable node and ten thousand are both `Error`-severity
diagnostics with a scene attached. Where "degraded" becomes "useless" is the
caller's judgement; a library that decides "mostly failed means failed" invents
a policy it cannot defend. `BuildPipeline` therefore hands back a partially
tessellated scene and lets the viewer decide whether to show it, while `convert`
declines to write one — the same facts, two callers, two judgements.

## How every failure adjudicates

| failure                          | channel                                            |
| -------------------------------- | -------------------------------------------------- |
| file will not open               | throw `kFatalImportFileNotFound`                    |
| bytes fail FlatBuffers verify     | throw `kFatalImportFileNotFound` from the codec     |
| no backend claims the format      | throw `kFatalImportFormatUnknown`                   |
| config will not parse or validate | throw from `read`; **report** from `check`          |
| unknown config key                | `Warning`                                           |
| selection drops the root          | throw `kFatalSelectionRootDropped` — `prune` declines to act, so the scene it would return is "the rules *not* applied", which is not what was promised |
| orphan hoisted                    | `Warning`                                           |
| unknown shape at tessellation     | `Error` diagnostic + a node with no mesh — the canonical partial result |
| boolean falls back to a bbox      | `Warning`                                           |
| default material substituted      | `Warning`                                           |
| deduplication merged N things     | `Info`                                              |
| export cannot write               | throw `kFatalExportWriteFailed`                     |
| handle refers to nothing          | throw `kFatalApiInvalidHandle`                      |
| backend absent from this build    | throw `kFatalApiBackendMissing`                     |
| out of memory                     | propagates unchanged                                |

## Signatures that stop lying

A return type that can only ever carry an empty list is a lie about the
contract. Once fatal failures throw:

- Both `write` overloads return **`void`**. All four exporters emit error
  diagnostics and nothing else, so with the errors thrown there is nothing left
  to hand back.
- `RenderScene::read` returns **`RenderScene`**, not `RenderResult`. Reading
  `.nhr` either produces the scene or throws; there is nothing non-fatal to
  observe about it.
- `deduplicate` keeps its `SemanticResult` and starts *using* it — the merge
  counts the CLI prints today become `Info` diagnostics instead of being
  discarded.

## Invariants

1. No returned `DiagnosticList` contains `Fatal` — structurally, because
   `diagnostics::List` has no `fatal()` method to put one there. The only
   producer is `Error::diagnostic()`.
2. Every entry point returns a valid result or throws. There are no invalid
   handles, and no result type has a "did it work" field.
3. `Error::diagnostic()` yields severity `Fatal`.
4. An `Error` carries the diagnostics observed before it, so a caller loses
   nothing by the failure being fatal.
5. `kFatal…` codes never appear in a `DiagnosticList`.
6. Every `catch` names its code; no bare `catch (...)` anywhere in the library.
7. `hasErrors()` means the same thing at both layers: *is the result partial?*
   It is never a test for whether a call worked.
