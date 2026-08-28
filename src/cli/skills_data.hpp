#pragma once

// The agent skills carried inside the binary.
//
// `cmake/GenerateSkillsData.cmake` turns `skills/<name>/**` into one translation
// unit of byte arrays and defines `embeddedSkills()` over them. This header is
// the hand-written half: the shape that generated file fills in, and the only
// thing `cmd_skills.cpp` includes.
//
// The payload is bytes rather than text on purpose. A skill is a *directory* --
// SKILL.md plus whatever `references/` or `scripts/` it carries -- and treating
// every entry as opaque bytes means adding a PNG to one needs no change here.
//
// Not in `include/nodehammer/`: an installed consumer has no use for this, and
// the public CLI header deliberately carries one function and a struct.

#include <span>
#include <string_view>

namespace nodehammer::cli::detail {

/// One file inside a skill, with the path it keeps when installed.
struct EmbeddedFile {
    /// Relative to the skill's own directory -- `SKILL.md`,
    /// `references/formats.md`. Always forward slashes, since it is written into
    /// generated source by CMake and read back on every platform.
    std::string_view path;
    std::span<const unsigned char> bytes;
};

/// One skill: the directory name under `skills/`, and everything in it.
struct EmbeddedSkill {
    std::string_view name;
    std::span<const EmbeddedFile> files;
};

/// Every skill compiled into this binary, sorted by name.
///
/// Empty is a legitimate answer -- a build configured before `skills/` had
/// anything in it -- and `skills list` says so rather than treating it as a
/// failure. It is *not* legitimate in a release, which is why
/// tests/cli/test_cli_skills.cpp asserts the payload is non-empty: a build that
/// silently ships no skill is the failure this whole mechanism exists to make
/// impossible, and it would otherwise show up only as a user reporting that
/// `skills install` installed nothing.
std::span<const EmbeddedSkill> embeddedSkills();

} // namespace nodehammer::cli::detail
