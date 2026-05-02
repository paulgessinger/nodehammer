#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace nodehammer::viewer {

/// Refcounted handle to an immutable byte sequence. Backends that produce
/// bytes wrap them in a `ByteBuffer`; consumers pass it around by copy
/// (refcount bump) or move. The bytes survive any backend mutation as
/// long as some `ByteBuffer` still references them.
///
/// Cache-backed backends (bag, URL, future ZIP) hold one `ByteBuffer` per
/// cached entry; resolve() hands out a copy. Storage-backed backends
/// (filesystem, future native bag) build a fresh `ByteBuffer` per
/// resolve from a freshly-read vector.
class ByteBuffer {
  public:
    ByteBuffer() = default;

    explicit ByteBuffer(std::vector<std::byte> bytes)
        : data_(std::make_shared<const std::vector<std::byte>>(std::move(bytes))) {}

    ByteBuffer(const ByteBuffer &) = default;
    ByteBuffer(ByteBuffer &&) noexcept = default;
    ByteBuffer &operator=(const ByteBuffer &) = default;
    ByteBuffer &operator=(ByteBuffer &&) noexcept = default;

    std::span<const std::byte> span() const noexcept {
        return data_ ? std::span<const std::byte>{*data_} : std::span<const std::byte>{};
    }

    std::size_t size() const noexcept { return data_ ? data_->size() : 0; }
    bool empty() const noexcept { return !data_ || data_->empty(); }

  private:
    std::shared_ptr<const std::vector<std::byte>> data_;
};

} // namespace nodehammer::viewer
