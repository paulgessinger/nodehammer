# GDML through Geant4

Enable `NODEHAMMER_WITH_GEANT4` to read `.gdml` files, or select the importer
explicitly with `--input-format gdml`. The implementation calls `G4GDMLParser`
and translates its geometry objects; it does not interpret GDML XML itself and
has no dependency on ROOT or DD4hep. The Geant4 installation must include GDML
support (CMake component `gdml`). The ordinary wheel keeps this backend disabled.

```sh
nodehammer inspect --output-format json summary -i detector.gdml
nodehammer convert -i detector.gdml -o detector.nhb -o detector.glb
```

## Representation and coverage

The importer preserves logical-volume sharing, physical placement names,
transforms, material names and density, and placement copy numbers as
`copyNumber` tags. Pointer suffixes in source names are retained. Source system
is `geant4`. Geant4 lengths are converted to nodehammer's centimetres, density
to g/cm³, and angles remain radians. GDML expressions, references and units are
resolved by Geant4.

Boxes, tubes, cones, trapezoids (`G4Trd`), polycones and union/intersection/
subtraction solids map to analytic semantic shapes. Boolean operand displacement
is preserved. Remaining solids use `G4VSolid::GetPolyhedron()` and become
triangle meshes, with an `NH0112` informational diagnostic. This includes
extruded solids and generic traps. Their mesh resolution is fixed at import
time by Geant4; nodehammer's later circle-resolution setting cannot refine them.
A solid without a usable Geant4 polyhedron fails the import.

Replica and parameterised placements currently fail explicitly (`NH0110`),
rather than importing only one copy. Sensitive-detector bindings, optical
properties, GDML auxiliary metadata and material composition are not translated.
GDML schema validation and overlap checks are not enabled during import; a
remote schema URL is not needed. Geant4 still performs its normal semantic
checks when constructing solids and volumes.

## Lifetime and diagnostics

The importer serializes access with a non-blocking lock; concurrent nodehammer
GDML imports fail. Applications must also avoid concurrent external Geant4
geometry mutation. Existing material, element and isotope names are temporarily hidden during the
read so references resolve to the new file's definitions, then restored before
conversion. The exception handler and Geant4 output streams are restored
on success and failure. Backend warnings become `NH0111` diagnostics, fatal
Geant4 exceptions become `NH0110` errors, and parser progress stays out of stdout.

Only newly allocated solids and physical/logical volumes are deleted after
conversion, including after parser errors; pre-existing geometry remains owned
by its caller. Geant4 material, isotope, element and other global tables remain
process-owned because backend caches may reference them. For large batches,
separate CLI processes bound this retained state. The returned semantic scene
contains no Geant4 pointers.

## Homebrew development build

After installing the repository's Conan dependencies (see the main README):

```sh
conan install . -s build_type=RelWithDebInfo --build=missing \
  -c tools.cmake.cmaketoolchain:generator=Ninja -o '&:viewer=True'
cmake --preset conan-relwithdebinfo -GNinja \
  -DNODEHAMMER_BUILD_TESTS=ON -DNODEHAMMER_WITH_VIEWER=ON \
  -DNODEHAMMER_WITH_DD4HEP=ON -DNODEHAMMER_WITH_GEANT4=ON \
  -DDD4hep_DIR="$(brew --prefix dd4hep)/cmake" \
  -DGeant4_DIR="$(brew --prefix geant4)/lib/cmake/Geant4" \
  -DROOT_DIR="$(brew --prefix root)/share/root/cmake"
cmake --build --preset conan-relwithdebinfo
```

Homebrew DD4hep's plugins live under `lib/dd4hep`. Set their runtime search path
when running DD4hep tests or imports:

```sh
export DD4hepINSTALL="$(brew --prefix dd4hep)"
export ROOT_INCLUDE_PATH="$(brew --prefix dd4hep)/include${ROOT_INCLUDE_PATH:+:$ROOT_INCLUDE_PATH}"
export DD4HEP_LIBRARY_PATH="$(brew --prefix dd4hep)/lib/dd4hep"
export DYLD_LIBRARY_PATH="$DD4HEP_LIBRARY_PATH${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export ROOT_LIBRARY_PATH="$DD4HEP_LIBRARY_PATH${ROOT_LIBRARY_PATH:+:$ROOT_LIBRARY_PATH}"
ctest --test-dir build/RelWithDebInfo --output-on-failure
```

GDML itself needs neither a DD4hep environment nor Geant4 physics datasets.
The tests under `[gdml]` cover shared placements, expression evaluation, mixed
units, placement and boolean rotations, facet winding, copy numbers, repeated
imports, failure recovery, changed material definitions on reload, clean JSON stdout and
NHB serialization.

The local sPHENIX integration input is not stored in this repository. Its Geant4
probe reports 965 logical volumes and 41,269 physical occurrences, with a world
bounding box of ±500 cm in x/y and ±550 cm in z. The importer preserves those
counts and dimensions. Rendering this detector also exercises boolean trees
deeper than 32 levels; the tessellator evaluates these operand graphs iteratively,
with shared-operand caching and explicit cycle detection.
