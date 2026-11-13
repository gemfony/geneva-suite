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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

using namespace Gem::Common;

// Each filesystem TEST_CASE uses a unique path inside the system temp dir so
// concurrent runs do not collide.
namespace {
std::filesystem::path scratch(std::string const &tag) {
    auto base = std::filesystem::temp_directory_path() / "geneva_helper_tests";
    std::filesystem::create_directories(base);
    return base / tag;
}
} // namespace

// ---------------------------------------------------------------------------
// serializationModeToString — pure constexpr lookup.

TEST_CASE("serializationModeToString: maps known modes to their labels",
          "[common][helper-nonT][serialization-mode]") {
    static_assert(serializationModeToString(serializationMode::GEM_BINARY) == "GArchive binary mode");
    static_assert(serializationModeToString(serializationMode::GEM_JSON)   == "GArchive JSON mode");
    SUCCEED();
}

// ---------------------------------------------------------------------------
// getNHardwareThreads — sanity bounds only (machine-dependent value).

TEST_CASE("getNHardwareThreads: returns a positive value",
          "[common][helper-nonT][threads]") {
    CHECK(getNHardwareThreads() >= 1u);
    // Default cap; the function applies a maximum to guard against silly
    // values. Bound is generous enough to survive 256-core CI rigs.
    CHECK(getNHardwareThreads() <= 4096u);
}

// ---------------------------------------------------------------------------
// touch_time / loadTextDataFromFile / loadTextLinesFromFile

TEST_CASE("touch_time: creates a file with the given content",
          "[common][helper-nonT][file]") {
    auto p = scratch("touch_create.txt");
    std::filesystem::remove(p);

    auto t = touch_time(p, "hello world", false);
    REQUIRE(std::filesystem::exists(p));
    CHECK(loadTextDataFromFile(p) == "hello world");
    (void)t; // returned timestamp is informational

    std::filesystem::remove(p);
}

TEST_CASE("touch_time: re-touch truncates the file to the new content",
          "[common][helper-nonT][file]") {
    // touch_time opens the path with std::ofstream (default trunc), so every
    // call replaces the existing content. This pins that contract down.
    auto p = scratch("touch_truncates.txt");
    std::filesystem::remove(p);

    touch_time(p, "before", false);
    REQUIRE(loadTextDataFromFile(p) == "before");
    touch_time(p, "after",  false);
    CHECK(loadTextDataFromFile(p) == "after");

    std::filesystem::remove(p);
}

TEST_CASE("touch_time: remove_if_not_present cleans up newly-created files",
          "[common][helper-nonT][file]") {
    auto p = scratch("touch_remove.txt");
    std::filesystem::remove(p);

    touch_time(p, "x", true);     // path did not exist, so this should remove it
    CHECK_FALSE(std::filesystem::exists(p));
}

TEST_CASE("loadTextLinesFromFile: returns one entry per non-empty line",
          "[common][helper-nonT][file]") {
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

TEST_CASE("splitString: splits on a separator character",
          "[common][helper-nonT][split]") {
    auto v = splitString("a,b,c", ",");
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "b");
    CHECK(v[2] == "c");
}

TEST_CASE("splitString: empty input returns empty vector or single empty element",
          "[common][helper-nonT][split]") {
    auto v = splitString("", ",");
    CHECK(v.size() <= 1u);     // boost::split semantics differ across versions
}

TEST_CASE("stringToUIntVec: parses comma-separated unsigned ints",
          "[common][helper-nonT][split]") {
    auto v = stringToUIntVec("1,2,3,4", ',');
    REQUIRE(v.size() == 4);
    CHECK(v[0] == 1u);
    CHECK(v[3] == 4u);
}

TEST_CASE("stringToDoubleVec: parses comma-separated doubles",
          "[common][helper-nonT][split]") {
    auto v = stringToDoubleVec("1.5,2.5,3.5");
    REQUIRE(v.size() == 3);
    CHECK(v[0] == 1.5);
    CHECK(v[2] == 3.5);
}

TEST_CASE("stringToUIntTupleVec: parses '(a,b) (c,d)' style input",
          "[common][helper-nonT][split]") {
    auto v = stringToUIntTupleVec("(1,2),(3,4)");
    REQUIRE(v.size() == 2);
    CHECK(std::get<0>(v[0]) == 1u);
    CHECK(std::get<1>(v[0]) == 2u);
    CHECK(std::get<0>(v[1]) == 3u);
    CHECK(std::get<1>(v[1]) == 4u);
}

// ---------------------------------------------------------------------------
// duration_from_string

TEST_CASE("duration_from_string: 'HH:MM:SS' converts to a duration<double>",
          "[common][helper-nonT][duration]") {
    auto d = duration_from_string("00:01:30");
    CHECK(d.count() == 90.0);

    auto d2 = duration_from_string("01:00:00");
    CHECK(d2.count() == 3600.0);
}

// ---------------------------------------------------------------------------
// currentTimeAsString / getMSSince1970: sanity checks (non-empty / numeric).

TEST_CASE("currentTimeAsString: returns a non-empty timestamp string",
          "[common][helper-nonT][time]") {
    CHECK_FALSE(currentTimeAsString().empty());
}

TEST_CASE("getMSSince1970: returns a numeric string with reasonable magnitude",
          "[common][helper-nonT][time]") {
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

TEST_CASE("time_point round-trip via ms representation",
          "[common][helper-nonT][time]") {
    auto now = std::chrono::high_resolution_clock::now();
    auto ms  = time_point_to_milliseconds(now);
    auto rt  = milliseconds_to_time_point(ms);

    // Re-encoding the recovered time_point should give an identical ms count
    // (the round-trip is lossy below ms precision but exact at ms granularity).
    CHECK(time_point_to_milliseconds(rt) == ms);
}

// ---------------------------------------------------------------------------
// condnotset

TEST_CASE("condnotset: raises geneva_exception",
          "[common][helper-nonT][condnotset]") {
    CHECK_THROWS_AS(condnotset("SOME_DEFINE", "test-context"), geneva_exception);
}

// ---------------------------------------------------------------------------
// backupExistingFile
//
// Regression guard. The six output sites in GPluggableOptimizationMonitors used to carry a
// hand-copied version of this block, and one copy had drifted: it tested and renamed
// file_name_txt_ while its warning named file_name_pth2_, i.e. it reported a file it had not
// touched. Routing every site through a single function that takes the name once makes that
// class of defect unrepresentable; these cases pin the function's contract.

namespace {

/** @brief A log sink that records every message it is handed, so the warning can be inspected */
struct BackupCapturingTarget : Gem::Common::GBaseLogTarget {
    void log(const std::string &msg) const override { messages.push_back(msg); }
    void logWithSource(const std::string &msg, const std::string &) const override {
        messages.push_back(msg);
    }
    mutable std::vector<std::string> messages;
};

} // anonymous namespace

TEST_CASE("backupExistingFile: renames an existing file out of the way",
          "[common][helper-nonT][backup]") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "geneva-backup-existing-file-test";
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
        if(m.find("GUnitTest: Warning!") != std::string::npos
           && m.find(expected_line) != std::string::npos) {
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

TEST_CASE("backupExistingFile: does nothing when the file is absent",
          "[common][helper-nonT][backup]") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "geneva-backup-absent-file-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::filesystem::path target = dir / "never-written.txt";
    REQUIRE_FALSE(std::filesystem::exists(target));

    CHECK_FALSE(Gem::Common::backupExistingFile(target.string(), "GUnitTest: Warning!"));

    // No stray backup was invented for a file that was never there.
    CHECK(std::filesystem::is_empty(dir));

    std::filesystem::remove_all(dir);
}
