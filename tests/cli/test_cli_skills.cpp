// `skills list | install`, and the clobber table above all.
//
// The install path runs repeatedly, over a directory named after this tool, in a
// place people also edit by hand -- so every row of the table in
// src/cli/cmd_skills.cpp has a case here. The rest of the command is a file
// copy; this is the part that can destroy somebody's work.

#include "cli_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>

namespace fs = std::filesystem;

namespace {

/// A home directory the tests own, with HOME pointed at it for the duration.
///
/// Restoring in the destructor matters more than usual: `skills install` writes
/// into `$HOME`, so a leaked override would have a later test -- or a later
/// *run* -- writing into the developer's real home.
class ScopedHome {
  public:
    ScopedHome() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                std::format("nh_skills_test_{}_{}", nhtest::currentProcessId(), tick);
        fs::create_directories(path_);
        if (const char *previous = std::getenv("HOME"); previous != nullptr) {
            saved_ = previous;
            hadSaved_ = true;
        }
        setHome(path_.string());
    }
    ~ScopedHome() {
        if (hadSaved_) {
            setHome(saved_);
        } else {
            unsetHome();
        }
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScopedHome(const ScopedHome &) = delete;
    ScopedHome &operator=(const ScopedHome &) = delete;

    [[nodiscard]] const fs::path &path() const { return path_; }
    [[nodiscard]] fs::path agents() const { return path_ / ".agents" / "skills" / "nodehammer"; }
    [[nodiscard]] fs::path claude() const { return path_ / ".claude" / "skills" / "nodehammer"; }

    /// Make `.claude/` exist, which is what turns the link on.
    void withClaudeDir() const { fs::create_directories(path_ / ".claude"); }

  private:
    static void setHome(const std::string &value) {
#ifdef _WIN32
        _putenv_s("HOME", value.c_str());
#else
        setenv("HOME", value.c_str(), 1);
#endif
    }
    static void unsetHome() {
#ifdef _WIN32
        _putenv_s("HOME", "");
#else
        unsetenv("HOME");
#endif
    }

    fs::path path_;
    std::string saved_;
    bool hadSaved_ = false;
};

void writeFile(const fs::path &target, std::string_view content) {
    fs::create_directories(target.parent_path());
    std::ofstream out{target, std::ios::binary | std::ios::trunc};
    out << content;
}

} // namespace

// ── list ─────────────────────────────────────────────────────────────────────

TEST_CASE("skills list names what the build actually carries", "[cli][skills]") {
    // The assertion that catches the whole class of packaging bug the embedding
    // exists to prevent: a build whose CMake plumbing silently produced no
    // payload still links, still runs, and installs nothing. Here that is a red
    // test rather than a user reporting an empty directory.
    const auto outcome = nhtest::runCaptured({"skills", "list"});

    INFO("stderr was: " << outcome.err);
    REQUIRE(outcome.code == 0);
    CHECK(outcome.out.find("nodehammer") != std::string::npos);
    CHECK(outcome.out.find("SKILL.md") != std::string::npos);
    CHECK(outcome.out.find("no skills are bundled") == std::string::npos);
}

TEST_CASE("skills list --output-format json reports a non-empty payload", "[cli][skills]") {
    const auto outcome = nhtest::runCaptured({"skills", "list", "--output-format", "json"});

    INFO("stderr was: " << outcome.err);
    REQUIRE(outcome.code == 0);
    const auto doc = nlohmann::json::parse(outcome.out);
    CHECK(doc.at("schema").get<int>() == 1);
    CHECK(doc.at("kind").get<std::string>() == "skills");
    REQUIRE(doc.at("skills").size() >= 1);

    const auto &skill = doc.at("skills").at(0);
    CHECK(skill.at("name").get<std::string>() == "nodehammer");
    // Bytes, not just presence: an embedded file of length zero would satisfy
    // every structural check above and still be a broken build.
    CHECK(skill.at("bytes").get<std::size_t>() > 0);
    REQUIRE(skill.at("files").size() >= 1);
    CHECK(skill.at("files").at(0).at("bytes").get<std::size_t>() > 0);
}

// ── install: destinations ────────────────────────────────────────────────────

TEST_CASE("skills install writes .agents and links .claude when it exists", "[cli][skills]") {
    const ScopedHome home;
    home.withClaudeDir();

    const auto outcome = nhtest::runCaptured({"skills", "install"});

    INFO("stderr was: " << outcome.err);
    REQUIRE(outcome.code == 0);

    // The real copy is a real directory holding real bytes.
    REQUIRE(fs::is_directory(home.agents()));
    REQUIRE(fs::is_regular_file(home.agents() / "SKILL.md"));
    CHECK(fs::file_size(home.agents() / "SKILL.md") > 0);

    // Claude Code's copy is a link to it, not a second copy -- so the two cannot
    // drift, which is the whole reason for the arrangement.
    REQUIRE(fs::is_symlink(home.claude()));
    CHECK(fs::weakly_canonical(home.claude()) == fs::weakly_canonical(home.agents()));

    // stdout is the answer: both paths, one per line.
    CHECK(outcome.out.find(home.agents().string()) != std::string::npos);
    CHECK(outcome.out.find(home.claude().string()) != std::string::npos);
}

TEST_CASE("skills install invents no .claude directory", "[cli][skills]") {
    // Absence is the signal that the user does not have Claude Code, and
    // creating the directory would be inventing a config home for a tool they
    // may never install.
    const ScopedHome home;

    const auto outcome = nhtest::runCaptured({"skills", "install"});

    REQUIRE(outcome.code == 0);
    CHECK(fs::is_directory(home.agents()));
    CHECK_FALSE(fs::exists(home.path() / ".claude"));
}

TEST_CASE("skills install --dir makes one plain copy", "[cli][skills]") {
    const ScopedHome home;
    const fs::path staged = home.path() / "staged";

    const auto outcome = nhtest::runCaptured({"skills", "install", "--dir", staged.string()});

    REQUIRE(outcome.code == 0);
    CHECK(fs::is_regular_file(staged / "nodehammer" / "SKILL.md"));
    // No layer invented underneath, and no second location: a caller who named a
    // directory is staging, not installing.
    CHECK_FALSE(fs::exists(staged / ".agents"));
    CHECK_FALSE(fs::exists(home.path() / ".agents"));
}

TEST_CASE("skills install --dry-run writes nothing but says where", "[cli][skills]") {
    const ScopedHome home;
    home.withClaudeDir();

    const auto outcome = nhtest::runCaptured({"skills", "install", "--dry-run"});

    REQUIRE(outcome.code == 0);
    CHECK(outcome.out.find(home.agents().string()) != std::string::npos);
    CHECK_FALSE(fs::exists(home.agents()));
    CHECK_FALSE(fs::exists(home.claude()));
}

TEST_CASE("skills install --dry-run leaves an existing install alone", "[cli][skills]") {
    // The bug this exists for: deciding and deleting were one function, so a dry
    // run took the "replace the skill already there" branch -- removing the
    // installed skill and then, being a dry run, writing nothing in its place.
    // Asking what would happen must never be the thing that happens.
    const ScopedHome home;
    home.withClaudeDir();
    REQUIRE(nhtest::runCaptured({"skills", "install"}).code == 0);
    const auto installed = fs::file_size(home.agents() / "SKILL.md");

    const auto outcome = nhtest::runCaptured({"skills", "install", "--dry-run"});

    REQUIRE(outcome.code == 0);
    CHECK(fs::is_regular_file(home.agents() / "SKILL.md"));
    CHECK(fs::file_size(home.agents() / "SKILL.md") == installed);
    CHECK(fs::is_symlink(home.claude()));
}

TEST_CASE("skills install --dry-run refuses without touching what it refused",
          "[cli][skills]") {
    // The other half: a dry run still reports the refusal -- that is most of
    // what it is for -- and `--force` on a dry run stays a plan rather than
    // becoming the deletion it describes.
    const ScopedHome home;
    writeFile(home.agents() / "notes.md", "mine, not yours");

    const auto refused = nhtest::runCaptured({"skills", "install", "--dry-run"});
    CHECK(refused.code != 0);
    CHECK(refused.err.find("NH1201") != std::string::npos);
    CHECK(fs::exists(home.agents() / "notes.md"));

    const auto forced = nhtest::runCaptured({"skills", "install", "--dry-run", "--force"});
    CHECK(forced.code == 0);
    CHECK(fs::exists(home.agents() / "notes.md"));
}

// ── install: the clobber table ───────────────────────────────────────────────

TEST_CASE("skills install is idempotent with no flags", "[cli][skills]") {
    // The common path must not be the awkward one. A reinstall replaces a skill
    // directory and relinks its own symlink without asking, because every
    // upgrade does exactly this.
    const ScopedHome home;
    home.withClaudeDir();

    REQUIRE(nhtest::runCaptured({"skills", "install"}).code == 0);
    const auto again = nhtest::runCaptured({"skills", "install"});

    INFO("stderr was: " << again.err);
    CHECK(again.code == 0);
    CHECK(fs::is_symlink(home.claude()));
}

TEST_CASE("skills install replaces a skill wholesale, not by merging", "[cli][skills]") {
    // A file a later version of the skill drops must not survive the upgrade: a
    // stale reference the agent still reads is worse than no reference at all.
    const ScopedHome home;
    writeFile(home.agents() / "SKILL.md", "---\nname: nodehammer\n---\nold\n");
    writeFile(home.agents() / "references" / "stale.md", "dropped by a later version");

    const auto outcome = nhtest::runCaptured({"skills", "install"});

    REQUIRE(outcome.code == 0);
    CHECK_FALSE(fs::exists(home.agents() / "references" / "stale.md"));
    CHECK(fs::file_size(home.agents() / "SKILL.md") > 100);
}

TEST_CASE("skills install refuses a symlink pointing somewhere else", "[cli][skills]") {
    // Almost always a deliberate "keep this pointed at my checkout". Writing
    // through it would edit their source tree.
    const ScopedHome home;
    const fs::path checkout = home.path() / "checkout";
    fs::create_directories(checkout);
    fs::create_directories(home.agents().parent_path());
    fs::create_directory_symlink(checkout, home.agents());

    const auto refused = nhtest::runCaptured({"skills", "install"});
    CHECK(refused.code != 0);
    CHECK(refused.err.find("NH1201") != std::string::npos);
    CHECK(fs::is_symlink(home.agents()));

    const auto forced = nhtest::runCaptured({"skills", "install", "--force"});
    CHECK(forced.code == 0);
    CHECK_FALSE(fs::is_symlink(home.agents()));
    CHECK(fs::is_regular_file(home.agents() / "SKILL.md"));
}

TEST_CASE("skills install refuses a directory that holds no SKILL.md", "[cli][skills]") {
    // Under a name like this tool's, that is far more likely to be the user's
    // work than a previous install of ours.
    const ScopedHome home;
    writeFile(home.agents() / "notes.md", "mine, not yours");

    const auto refused = nhtest::runCaptured({"skills", "install"});
    CHECK(refused.code != 0);
    CHECK(refused.err.find("NH1201") != std::string::npos);
    CHECK(fs::exists(home.agents() / "notes.md"));

    const auto forced = nhtest::runCaptured({"skills", "install", "--force"});
    CHECK(forced.code == 0);
    CHECK_FALSE(fs::exists(home.agents() / "notes.md"));
}

TEST_CASE("skills install refuses a non-directory even with --force", "[cli][skills]") {
    // No escape hatch on this row. A regular file under this name is something
    // the command has no story for, and guessing is how a tool deletes work.
    const ScopedHome home;
    writeFile(home.agents(), "a file, not a skill");

    const auto outcome = nhtest::runCaptured({"skills", "install", "--force"});

    CHECK(outcome.code != 0);
    CHECK(outcome.err.find("NH1201") != std::string::npos);
    CHECK(fs::is_regular_file(home.agents()));
}

// ── install: selection ───────────────────────────────────────────────────────

TEST_CASE("skills install repairs a link whose target went missing", "[cli][skills]") {
    // A reachable state: `.agents/skills/<name>` deleted by hand, or a write that
    // failed after the link was made. `weakly_canonical` does not follow a
    // dangling link, so comparing only that would read this as somebody else's
    // link and refuse the reinstall that repairs it.
    const ScopedHome home;
    home.withClaudeDir();
    REQUIRE(nhtest::runCaptured({"skills", "install"}).code == 0);
    fs::remove_all(home.agents());
    REQUIRE(fs::is_symlink(home.claude()));

    const auto outcome = nhtest::runCaptured({"skills", "install"});

    INFO("stderr was: " << outcome.err);
    CHECK(outcome.code == 0);
    CHECK(fs::is_regular_file(home.agents() / "SKILL.md"));
    CHECK(fs::is_symlink(home.claude()));
    CHECK(fs::exists(home.claude() / "SKILL.md"));
}

TEST_CASE("skills install names what it has when asked for something else", "[cli][skills]") {
    const ScopedHome home;

    const auto outcome = nhtest::runCaptured({"skills", "install", "no-such-skill"});

    CHECK(outcome.code != 0);
    CHECK(outcome.err.find("NH1200") != std::string::npos);
    // The remedy is in the message rather than a suggestion to run another
    // command to discover it.
    CHECK(outcome.err.find("nodehammer") != std::string::npos);
}

TEST_CASE("skills install takes a skill by name", "[cli][skills]") {
    const ScopedHome home;

    const auto outcome = nhtest::runCaptured({"skills", "install", "nodehammer"});

    INFO("stderr was: " << outcome.err);
    CHECK(outcome.code == 0);
    CHECK(fs::is_regular_file(home.agents() / "SKILL.md"));
}
