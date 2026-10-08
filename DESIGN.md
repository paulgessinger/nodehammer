# NHS9 size follow-up investigation

Investigation of commit e7ce77b; no production changes or proposed wire contract committed. Scripts parse existing FlatBuffers directly without constructing the canonical scene or expanding occurrences. Source files under the main worktree's analysis/fccee/flatbuffers-full were read only. Whole-file zstd remains the container.

## Baseline measured on full detector files

| Geometry | Raw MiB | Existing zstd MiB | Stored volumes | Placements | Physical occurrences |
|---|---:|---:|---:|---:|---:|
| ALLEGRO o1 v01 | 9.42 | 1.35 | 2,064 | 15,715 | 94,998 |
| IDEA o1 v03 | 133.89 | 22.10 | 730,172 | 898,013 | 260,245,099 |
| IDEA o2 v01 | 614.68 | 121.87 | 2,958,992 | 3,544,564 | 265,922,033 |

Volume and placement table bodies plus their referencing vector entries alone occupy 0.49 / 52.74 / 204.92 MiB respectively, excluding vtables, strings and daughter-vector length words. IDEA o2 also has 2,959,352 shapes (2,554,997 boxes and 403,213 tubes). Optimizing transforms alone misses most of its payload.

## Small prototypes: caution about compressed size

The following are standalone *section* compression experiments using zstd level 3, the code's default. They are not whole-file size predictions, runtime codec benchmarks, nor a complete prospective FlatBuffer. Full transform rows were split bit-exactly into pooled 3x3 linear parts, translations, and a separately pooled final row, each referenced by uint32 columns. Keeping the final row preserves arbitrary matrices and signed zeros; all tested files happened to contain one distinct final row.

| Geometry | Unique full / linear / translation matrices | Full raw bytes | Split raw bytes | Full section zstd bytes | Split section zstd bytes |
|---|---:|---:|---:|---:|---:|
| ALLEGRO | 4,068 / 3,139 / 4,065 | 520,704 | 372,416 | 104,383 | 107,672 |
| IDEA o1 | 321,457 / 23,616 / 309,814 | 41,146,496 | 12,993,404 | 1,740,629 | 2,277,361 |
| IDEA o2 | 627,674 / 15,609 / 615,862 | 80,342,272 | 23,436,656 | 2,160,706 | 3,497,814 |

The same warning applies to narrow integer columns: concatenating IDs, shape/material references, daughter counts, daughter volume references and transform references into uniform uint32 columns reduced raw IDEA o2 bytes from 137,222,512 to 75,700,384, but standalone zstd bytes increased from 11,729,103 to 26,072,041. All numeric values in these particular files fit uint32. This probe intentionally holds column order fixed, without frequency ordering, delta encoding, or real table overhead. It demonstrates that narrower is not necessarily smaller after compression; it does not establish that uint32 is bad in a finished layout.

Scripts: measure.py, metadata_probe.py. Largest Python-process peak RSS was approximately 987 MiB while scanning the 615 MiB mapped file; no physical occurrence tree was produced. Durations are script scan times, not codec read/write performance.

Metadata scan found nearly four copies of every occurrence path: ALLEGRO stores 163,783 path vectors for 40,946 unique paths; IDEA o1 395,267 for 98,817; IDEA o2 387,571 for 96,893. The corresponding raw uint64 component payloads are 4,802,688 / 14,195,136 / 13,835,296 bytes, versus 1,200,672 / 3,548,784 / 3,458,824 bytes after exact path pooling (before dictionary references and offsets). Maximum observed daughter index is 4,634 / 815 / 815. This is direct evidence for path pooling; compressed savings remain unmeasured.

## Proposed layout to prototype, ranked

1. **Columnar volumes and placements, with exact catalog IDs.** Volume columns: original ID (optional only when exactly a declared deterministic sequence), name reference, shape reference, material reference, daughter range. Concatenate daughter name references, volume references and transform references, preserving each original daughter list's order. Root remains one placement record. References use dense local indices; optional source ID columns preserve arbitrary uint64 IDs, including the existing >2^54 test. Do not silently renumber user-visible catalog identities. A uint32 baseline is simpler; test adaptive uint16 against actual compressed results before enabling it.
2. **Columnar sparse metadata and pooled occurrence paths.** Store a common path dictionary (range plus concatenated daughter indices), and use path references in name, tag, provenance and degradation columns. Repeated path vectors currently appear independently in several metadata maps. Preserve explicit empty tag sets, all override kinds, missing versus present entries, tag-set ordering, and default source system. IDEA o2 has 2,105,378 volume-tag records: dense volume references and compact tag-set references merit measurement before transforms.
3. **Shape wrapper columns if profiling confirms their benefit.** Keep existing shape union members and exact parameter payloads unchanged initially. Move outer shape IDs/type/value references into columns to remove millions of wrapper tables; retain ordered union discriminants. Box/tube parameter columns are a subsequent experiment, not prerequisite scope. Do not deduplicate equal shapes automatically: that changes catalog identities and is a separate semantic optimization.
4. **Split transform pool experiment.** Retain full-double exact-bit semantics, including shear/scale, last-row values and signed zeros. Never assume a rotation-only matrix. Encode a canonical affine last row implicitly only when its exact bits match; retain explicit exception rows otherwise. The isolated results above argue against making split pools the first default change for compressed disk size. They may still improve decompressed-buffer footprint, pending complete-file measurement.
5. **String dictionary frequency ordering / reference-width tuning / row locality.** Current codec already uses shared strings. Pooling strings alone is not new savings; explicit indexed columns remove offset/table overhead and may improve compression. Reorder definition rows only with correct reference remapping and source-ID preservation. Never reorder daughters: that changes occurrence identity and retargets metadata.

Dense references require explicit null/sentinel policy (for example zero=null, index+1 for non-null); width threshold checks include the sentinel. Keep physical occurrence counts uint64. Check vector lengths, offset arithmetic, missing references, duplicate IDs, path components, selector widths and mutually exclusive columns before allocation. FlatBuffers' existing single-buffer size ceiling remains; none of this is chunking or a remedy for a genuinely oversized buffer.

## Compatibility recommendation

Keep NHS8 frozen reader + existing adapter, and preserve the current NHS9 version-1 reader. Introduce NHS9 schema_version=2 only when a measured replacement layout is accepted; do not reinterpret version-1 slots or change their scalar defaults/types. The current version-1 reader already rejects unknown schema versions, so old executables fail clearly on version 2. New normal imports accept NHS8, NHS9 v1 and v2 transparently; existing upgrade command emits latest supported representation. Existing NHS8 warning policy stays unchanged.

Implementation choice to settle: append a v2 payload field to the current root and keep small valid empty legacy vectors/records to satisfy current required-field verification, or freeze a dedicated v1 schema and add a dedicated v2 schema with safely verified version dispatch. The former is less code and adds only constant overhead but leaves legacy root placeholders; the latter is cleaner wire separation but needs explicit bounded header verification before selecting the full schema verifier. Do not casually remove `required` from v1 fields: generated verification would become weaker, requiring equivalent manual checks.

Prefer preserving one identifier and using the existing schema-version mechanism, subject to agreement on how much compatibility scaffolding is acceptable. Full-file zstd remains external and unchanged. No independent executable and no CBOR/chunk format.

## Acceptance and staged work

- First implement a disposable complete writer/reader prototype for columnar volumes/placements and metadata; measure the whole compressed file, not sums of compressed sections.
- Measure raw and compressed sizes, write/read wall time and peak RSS at the same compression level on these three fixtures. Test smaller synthetic and legacy-upgraded scenes too.
- Exact round-trip comparison: uint64 IDs, all shape parameters and transform bits, original daughter order, source filename, every metadata map including empty tag overrides, counts and determinism. Include signed-zero last rows, arbitrary non-affine matrices if currently accepted, empty scenes, >2^54 IDs, threshold boundaries and corrupt inputs.
- Keep immutable pre-change NHS9 fixture bytes: generating v1 fixtures with the current writer during the test would stop testing backwards compatibility once that writer changes.
- Benchmarks stay local; correctness and compatibility tests go to CI. Native timing is a useful WASM proxy; no claim of measured WASM speed without its SDK/runtime.
- Choose an encoding based on whole-file results. Smaller raw data may still be worthwhile for memory, but explicitly separate that decision from compressed-size reduction.

Historical evidence examined: 2d30247 (columns/pools), 0b73885 (narrow fields), 664e0ca (implicit node IDs), 1c6ab78 (adaptive widths), 062cd48 (design rationale), current src/ir/legacy/nhs8.cpp and docs/nhs9-size-audit.txt. Expanded-node parent/ID encoding has no counterpart to carry over because NHS9 does not store expanded nodes.
