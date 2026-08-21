#pragma once

// Assembling the directory the server will serve.
//
// The web viewer has exactly two postures, and which one it comes up in is
// decided by a single file's presence: an `nh_manifest.json` sidecar next to the
// shell means "viewer mode, here is the archive, locked"; its absence means "the
// application", which starts empty and accepts drops. Staging is therefore not
// copying-plus-configuration — the sidecar *is* the configuration, and writing
// one is the whole difference between the two postures the viewer already
// ships. See docs/viewer-project-strategy.md.
//
// This is `scripts/stage_wasm_viewer.sh` promoted out of shell, which leaves
// that script as the dev-loop tool it actually is rather than the definition of
// the payload.

#include <filesystem>
#include <string>

namespace nodehammer::web {

/// Which of the viewer's two postures a staged root selects.
enum class Posture {
    /// No sidecar: the viewer comes up empty, restores from IndexedDB and
    /// accepts dropped files.
    Application,
    /// A sidecar naming a locked archive: opening the page just builds it.
    Viewer,
};

struct StageOptions {
    /// A validated runtime directory — what `locateRuntime` returned.
    std::filesystem::path runtime;

    /// Where to assemble. Created if absent; existing runtime files are
    /// overwritten, so re-staging into one directory is safe.
    std::filesystem::path target;

    /// An existing `.nhproj`, staged as-is. Wins over `config`/`geometry`.
    std::filesystem::path project{};

    /// Loose entry config and geometry, packed into an archive together with
    /// the config's transitive `include` chain. Both or neither.
    std::filesystem::path config{};
    std::filesystem::path geometry{};

    /// Browser tab title in viewer mode. A default is derived when empty.
    std::string title{};
};

struct StagedRoot {
    std::filesystem::path dir;
    Posture posture{Posture::Application};
    /// The archive's file name within `dir`. Empty in application mode.
    std::string archive;
};

/// Assemble a servable directory. Throws `Error` on anything that would produce
/// a root the browser cannot open.
[[nodiscard]] StagedRoot stageRoot(const StageOptions &options);

} // namespace nodehammer::web
