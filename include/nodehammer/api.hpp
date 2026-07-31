#pragma once

// Visibility decoration for the public API.
//
// The add-on to the public/internal split, not the split itself: what keeps
// internals unreachable is physical, since `include/nodehammer/` holds only
// public headers and everything else lives under `src/` and is never installed.
// NH_API adds the link-time half — internals compile hidden, so they are not
// merely un-includable but un-linkable.
//
// Three situations, three expansions:
//
//   NH_STATIC   consumer of the in-tree archive (CLI, viewer, tests). Expands
//               to nothing. Defined PUBLIC on nodehammer_lib so no in-tree
//               target has to remember it.
//   NH_EXPORTS  building the shared library. Defined PRIVATE on
//               nodehammer_shared.
//   neither     consumer of the *installed* library — the case that must work
//               with no macros defined at all, since a consumer's build system
//               is not ours to configure. Hence dllimport / default visibility
//               as the fallback rather than the opt-in.
//
// On ELF and Mach-O the annotation only ever *raises* visibility against
// -fvisibility=hidden, so it is harmless in any configuration. On Windows it is
// load-bearing in both directions, which is why the static case needs a macro
// rather than being inferred.

#if defined(NH_STATIC)
#define NH_API
#elif defined(_WIN32)
#if defined(NH_EXPORTS)
#define NH_API __declspec(dllexport)
#else
#define NH_API __declspec(dllimport)
#endif
#else
#define NH_API __attribute__((visibility("default")))
#endif
