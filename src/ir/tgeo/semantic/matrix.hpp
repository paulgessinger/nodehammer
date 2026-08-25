#pragma once

// The one conversion every TGeo traversal needs: ROOT's row-major 3×3 rotation
// plus translation, as a glm column-major 4×4.
//
// Its own header because two translation units need it — the tree walk in
// importer.cpp and the boolean-shape decomposition in shape_dispatch.cpp — and
// each had grown its own copy in an anonymous namespace. Identical copies, so
// nothing was wrong with either; they were simply free to drift, and an
// amalgamated build that concatenates both into one translation unit would have
// found them as a redefinition rather than as a warning.

#include <glm/glm.hpp>

#include <TGeoMatrix.h>

namespace nodehammer::ir {

/// ROOT stores the rotation row-major and the translation separately; glm wants
/// four columns. The transpose is in the indexing — `r[0], r[3], r[6]` walks a
/// *column* of ROOT's row-major array — so this is element access, not matrix
/// math, and it is the only shape either caller needs it in.
inline glm::dmat4 tgeoMatrixToGlm(const TGeoMatrix *m) {
    const double *r = m->GetRotationMatrix(); // 3×3 row-major
    const double *t = m->GetTranslation();
    return glm::dmat4{
        glm::dvec4{r[0], r[3], r[6], 0.0}, // col 0
        glm::dvec4{r[1], r[4], r[7], 0.0}, // col 1
        glm::dvec4{r[2], r[5], r[8], 0.0}, // col 2
        glm::dvec4{t[0], t[1], t[2], 1.0}, // col 3
    };
}

} // namespace nodehammer::ir
