// `nodehammer skills list | install` -- the agent skills carried in the binary.
//
// A `--help` tells an agent what the flags are. It does not tell it which
// command blocks until a human closes a window, which output is a stable
// contract and which is prose, or that a missing importer is a compile-time
// option rather than a broken file. That is what `skills/nodehammer/SKILL.md`
// says, and this is what puts it where an agent will read it.
//
// Native-only, gated in CMakeLists.txt rather than with an #ifdef here: every
// line of this file writes to a filesystem the user keeps, and under Emscripten
// that is a virtual one which vanishes with the tab.

#include "cli_common.hpp"
#include "run_internal.hpp"
#include "skills_data.hpp"

#include <detail/env.hpp>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

using nodehammer::cli::detail::EmbeddedSkill;

/// The version of the `--output-format json` document below.
///
/// Same role, and the same reason, as `inspect`'s: the field names are API from
/// the moment they ship, so the shape is stamped rather than inferred.
constexpr int kJsonSchema = 1;

/// Where the real copy goes, under whichever root was chosen.
///
/// `.agents/skills` rather than `.claude/skills` because it is the cross-agent
/// location: the Agent Skills standard that grew out of Claude Code is read by
/// Codex CLI, Gemini CLI, Copilot and Cursor too, so one copy serves all of
/// them. Claude Code does not read it -- it gets a symlink, below.
const fs::path kAgentsSkills = fs::path{".agents"} / "skills";

/// Claude Code's own directory, which gets a link rather than a second copy.
const fs::path kClaudeSkills = fs::path{".claude"} / "skills";

/// Everything embedded, by name, or `nullptr`.
const EmbeddedSkill *findSkill(std::string_view name) {
    for (const auto &skill : nodehammer::cli::detail::embeddedSkills()) {
        if (skill.name == name) {
            return &skill;
        }
    }
    return nullptr;
}

std::string knownSkillNames() {
    std::string names;
    for (const auto &skill : nodehammer::cli::detail::embeddedSkills()) {
        if (!names.empty()) {
            names += ", ";
        }
        names += std::string{skill.name};
    }
    return names.empty() ? std::string{"(none)"} : names;
}

/// The user's home directory.
///
/// Read from the environment rather than through platform_folders, which is a
/// viewer-gated dependency: this command is in the headless core, and a config
/// *home* is not what is wanted anyway -- `~/.agents/skills` is specified
/// relative to `$HOME` itself.
fs::path homeRoot() {
    for (const char *var : {"HOME", "USERPROFILE"}) {
        if (const std::string value = nodehammer::detail::getEnv(var); !value.empty()) {
            return fs::path{value};
        }
    }
    throw nodehammer::Error{nodehammer::codes::kFatalSkillsInstall,
                            "cannot find your home directory: neither HOME nor USERPROFILE is set"};
}

/// The repository root above the working directory, or the working directory.
///
/// Deliberately checks `.git` *and* `.jj`, and neither is nodehammer's own: a
/// tool that only recognised one VCS could not install into a checkout of the
/// other, and this repository happens to be colocated in both. `.git` is
/// matched whether it is a directory or a file, because in a git worktree it is
/// a file pointing elsewhere.
///
/// Falling back to the working directory rather than failing: `--scope project`
/// in a plain directory is a reasonable thing to mean, and refusing would make
/// the caller find a root themselves only to pass it back via `--dir`.
fs::path projectRoot() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) {
        throw nodehammer::Error{
            nodehammer::codes::kFatalSkillsInstall,
            std::format("cannot read current working directory: {}", ec.message())};
    }
    const fs::path start = dir;
    while (true) {
        if (fs::exists(dir / ".git", ec) || fs::exists(dir / ".jj", ec)) {
            return dir;
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir || parent.empty()) {
            return start;
        }
        dir = parent;
    }
}

/// What is sitting where we want to write, in the terms the rules care about.
enum class Occupant {
    Absent,
    OurSymlink,     ///< a link already pointing where we would point it
    ForeignSymlink, ///< a link somewhere else -- somebody's deliberate choice
    SkillDir,       ///< a directory holding a SKILL.md: a previous install
    OtherDir,       ///< a directory that is not a skill: far more likely theirs
    NonDirectory,   ///< a regular file, a socket, something we will not touch
};

/// Does `link` resolve to `target`?
///
/// Two comparisons, because one is not enough. `weakly_canonical` handles the
/// ordinary case, including a *relative* link that lands in the right place --
/// which is still the link we would have made.
///
/// But it does not follow a link whose target is missing: it resolves the
/// existing prefix and appends the rest lexically, so a **dangling** link
/// compares equal to its own path and would be classified as somebody else's.
/// That state is reachable -- delete `.agents/skills/<name>` by hand, or have a
/// write fail after the link was made -- and reading it as foreign would refuse
/// the very reinstall that repairs it. So the literal target is compared as
/// well, resolved against the link's own directory when it is relative.
bool pointsAt(const fs::path &link, const fs::path &target) {
    std::error_code ec;
    const fs::path resolved = fs::weakly_canonical(link, ec);
    const fs::path wanted = fs::weakly_canonical(target, ec);
    if (!ec && resolved == wanted) {
        return true;
    }

    const fs::path literal = fs::read_symlink(link, ec);
    if (ec) {
        return false;
    }
    const fs::path absolute = literal.is_absolute() ? literal : link.parent_path() / literal;
    return absolute.lexically_normal() == target.lexically_normal();
}

/// Classify a destination. `expectedLink` empty means "a symlink here is foreign
/// whatever it points at" -- which is the case for the real copy, where a link
/// means somebody pointed this name at their own checkout.
Occupant classify(const fs::path &dest, const fs::path &expectedLink) {
    std::error_code ec;
    const auto status = fs::symlink_status(dest, ec);
    if (ec || status.type() == fs::file_type::not_found) {
        return Occupant::Absent;
    }
    if (fs::is_symlink(status)) {
        if (!expectedLink.empty() && pointsAt(dest, expectedLink)) {
            return Occupant::OurSymlink;
        }
        return Occupant::ForeignSymlink;
    }
    if (!fs::is_directory(status)) {
        return Occupant::NonDirectory;
    }
    return fs::exists(dest / "SKILL.md", ec) ? Occupant::SkillDir : Occupant::OtherDir;
}

/// Decide what writing here would mean, and refuse now if it would mean harm.
///
/// **Nothing is removed by this function.** The decision and the deletion are
/// deliberately two steps: `--dry-run` runs only this one, so asking what would
/// happen cannot be the thing that happens. Folding them together is how a dry
/// run deletes an installed skill and then writes nothing in its place.
///
/// These rules are the interesting part of the command: it runs repeatedly, over
/// a directory named after the tool, in a place people also edit by hand.
Occupant planDestination(const fs::path &dest, const fs::path &expectedLink, bool force) {
    const Occupant occupant = classify(dest, expectedLink);
    const std::string shown = dest.string();

    switch (occupant) {
    case Occupant::Absent:
    case Occupant::OurSymlink:
    case Occupant::SkillDir:
        // All three are ours to replace, and none of them needs a flag: every
        // reinstall rewrites its own link and replaces its own directory, so
        // demanding `--force` would make the common path the awkward one.
        return occupant;

    case Occupant::ForeignSymlink:
        if (!force) {
            throw nodehammer::Error{
                nodehammer::codes::kFatalSkillsInstall,
                std::format("{} is a symlink to somewhere else -- refusing to write through it. "
                            "That is usually a deliberate link to a checkout; pass --force to "
                            "replace it",
                            shown),
                shown};
        }
        return occupant;

    case Occupant::OtherDir:
        if (!force) {
            throw nodehammer::Error{
                nodehammer::codes::kFatalSkillsInstall,
                std::format("{} exists and holds no SKILL.md -- refusing to replace it. "
                            "Pass --force if it really is ours",
                            shown),
                shown};
        }
        return occupant;

    case Occupant::NonDirectory:
        // No --force escape. A skill is a directory; a regular file under this
        // name is something we have no story for, and guessing is how a tool
        // deletes somebody's work.
        throw nodehammer::Error{
            nodehammer::codes::kFatalSkillsInstall,
            std::format("{} exists and is not a directory -- refusing to replace it", shown),
            shown};
    }
    return occupant;
}

/// Carry out the removal `planDestination` already approved.
///
/// Separate, and called only on the writing path. Every refusal has been thrown
/// by the time this runs, so it has no rules of its own to get out of step.
void clearDestination(Occupant occupant, const fs::path &dest,
                      const nodehammer::cli::Narrator &say) {
    const std::string shown = dest.string();
    switch (occupant) {
    case Occupant::Absent:
        return;
    case Occupant::OurSymlink:
        // Silently: this is the expected state of every reinstall.
        fs::remove(dest);
        return;
    case Occupant::SkillDir:
        // Wholesale, not a merge: a file a later version of the skill drops must
        // not survive the upgrade as a stale reference the agent still reads.
        say("replacing the skill already at {}", shown);
        fs::remove_all(dest);
        return;
    case Occupant::ForeignSymlink:
        say("--force: replacing the symlink at {}", shown);
        fs::remove(dest);
        return;
    case Occupant::OtherDir:
        say("--force: replacing the directory at {}", shown);
        fs::remove_all(dest);
        return;
    case Occupant::NonDirectory:
        // Unreachable: planDestination throws on this row unconditionally.
        return;
    }
}

/// Write one skill's files out, creating the directories they need.
void writeSkill(const EmbeddedSkill &skill, const fs::path &dest) {
    for (const auto &file : skill.files) {
        const fs::path target = dest / fs::path{file.path};
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            throw nodehammer::Error{
                nodehammer::codes::kFatalSkillsInstall,
                std::format("cannot create {}: {}", target.parent_path().string(), ec.message()),
                target.parent_path().string()};
        }
        std::ofstream out{target, std::ios::binary | std::ios::trunc};
        if (!out) {
            throw nodehammer::Error{nodehammer::codes::kFatalSkillsInstall,
                                    std::format("cannot open {} for writing", target.string()),
                                    target.string()};
        }
        if (!file.bytes.empty()) {
            out.write(reinterpret_cast<const char *>(file.bytes.data()),
                      static_cast<std::streamsize>(file.bytes.size()));
        }
        if (!out) {
            throw nodehammer::Error{nodehammer::codes::kFatalSkillsInstall,
                                    std::format("cannot write {}", target.string()),
                                    target.string()};
        }
    }
}

/// Copy a directory tree, for when a symlink cannot be made.
void copyTree(const fs::path &from, const fs::path &to) {
    std::error_code ec;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw nodehammer::Error{
            nodehammer::codes::kFatalSkillsInstall,
            std::format("cannot copy {} to {}: {}", from.string(), to.string(), ec.message()),
            to.string()};
    }
}

} // namespace

namespace nodehammer::cli::detail {

void registerCmdSkills(CLI::App &app, const RunOptions &options) {
    const Narrator say{options};

    auto *sub =
        app.add_subcommand("skills", "Agent skills bundled with this build")->require_subcommand(1);

    // ── list ─────────────────────────────────────────────────────────────────
    auto *listSub = sub->add_subcommand("list", "Show the skills carried in this binary");
    auto *listFormatOpt = listSub->add_option("--output-format", "Report format: text, json")
                              ->default_val("text")
                              ->check(CLI::IsMember({"text", "json"}));

    listSub->callback([=] {
        runOrReport("skills list", [&] {
            const auto skills = embeddedSkills();
            if (listFormatOpt->as<std::string>() == "json") {
                nlohmann::json rows = nlohmann::json::array();
                for (const auto &skill : skills) {
                    nlohmann::json files = nlohmann::json::array();
                    std::size_t total = 0;
                    for (const auto &file : skill.files) {
                        files.push_back({{"path", file.path}, {"bytes", file.bytes.size()}});
                        total += file.bytes.size();
                    }
                    rows.push_back(
                        {{"name", skill.name}, {"files", std::move(files)}, {"bytes", total}});
                }
                std::println("{}", nlohmann::json{{"schema", kJsonSchema},
                                                  {"kind", "skills"},
                                                  {"skills", std::move(rows)}}
                                       .dump(2));
                return;
            }

            if (skills.empty()) {
                // Not a failure: a build can legitimately have been configured
                // before `skills/` had anything in it. It is still worth saying
                // plainly, because the alternative reading -- "install did
                // nothing" -- is the one a user would otherwise reach.
                std::println("no skills are bundled in this build");
                return;
            }
            for (const auto &skill : skills) {
                std::size_t total = 0;
                for (const auto &file : skill.files) {
                    total += file.bytes.size();
                }
                std::println("{}  {} file(s), {} bytes", skill.name, skill.files.size(), total);
                for (const auto &file : skill.files) {
                    std::println("    {}", file.path);
                }
            }
        });
    });

    // ── install ──────────────────────────────────────────────────────────────
    auto *installSub =
        sub->add_subcommand("install", "Copy the skills where agents will read them");
    auto *namesOpt = installSub->add_option("names", "Skills to install (default: all of them)")
                         ->type_name("NAME");
    auto *scopeOpt = installSub->add_option("--scope", "Install under your home, or this project")
                         ->default_val("user")
                         ->check(CLI::IsMember({"user", "project"}));
    auto *dirOpt =
        installSub->add_option("--dir", "Install into this directory instead")->type_name("PATH");
    auto *forceOpt = installSub->add_flag("--force", "Replace something that is not ours");
    auto *dryRunOpt = installSub->add_flag("--dry-run", "Say what would be written, write nothing");

    installSub->callback([=] {
        runOrReport("skills install", [&] {
            const bool force = forceOpt->count() > 0;
            const bool dryRun = dryRunOpt->count() > 0;

            // Which skills. An empty list means all of them, which is what makes
            // adding a second skill to the repository a no-op for every caller
            // that already types the bare command.
            std::vector<std::string> names;
            if (*namesOpt) {
                namesOpt->results(names);
            }
            std::vector<const EmbeddedSkill *> wanted;
            if (names.empty()) {
                for (const auto &skill : embeddedSkills()) {
                    wanted.push_back(&skill);
                }
                if (wanted.empty()) {
                    throw Error{codes::kFatalSkillsUnknown,
                                "this build bundles no skills, so there is nothing to install"};
                }
            } else {
                for (const auto &name : names) {
                    const EmbeddedSkill *skill = findSkill(name);
                    if (skill == nullptr) {
                        throw Error{codes::kFatalSkillsUnknown,
                                    std::format("no bundled skill named '{}'. This build has: {}",
                                                name, knownSkillNames()),
                                    name};
                    }
                    wanted.push_back(skill);
                }
            }

            // Where. `--dir` is one plain copy and no second location: a caller
            // that named a directory is staging, not installing, and inventing a
            // `.agents` layer underneath would surprise them.
            const bool explicitDir = dirOpt->count() > 0;
            fs::path root;
            if (explicitDir) {
                root = fs::path{dirOpt->as<std::string>()};
            } else if (scopeOpt->as<std::string>() == "project") {
                root = projectRoot();
            } else {
                root = homeRoot();
            }

            for (const EmbeddedSkill *skill : wanted) {
                const std::string name{skill->name};
                const fs::path real = explicitDir ? root / name : root / kAgentsSkills / name;

                // Planned in both modes, so a run that would refuse says so
                // here rather than at the write -- which is most of what a dry
                // run is for. Only the writing path acts on the answer.
                const Occupant plan = planDestination(real, {}, force);
                if (dryRun) {
                    std::println("{}", real.string());
                } else {
                    clearDestination(plan, real, say);
                    std::error_code ec;
                    fs::create_directories(real, ec);
                    if (ec) {
                        throw Error{
                            codes::kFatalSkillsInstall,
                            std::format("cannot create {}: {}", real.string(), ec.message()),
                            real.string()};
                    }
                    writeSkill(*skill, real);
                    // stdout is the answer: where the skill now lives, one line
                    // per skill, so a caller can pipe it.
                    std::println("{}", real.string());
                }

                if (explicitDir) {
                    continue;
                }

                // Claude Code discovers skills only under `.claude/skills`, and
                // follows a symlink out of it. So one copy serves both locations
                // and they cannot drift.
                //
                // Only when `.claude/` is already there. Creating it would be
                // inventing a config directory for a tool the user may not have;
                // its absence is the signal, and it is respected.
                const fs::path claudeHome = root / ".claude";
                std::error_code ec;
                if (!fs::is_directory(claudeHome, ec)) {
                    say("{} does not exist, so nothing was linked for Claude Code",
                        claudeHome.string());
                    continue;
                }
                const fs::path link = root / kClaudeSkills / name;
                const Occupant linkPlan = planDestination(link, real, force);
                if (dryRun) {
                    std::println("{}", link.string());
                    continue;
                }
                clearDestination(linkPlan, link, say);
                fs::create_directories(link.parent_path(), ec);
                if (ec) {
                    throw Error{codes::kFatalSkillsInstall,
                                std::format("cannot create {}: {}", link.parent_path().string(),
                                            ec.message()),
                                link.parent_path().string()};
                }
                try {
                    fs::create_directory_symlink(real, link);
                } catch (const fs::filesystem_error &) {
                    // Windows without developer mode, and some filesystems. A
                    // copy that can drift beats an install that failed.
                    say("cannot symlink here, so {} is a copy rather than a link", link.string());
                    copyTree(real, link);
                }
                std::println("{}", link.string());
            }

            if (dryRun) {
                say("dry run: nothing was written");
            }
        });
    });
}

} // namespace nodehammer::cli::detail
