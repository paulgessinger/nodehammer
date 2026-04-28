#include "ibl_cache.hpp"

#include <emscripten/emscripten.h>

#include <cstdint>
#include <print>
#include <span>

namespace nodehammer::viewer {

namespace {

constexpr const char *kIdbDbName = "nodehammer";
constexpr const char *kIdbKey = "ibl_cache_v1";

} // namespace

struct IblCacheLoad::Impl {
    enum class State : uint8_t { Idle, Pending, Hit, Miss };
    State state{State::Idle};
    std::optional<IblBakeData> data;

    // Static C-style callbacks dispatch back to the instance via user_data.
    static void onLoad(void *user_data, void *bytes, int size);
    static void onError(void *user_data);
};

void IblCacheLoad::Impl::onLoad(void *user_data, void *bytes, int size) {
    auto *self = static_cast<Impl *>(user_data);
    if (size <= 0 || bytes == nullptr) {
        self->state = State::Miss;
        return;
    }
    auto span = std::span{static_cast<const std::byte *>(bytes), static_cast<size_t>(size)};
    self->data = deserializeIblCache(span);
    self->state = self->data.has_value() ? State::Hit : State::Miss;
}

void IblCacheLoad::Impl::onError(void *user_data) {
    auto *self = static_cast<Impl *>(user_data);
    self->state = State::Miss;
}

IblCacheLoad::IblCacheLoad() : impl_(std::make_unique<Impl>()) {}
IblCacheLoad::~IblCacheLoad() = default;

void IblCacheLoad::start() {
    if (impl_->state != Impl::State::Idle) {
        return;
    }
    impl_->state = Impl::State::Pending;
    std::println("viewer: looking up IBL cache in IndexedDB ({}/{})", kIdbDbName, kIdbKey);
    emscripten_idb_async_load(kIdbDbName, kIdbKey, impl_.get(), &Impl::onLoad, &Impl::onError);
}

bool IblCacheLoad::poll() {
    return impl_->state == Impl::State::Hit || impl_->state == Impl::State::Miss;
}

std::optional<IblBakeData> IblCacheLoad::take() { return std::move(impl_->data); }

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

} // namespace nodehammer::viewer
