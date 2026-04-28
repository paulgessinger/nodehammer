#include "ibl_cache.hpp"

#include <sago/platform_folders.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <print>
#include <vector>

namespace nodehammer::viewer {

namespace {

std::filesystem::path cachePath() {
    return std::filesystem::path{sago::getCacheDir()} / "nodehammer" / "ibl.fb";
}

} // namespace

struct IblCacheLoad::Impl {
    enum class State : uint8_t { Idle, Pending, Hit, Miss };
    State state{State::Idle};
    std::optional<IblBakeData> data;
};

IblCacheLoad::IblCacheLoad() : impl_(std::make_unique<Impl>()) {}
IblCacheLoad::~IblCacheLoad() = default;

void IblCacheLoad::start() {
    if (impl_->state != Impl::State::Idle) {
        return;
    }
    impl_->state = Impl::State::Pending;
    const auto path = cachePath();
    std::println("viewer: looking up IBL cache at {}", path.string());
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        impl_->state = Impl::State::Miss;
        return;
    }
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in) {
        impl_->state = Impl::State::Miss;
        return;
    }
    // `vector<byte>` can't be range-constructed from a `char` iterator
    // (`byte` is not constructible from `char`), so size-then-read into a
    // raw byte buffer instead.
    const auto size = static_cast<std::streamsize>(in.tellg());
    if (size <= 0) {
        impl_->state = Impl::State::Miss;
        return;
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> buf(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char *>(buf.data()), size)) {
        impl_->state = Impl::State::Miss;
        return;
    }
    impl_->data = deserializeIblCache(buf);
    impl_->state = impl_->data.has_value() ? Impl::State::Hit : Impl::State::Miss;
}

bool IblCacheLoad::poll() {
    return impl_->state == Impl::State::Hit || impl_->state == Impl::State::Miss;
}

std::optional<IblBakeData> IblCacheLoad::take() { return std::move(impl_->data); }

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

} // namespace nodehammer::viewer
