#pragma once

#include "ibl.hpp"

#include <cstddef>

// Bake-side internals shared between the synchronous `bakeIbl()` (in
// ibl_common.cpp) and the per-platform `IblBakeJob` drivers in
// ibl_native.cpp / ibl_web.cpp. Not exposed in the public header.

namespace nodehammer::viewer {

inline constexpr int kBrdfSamples = 1024;
inline constexpr int kIrradianceSamples = 1024;
inline constexpr int kPrefilterSamples = 256;

inline constexpr int kBrdfLutSize = IblBakeData::kBrdfLutSize;
inline constexpr int kIrradianceSize = IblBakeData::kIrradianceSize;
inline constexpr int kPrefilterSize = IblBakeData::kPrefilterSize;
inline constexpr int kPrefilterMips = IblBakeData::kPrefilterMips;

void bakeBrdfPixel(int x, int y, std::byte *dst);
void bakeIrradiancePixel(int face, int x, int y, std::byte *dst);
void bakePrefilterPixel(int mip, int face, int x, int y, std::byte *dst);

void allocateBakeBuffers(IblBakeData &data);
[[nodiscard]] size_t irradianceFaceBytes();
[[nodiscard]] size_t prefilterFaceBytes(int mip);

} // namespace nodehammer::viewer
