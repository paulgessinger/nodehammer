#pragma once

// Iterating an ID-keyed IR map in ID order, for the exporters that emit in
// whatever order they iterate.
//
// The IR's maps are `ankerl::unordered_dense::map`, which iterates its dense
// backing vector — so insertion order, and identical on every platform. That is
// why nothing has ever gone wrong here. But it makes the *emitted* order of
// glTF meshes, scenes and materials, and of the `dump-render` JSON arrays, a
// property of the container rather than of the format: swap in
// `std::unordered_map` and the same scene emits a different file, differently
// on libstdc++, libc++ and MSVC.
//
// That is a constraint nobody wrote down, and it is the wrong one to have. The
// `.nhb` writer already sorts by ID before emitting anything
// (ir/fb/semantic/flatbuffer.cpp); this is the same discipline for the two
// exporters that skipped it.
//
// Note what is *not* affected, and does not need this: glTF node indices come
// from a BFS over each node's `children` vector, so they were already ordered
// by the graph rather than by the map.

#include <algorithm>
#include <vector>

namespace nodehammer::ir {

/// Pointers to a map's entries, ordered by key.
///
/// Pointers rather than copies because the values are scenes' worth of geometry,
/// and pointers-to-pair rather than keys because the caller wants both halves
/// and a second lookup per entry would be pure waste.
///
///     for (const auto *entry : ir::entriesById(scene.materials)) {
///         const auto &[id, mat] = *entry;
///
template <typename Map> std::vector<const typename Map::value_type *> entriesById(const Map &map) {
    std::vector<const typename Map::value_type *> out;
    out.reserve(map.size());
    for (const auto &entry : map) {
        out.push_back(&entry);
    }
    std::sort(out.begin(), out.end(),
              [](const auto *a, const auto *b) { return a->first < b->first; });
    return out;
}

} // namespace nodehammer::ir
