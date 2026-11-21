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

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "common/GCommonEnums.hpp"
#include "common/GExceptions.hpp"
#include "common/GJsonIO.hpp"
#include "common/GParserBuilder.hpp"

using namespace Gem::Common;
using Catch::Matchers::ContainsSubstring;

namespace {

std::filesystem::path scratch(std::string const &tag) {
    auto base = std::filesystem::temp_directory_path() / "gem_parserbuilder_tests";
    std::filesystem::create_directories(base);
    return base / (tag + ".json");
}

/** An argv for parseCommandLine(): "prog" followed by @p args; the strings live as long as it does */
class Argv {
public:
    explicit Argv(std::vector<std::string> args)
      : args_(std::move(args)) {
        args_.insert(args_.begin(), "prog");
        for(auto &arg : args_) {
            pointers_.push_back(arg.data());
        }
        pointers_.push_back(nullptr);
    }
    [[nodiscard]] int argc() const { return static_cast<int>(args_.size()); }
    [[nodiscard]] char **argv() { return pointers_.data(); }

private:
    std::vector<std::string> args_;
    std::vector<char *> pointers_;
};

/** Collects what std::cout receives while it lives */
class CoutCapture {
public:
    CoutCapture()
      : old_(std::cout.rdbuf(captured_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_); }
    CoutCapture(CoutCapture const &) = delete;
    CoutCapture(CoutCapture &&) = delete;
    CoutCapture &operator=(CoutCapture const &) = delete;
    CoutCapture &operator=(CoutCapture &&) = delete;
    [[nodiscard]] std::string text() const { return captured_.str(); }

private:
    std::ostringstream captured_;
    std::streambuf *old_;
};

} // namespace

// ---------------------------------------------------------------------------
// Empty state

TEST_CASE("GParserBuilder: fresh instance reports zero options", "[common][parser-builder]") {
    GParserBuilder const gpb;
    CHECK(gpb.numberOfFileOptions() == 0);
    CHECK(gpb.numberOfCLOptions() == 0);
}

// ---------------------------------------------------------------------------
// registerFileParameter (reference form): default value applied when missing.

TEST_CASE(
    "GParserBuilder: writeConfigFile + parseConfigFile round-trip "
    "(missing file → default values written)",
    "[common][parser-builder]"
) {
    auto cfg = scratch("rt_default");
    std::filesystem::remove(cfg);

    {
        GParserBuilder gpb;
        int answer = 0;
        double scale = 0.0;
        gpb.registerFileParameter<int>("answer", answer, 42, VAR_IS_ESSENTIAL, "the int");
        gpb.registerFileParameter<double>("scale", scale, 3.14, VAR_IS_ESSENTIAL, "the double");
        REQUIRE(gpb.numberOfFileOptions() == 2);

        // First parse: file is absent, so GParserBuilder writes one with
        // defaults; parsing then returns the defaults.
        REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
        CHECK(answer == 42);
        CHECK(scale == 3.14);
        REQUIRE(std::filesystem::exists(cfg));
    }

    // Second parse re-reads the generated file. Values should still be the
    // defaults because nothing has changed.
    {
        GParserBuilder gpb;
        int answer = -1;
        double scale = -1.0;
        gpb.registerFileParameter<int>("answer", answer, 0, VAR_IS_ESSENTIAL);
        gpb.registerFileParameter<double>("scale", scale, 0.0, VAR_IS_ESSENTIAL);
        REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
        CHECK(answer == 42);
        CHECK(scale == 3.14);
    }

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Regression: a vector file-parameter with an EMPTY default must emit + round-trip.
// Its save_to() previously threw "You need to provide at least one default value" on an
// empty default vector, which broke config emission (--update-configs) for any binary
// registering such a parameter (e.g. Go2's list of module paths). An empty default is
// valid -- it denotes a list that is empty unless the user fills it in.

TEST_CASE(
    "GParserBuilder: vector file-parameter with an empty default round-trips",
    "[common][parser-builder]"
) {
    auto cfg = scratch("rt_empty_vector");
    std::filesystem::remove(cfg);

    {
        GParserBuilder gpb;
        std::vector<std::string> paths;
        // Missing file -> GParserBuilder emits it from the (empty) default, then parses it back.
        // Before the fix this parseConfigFile() threw during emission; it must now succeed and
        // write an empty array.
        gpb.registerFileParameter<std::string>(
            "paths",
            paths,
            std::vector<std::string>{},
            VAR_IS_SECONDARY,
            "optional paths"
        );
        REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
        REQUIRE(std::filesystem::exists(cfg));
        CHECK(paths.empty());
    }

    // Re-parse the generated file: the empty list must survive.
    {
        GParserBuilder gpb;
        std::vector<std::string> paths{"stale", "values"};
        gpb.registerFileParameter<std::string>(
            "paths",
            paths,
            std::vector<std::string>{},
            VAR_IS_SECONDARY,
            "optional paths"
        );
        REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
        CHECK(paths.empty());
    }

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Call-back form: callback receives the parsed value.

TEST_CASE(
    "GParserBuilder::registerFileParameter (callback form) fires after parse",
    "[common][parser-builder]"
) {
    auto cfg = scratch("callback");
    std::filesystem::remove(cfg);

    int sink = 0;
    GParserBuilder gpb;
    gpb.registerFileParameter<int>(
        "value",
        99,
        [&sink](int v) { sink = v; },
        VAR_IS_ESSENTIAL,
        "via callback"
    );
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(sink == 99);

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// file_at<>: locate a registered parameter by name.

TEST_CASE(
    "GParserBuilder::file_at<T> returns the matching shared_ptr",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    int sink = 0;
    gpb.registerFileParameter<int>("hit", sink, 5);

    auto p = gpb.file_at<GFileParsableI>("hit");
    REQUIRE(p);
    CHECK(p->GParsableI::optionName(0) == "hit");

    auto miss = gpb.file_at<GFileParsableI>("nope");
    CHECK_FALSE(miss);
}

// ---------------------------------------------------------------------------
// numberOfFileOptions grows with each new registration.

TEST_CASE("GParserBuilder: numberOfFileOptions counts registrations", "[common][parser-builder]") {
    GParserBuilder gpb;
    int a = 0;
    int b = 0;
    int c = 0;
    gpb.registerFileParameter<int>("a", a, 1);
    gpb.registerFileParameter<int>("b", b, 2);
    gpb.registerFileParameter<int>("c", c, 3);
    CHECK(gpb.numberOfFileOptions() == 3);
}

// ---------------------------------------------------------------------------
// Comment-arg branch of registerFileParameter (single / reference).

TEST_CASE(
    "GParserBuilder::registerFileParameter (reference + non-empty comment)",
    "[common][parser-builder]"
) {
    auto cfg = scratch("ref_with_comment");
    std::filesystem::remove(cfg);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 11, VAR_IS_ESSENTIAL, "a numeric value");
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(v == 11);
    std::filesystem::remove(cfg);
}

TEST_CASE(
    "GParserBuilder::registerFileParameter (callback + non-empty comment)",
    "[common][parser-builder]"
) {
    auto cfg = scratch("cb_with_comment");
    std::filesystem::remove(cfg);

    int sink = 0;
    GParserBuilder gpb;
    gpb.registerFileParameter<int>(
        "v",
        13,
        [&sink](int v) { sink = v; },
        VAR_IS_SECONDARY,
        "comment-text"
    );
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(sink == 13);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Two-parameter (combined) form.

TEST_CASE(
    "GParserBuilder::registerFileParameter (two-parameter combined form)",
    "[common][parser-builder]"
) {
    auto cfg = scratch("combined");
    std::filesystem::remove(cfg);

    int captured_int = 0;
    double captured_dbl = 0.0;
    GParserBuilder gpb;
    gpb.registerFileParameter<int, double>(
        "alpha",
        "beta",
        17,
        2.5,
        [&](int i, double d) {
            captured_int = i;
            captured_dbl = d;
        },
        "combined_label",
        VAR_IS_ESSENTIAL
    );
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(captured_int == 17);
    CHECK(captured_dbl == 2.5);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Vector forms: callback and reference.

TEST_CASE(
    "GParserBuilder::registerFileParameter (vector callback form)",
    "[common][parser-builder]"
) {
    auto cfg = scratch("vec_cb");
    std::filesystem::remove(cfg);

    std::vector<int> received;
    std::vector<int> def_val{1, 2, 3};
    GParserBuilder gpb;
    gpb.registerFileParameter<int>(
        "values",
        def_val,
        [&received](std::vector<int> v) { received = std::move(v); },
        VAR_IS_ESSENTIAL,
        "a vector"
    );
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(received == def_val);
    std::filesystem::remove(cfg);
}

TEST_CASE(
    "GParserBuilder::registerFileParameter (vector reference form)",
    "[common][parser-builder]"
) {
    auto cfg = scratch("vec_ref");
    std::filesystem::remove(cfg);

    std::vector<double> sink;
    std::vector<double> def_val{1.5, 2.5};
    GParserBuilder gpb;
    gpb.registerFileParameter<double>("vals", sink, def_val, VAR_IS_ESSENTIAL, "doubles");
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(sink == def_val);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Array forms (callback + reference, with std::array<T,N>).

TEST_CASE(
    "GParserBuilder::registerFileParameter (std::array callback form)",
    "[common][parser-builder]"
) {
    auto cfg = scratch("arr_cb");
    std::filesystem::remove(cfg);

    constexpr std::size_t N = 4;
    std::array<int, N> received{};
    std::array<int, N> def_val{10, 20, 30, 40};

    GParserBuilder gpb;
    gpb.registerFileParameter<int, N>(
        "arr",
        def_val,
        [&received](std::array<int, N> v) { received = v; },
        VAR_IS_ESSENTIAL,
        "array via callback"
    );
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(received == def_val);
    std::filesystem::remove(cfg);
}

TEST_CASE(
    "GParserBuilder::registerFileParameter (std::array reference form)",
    "[common][parser-builder]"
) {
    auto cfg = scratch("arr_ref");
    std::filesystem::remove(cfg);

    constexpr std::size_t N = 3;
    std::array<double, N> sink{};
    std::array<double, N> def_val{1.0, 2.0, 3.0};

    GParserBuilder gpb;
    gpb.registerFileParameter<double, N>("arr", sink, def_val, VAR_IS_ESSENTIAL, "array ref");
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(sink == def_val);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Command-line parameter registration and parsing.

TEST_CASE("GParserBuilder::registerCLParameter + parseCommandLine", "[common][parser-builder]") {
    GParserBuilder gpb;
    int intval = 0;
    double dblval = 0.0;
    gpb.registerCLParameter<int>("intopt", intval, 100, "an integer");
    gpb.registerCLParameter<double>("dblopt", dblval, 0.5, "a double");
    CHECK(gpb.numberOfCLOptions() == 2);

    // Build an argv that supplies both options.
    char prog[] = "prog";
    char opt1[] = "--intopt=42";
    char opt2[] = "--dblopt=7.25";
    char *argv[] = {prog, opt1, opt2, nullptr};
    // parseCommandLine returns GCL_HELP_REQUESTED (true) only when --help is
    // present; success returns GCL_NO_HELP_REQUESTED (false).
    CHECK_FALSE(gpb.parseCommandLine(3, argv, /*verbose*/ false));

    CHECK(intval == 42);
    CHECK(dblval == 7.25);
}

TEST_CASE(
    "GParserBuilder::registerCLParameter applies defaults when option absent",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    int v = -1;
    gpb.registerCLParameter<int>("k", v, 999);
    char prog[] = "prog";
    char *argv[] = {prog, nullptr};
    CHECK_FALSE(gpb.parseCommandLine(1, argv, false));
    CHECK(v == 999);
}

TEST_CASE(
    "GParserBuilder::parseCommandLine: --help returns GCL_HELP_REQUESTED",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    int v = 0;
    gpb.registerCLParameter<int>("k", v, 1);

    char prog[] = "prog";
    char help[] = "--help";
    char *argv[] = {prog, help, nullptr};
    CHECK(gpb.parseCommandLine(2, argv, false));
}

// ---------------------------------------------------------------------------
// resetFileParameterDefaults (single + vector + array shapes).

TEST_CASE("GParserBuilder::resetFileParameterDefaults (single value)", "[common][parser-builder]") {
    auto cfg = scratch("reset_single");
    std::filesystem::remove(cfg);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("k", v, 1);
    gpb.resetFileParameterDefaults<int>("k", 77);

    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(v == 77);
    std::filesystem::remove(cfg);
}

TEST_CASE("GParserBuilder::resetFileParameterDefaults (vector value)", "[common][parser-builder]") {
    auto cfg = scratch("reset_vec");
    std::filesystem::remove(cfg);

    GParserBuilder gpb;
    std::vector<int> sink;
    gpb.registerFileParameter<int>("vs", sink, std::vector<int>{1, 2});
    gpb.resetFileParameterDefaults<int>("vs", std::vector<int>{10, 20, 30});

    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(sink == std::vector<int>{10, 20, 30});
    std::filesystem::remove(cfg);
}

TEST_CASE("GParserBuilder::resetFileParameterDefaults (array value)", "[common][parser-builder]") {
    auto cfg = scratch("reset_arr");
    std::filesystem::remove(cfg);

    constexpr std::size_t N = 2;
    GParserBuilder gpb;
    std::array<int, N> sink{};
    gpb.registerFileParameter<int, N>("ar", sink, std::array<int, N>{1, 1});
    gpb.resetFileParameterDefaults<int, N>("ar", std::array<int, N>{42, 43});

    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(sink == std::array<int, N>{42, 43});
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// resetFileParameterDefaults: missing key throws.

TEST_CASE(
    "GParserBuilder::resetFileParameterDefaults: missing option throws",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    CHECK_THROWS_AS(gpb.resetFileParameterDefaults<int>("nope", 1), gem_exception);
    CHECK_THROWS_AS(
        gpb.resetFileParameterDefaults<int>("nope", std::vector<int>{1}),
        gem_exception
    );
}

// ---------------------------------------------------------------------------
// writeConfigFile

TEST_CASE("GParserBuilder::writeConfigFile produces a parseable JSON", "[common][parser-builder]") {
    auto cfg = scratch("write_only");
    std::filesystem::remove(cfg);

    {
        GParserBuilder gpb;
        int a = 0;
        double b = 0.0;
        gpb.registerFileParameter<int>("a", a, 1, VAR_IS_ESSENTIAL, "alpha");
        gpb.registerFileParameter<double>("b", b, 2.5, VAR_IS_ESSENTIAL, "beta");
        gpb.writeConfigFile(cfg, "test-header", /*pretty*/ true);
    }
    REQUIRE(std::filesystem::exists(cfg));

    // Reading the file back into a fresh builder should yield the same values.
    GParserBuilder gpb2;
    int a = 0;
    double b = 0.0;
    gpb2.registerFileParameter<int>("a", a, 99);
    gpb2.registerFileParameter<double>("b", b, 0.0);
    REQUIRE_NOTHROW(gpb2.parseConfigFile(cfg));
    CHECK(a == 1);
    CHECK(b == 2.5);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// writeConfigFile: error branches

TEST_CASE(
    "GParserBuilder::writeConfigFile throws when target is an existing directory",
    "[common][parser-builder]"
) {
    // The scratch helper guarantees this directory exists.
    auto dir = std::filesystem::temp_directory_path() / "gem_parserbuilder_tests";
    std::filesystem::create_directories(dir);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.writeConfigFile(dir), gem_exception);
}

TEST_CASE(
    "GParserBuilder::writeConfigFile throws when target file already exists",
    "[common][parser-builder]"
) {
    auto cfg = scratch("already_exists");
    {
        std::ofstream ofs(cfg);
        ofs << "{}";
    }
    REQUIRE(std::filesystem::exists(cfg));

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.writeConfigFile(cfg), gem_exception);
    std::filesystem::remove(cfg);
}

TEST_CASE(
    "GParserBuilder::writeConfigFile creates a missing parent directory",
    "[common][parser-builder]"
) {
    // A missing parent directory is created for the user (with a logged note) rather than treated
    // as an error -- this is what lets a fresh ./config directory come into being on first run.
    auto missing_dir = std::filesystem::temp_directory_path() / "gpb_no_such_parent_dir_xyz";
    std::filesystem::remove_all(missing_dir);
    auto cfg = missing_dir / "file.json";

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_NOTHROW(gpb.writeConfigFile(cfg));
    CHECK(std::filesystem::is_directory(missing_dir));
    CHECK(std::filesystem::exists(cfg));

    std::filesystem::remove_all(missing_dir);
}

TEST_CASE(
    "GParserBuilder::writeConfigFile throws when the parent path is not a directory",
    "[common][parser-builder]"
) {
    // If the intended parent path already exists but is a regular file, it cannot be turned into a
    // configuration directory -- that is a genuine error.
    auto not_a_dir = std::filesystem::temp_directory_path() / "gpb_parent_is_a_file_xyz";
    std::filesystem::remove_all(not_a_dir);
    {
        std::ofstream ofs(not_a_dir);
        ofs << "x";
    }
    REQUIRE(std::filesystem::is_regular_file(not_a_dir));
    auto cfg = not_a_dir / "file.json";

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.writeConfigFile(cfg), gem_exception);

    std::filesystem::remove(not_a_dir);
}

TEST_CASE(
    "GParserBuilder::writeConfigFile throws for a non-.json extension",
    "[common][parser-builder]"
) {
    auto base = std::filesystem::temp_directory_path() / "gem_parserbuilder_tests";
    std::filesystem::create_directories(base);
    auto cfg = base / "config_bad_ext.txt";
    std::filesystem::remove(cfg);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.writeConfigFile(cfg), gem_exception);
}

TEST_CASE(
    "GParserBuilder::writeConfigFile with writeAll=false omits secondary parameters",
    "[common][parser-builder]"
) {
    auto cfg = scratch("write_not_all");
    std::filesystem::remove(cfg);

    {
        GParserBuilder gpb;
        int e = 0;
        int s = 0;
        gpb.registerFileParameter<int>("essential_p", e, 42, VAR_IS_ESSENTIAL, "essential");
        gpb.registerFileParameter<int>("secondary_p", s, 99, VAR_IS_SECONDARY, "secondary");
        gpb.writeConfigFile(cfg, "", false); // writeAll=false skips secondary
    }

    // Re-parse: essential should be 42; secondary key absent → default stays 0
    GParserBuilder gpb2;
    int e2 = 0;
    int s2 = 0;
    gpb2.registerFileParameter<int>("essential_p", e2, 0);
    gpb2.registerFileParameter<int>("secondary_p", s2, 0);
    REQUIRE_NOTHROW(gpb2.parseConfigFile(cfg));
    CHECK(e2 == 42);
    CHECK(s2 == 0);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// parseConfigFile: error branches

TEST_CASE(
    "GParserBuilder::parseConfigFile throws when path is an existing directory",
    "[common][parser-builder]"
) {
    // Any existing directory makes is_regular_file() return false -> parseConfigFile throws
    // (it used to return false, which a caller could ignore and run on its defaults).
    auto dir = std::filesystem::temp_directory_path() / "gem_parserbuilder_tests";
    std::filesystem::create_directories(dir);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.parseConfigFile(dir), gem_exception);
}

// ---------------------------------------------------------------------------
// parseCommandLine: verbose=true branch

TEST_CASE(
    "GParserBuilder::parseCommandLine with verbose=true produces no exception",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    int v = 0;
    gpb.registerCLParameter<int>("k", v, 7);
    char prog[] = "prog";
    char *argv[] = {prog, nullptr};
    CHECK_FALSE(gpb.parseCommandLine(1, argv, /*verbose*/ true));
    CHECK(v == 7);
}

// ---------------------------------------------------------------------------
// resetFileParameterDefaults: missing-key error for combined and array forms

TEST_CASE(
    "GParserBuilder::resetFileParameterDefaults (combined form): missing option throws",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    CHECK_THROWS_AS((gpb.resetFileParameterDefaults<int, double>("nope", 1, 2.0)), gem_exception);
}

TEST_CASE(
    "GParserBuilder::resetFileParameterDefaults (array form): missing option throws",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    CHECK_THROWS_AS(
        (gpb.resetFileParameterDefaults<int, 2>("nope", std::array<int, 2>{1, 2})),
        gem_exception
    );
}

// ---------------------------------------------------------------------------
// updateConfigFile: the update-in-place contract (drop stale keys, preserve
// existing values, add newly-registered keys with defaults).

TEST_CASE(
    "GParserBuilder::updateConfigFile drops stale keys and keeps values while defaulting new keys",
    "[common][parser-builder]"
) {
    namespace json = boost::json;
    auto cfg = scratch("update_inplace");
    std::filesystem::remove(cfg);

    // Write a v1 config by hand: a CUSTOMIZED "answer" value (13, not the default 42), a normal
    // "scale", and a "stale_key" that the v2 schema below no longer registers.
    {
        json::object t;
        t["answer"] = json::object{{"default", "42"}, {"value", "13"}}; // customized on disk
        t["scale"] = json::object{{"default", "3.14"}, {"value", "3.14"}};
        t["stale_key"] = json::object{{"default", "999"}, {"value", "999"}};
        Gem::Common::writeJsonFile(cfg, json::value(std::move(t)));
    }

    // v2 schema: {answer, scale, fresh} -- "stale_key" gone, "fresh" new.
    GParserBuilder gpb;
    int answer = 0;
    double scale = 0.0;
    int fresh = 0;
    gpb.registerFileParameter<int>("answer", answer, 42, VAR_IS_ESSENTIAL, "the int");
    gpb.registerFileParameter<double>("scale", scale, 3.14, VAR_IS_ESSENTIAL, "the double");
    gpb.registerFileParameter<int>("fresh", fresh, 7, VAR_IS_ESSENTIAL, "new in v2");

    REQUIRE_NOTHROW(gpb.updateConfigFile(cfg));

    // Applied (in-memory) values: existing preserved, new defaulted.
    CHECK(answer == 13); // preserved from disk (not reset to the 42 default)
    CHECK(scale == 3.14);
    CHECK(fresh == 7); // absent from v1 -> registered default

    // On-disk shape after the rewrite. Arithmetic scalars are stored as native JSON numbers (the input
    // above was hand-written in the earlier all-strings form, which the reader still accepts).
    const json::value after = Gem::Common::parseJsonFile(cfg);
    REQUIRE(after.is_object());
    const json::object &ao = after.get_object();
    CHECK(ao.at("answer").at("value").to_number<int>() == 13); // value preserved on disk
    CHECK(ao.at("scale").at("value").to_number<double>() == 3.14);
    CHECK(ao.at("fresh").at("value").to_number<int>() == 7); // new key present, defaulted
    CHECK_FALSE(ao.contains("stale_key"));                   // stale key dropped
    CHECK(ao.contains("header"));                            // canonical header written

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Regression (vector load_from): a config that lacks a registered vector
// parameter must keep that parameter's defaults instead of throwing. Guards the
// get_child_optional() fix that makes update-in-place work when a NEW vector
// parameter is added to the schema.

TEST_CASE(
    "GParserBuilder: a config missing a registered vector parameter keeps its defaults",
    "[common][parser-builder]"
) {
    namespace json = boost::json;
    auto cfg = scratch("vec_newkey");
    std::filesystem::remove(cfg);

    // A config with a scalar but WITHOUT the vector key "vints".
    {
        json::object t;
        t["other"] = json::object{{"default", "5"}, {"value", "5"}};
        Gem::Common::writeJsonFile(cfg, json::value(std::move(t)));
    }

    GParserBuilder gpb;
    int other = 0;
    std::vector<int> vints;
    gpb.registerFileParameter<int>("other", other, 5, VAR_IS_ESSENTIAL, "scalar");
    gpb.registerFileParameter<int>(
        "vints",
        vints,
        std::vector<int>{10, 20, 30},
        VAR_IS_ESSENTIAL,
        "a vector"
    );

    // Before the fix a missing vector key aborted the parse; now it keeps the registered defaults.
    REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    CHECK(other == 5);
    CHECK(vints == std::vector<int>{10, 20, 30}); // the registered defaults survive

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Regression (Inv 15): a multi-element vector parameter must round-trip through a
// written-then-parsed config with ALL its elements intact. The old ptree format
// stored vectors as duplicate "item" keys, which a strict JSON parser collapses to
// one (silent data loss); the JSON-array representation keeps every element.

TEST_CASE(
    "GParserBuilder: a multi-element vector round-trips as a JSON array (no dup-key collapse)",
    "[common][parser-builder][regression]"
) {
    namespace json = boost::json;
    auto cfg = scratch("vec_roundtrip");
    std::filesystem::remove(cfg);

    // Hand-write a config whose vector "value" is a 4-element JSON array of non-default values.
    {
        json::object t;
        t["vec"] = json::object{
            {"default", json::array{"1", "1", "1", "1"}},
            {"value", json::array{"7", "8", "9", "10"}}
        };
        Gem::Common::writeJsonFile(cfg, json::value(std::move(t)));
    }

    // Reading it back restores every element (the old dup-key format would have collapsed to one).
    GParserBuilder reader;
    std::vector<int> vec_in;
    reader.registerFileParameter<int>(
        "vec",
        vec_in,
        std::vector<int>{1, 1, 1, 1},
        VAR_IS_ESSENTIAL,
        "a vector"
    );
    REQUIRE_NOTHROW(reader.parseConfigFile(cfg));
    CHECK(vec_in == std::vector<int>{7, 8, 9, 10});

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Native scalar typing: arithmetic types and booleans are written as native JSON
// numbers / bools (not quoted strings), while a config hand-written in the earlier
// all-strings form still loads (back-compat of the read path).

TEST_CASE(
    "GParserBuilder: arithmetic and bool scalars write as native JSON and the old string form "
    "still reads",
    "[common][parser-builder][regression]"
) {
    namespace json = boost::json;
    auto cfg = scratch("native_scalars");
    std::filesystem::remove(cfg);

    // parseConfigFile on a non-existent file creates it from the registered defaults.
    {
        GParserBuilder gpb;
        int i = 20;
        double d = 3.5;
        bool b = true;
        std::string s = "hello";
        gpb.registerFileParameter<int>("i", i, 20, VAR_IS_ESSENTIAL, "an int");
        gpb.registerFileParameter<double>("d", d, 3.5, VAR_IS_ESSENTIAL, "a double");
        gpb.registerFileParameter<bool>("b", b, true, VAR_IS_ESSENTIAL, "a bool");
        gpb.registerFileParameter<std::string>(
            "s",
            s,
            std::string("hi"),
            VAR_IS_ESSENTIAL,
            "a string"
        );
        REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    }

    const json::value doc = Gem::Common::parseJsonFile(cfg);
    REQUIRE(doc.is_object());
    const json::object &o = doc.get_object();
    CHECK(o.at("i").at("value").is_int64()); // native number, not "20"
    CHECK(o.at("i").at("value").to_number<int>() == 20);
    CHECK(o.at("d").at("value").is_double()); // native number
    CHECK(o.at("b").at("value").is_bool());   // native bool, not "true"
    CHECK(o.at("b").at("value").as_bool());
    CHECK(o.at("s").at("value").is_string()); // strings stay strings

    // A config hand-written in the earlier all-strings form must still load.
    {
        json::object t;
        t["i"] = json::object{{"default", "20"}, {"value", "33"}};
        t["d"] = json::object{{"default", "3.5"}, {"value", "9.5"}};
        t["b"] = json::object{{"default", "true"}, {"value", "false"}};
        t["s"] = json::object{{"default", "hi"}, {"value", "world"}};
        Gem::Common::writeJsonFile(cfg, json::value(std::move(t)));
    }
    GParserBuilder reader;
    int i2 = 0;
    double d2 = 0.0;
    bool b2 = true;
    std::string s2;
    reader.registerFileParameter<int>("i", i2, 20, VAR_IS_ESSENTIAL, "an int");
    reader.registerFileParameter<double>("d", d2, 3.5, VAR_IS_ESSENTIAL, "a double");
    reader.registerFileParameter<bool>("b", b2, true, VAR_IS_ESSENTIAL, "a bool");
    reader.registerFileParameter<std::string>(
        "s",
        s2,
        std::string("hi"),
        VAR_IS_ESSENTIAL,
        "a string"
    );
    REQUIRE_NOTHROW(reader.parseConfigFile(cfg));
    CHECK(i2 == 33);
    CHECK(d2 == 9.5);
    CHECK_FALSE(b2);
    CHECK(s2 == "world");

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Regression: a std::uint8_t parameter (e.g. GEvolutionaryAlgorithm's step_control)
// must round-trip by VALUE. Because (u)int8_t aliases a character type, a naive stream
// conversion of "1" reads the character '1' (49) rather than the number 1; the scalar
// helpers widen it so the numeric value survives, in both the native and the string form.

TEST_CASE(
    "GParserBuilder: a uint8_t scalar round-trips by value and not as a character",
    "[common][parser-builder][regression]"
) {
    namespace json = boost::json;
    auto cfg = scratch("uint8_roundtrip");
    std::filesystem::remove(cfg);

    // A freshly created config stores the uint8_t value 1 as the NUMBER 1 (not 49).
    {
        GParserBuilder gpb;
        std::uint8_t code = 1;
        gpb.registerFileParameter<std::uint8_t>(
            "code",
            code,
            std::uint8_t(1),
            VAR_IS_ESSENTIAL,
            "8-bit code"
        );
        REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
    }
    {
        const json::value doc = Gem::Common::parseJsonFile(cfg);
        const json::object &o = doc.get_object();
        CHECK(o.at("code").at("value").to_number<int>() == 1); // the number 1, not 49
        CHECK(o.at("code").at("default").to_number<int>() == 1);
    }
    {
        GParserBuilder reader;
        std::uint8_t code = 0;
        reader.registerFileParameter<std::uint8_t>(
            "code",
            code,
            std::uint8_t(0),
            VAR_IS_ESSENTIAL,
            "8-bit code"
        );
        REQUIRE_NOTHROW(reader.parseConfigFile(cfg));
        CHECK(int(code) == 1);
    }

    // The earlier all-strings form ("1") must also read back as 1, not 49.
    {
        json::object t;
        t["code"] = json::object{{"default", "1"}, {"value", "1"}};
        Gem::Common::writeJsonFile(cfg, json::value(std::move(t)));
        GParserBuilder reader;
        std::uint8_t code = 0;
        reader.registerFileParameter<std::uint8_t>(
            "code",
            code,
            std::uint8_t(0),
            VAR_IS_ESSENTIAL,
            "8-bit code"
        );
        REQUIRE_NOTHROW(reader.parseConfigFile(cfg));
        CHECK(int(code) == 1);
    }

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// setEmitTimestamp: the header's creation-timestamp line can be suppressed for byte-stable,
// version-controlled output (the config-reference tree). Regression guard for that flag.

TEST_CASE(
    "GParserBuilder::setEmitTimestamp toggles the header creation timestamp",
    "[common][parser-builder]"
) {
    auto with_ts = scratch("ts_on");
    auto without_ts = scratch("ts_off");
    std::filesystem::remove(with_ts);
    std::filesystem::remove(without_ts);

    auto header_lines = [](std::filesystem::path const &p) {
        return Gem::Common::parseJsonFile(p)
            .as_object()
            .at("header")
            .as_object()
            .at("comment")
            .as_array()
            .size();
    };

    // The flag is a process-global; it defaults to on.
    REQUIRE(GParserBuilder::emitTimestamp());
    {
        GParserBuilder gpb;
        int v = 0;
        gpb.registerFileParameter<int>("v", v, 1, VAR_IS_ESSENTIAL, "a value");
        gpb.writeConfigFile(with_ts, "created", true);
    }

    GParserBuilder::setEmitTimestamp(false);
    CHECK_FALSE(GParserBuilder::emitTimestamp());
    {
        GParserBuilder gpb;
        int v = 0;
        gpb.registerFileParameter<int>("v", v, 1, VAR_IS_ESSENTIAL, "a value");
        gpb.writeConfigFile(without_ts, "created", true);
    }
    GParserBuilder::setEmitTimestamp(
        true
    ); // restore the global default before any assertion can throw

    // With the timestamp on the header has exactly one extra comment line (the creation time).
    CHECK(header_lines(with_ts) == header_lines(without_ts) + 1);

    std::filesystem::remove(with_ts);
    std::filesystem::remove(without_ts);
}

// ---------------------------------------------------------------------------
// Regression (2026-07-18): a comment-less registration must keep the caller's is_essential
// choice. The empty-comment fast path used to route through the comment-less node constructor,
// which hardcodes VAR_IS_ESSENTIAL -- a comment-less SECONDARY parameter was silently promoted
// to essential and leaked into essential-only config files.

TEST_CASE(
    "GParserBuilder: comment-less registration honors is_essential",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;

    int essential_v = 0;
    int secondary_v = 0;
    gpb.registerFileParameter<int>("essential_option", essential_v, 1, VAR_IS_ESSENTIAL);
    gpb.registerFileParameter<int>("secondary_option", secondary_v, 2, VAR_IS_SECONDARY);

    // The registered proxies must carry the requested essentiality ...
    CHECK(gpb.file_at<GFileParsableI>("essential_option")->isEssential());
    CHECK_FALSE(gpb.file_at<GFileParsableI>("secondary_option")->isEssential());

    // ... and an essential-only config write must omit the secondary parameter.
    const auto path = scratch("essential_only");
    gpb.writeConfigFile(path, "essential-only test", /*write_all=*/false);

    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("essential_option") != std::string::npos);
    CHECK(text.find("secondary_option") == std::string::npos);

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// Regression: a known key whose entry is not an object with a "value" was skipped without a
// word, so the registered default applied while the file said otherwise (a flat
// "scan_randomly": false). It is refused, naming the key and the form it needs, for every
// parameter kind; an absent key still keeps its default.

namespace {

// One parameter of each kind (single, combined, vector, array), loaded from a JSON text.
struct OneOfEachKind {
    bool flag = true;
    int alpha = 0;
    double beta = 0.;
    std::vector<int> list;
    std::array<int, 2> pair{};
    GParserBuilder gpb;

    OneOfEachKind() {
        gpb.registerFileParameter<bool>("flag", flag, true);
        gpb.registerFileParameter<int, double>(
            "alpha",
            "beta",
            1,
            2.5,
            [this](int a, double b) {
                alpha = a;
                beta = b;
            },
            "group"
        );
        gpb.registerFileParameter<int>("list", list, std::vector<int>{1, 2});
        gpb.registerFileParameter<int, 2>("pair", pair, std::array<int, 2>{3, 4});
    }

    void load(std::string const &text) {
        gpb.loadFromDocument(boost::json::parse(text));
    }
};

} // namespace

TEST_CASE(
    "GParserBuilder: a known key of the wrong form is refused with its key and form named",
    "[common][parser-builder]"
) {
    using Catch::Matchers::ContainsSubstring;
    OneOfEachKind k;

    SECTION("the object form is read; an absent key keeps its default") {
        k.load(R"({ "flag": { "value": false }, "group": { "alpha": { "value": 7 } },
                    "list": { "value": [5, 6, 7] } })");
        CHECK_FALSE(k.flag);
        CHECK(k.alpha == 7);
        CHECK(k.beta == 2.5);
        CHECK(k.list == std::vector<int>{5, 6, 7});
        CHECK(k.pair == std::array<int, 2>{3, 4});
    }

    SECTION("a single parameter") {
        CHECK_THROWS_WITH(
            k.load(R"({ "flag": false })"),
            ContainsSubstring(R"("flag" has the wrong form: false)") &&
                ContainsSubstring(R"(e.g. "flag": { "value": ... })")
        );
        CHECK_THROWS_WITH(
            k.load(R"({ "flag": { "default": false } })"),
            ContainsSubstring(R"("flag")")
        );
        CHECK_THROWS_WITH(
            k.load(R"({ "flag": { "value": [false] } })"),
            ContainsSubstring(R"("flag")")
        );
        CHECK(k.flag);
    }

    SECTION("a combined parameter") {
        CHECK_THROWS_WITH(
            k.load(R"({ "group": 7 })"),
            ContainsSubstring(R"("group" has the wrong form)")
        );
        CHECK_THROWS_WITH(
            k.load(R"({ "group": { "alpha": 7 } })"),
            ContainsSubstring(R"("group/alpha" has the wrong form)")
        );
        CHECK(k.alpha == 0);
    }

    SECTION("a vector parameter") {
        CHECK_THROWS_WITH(
            k.load(R"({ "list": [5, 6] })"),
            ContainsSubstring(R"("list" has the wrong form)") &&
                ContainsSubstring(R"(e.g. "list": { "value": [ ... ] })")
        );
        CHECK_THROWS_WITH(k.load(R"({ "list": { "value": 5 } })"), ContainsSubstring(R"("list")"));
        CHECK(k.list.empty());
    }

    SECTION("an array parameter") {
        CHECK_THROWS_WITH(
            k.load(R"({ "pair": [5, 6] })"),
            ContainsSubstring(R"("pair" has the wrong form)")
        );
        CHECK(k.pair == std::array<int, 2>{});
    }
}

// ---------------------------------------------------------------------------
// Regression: parseConfigFile() returned false on any error, and most callers ignored the
// result (a direct readConfigFile() of an optimization algorithm among them), so a file that could not
// be applied left the defaults in force without a word. It throws, naming the file and the cause.

TEST_CASE(
    "GParserBuilder::parseConfigFile throws on a file it cannot apply and names file and key",
    "[common][parser-builder]"
) {
    using Catch::Matchers::ContainsSubstring;
    auto cfg = scratch("flat_entry");
    {
        std::ofstream out(cfg);
        out << R"({ "scan_randomly": false })" << '\n';
    }

    GParserBuilder gpb;
    bool scan_randomly = true;
    gpb.registerFileParameter<bool>("scan_randomly", scan_randomly, true);
    CHECK_THROWS_WITH(
        gpb.parseConfigFile(cfg),
        ContainsSubstring(cfg.string()) &&
            ContainsSubstring(R"("scan_randomly" has the wrong form)")
    );
    CHECK_THROWS_AS(gpb.updateConfigFile(cfg), gem_exception);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Regression: a boolean that was not "true" or "1" read as false, and a number that did not read
// as its type was read as far as it went ("2.5" as 2) or left indeterminate ("two"). Such a value
// is refused, naming the key; the all-strings form of a valid value still reads.

TEST_CASE(
    "GParserBuilder: a value that does not read as its type is refused with its key named",
    "[common][parser-builder]"
) {
    using Catch::Matchers::ContainsSubstring;
    OneOfEachKind k;

    CHECK_THROWS_WITH(
        k.load(R"({ "flag": { "value": "yes" } })"),
        ContainsSubstring(R"("flag" has the value "yes", which is not a boolean)")
    );
    CHECK_THROWS_WITH(
        k.load(R"({ "group": { "alpha": { "value": 2.5 } } })"),
        ContainsSubstring(R"("group/alpha" has the value)")
    );
    CHECK_THROWS_WITH(
        k.load(R"({ "list": { "value": [1, "two"] } })"),
        ContainsSubstring(R"("list" has the value "two", which is not an integer)")
    );
    CHECK_THROWS_WITH(
        k.load(R"({ "pair": { "value": [1, "x"] } })"),
        ContainsSubstring(R"("pair")")
    );

    k.load(R"({ "flag": { "value": "false" }, "group": { "beta": { "value": "1e-3" } },
                "list": { "value": [" 4 ", 5] } })");
    CHECK_FALSE(k.flag);
    CHECK(k.beta == 1e-3);
    CHECK(k.list == std::vector<int>{4, 5});
}

// ---------------------------------------------------------------------------
// Regression: an enumeration read any text as 0 ("MAXIMIZE" for maxmode minimized), a fraction as
// its integer part and a number that is none of its values as an enumerator that does not exist;
// a negative number for an unsigned key wrapped around ("-1" as 4294967295); an 8-bit key kept
// the low byte of a larger number. Each is refused, naming the key and what the value must be.

TEST_CASE(
    "GParserBuilder: a value outside the range of its type is refused",
    "[common][parser-builder]"
) {
    using Catch::Matchers::ContainsSubstring;
    Gem::Common::sortOrder order = Gem::Common::sortOrder::LOWERISBETTER;
    Gem::Common::dimensions dims = Gem::Common::dimensions::Dim1;
    std::uint32_t count = 0;
    std::vector<std::size_t> sizes;
    std::uint8_t small = 0;
    GParserBuilder gpb;
    gpb.registerFileParameter<Gem::Common::sortOrder>(
        "order",
        order,
        Gem::Common::sortOrder::LOWERISBETTER
    );
    gpb.registerFileParameter<Gem::Common::dimensions>("dims", dims, Gem::Common::dimensions::Dim1);
    gpb.registerFileParameter<std::uint32_t>("count", count, 1);
    gpb.registerFileParameter<std::size_t>("sizes", sizes, std::vector<std::size_t>{1});
    gpb.registerFileParameter<std::uint8_t>("small", small, 1);
    auto load = [&gpb](std::string const &text) { gpb.loadFromDocument(boost::json::parse(text)); };

    CHECK_THROWS_WITH(
        load(R"({ "order": { "value": "HIGHERISBETTER" } })"),
        ContainsSubstring(
            R"("order" has the value "HIGHERISBETTER", which is not one of the values 0 or 1)"
        )
    );
    CHECK_THROWS_WITH(load(R"({ "order": { "value": "1.5" } })"), ContainsSubstring(R"("order")"));
    CHECK_THROWS_WITH(load(R"({ "order": { "value": "7" } })"), ContainsSubstring(R"("order")"));
    CHECK_THROWS_WITH(
        load(R"({ "dims": { "value": 0 } })"),
        ContainsSubstring(R"(which is not one of the values 1, 2, 3 or 4)")
    );
    CHECK_THROWS_WITH(
        load(R"({ "count": { "value": -1 } })"),
        ContainsSubstring(R"("count" has the value "-1", which is not an integer >= 0)")
    );
    CHECK_THROWS_WITH(
        load(R"({ "sizes": { "value": [2, " -1"] } })"),
        ContainsSubstring(R"("sizes")")
    );
    CHECK_THROWS_WITH(
        load(R"({ "small": { "value": 300 } })"),
        ContainsSubstring(R"("small" has the value "300", which is not an integer from 0 to 255)")
    );
    CHECK(order == Gem::Common::sortOrder::LOWERISBETTER);
    CHECK(count == 1);

    load(R"({ "order": { "value": "1" }, "dims": { "value": 4 }, "count": { "value": 4294967295 },
              "sizes": { "value": [0, 3] }, "small": { "value": 255 } })");
    CHECK(order == Gem::Common::sortOrder::HIGHERISBETTER);
    CHECK(dims == Gem::Common::dimensions::Dim4);
    CHECK(count == 4294967295U);
    CHECK(sizes == std::vector<std::size_t>{0, 3});
    CHECK(small == 255);
}

TEST_CASE(
    "Gem::Common: a numeric enumeration reads only its own values from a stream",
    "[common][parser-builder]"
) {
    Gem::Common::serializationMode mode = Gem::Common::serializationMode::GEM_JSON;
    std::istringstream none("2");
    none >> mode;
    CHECK(none.fail());
    CHECK(mode == Gem::Common::serializationMode::GEM_JSON);
    std::istringstream binary("3");
    binary >> mode;
    CHECK_FALSE(binary.fail());
    CHECK(mode == Gem::Common::serializationMode::GEM_BINARY);
    CHECK(Gem::Common::numericEnumValueList<Gem::Common::logType>() == "0, 1, 2, 3, 4, 5, 6 or 7");
}

// ---------------------------------------------------------------------------
// The command line: a malformed one throws GCommandLineError (the parser does not end the process)

TEST_CASE(
    "GParserBuilder::parseCommandLine: a malformed command line throws and carries the help text",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    int count = 0;
    gpb.registerCLParameter<int>("count", count, 3, "how many");

    Argv unknown({"--count=4", "--nonsense"});
    try {
        gpb.parseCommandLine(unknown.argc(), unknown.argv());
        FAIL("an unknown option was accepted");
    }
    catch(GCommandLineError const &e) {
        CHECK_THAT(std::string(e.what()), ContainsSubstring("nonsense"));
        CHECK_THAT(e.usage(), ContainsSubstring("Usage: prog [options]"));
        CHECK_THAT(e.usage(), ContainsSubstring("--count"));
        CHECK_THAT(e.usage(), ContainsSubstring("how many"));
    }

    Argv bad_value({"--count=many"});
    CHECK_THROWS_AS(gpb.parseCommandLine(bad_value.argc(), bad_value.argv()), GCommandLineError);
    Argv no_value({"--count"});
    CHECK_THROWS_AS(gpb.parseCommandLine(no_value.argc(), no_value.argv()), GCommandLineError);
}

// ---------------------------------------------------------------------------
// The command line: sections of the help text and options listed only by --showAll

TEST_CASE(
    "GParserBuilder: --help and --showAll print what Go2 prints for the same options",
    "[common][parser-builder]"
) {
    namespace po = boost::program_options;

    GParserBuilder gpb;
    bool client = false;
    int items = 0;
    int port = 0;
    int max_iterations = 0;
    gpb.beginCLSection("Basic options");
    gpb.registerCLFlag("client", client, "run as a client");
    gpb.beginCLSection("User options");
    gpb.registerCLParameter<int>("nItems,n", items, 5, "the number of items");
    gpb.beginCLSection(
        "Global algorithm- and consumer-options",
        "Hidden algorithm- and consumer-options"
    );
    gpb.registerCLParameter<int>("eaMaxIterations", max_iterations, 0, "the iterations").setHidden();
    gpb.registerCLParameter<int>("port", port, 10000, "the port");

    // Go2's construction (Go2::parseCommandLine(), Go2::emitHelpIfRequested()), by hand
    po::options_description general("Usage: prog [options]");
    po::options_description basic("Basic options");
    basic.add_options()("help,h", "Emit help message")("showAll", "Show all available options")(
        "client",
        "run as a client"
    );
    po::options_description user("User options");
    user.add_options()("nItems,n", po::value<int>()->default_value(5), "the number of items");
    po::options_description visible("Global algorithm- and consumer-options");
    visible.add_options()("port", po::value<int>()->default_value(10000), "the port");
    po::options_description hidden("Hidden algorithm- and consumer-options");
    hidden.add_options()("eaMaxIterations", po::value<int>()->default_value(0), "the iterations");
    general.add(basic).add(user).add(visible).add(hidden);
    po::options_description selected("Usage: prog [options]");
    selected.add(basic).add(user).add(visible);
    std::ostringstream go2_all;
    go2_all << general << '\n';
    std::ostringstream go2_help;
    go2_help << selected << '\n';

    {
        CoutCapture capture;
        Argv help({"--help"});
        CHECK(gpb.parseCommandLine(help.argc(), help.argv()) == GCL_HELP_REQUESTED);
        CHECK(capture.text() == go2_help.str());
        CHECK_THAT(capture.text(), !ContainsSubstring("eaMaxIterations"));
    }
    {
        CoutCapture capture;
        Argv show_all({"--showAll"});
        CHECK(gpb.parseCommandLine(show_all.argc(), show_all.argv()) == GCL_HELP_REQUESTED);
        CHECK(capture.text() == go2_all.str());
    }

    // A hidden option is read like any other
    Argv given({"--eaMaxIterations=7", "-n", "3"});
    CHECK(gpb.parseCommandLine(given.argc(), given.argv()) == GCL_NO_HELP_REQUESTED);
    CHECK(max_iterations == 7);
    CHECK(items == 3);
}

TEST_CASE(
    "GParserBuilder: without sections the help text is listed under the usage line",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    int count = 0;
    int secret = 0;
    gpb.registerCLParameter<int>("count", count, 3, "how many");
    gpb.registerCLParameter<int>("secret", secret, 1, "not for everybody").setHidden();
    CHECK(gpb.numberOfCLOptions() == 2);

    std::string help_text;
    {
        CoutCapture capture;
        Argv help({"-h"});
        CHECK(gpb.parseCommandLine(help.argc(), help.argv()) == GCL_HELP_REQUESTED);
        help_text = capture.text();
    }
    CHECK_THAT(help_text, Catch::Matchers::StartsWith("Usage: prog [options]:\n  -h [ --help ]"));
    CHECK_THAT(help_text, ContainsSubstring("--showAll"));
    CHECK_THAT(help_text, ContainsSubstring("--count arg (=3)"));
    CHECK_THAT(help_text, !ContainsSubstring("secret"));

    std::string all_text;
    {
        CoutCapture capture;
        Argv show_all({"--showAll"});
        CHECK(gpb.parseCommandLine(show_all.argc(), show_all.argv()) == GCL_HELP_REQUESTED);
        all_text = capture.text();
    }
    CHECK_THAT(all_text, ContainsSubstring("--secret arg (=1)"));
    CHECK_THAT(all_text, ContainsSubstring("not for everybody"));

    // The help text of an error leaves the hidden option out, as --help does
    Argv unknown({"--nonsense"});
    try {
        gpb.parseCommandLine(unknown.argc(), unknown.argv());
        FAIL("an unknown option was accepted");
    }
    catch(GCommandLineError const &e) {
        CHECK(e.usage() + '\n' == help_text);
    }
}

TEST_CASE(
    "GParserBuilder::registerCLFlag: true if given and false if not; it takes no value",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    bool client = true; // not the default: a parse without the flag sets it to false
    gpb.registerCLFlag("client,c", client, "run as a client");

    Argv absent({});
    CHECK(gpb.parseCommandLine(absent.argc(), absent.argv()) == GCL_NO_HELP_REQUESTED);
    CHECK_FALSE(client);

    Argv given({"--client"});
    CHECK(gpb.parseCommandLine(given.argc(), given.argv()) == GCL_NO_HELP_REQUESTED);
    CHECK(client);

    Argv short_form({"-c"});
    client = false;
    CHECK(gpb.parseCommandLine(short_form.argc(), short_form.argv()) == GCL_NO_HELP_REQUESTED);
    CHECK(client);

    Argv with_value({"--client=true"});
    CHECK_THROWS_AS(gpb.parseCommandLine(with_value.argc(), with_value.argv()), GCommandLineError);
}

TEST_CASE(
    "GParserBuilder::registerCLRepeatableParameter: every value in the order given",
    "[common][parser-builder]"
) {
    GParserBuilder gpb;
    std::vector<std::string> modules{"left over"};
    std::vector<int> ranks;
    gpb.registerCLRepeatableParameter("module,m", modules, "a module to load (repeatable)");
    gpb.registerCLRepeatableParameter("rank", ranks, "a rank (repeatable)");

    Argv given({"--module", "a.so", "-m", "b.so", "--rank=2", "--module=c.so", "--rank", "0"});
    CHECK(gpb.parseCommandLine(given.argc(), given.argv()) == GCL_NO_HELP_REQUESTED);
    CHECK(modules == std::vector<std::string>{"a.so", "b.so", "c.so"});
    CHECK(ranks == std::vector<int>{2, 0});

    Argv absent({});
    CHECK(gpb.parseCommandLine(absent.argc(), absent.argv()) == GCL_NO_HELP_REQUESTED);
    CHECK(modules.empty());
    CHECK(ranks.empty());

    Argv bad_value({"--rank=first"});
    CHECK_THROWS_AS(gpb.parseCommandLine(bad_value.argc(), bad_value.argv()), GCommandLineError);
}
