/********************************************************************************
 *
 * This file is part of the Geneva library collection. The following license
 * applies to this file:
 *
 * ------------------------------------------------------------------------------
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ------------------------------------------------------------------------------
 *
 * Note that other files in the Geneva library collection may use a different
 * license. Please see the licensing information in each file.
 *
 ********************************************************************************
 *
 * See the NOTICE file in the top-level directory of the Geneva library
 * collection for a list of contributors and copyright information.
 *
 ********************************************************************************/

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h> // sched_getaffinity
#endif
#include <unistd.h> // geteuid

#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

using namespace Gem::Common;

// Each filesystem TEST_CASE uses a unique path inside the system temp dir so
// concurrent runs do not collide.
namespace {
std::filesystem::path scratch(std::string const &tag) {
    auto base = std::filesystem::temp_directory_path() / "gem_helper_tests";
    std::filesystem::create_directories(base);
    return base / tag;
}
} // namespace

namespace {

/** @brief A fresh, empty directory for one test case */
std::filesystem::path freshDir(std::string const &tag) {
    auto const dir = scratch(tag);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/** @brief The number of entries in @p dir (target and temporary files alike) */
std::size_t entriesIn(std::filesystem::path const &dir) {
    std::size_t n = 0;
    for([[maybe_unused]] auto const &entry : std::filesystem::directory_iterator(dir)) {
        ++n;
    }
    return n;
}

/** @brief The whole content of @p p */
std::string wholeFile(std::filesystem::path const &p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

/** @brief Writes @p text through replaceFileAtomically() */
void replaceWith(std::filesystem::path const &target, std::string const &text) {
    replaceFileAtomically(target, [&text](std::ostream &os) { os << text; }, "GUnitTest");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// serializationModeToString — pure constexpr lookup.

TEST_CASE(
    "serializationModeToString: maps known modes to their labels",
    "[common][helper-nonT][serialization-mode]"
) {
    static_assert(
        serializationModeToString(serializationMode::GEM_BINARY) == "GArchive binary mode"
    );
    static_assert(serializationModeToString(serializationMode::GEM_JSON) == "GArchive JSON mode");
    SUCCEED();
}

// ---------------------------------------------------------------------------
// getNHardwareThreads — sanity bounds only (machine-dependent value).

TEST_CASE("getNHardwareThreads: returns a positive value", "[common][helper-nonT][threads]") {
    CHECK(getNHardwareThreads() >= 1u);
    // Default cap; the function applies a maximum to guard against silly
    // values. Bound is generous enough to survive 256-core CI rigs.
    CHECK(getNHardwareThreads() <= 4096u);
    // Read once: the same number on every call
    CHECK(getNHardwareThreads() == getNHardwareThreads());
}

// Regression: getNHardwareThreads() reported the hardware threads of the machine, also for a
// process that a batch system or taskset had given a few of them. The test is meaningful in the
// run that CTest makes under "taskset -c 0" (GCommonHelperFunctionsTests_affinity).
TEST_CASE(
    "getNHardwareThreads: counts the threads assigned to the process",
    "[common][helper-nonT][threads][affinity]"
) {
#if defined(__linux__)
    cpu_set_t assigned;
    CPU_ZERO(&assigned);
    if(0 == sched_getaffinity(0, sizeof(assigned), &assigned)) {
        CHECK(getNHardwareThreads() == static_cast<unsigned int>(CPU_COUNT(&assigned)));
    }
    else { // more processors than a cpu_set_t holds: the function falls back to the machine's
        CHECK(getNHardwareThreads() >= 1u);
    }
#else
    CHECK(getNHardwareThreads() >= 1u);
#endif
}

TEST_CASE(
    "DEFAULTMAXNHARDWARETHREADS: not below the fallback thread count",
    "[common][helper-nonT][threads]"
) {
    STATIC_CHECK(DEFAULTNHARDWARETHREADS >= 1u);
    STATIC_CHECK(DEFAULTMAXNHARDWARETHREADS >= DEFAULTNHARDWARETHREADS);
}

// ---------------------------------------------------------------------------
// randomBits64 -- whether two engines share a seed is a matter of probability that a unit test
// cannot show (see the function); what can be pinned is that the draws of many threads differ.

TEST_CASE(
    "randomBits64: the draws of eight threads all differ",
    "[common][helper-nonT][random-bits]"
) {
    constexpr std::size_t N_THREADS = 8;
    constexpr std::size_t N_DRAWS = 10000;
    std::vector<std::vector<std::uint64_t>> draws(N_THREADS);
    {
        std::vector<std::jthread> threads;
        for(auto &mine : draws) {
            threads.emplace_back([&mine] {
                for(std::size_t i = 0; i < N_DRAWS; ++i) {
                    mine.push_back(randomBits64());
                }
            });
        }
    }
    std::set<std::uint64_t> all;
    for(const auto &mine : draws) {
        all.insert(mine.begin(), mine.end());
    }
    CHECK(all.size() == N_THREADS * N_DRAWS);
}

// ---------------------------------------------------------------------------
// touch_time / loadTextDataFromFile / loadTextLinesFromFile

TEST_CASE("touch_time: creates a file with the given content", "[common][helper-nonT][file]") {
    auto p = scratch("touch_create.txt");
    std::filesystem::remove(p);

    auto t = touch_time(p, "hello world", false);
    REQUIRE(std::filesystem::exists(p));
    CHECK(loadTextDataFromFile(p) == "hello world");
    (void)t; // returned timestamp is informational

    std::filesystem::remove(p);
}

TEST_CASE(
    "touch_time: re-touch truncates the file to the new content",
    "[common][helper-nonT][file]"
) {
    // touch_time opens the path with std::ofstream (default trunc), so every
    // call replaces the existing content. This pins that contract down.
    auto p = scratch("touch_truncates.txt");
    std::filesystem::remove(p);

    touch_time(p, "before", false);
    REQUIRE(loadTextDataFromFile(p) == "before");
    touch_time(p, "after", false);
    CHECK(loadTextDataFromFile(p) == "after");

    std::filesystem::remove(p);
}

TEST_CASE(
    "touch_time: remove_if_not_present cleans up newly-created files",
    "[common][helper-nonT][file]"
) {
    auto p = scratch("touch_remove.txt");
    std::filesystem::remove(p);

    touch_time(p, "x", true); // path did not exist, so this should remove it
    CHECK_FALSE(std::filesystem::exists(p));
}

TEST_CASE(
    "loadTextLinesFromFile: returns one entry per non-empty line",
    "[common][helper-nonT][file]"
) {
    auto p = scratch("lines.txt");
    {
        std::ofstream o(p);
        o << "first\nsecond\nthird\n";
    }
    auto lines = loadTextLinesFromFile(p);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0] == "first");
    CHECK(lines[1] == "second");
    CHECK(lines[2] == "third");

    std::filesystem::remove(p);
}

// ---------------------------------------------------------------------------
// splitString / stringToUIntVec / stringToDoubleVec / stringToUIntTupleVec

TEST_CASE("splitString: splits on a separator character", "[common][helper-nonT][split]") {
    auto v = splitString("a,b,c", ",");
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "b");
    CHECK(v[2] == "c");
}

TEST_CASE(
    "splitString: empty input returns empty vector or single empty element",
    "[common][helper-nonT][split]"
) {
    auto v = splitString("", ",");
    CHECK(v.size() <= 1u); // boost::split semantics differ across versions
}

TEST_CASE(
    "splitString: a separator of more than one character throws",
    "[common][helper-nonT][split]"
) {
    CHECK_THROWS_AS(splitString("a,b", ",;"), gem_exception);
    CHECK_THROWS_AS(splitString("a,b", ""), gem_exception);
}

TEST_CASE("stringToUIntVec: parses comma-separated unsigned ints", "[common][helper-nonT][split]") {
    auto v = stringToUIntVec("1,2,3,4", ',');
    REQUIRE(v.size() == 4);
    CHECK(v[0] == 1u);
    CHECK(v[3] == 4u);
}

TEST_CASE("stringToDoubleVec: parses comma-separated doubles", "[common][helper-nonT][split]") {
    auto v = stringToDoubleVec("1.5,2.5,3.5");
    REQUIRE(v.size() == 3);
    CHECK(v[0] == 1.5);
    CHECK(v[2] == 3.5);
}

TEST_CASE(
    "stringToUIntTupleVec: parses a sequence of parenthesized pairs",
    "[common][helper-nonT][split]"
) {
    auto v = stringToUIntTupleVec("(1,2),(3,4)");
    REQUIRE(v.size() == 2);
    CHECK(std::get<0>(v[0]) == 1u);
    CHECK(std::get<1>(v[0]) == 2u);
    CHECK(std::get<0>(v[1]) == 3u);
    CHECK(std::get<1>(v[1]) == 4u);
}

// The messages said "Parsing failed." and where the reading stopped, but neither the text nor what
// was expected (and carried a stray '": ').
TEST_CASE(
    "the list readers name the text and what was expected and where it stopped",
    "[common][helper-nonT][split]"
) {
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        stringToUIntVec("1,x,3", ','),
        ContainsSubstring("\"1,x,3\"") && ContainsSubstring("integers >= 0") &&
            ContainsSubstring("at \"x,3\"")
    );
    CHECK_THROWS_WITH(
        stringToDoubleVec("1.5;2"),
        ContainsSubstring("\"1.5;2\"") && ContainsSubstring("numbers") &&
            ContainsSubstring("at \";2\"")
    );
    CHECK_THROWS_WITH(
        stringToUIntTupleVec("(1,2),(3,x)"),
        ContainsSubstring("\"(1,2),(3,x)\"") && ContainsSubstring("pairs") &&
            ContainsSubstring("at \"x)\"")
    );
}

// ---------------------------------------------------------------------------
// duration_from_string

TEST_CASE(
    "duration_from_string: 'HH:MM:SS' converts to a duration<double>",
    "[common][helper-nonT][duration]"
) {
    auto d = duration_from_string("00:01:30");
    CHECK(d.count() == 90.0);

    auto d2 = duration_from_string("01:00:00");
    CHECK(d2.count() == 3600.0);
}

// ---------------------------------------------------------------------------
// currentTimeAsString / getMSSince1970: sanity checks (non-empty / numeric).

TEST_CASE(
    "currentTimeAsString: returns a non-empty timestamp string",
    "[common][helper-nonT][time]"
) {
    CHECK_FALSE(currentTimeAsString().empty());
}

TEST_CASE(
    "getMSSince1970: returns a numeric string with reasonable magnitude",
    "[common][helper-nonT][time]"
) {
    auto s = getMSSince1970();
    REQUIRE_FALSE(s.empty());
    for(char const c : s) {
        CHECK(std::isdigit(static_cast<unsigned char>(c)));
    }
    // > 1e12 (well past year 2001 in ms)
    auto ms = std::stoll(s);
    CHECK(ms > 1'000'000'000'000LL);
}

// ---------------------------------------------------------------------------
// time_point_to_milliseconds / milliseconds_to_time_point

TEST_CASE("time_point round-trip via ms representation", "[common][helper-nonT][time]") {
    auto now = std::chrono::high_resolution_clock::now();
    auto ms = time_point_to_milliseconds(now);
    auto rt = milliseconds_to_time_point(ms);

    // Re-encoding the recovered time_point should give an identical ms count
    // (the round-trip is lossy below ms precision but exact at ms granularity).
    CHECK(time_point_to_milliseconds(rt) == ms);
}

// ---------------------------------------------------------------------------
// condnotset

TEST_CASE("condnotset: raises gem_exception", "[common][helper-nonT][condnotset]") {
    CHECK_THROWS_AS(condnotset("SOME_DEFINE", "test-context"), gem_exception);
}

// ---------------------------------------------------------------------------
// backupExistingFile
//
// Regression guard. The output sites of the optimization monitors once carried hand-copied
// versions of this block, and one copy had drifted: it renamed one file while its warning named
// another. Routing every site through a single function that takes the name once made that class
// of defect unrepresentable; today the recorder's file and plot sinks (Gem::Common::Record) are
// those sites. These cases pin the function's contract.

namespace {

/** @brief A log sink that records every message it is handed, so the warning can be inspected */
struct BackupCapturingTarget : Gem::Common::GBaseLogTarget {
    void log(const std::string &msg) const override {
        messages.push_back(msg);
    }
    void logWithSource(const std::string &msg, const std::string &) const override {
        messages.push_back(msg);
    }
    mutable std::vector<std::string> messages;
};

} // anonymous namespace

TEST_CASE(
    "backupExistingFile: renames an existing file out of the way",
    "[common][helper-nonT][backup]"
) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "gem-backup-existing-file-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::filesystem::path target = dir / "results.txt";
    {
        std::ofstream ofs(target);
        ofs << "original content" << std::endl;
    }
    REQUIRE(std::filesystem::exists(target));

    // A decoy that must never be named: the original defect was a block that renamed one file
    // while its warning reported a DIFFERENT member, so the diagnostic sent the user looking at
    // an untouched file. Pin the message to the file actually acted upon.
    const std::filesystem::path decoy = dir / "unrelated-other-output.txt";
    {
        std::ofstream ofs(decoy);
        ofs << "decoy" << std::endl;
    }

    auto capture = std::make_shared<BackupCapturingTarget>();
    glogger.addLogTarget(capture);

    CHECK(Gem::Common::backupExistingFile(target.string(), "GUnitTest: Warning!"));

    // Match the "which file?" line SPECIFICALLY. Checking only that the target path occurs
    // somewhere in the message is not enough: the backup name is the target name plus a
    // suffix, so it contains the target path as a substring and would mask a wrong report.
    const std::string expected_line = "Attempt to output information to file " + target.string();

    bool names_the_renamed_file = false;
    for(const auto &m : capture->messages) {
        if(m.find("GUnitTest: Warning!") != std::string::npos &&
           m.find(expected_line) != std::string::npos) {
            names_the_renamed_file = true;
        }
        // ...and no message may mention a file this call did not touch.
        CHECK(m.find(decoy.filename().string()) == std::string::npos);
    }
    CHECK(names_the_renamed_file);

    glogger.resetLogTargets();
    std::filesystem::remove(decoy);

    // The original name is free again, and exactly one backup carrying the content exists.
    CHECK_FALSE(std::filesystem::exists(target));

    std::vector<std::filesystem::path> backups;
    for(const auto &entry : std::filesystem::directory_iterator(dir)) {
        if(entry.path().filename().string().starts_with("results.txt.bak_")) {
            backups.push_back(entry.path());
        }
    }
    REQUIRE(backups.size() == 1);

    std::ifstream ifs(backups.front());
    std::string content;
    std::getline(ifs, content);
    CHECK(content == "original content");

    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "backupExistingFile: does nothing when the file is absent",
    "[common][helper-nonT][backup]"
) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "gem-backup-absent-file-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::filesystem::path target = dir / "never-written.txt";
    REQUIRE_FALSE(std::filesystem::exists(target));

    CHECK_FALSE(Gem::Common::backupExistingFile(target.string(), "GUnitTest: Warning!"));

    // No stray backup was invented for a file that was never there.
    CHECK(std::filesystem::is_empty(dir));

    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "backupExistingFile: backups within one millisecond keep their own names",
    "[common][helper-nonT][backup]"
) {
    // Regression: the backup's name carried only the millisecond, and a rename over an existing
    // file succeeds silently, so a second backup within one millisecond replaced the first.
    constexpr std::size_t N_BACKUPS = 20;
    auto const dir = freshDir("backup-same-millisecond");
    auto const target = dir / "results.txt";

    for(std::size_t i = 0; i < N_BACKUPS; ++i) {
        std::ofstream(target) << "content " << i;
        REQUIRE(backupExistingFile(target.string(), "GUnitTest: Warning!"));
    }

    std::set<std::string> contents;
    for(auto const &entry : std::filesystem::directory_iterator(dir)) {
        contents.insert(wholeFile(entry.path()));
    }
    CHECK(contents.size() == N_BACKUPS);
    CHECK(entriesIn(dir) == N_BACKUPS);

    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "backupExistingFile: a failed rename is a gem_exception and no backup is reported",
    "[common][helper-nonT][backup]"
) {
    // Regression: the warning that names the backup came before the rename, and a failed rename
    // escaped as a std::filesystem::filesystem_error.
    if(::geteuid() == 0) {
        SKIP("run as root: a directory without write permission does not stop the rename");
    }
    auto const dir = freshDir("backup-rename-fails");
    auto const target = dir / "results.txt";
    std::ofstream(target) << "original";
    std::filesystem::permissions(
        dir,
        std::filesystem::perms::owner_write,
        std::filesystem::perm_options::remove
    );

    auto capture = std::make_shared<BackupCapturingTarget>();
    glogger.addLogTarget(capture);
    CHECK_THROWS_AS(backupExistingFile(target.string(), "GUnitTest: Warning!"), gem_exception);
    glogger.resetLogTargets();
    for(auto const &m : capture->messages) {
        CHECK(m.find(".bak_") == std::string::npos);
    }

    std::filesystem::permissions(dir, std::filesystem::perms::owner_all);
    CHECK(wholeFile(target) == "original");
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// replaceFileAtomically -- what the declaration promises: the new content as a whole, or nothing
// changes, and no temporary file is left behind; also with several writers of one file at once.

TEST_CASE(
    "replaceFileAtomically: creates and replaces a file and leaves no temporary file",
    "[common][helper-nonT][replace-file]"
) {
    auto const dir = freshDir("replace-creates");
    auto const target = dir / "data.txt";

    replaceWith(target, "first");
    CHECK(wholeFile(target) == "first");
    replaceWith(target, "second, longer content");
    CHECK(wholeFile(target) == "second, longer content");
    CHECK(entriesIn(dir) == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "replaceFileAtomically: a throwing writer leaves the target as it was",
    "[common][helper-nonT][replace-file]"
) {
    auto const dir = freshDir("replace-throws");
    auto const target = dir / "data.txt";
    replaceWith(target, "old");

    CHECK_THROWS_AS(
        replaceFileAtomically(
            target,
            [](std::ostream &os) {
                os << "half of the new";
                throw std::runtime_error("the writer fails");
            },
            "GUnitTest"
        ),
        std::runtime_error // passed on as it is
    );
    CHECK(wholeFile(target) == "old");
    CHECK(entriesIn(dir) == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "replaceFileAtomically: a failed stream is a gem_exception and leaves the target as it was",
    "[common][helper-nonT][replace-file]"
) {
    auto const dir = freshDir("replace-stream-fails");
    auto const target = dir / "data.txt";
    replaceWith(target, "old");

    // A full disk shows as a failed stream; the writer sets the state that a failed write sets
    CHECK_THROWS_AS(
        replaceFileAtomically(
            target,
            [](std::ostream &os) {
                os << "new";
                os.setstate(std::ios::badbit);
            },
            "GUnitTest"
        ),
        gem_exception
    );
    CHECK(wholeFile(target) == "old");
    CHECK(entriesIn(dir) == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "replaceFileAtomically: a missing directory is a gem_exception",
    "[common][helper-nonT][replace-file]"
) {
    auto const dir = freshDir("replace-no-dir");
    CHECK_THROWS_AS(replaceWith(dir / "absent" / "data.txt", "x"), gem_exception);
    CHECK(entriesIn(dir) == 0);

    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "replaceFileAtomically: a target that cannot be replaced is a gem_exception",
    "[common][helper-nonT][replace-file]"
) {
    auto const dir = freshDir("replace-target-is-dir");
    auto const target = dir / "data.txt";
    std::filesystem::create_directories(target / "inside"); // a non-empty directory
    CHECK_THROWS_AS(replaceWith(target, "x"), gem_exception);
    CHECK(std::filesystem::is_directory(target / "inside"));
    CHECK(entriesIn(dir) == 1); // the temporary file is gone

    std::filesystem::remove_all(dir);
}

TEST_CASE(
    "replaceFileAtomically: several writers of one file at once",
    "[common][helper-nonT][replace-file]"
) {
    // Regression: the temporary name depended on the target only, so writers that replaced one
    // file at the same time filled one temporary file; a rename published a mixture, and the
    // writer whose temporary file had already been renamed away failed with "No such file".
    constexpr std::size_t N_WRITERS = 8;
    constexpr std::size_t N_ROUNDS = 200;
    constexpr std::size_t LENGTH = 8192;

    auto const dir = freshDir("replace-several-writers");
    auto const target = dir / "data.txt";
    replaceWith(target, std::string(LENGTH, 'a'));

    // A content is whole if it is LENGTH times the letter of one writer
    auto const whole = [](std::string const &content) {
        return content.size() == LENGTH &&
               std::ranges::all_of(content, [&](char c) { return c == content.front(); });
    };

    std::atomic<std::size_t> failed_writes{0};
    std::atomic<std::size_t> partial_reads{0};
    std::atomic<bool> writing{true};
    std::vector<std::thread> writers;
    for(std::size_t w = 0; w < N_WRITERS; ++w) {
        writers.emplace_back([&, w] {
            std::string const mine(LENGTH, static_cast<char>('a' + w));
            for(std::size_t r = 0; r < N_ROUNDS; ++r) {
                try {
                    replaceWith(target, mine);
                }
                catch(...) {
                    ++failed_writes;
                }
            }
        });
    }
    std::thread reader([&] {
        while(writing) {
            if(not whole(wholeFile(target))) {
                ++partial_reads;
            }
        }
    });
    for(auto &t : writers) {
        t.join();
    }
    writing = false;
    reader.join();

    CHECK(failed_writes == 0);
    CHECK(partial_reads == 0);
    CHECK(whole(wholeFile(target)));
    CHECK(entriesIn(dir) == 1);

    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------
// runExternalCommand -- programs are started without a shell. The tests run sh with a script of
// their own as an ordinary program; "sh" is given without a directory, so the PATH lookup is
// exercised too.

namespace {
std::string fileContent(std::filesystem::path const &p) {
    std::ifstream in(p);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}
} // anonymous namespace

TEST_CASE(
    "runExternalCommand: an argument with spaces arrives as one argument",
    "[common][helper-nonT][external-command]"
) {
    // Regression: the former implementation joined the arguments into one string for system(), so
    // the shell split "two  words here" into three arguments
    auto const out = scratch("external-command-spaces.txt");
    std::string full_command;
    int const code = runExternalCommand(
        "sh",
        {"-c", R"(printf '%s|%s' "$#" "$1")", "sh", "two  words here"},
        out,
        full_command
    );
    CHECK(code == 0);
    CHECK(fileContent(out) == "1|two  words here");
    // Shown as a shell would need it: words with spaces or quotes in single quotes
    CHECK(full_command.starts_with(
        "sh -c 'printf '\\''%s|%s'\\'' \"$#\" \"$1\"' sh 'two  words here' > "
    ));
    std::filesystem::remove(out);
}

TEST_CASE(
    "runExternalCommand: standard output and standard error go to the emptied output file",
    "[common][helper-nonT][external-command]"
) {
    auto const out = scratch("external-command-output.txt");
    {
        std::ofstream old(out);
        old << "content of an earlier run that must be gone";
    }
    std::string full_command;
    int const code =
        runExternalCommand("sh", {"-c", "echo to-stdout; echo to-stderr >&2"}, out, full_command);
    CHECK(code == 0);
    CHECK(fileContent(out) == "to-stdout\nto-stderr\n");
    std::filesystem::remove(out);
}

TEST_CASE(
    "runExternalCommand: returns the exit status or the signal that ended the program",
    "[common][helper-nonT][external-command]"
) {
    std::string full_command;
    CHECK(runExternalCommand("sh", {"-c", "exit 3"}, {}, full_command) == 3);
    CHECK(runExternalCommand("sh", {"-c", "exit 0"}, {}, full_command) == 0);
    CHECK(runExternalCommand("sh", {"-c", "kill -TERM $$"}, {}, full_command) == 15);
}

TEST_CASE(
    "runExternalCommand: a missing program raises gem_exception",
    "[common][helper-nonT][external-command]"
) {
    std::string full_command;
    // With a directory part the path is used as given; without one PATH is searched
    CHECK_THROWS_AS(
        runExternalCommand("/gem-no-such-directory/gem-no-such-program", {"x"}, {}, full_command),
        gem_exception
    );
    CHECK(full_command == "/gem-no-such-directory/gem-no-such-program x");
    CHECK_THROWS_AS(
        runExternalCommand("gem-no-such-program-in-path", {}, {}, full_command),
        gem_exception
    );
}

TEST_CASE(
    "runExternalCommand: concurrent calls from several threads keep their own arguments and output",
    "[common][helper-nonT][external-command]"
) {
    constexpr int n_threads = 8;
    constexpr int calls_per_thread = 5;
    std::vector<int> failures(n_threads, 0);
    {
        std::vector<std::jthread> threads;
        for(int t = 0; t < n_threads; ++t) {
            threads.emplace_back([t, &failures] {
                auto const out = scratch("external-command-thread-" + std::to_string(t) + ".txt");
                std::string const word = "thread " + std::to_string(t) + " says hello";
                for(int i = 0; i < calls_per_thread; ++i) {
                    std::string full_command;
                    int const code = runExternalCommand(
                        "sh",
                        {"-c", R"(printf '%s' "$1"; exit "$2")", "sh", word, std::to_string(t % 3)},
                        out,
                        full_command
                    );
                    if(code != t % 3 or fileContent(out) != word) {
                        ++failures[static_cast<std::size_t>(t)];
                    }
                }
                std::filesystem::remove(out);
            });
        }
    }
    for(int t = 0; t < n_threads; ++t) {
        CHECK(failures[static_cast<std::size_t>(t)] == 0);
    }
}
