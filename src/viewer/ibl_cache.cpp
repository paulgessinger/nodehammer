#include "ibl_cache.hpp"

#include "ibl_cache_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstdio>
#include <print>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#include <filesystem>
#include <fstream>
#include <sago/platform_folders.h>
#endif

namespace nodehammer::viewer {

namespace {

#ifdef __EMSCRIPTEN__
constexpr const char *kIdbDbName = "nodehammer";
constexpr const char *kIdbKey = "ibl_cache_v1";
#endif

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

#ifndef __EMSCRIPTEN__
std::filesystem::path cachePath() {
    return std::filesystem::path{sago::getCacheDir()} / "nodehammer" / "ibl.fb";
}
#endif

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

// ── IblCacheLoad ────────────────────────────────────────────────────────────

IblCacheLoad::~IblCacheLoad() = default;

#ifdef __EMSCRIPTEN__

void IblCacheLoad::start() {
    if (state_ != State::Idle) {
        return;
    }
    state_ = State::Pending;
    std::println("viewer: looking up IBL cache in IndexedDB ({}/{})", kIdbDbName, kIdbKey);
    emscripten_idb_async_load(kIdbDbName, kIdbKey, this, &IblCacheLoad::onLoad,
                              &IblCacheLoad::onError);
}

bool IblCacheLoad::poll() { return state_ == State::Hit || state_ == State::Miss; }

void IblCacheLoad::onLoad(void *user_data, void *bytes, int size) {
    auto *self = static_cast<IblCacheLoad *>(user_data);
    if (size <= 0 || bytes == nullptr) {
        self->state_ = State::Miss;
        return;
    }
    auto span = std::span{static_cast<const std::byte *>(bytes), static_cast<size_t>(size)};
    self->data_ = deserializeIblCache(span);
    self->state_ = self->data_.has_value() ? State::Hit : State::Miss;
}

void IblCacheLoad::onError(void *user_data) {
    auto *self = static_cast<IblCacheLoad *>(user_data);
    self->state_ = State::Miss;
}

void saveIblCache(const IblBakeData &data) {
    auto bytes = serializeIblCache(data);
    std::println("viewer: saving IBL cache to IndexedDB ({}/{}, {} bytes)", kIdbDbName, kIdbKey,
                 bytes.size());
    // emscripten_idb_async_store copies the buffer into JS heap before
    // returning to JS, but the call itself is synchronous on the C side —
    // safe to let `bytes` go out of scope after the call. (The callbacks
    // are nullptr because we don't care about the result here.)
    emscripten_idb_async_store(kIdbDbName, kIdbKey, bytes.data(), static_cast<int>(bytes.size()),
                               /*arg=*/nullptr, /*onstore=*/nullptr,
                               /*onerror=*/nullptr);
}

void clearIblCache() {
    std::println("viewer: clearing IBL cache from IndexedDB ({}/{})", kIdbDbName, kIdbKey);
    emscripten_idb_async_delete(kIdbDbName, kIdbKey,
                                /*arg=*/nullptr, /*ondelete=*/nullptr,
                                /*onerror=*/nullptr);
}

#else

void IblCacheLoad::start() {
    if (state_ != State::Idle) {
        return;
    }
    state_ = State::Pending;
    const auto path = cachePath();
    std::println("viewer: looking up IBL cache at {}", path.string());
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        state_ = State::Miss;
        return;
    }
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        state_ = State::Miss;
        return;
    }
    // `vector<byte>` can't be range-constructed from a `char` iterator
    // (`byte` is not constructible from `char`), so size-then-read into a
    // raw byte buffer instead.
    const auto size = static_cast<std::streamsize>(in.tellg());
    if (size <= 0) {
        state_ = State::Miss;
        return;
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> buf(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char *>(buf.data()), size)) {
        state_ = State::Miss;
        return;
    }
    data_ = deserializeIblCache(buf);
    state_ = data_.has_value() ? State::Hit : State::Miss;
}

bool IblCacheLoad::poll() { return state_ == State::Hit || state_ == State::Miss; }

void saveIblCache(const IblBakeData &data) {
    const auto path = cachePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        std::println(stderr, "viewer: IBL cache mkdir failed ({}): {}", path.parent_path().string(),
                     ec.message());
        return;
    }
    auto bytes = serializeIblCache(data);
    std::println("viewer: saving IBL cache to {} ({} bytes)", path.string(), bytes.size());
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out) {
        std::println(stderr, "viewer: IBL cache open-for-write failed: {}", path.string());
        return;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void clearIblCache() {
    const auto path = cachePath();
    std::error_code ec;
    if (std::filesystem::remove(path, ec)) {
        std::println("viewer: cleared IBL cache at {}", path.string());
    } else if (ec) {
        std::println(stderr, "viewer: IBL cache clear failed ({}): {}", path.string(),
                     ec.message());
    } else {
        std::println("viewer: no IBL cache to clear at {}", path.string());
    }
}

#endif

std::optional<IblBakeData> IblCacheLoad::take() { return std::move(data_); }

} // namespace nodehammer::viewer
