#pragma once

#include <nodehammer/viewer/project_fs.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace nodehammer::viewer {

/// ProjectFs backend that lazily fetches files from URLs via
/// emscripten_fetch. Stage 2 turned this into a generic byte fetcher:
/// no TOML knowledge, no include walking, no MEMFS writeback. The
/// BuildSession drives resolution by calling `resolve(key)` for each
/// file it needs; the URL backend kicks off the fetch on first miss
/// and returns Pending until the bytes land.
///
/// Web-only: this header is included from viewer_main.cpp which is
/// itself part of the wasm target. Native viewer entry points feed the
/// App a `BagProjectFs` (drag-drop / picker / CLI) instead.
class UrlProjectFs final : public ProjectFs {
  public:
    UrlProjectFs();
    ~UrlProjectFs() override;

    /// Optional URL prefix prepended to each resolved key at fetch time.
    /// Empty for root-served deployments; set to the document directory
    /// (e.g. `/foo/bar`) for sub-path deployments. Must not end in a
    /// trailing slash. Setting after fetches have already started is
    /// supported but only affects keys resolved from that point on.
    void setAssetBase(std::string asset_base);

    void poll() override;
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    const std::string &errorMessage() const override;
    std::string_view name() const override { return "url"; }
    ResolveResult resolve(std::string_view key) const override;
    std::uint64_t generation() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
