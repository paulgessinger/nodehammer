#pragma once

// Visibility decoration for the public API.
//
// This is the *add-on* to the public/internal split, not the split itself.
// What keeps internals unreachable is physical: `include/nodehammer/` holds
// only public headers, everything else lives next to its source under `src/`
// and is never installed. NH_API adds the second half of the guarantee for the
// shared build — internal symbols are compiled with hidden visibility, so they
// are not merely un-includable but un-linkable.
//
// Three build situations, three expansions:
//
//   NH_STATIC   consumer of the in-tree static library (the CLI, the viewer,
//               the tests, and the future Python extension). Expands to
//               nothing: an archive has no export table, and the static target
//               is compiled with hidden visibility precisely so its symbols do
//               *not* end up in the export table of whatever .so links it.
//               Defined PUBLIC on nodehammer_lib, so no in-tree target has to
//               remember it.
//
//   NH_EXPORTS  building the shared library itself. Defined PRIVATE on
//               nodehammer_shared.
//
//   neither     consumer of the *installed* shared library. This is the case
//               that must work with no macro definitions at all, since a
//               consumer's build system is not ours to configure — hence
//               dllimport / default visibility as the fallback rather than as
//               an opt-in.
//
// On ELF and Mach-O the annotation only ever *raises* visibility back to
// default against the target-wide -fvisibility=hidden, so decorating a
// declaration is harmless in every configuration. On Windows it is
// load-bearing in both directions, which is why the static case has to be
// distinguished by a macro rather than inferred.

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
