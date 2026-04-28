#include "ibl_cache.hpp"

#include "ibl_cache_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>

namespace nodehammer::viewer {

namespace {

// Per-mip face count × bytes-per-pixel — mip dimension comes from
// IblBakeData::kPrefilterSize >> mip_index, kept in lockstep with bakeIbl().
constexpr size_t kFaces = 6;
constexpr size_t kBpp = 4;

constexpr size_t expectedBrdfBytes() {
    return static_cast<size_t>(IblBakeData::kBrdfLutSize) *
           static_cast<size_t>(IblBakeData::kBrdfLutSize) * kBpp;
}
constexpr size_t expectedIrradianceBytes() {
    return static_cast<size_t>(IblBakeData::kIrradianceSize) *
           static_cast<size_t>(IblBakeData::kIrradianceSize) * kFaces * kBpp;
}
constexpr size_t expectedPrefilterMipBytes(int mip) {
    const size_t sz = static_cast<size_t>(IblBakeData::kPrefilterSize) >> static_cast<size_t>(mip);
    return sz * sz * kFaces * kBpp;
}

} // namespace

std::vector<std::byte> serializeIblCache(const IblBakeData &data) {
    flatbuffers::FlatBufferBuilder builder{1 << 20};

    auto brdf = builder.CreateVector(reinterpret_cast<const uint8_t *>(data.brdf_lut.data()),
                                     data.brdf_lut.size());
    auto irr =
        builder.CreateVector(reinterpret_cast<const uint8_t *>(data.irradiance_combined.data()),
                             data.irradiance_combined.size());

    std::vector<flatbuffers::Offset<fbs::IblMip>> mip_offsets;
    mip_offsets.reserve(data.prefilter_mips.size());
    for (const auto &mip : data.prefilter_mips) {
        auto bytes =
            builder.CreateVector(reinterpret_cast<const uint8_t *>(mip.data()), mip.size());
        mip_offsets.push_back(fbs::CreateIblMip(builder, bytes));
    }
    auto mips = builder.CreateVector(mip_offsets);

    auto root = fbs::CreateIblCache(builder, kIblCacheVersion, brdf, irr, mips);
    fbs::FinishIblCacheBuffer(builder, root);

    auto *ptr = builder.GetBufferPointer();
    auto size = builder.GetSize();
    auto span = std::as_bytes(std::span{ptr, size});
    return std::vector<std::byte>(span.begin(), span.end());
}

std::optional<IblBakeData> deserializeIblCache(std::span<const std::byte> buf) {
    if (buf.empty()) {
        return std::nullopt;
    }
    const auto *ptr = reinterpret_cast<const uint8_t *>(buf.data());
    flatbuffers::Verifier verifier{ptr, buf.size()};
    if (!fbs::VerifyIblCacheBuffer(verifier)) {
        return std::nullopt;
    }
    const auto *fb = fbs::GetIblCache(ptr);
    if (fb->version() != kIblCacheVersion) {
        return std::nullopt;
    }

    const auto *brdf = fb->brdf_lut();
    const auto *irr = fb->irradiance();
    const auto *mips = fb->prefilter_mips();
    if (brdf == nullptr || irr == nullptr || mips == nullptr) {
        return std::nullopt;
    }
    if (brdf->size() != expectedBrdfBytes() || irr->size() != expectedIrradianceBytes() ||
        mips->size() != IblBakeData::kPrefilterMips) {
        return std::nullopt;
    }

    IblBakeData out;
    const auto to_bytes = [](const flatbuffers::Vector<uint8_t> &v) {
        const auto *p = reinterpret_cast<const std::byte *>(v.data());
        return std::vector<std::byte>(p, p + v.size());
    };
    out.brdf_lut = to_bytes(*brdf);
    out.irradiance_combined = to_bytes(*irr);
    for (int m = 0; m < IblBakeData::kPrefilterMips; ++m) {
        const auto *mip = mips->Get(static_cast<flatbuffers::uoffset_t>(m));
        if (mip == nullptr || mip->data() == nullptr ||
            mip->data()->size() != expectedPrefilterMipBytes(m)) {
            return std::nullopt;
        }
        out.prefilter_mips[static_cast<size_t>(m)] = to_bytes(*mip->data());
    }
    return out;
}

} // namespace nodehammer::viewer
