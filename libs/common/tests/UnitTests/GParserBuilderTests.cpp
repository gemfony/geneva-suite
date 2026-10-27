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

#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "common/GExceptions.hpp"
#include "common/GJsonIO.hpp"
#include "common/GParserBuilder.hpp"

using namespace Gem::Common;

namespace {

std::filesystem::path scratch(std::string const &tag) {
    auto base = std::filesystem::temp_directory_path() / "geneva_parserbuilder_tests";
    std::filesystem::create_directories(base);
    return base / (tag + ".json");
}

} // namespace

// ---------------------------------------------------------------------------
// Empty state

TEST_CASE("GParserBuilder: fresh instance reports zero options",
          "[common][parser-builder]") {
    GParserBuilder const gpb;
    CHECK(gpb.numberOfFileOptions() == 0);
    CHECK(gpb.numberOfCLOptions()   == 0);
}

// ---------------------------------------------------------------------------
// registerFileParameter (reference form): default value applied when missing.

TEST_CASE("GParserBuilder: writeConfigFile + parseConfigFile round-trip "
          "(missing file → default values written)",
          "[common][parser-builder]") {
    auto cfg = scratch("rt_default");
    std::filesystem::remove(cfg);

    {
        GParserBuilder gpb;
        int    answer = 0;
        double scale  = 0.0;
        gpb.registerFileParameter<int>   ("answer", answer, 42,   VAR_IS_ESSENTIAL, "the int");
        gpb.registerFileParameter<double>("scale",  scale,  3.14, VAR_IS_ESSENTIAL, "the double");
        REQUIRE(gpb.numberOfFileOptions() == 2);

        // First parse: file is absent, so GParserBuilder writes one with
        // defaults; parsing then returns the defaults.
        REQUIRE(gpb.parseConfigFile(cfg));
        CHECK(answer == 42);
        CHECK(scale  == 3.14);
        REQUIRE(std::filesystem::exists(cfg));
    }

    // Second parse re-reads the generated file. Values should still be the
    // defaults because nothing has changed.
    {
        GParserBuilder gpb;
        int    answer = -1;
        double scale  = -1.0;
        gpb.registerFileParameter<int>   ("answer", answer, 0,   VAR_IS_ESSENTIAL);
        gpb.registerFileParameter<double>("scale",  scale,  0.0, VAR_IS_ESSENTIAL);
        REQUIRE(gpb.parseConfigFile(cfg));
        CHECK(answer == 42);
        CHECK(scale  == 3.14);
    }

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Regression: a vector file-parameter with an EMPTY default must emit + round-trip.
// Its save_to() previously threw "You need to provide at least one default value" on an
// empty default vector, which broke config emission (--update-configs) for any binary
// registering such a parameter (e.g. Go2's list of module paths). An empty default is
// valid -- it denotes a list that is empty unless the user fills it in.

TEST_CASE("GParserBuilder: vector file-parameter with an empty default round-trips",
          "[common][parser-builder]") {
    auto cfg = scratch("rt_empty_vector");
    std::filesystem::remove(cfg);

    {
        GParserBuilder gpb;
        std::vector<std::string> paths;
        // Missing file -> GParserBuilder emits it from the (empty) default, then parses it back.
        // Before the fix this parseConfigFile() threw during emission; it must now succeed and
        // write an empty array.
        gpb.registerFileParameter<std::string>(
            "paths", paths, std::vector<std::string>{}, VAR_IS_SECONDARY, "optional paths");
        REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
        REQUIRE(std::filesystem::exists(cfg));
        CHECK(paths.empty());
    }

    // Re-parse the generated file: the empty list must survive.
    {
        GParserBuilder gpb;
        std::vector<std::string> paths{"stale", "values"};
        gpb.registerFileParameter<std::string>(
            "paths", paths, std::vector<std::string>{}, VAR_IS_SECONDARY, "optional paths");
        REQUIRE_NOTHROW(gpb.parseConfigFile(cfg));
        CHECK(paths.empty());
    }

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Call-back form: callback receives the parsed value.

TEST_CASE("GParserBuilder::registerFileParameter (callback form) fires after parse",
          "[common][parser-builder]") {
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
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(sink == 99);

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// file_at<>: locate a registered parameter by name.

TEST_CASE("GParserBuilder::file_at<T> returns the matching shared_ptr",
          "[common][parser-builder]") {
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

TEST_CASE("GParserBuilder: numberOfFileOptions counts registrations",
          "[common][parser-builder]") {
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

TEST_CASE("GParserBuilder::registerFileParameter (reference + non-empty comment)",
          "[common][parser-builder]") {
    auto cfg = scratch("ref_with_comment");
    std::filesystem::remove(cfg);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 11, VAR_IS_ESSENTIAL, "a numeric value");
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(v == 11);
    std::filesystem::remove(cfg);
}

TEST_CASE("GParserBuilder::registerFileParameter (callback + non-empty comment)",
          "[common][parser-builder]") {
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
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(sink == 13);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Two-parameter (combined) form.

TEST_CASE("GParserBuilder::registerFileParameter (two-parameter combined form)",
          "[common][parser-builder]") {
    auto cfg = scratch("combined");
    std::filesystem::remove(cfg);

    int    captured_int = 0;
    double captured_dbl = 0.0;
    GParserBuilder gpb;
    gpb.registerFileParameter<int, double>(
        "alpha", "beta",
        17, 2.5,
        [&](int i, double d) { captured_int = i; captured_dbl = d; },
        "combined_label",
        VAR_IS_ESSENTIAL
    );
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(captured_int == 17);
    CHECK(captured_dbl == 2.5);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Vector forms: callback and reference.

TEST_CASE("GParserBuilder::registerFileParameter (vector callback form)",
          "[common][parser-builder]") {
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
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(received == def_val);
    std::filesystem::remove(cfg);
}

TEST_CASE("GParserBuilder::registerFileParameter (vector reference form)",
          "[common][parser-builder]") {
    auto cfg = scratch("vec_ref");
    std::filesystem::remove(cfg);

    std::vector<double> sink;
    std::vector<double> def_val{1.5, 2.5};
    GParserBuilder gpb;
    gpb.registerFileParameter<double>(
        "vals", sink, def_val, VAR_IS_ESSENTIAL, "doubles"
    );
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(sink == def_val);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Array forms (callback + reference, with std::array<T,N>).

TEST_CASE("GParserBuilder::registerFileParameter (std::array callback form)",
          "[common][parser-builder]") {
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
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(received == def_val);
    std::filesystem::remove(cfg);
}

TEST_CASE("GParserBuilder::registerFileParameter (std::array reference form)",
          "[common][parser-builder]") {
    auto cfg = scratch("arr_ref");
    std::filesystem::remove(cfg);

    constexpr std::size_t N = 3;
    std::array<double, N> sink{};
    std::array<double, N> def_val{1.0, 2.0, 3.0};

    GParserBuilder gpb;
    gpb.registerFileParameter<double, N>(
        "arr", sink, def_val, VAR_IS_ESSENTIAL, "array ref"
    );
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(sink == def_val);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Command-line parameter registration and parsing.

TEST_CASE("GParserBuilder::registerCLParameter + parseCommandLine",
          "[common][parser-builder]") {
    GParserBuilder gpb;
    int    intval = 0;
    double dblval = 0.0;
    gpb.registerCLParameter<int>   ("intopt", intval, 100, "an integer");
    gpb.registerCLParameter<double>("dblopt", dblval, 0.5, "a double");
    CHECK(gpb.numberOfCLOptions() == 2);

    // Build an argv that supplies both options.
    char prog[]   = "prog";
    char opt1[]   = "--intopt=42";
    char opt2[]   = "--dblopt=7.25";
    char *argv[]  = {prog, opt1, opt2, nullptr};
    // parseCommandLine returns GCL_HELP_REQUESTED (true) only when --help is
    // present; success returns GCL_NO_HELP_REQUESTED (false).
    CHECK_FALSE(gpb.parseCommandLine(3, argv, /*verbose*/ false));

    CHECK(intval == 42);
    CHECK(dblval == 7.25);
}

TEST_CASE("GParserBuilder::registerCLParameter applies defaults when option absent",
          "[common][parser-builder]") {
    GParserBuilder gpb;
    int v = -1;
    gpb.registerCLParameter<int>("k", v, 999);
    char prog[] = "prog";
    char *argv[] = {prog, nullptr};
    CHECK_FALSE(gpb.parseCommandLine(1, argv, false));
    CHECK(v == 999);
}

TEST_CASE("GParserBuilder::parseCommandLine: --help returns GCL_HELP_REQUESTED",
          "[common][parser-builder]") {
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

TEST_CASE("GParserBuilder::resetFileParameterDefaults (single value)",
          "[common][parser-builder]") {
    auto cfg = scratch("reset_single");
    std::filesystem::remove(cfg);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("k", v, 1);
    gpb.resetFileParameterDefaults<int>("k", 77);

    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(v == 77);
    std::filesystem::remove(cfg);
}

TEST_CASE("GParserBuilder::resetFileParameterDefaults (vector value)",
          "[common][parser-builder]") {
    auto cfg = scratch("reset_vec");
    std::filesystem::remove(cfg);

    GParserBuilder gpb;
    std::vector<int> sink;
    gpb.registerFileParameter<int>("vs", sink, std::vector<int>{1, 2});
    gpb.resetFileParameterDefaults<int>("vs", std::vector<int>{10, 20, 30});

    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(sink == std::vector<int>{10, 20, 30});
    std::filesystem::remove(cfg);
}

TEST_CASE("GParserBuilder::resetFileParameterDefaults (array value)",
          "[common][parser-builder]") {
    auto cfg = scratch("reset_arr");
    std::filesystem::remove(cfg);

    constexpr std::size_t N = 2;
    GParserBuilder gpb;
    std::array<int, N> sink{};
    gpb.registerFileParameter<int, N>("ar", sink, std::array<int, N>{1, 1});
    gpb.resetFileParameterDefaults<int, N>("ar", std::array<int, N>{42, 43});

    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(sink == std::array<int, N>{42, 43});
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// resetFileParameterDefaults: missing key throws.

TEST_CASE("GParserBuilder::resetFileParameterDefaults: missing option throws",
          "[common][parser-builder]") {
    GParserBuilder gpb;
    CHECK_THROWS_AS(gpb.resetFileParameterDefaults<int>("nope", 1),
                    geneva_exception);
    CHECK_THROWS_AS(gpb.resetFileParameterDefaults<int>("nope", std::vector<int>{1}),
                    geneva_exception);
}

// ---------------------------------------------------------------------------
// writeConfigFile

TEST_CASE("GParserBuilder::writeConfigFile produces a parseable JSON",
          "[common][parser-builder]") {
    auto cfg = scratch("write_only");
    std::filesystem::remove(cfg);

    {
        GParserBuilder gpb;
        int    a = 0;
        double b = 0.0;
        gpb.registerFileParameter<int>   ("a", a, 1, VAR_IS_ESSENTIAL, "alpha");
        gpb.registerFileParameter<double>("b", b, 2.5, VAR_IS_ESSENTIAL, "beta");
        gpb.writeConfigFile(cfg, "test-header", /*pretty*/ true);
    }
    REQUIRE(std::filesystem::exists(cfg));

    // Reading the file back into a fresh builder should yield the same values.
    GParserBuilder gpb2;
    int    a = 0;
    double b = 0.0;
    gpb2.registerFileParameter<int>   ("a", a, 99);
    gpb2.registerFileParameter<double>("b", b, 0.0);
    REQUIRE(gpb2.parseConfigFile(cfg));
    CHECK(a == 1);
    CHECK(b == 2.5);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// writeConfigFile: error branches

TEST_CASE("GParserBuilder::writeConfigFile throws when target is an existing directory",
          "[common][parser-builder]") {
    // The scratch helper guarantees this directory exists.
    auto dir = std::filesystem::temp_directory_path() / "geneva_parserbuilder_tests";
    std::filesystem::create_directories(dir);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.writeConfigFile(dir), geneva_exception);
}

TEST_CASE("GParserBuilder::writeConfigFile throws when target file already exists",
          "[common][parser-builder]") {
    auto cfg = scratch("already_exists");
    { std::ofstream ofs(cfg); ofs << "{}"; }
    REQUIRE(std::filesystem::exists(cfg));

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.writeConfigFile(cfg), geneva_exception);
    std::filesystem::remove(cfg);
}

TEST_CASE("GParserBuilder::writeConfigFile creates a missing parent directory",
          "[common][parser-builder]") {
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

TEST_CASE("GParserBuilder::writeConfigFile throws when the parent path is not a directory",
          "[common][parser-builder]") {
    // If the intended parent path already exists but is a regular file, it cannot be turned into a
    // configuration directory -- that is a genuine error.
    auto not_a_dir = std::filesystem::temp_directory_path() / "gpb_parent_is_a_file_xyz";
    std::filesystem::remove_all(not_a_dir);
    { std::ofstream ofs(not_a_dir); ofs << "x"; }
    REQUIRE(std::filesystem::is_regular_file(not_a_dir));
    auto cfg = not_a_dir / "file.json";

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.writeConfigFile(cfg), geneva_exception);

    std::filesystem::remove(not_a_dir);
}

TEST_CASE("GParserBuilder::writeConfigFile throws for a non-.json extension",
          "[common][parser-builder]") {
    auto base = std::filesystem::temp_directory_path() / "geneva_parserbuilder_tests";
    std::filesystem::create_directories(base);
    auto cfg = base / "config_bad_ext.txt";
    std::filesystem::remove(cfg);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_THROWS_AS(gpb.writeConfigFile(cfg), geneva_exception);
}

TEST_CASE("GParserBuilder::writeConfigFile with writeAll=false omits secondary parameters",
          "[common][parser-builder]") {
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
    REQUIRE(gpb2.parseConfigFile(cfg));
    CHECK(e2 == 42);
    CHECK(s2 == 0);
    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// parseConfigFile: error branches

TEST_CASE("GParserBuilder::parseConfigFile returns false when path is an existing directory",
          "[common][parser-builder]") {
    // Any existing directory makes is_regular_file() return false → internal
    // throw is caught → parseConfigFile returns false.
    auto dir = std::filesystem::temp_directory_path() / "geneva_parserbuilder_tests";
    std::filesystem::create_directories(dir);

    GParserBuilder gpb;
    int v = 0;
    gpb.registerFileParameter<int>("v", v, 1);
    CHECK_FALSE(gpb.parseConfigFile(dir));
}

// ---------------------------------------------------------------------------
// parseCommandLine: verbose=true branch

TEST_CASE("GParserBuilder::parseCommandLine with verbose=true produces no exception",
          "[common][parser-builder]") {
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

TEST_CASE("GParserBuilder::resetFileParameterDefaults (combined form): missing option throws",
          "[common][parser-builder]") {
    GParserBuilder gpb;
    CHECK_THROWS_AS(
        (gpb.resetFileParameterDefaults<int, double>("nope", 1, 2.0)),
        geneva_exception
    );
}

TEST_CASE("GParserBuilder::resetFileParameterDefaults (array form): missing option throws",
          "[common][parser-builder]") {
    GParserBuilder gpb;
    CHECK_THROWS_AS(
        (gpb.resetFileParameterDefaults<int, 2>("nope", std::array<int, 2>{1, 2})),
        geneva_exception
    );
}

// ---------------------------------------------------------------------------
// updateConfigFile: the update-in-place contract (drop stale keys, preserve
// existing values, add newly-registered keys with defaults).

TEST_CASE("GParserBuilder::updateConfigFile drops stale keys, preserves values, defaults new keys",
          "[common][parser-builder]") {
    namespace json = boost::json;
    auto cfg = scratch("update_inplace");
    std::filesystem::remove(cfg);

    // Write a v1 config by hand: a CUSTOMIZED "answer" value (13, not the default 42), a normal
    // "scale", and a "stale_key" that the v2 schema below no longer registers.
    {
        json::object t;
        t["answer"]    = json::object{{"default", "42"}, {"value", "13"}}; // customized on disk
        t["scale"]     = json::object{{"default", "3.14"}, {"value", "3.14"}};
        t["stale_key"] = json::object{{"default", "999"}, {"value", "999"}};
        Gem::Common::writeJsonFile(cfg, json::value(std::move(t)));
    }

    // v2 schema: {answer, scale, fresh} -- "stale_key" gone, "fresh" new.
    GParserBuilder gpb;
    int    answer = 0;
    double scale  = 0.0;
    int    fresh  = 0;
    gpb.registerFileParameter<int>   ("answer", answer, 42,   VAR_IS_ESSENTIAL, "the int");
    gpb.registerFileParameter<double>("scale",  scale,  3.14, VAR_IS_ESSENTIAL, "the double");
    gpb.registerFileParameter<int>   ("fresh",  fresh,  7,    VAR_IS_ESSENTIAL, "new in v2");

    REQUIRE(gpb.updateConfigFile(cfg));

    // Applied (in-memory) values: existing preserved, new defaulted.
    CHECK(answer == 13);   // preserved from disk (not reset to the 42 default)
    CHECK(scale  == 3.14);
    CHECK(fresh  == 7);    // absent from v1 -> registered default

    // On-disk shape after the rewrite. Arithmetic scalars are stored as native JSON numbers (the input
    // above was hand-written in the earlier all-strings form, which the reader still accepts).
    const json::value after = Gem::Common::parseJsonFile(cfg);
    REQUIRE(after.is_object());
    const json::object &ao = after.get_object();
    CHECK(ao.at("answer").at("value").to_number<int>() == 13);   // value preserved on disk
    CHECK(ao.at("scale").at("value").to_number<double>() == 3.14);
    CHECK(ao.at("fresh").at("value").to_number<int>()  == 7);    // new key present, defaulted
    CHECK_FALSE(ao.contains("stale_key")); // stale key dropped
    CHECK(ao.contains("header"));          // canonical header written

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Regression (vector load_from): a config that lacks a registered vector
// parameter must keep that parameter's defaults instead of throwing. Guards the
// get_child_optional() fix that makes update-in-place work when a NEW vector
// parameter is added to the schema.

TEST_CASE("GParserBuilder: a config missing a registered vector parameter keeps its defaults",
          "[common][parser-builder]") {
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
    int              other = 0;
    std::vector<int> vints;
    gpb.registerFileParameter<int>("other", other, 5, VAR_IS_ESSENTIAL, "scalar");
    gpb.registerFileParameter<int>("vints", vints, std::vector<int>{10, 20, 30}, VAR_IS_ESSENTIAL, "a vector");

    // Before the fix a missing vector key aborted the parse; now it keeps the registered defaults.
    REQUIRE(gpb.parseConfigFile(cfg));
    CHECK(other == 5);
    CHECK(vints == std::vector<int>{10, 20, 30}); // the registered defaults survive

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Regression (Inv 15): a multi-element vector parameter must round-trip through a
// written-then-parsed config with ALL its elements intact. The old ptree format
// stored vectors as duplicate "item" keys, which a strict JSON parser collapses to
// one (silent data loss); the JSON-array representation keeps every element.

TEST_CASE("GParserBuilder: a multi-element vector round-trips as a JSON array (no dup-key collapse)",
          "[common][parser-builder][regression]") {
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
    reader.registerFileParameter<int>("vec", vec_in, std::vector<int>{1, 1, 1, 1}, VAR_IS_ESSENTIAL, "a vector");
    REQUIRE(reader.parseConfigFile(cfg));
    CHECK(vec_in == std::vector<int>{7, 8, 9, 10});

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// Native scalar typing: arithmetic types and booleans are written as native JSON
// numbers / bools (not quoted strings), while a config hand-written in the earlier
// all-strings form still loads (back-compat of the read path).

TEST_CASE("GParserBuilder: arithmetic and bool scalars write as native JSON, old string form still reads",
          "[common][parser-builder][regression]") {
    namespace json = boost::json;
    auto cfg = scratch("native_scalars");
    std::filesystem::remove(cfg);

    // parseConfigFile on a non-existent file creates it from the registered defaults.
    {
        GParserBuilder gpb;
        int         i = 20;
        double      d = 3.5;
        bool        b = true;
        std::string s = "hello";
        gpb.registerFileParameter<int>        ("i", i, 20,                  VAR_IS_ESSENTIAL, "an int");
        gpb.registerFileParameter<double>     ("d", d, 3.5,                 VAR_IS_ESSENTIAL, "a double");
        gpb.registerFileParameter<bool>       ("b", b, true,                VAR_IS_ESSENTIAL, "a bool");
        gpb.registerFileParameter<std::string>("s", s, std::string("hi"),   VAR_IS_ESSENTIAL, "a string");
        REQUIRE(gpb.parseConfigFile(cfg));
    }

    const json::value doc = Gem::Common::parseJsonFile(cfg);
    REQUIRE(doc.is_object());
    const json::object &o = doc.get_object();
    CHECK(o.at("i").at("value").is_int64());            // native number, not "20"
    CHECK(o.at("i").at("value").to_number<int>() == 20);
    CHECK(o.at("d").at("value").is_double());           // native number
    CHECK(o.at("b").at("value").is_bool());             // native bool, not "true"
    CHECK(o.at("b").at("value").as_bool());
    CHECK(o.at("s").at("value").is_string());           // strings stay strings

    // A config hand-written in the earlier all-strings form must still load.
    {
        json::object t;
        t["i"] = json::object{{"default", "20"},    {"value", "33"}};
        t["d"] = json::object{{"default", "3.5"},   {"value", "9.5"}};
        t["b"] = json::object{{"default", "true"},  {"value", "false"}};
        t["s"] = json::object{{"default", "hi"},    {"value", "world"}};
        Gem::Common::writeJsonFile(cfg, json::value(std::move(t)));
    }
    GParserBuilder reader;
    int         i2 = 0;
    double      d2 = 0.0;
    bool        b2 = true;
    std::string s2;
    reader.registerFileParameter<int>        ("i", i2, 20,                VAR_IS_ESSENTIAL, "an int");
    reader.registerFileParameter<double>     ("d", d2, 3.5,               VAR_IS_ESSENTIAL, "a double");
    reader.registerFileParameter<bool>       ("b", b2, true,              VAR_IS_ESSENTIAL, "a bool");
    reader.registerFileParameter<std::string>("s", s2, std::string("hi"), VAR_IS_ESSENTIAL, "a string");
    REQUIRE(reader.parseConfigFile(cfg));
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

TEST_CASE("GParserBuilder: a uint8_t scalar round-trips by value, not as a character",
          "[common][parser-builder][regression]") {
    namespace json = boost::json;
    auto cfg = scratch("uint8_roundtrip");
    std::filesystem::remove(cfg);

    // A freshly created config stores the uint8_t value 1 as the NUMBER 1 (not 49).
    {
        GParserBuilder gpb;
        std::uint8_t code = 1;
        gpb.registerFileParameter<std::uint8_t>("code", code, std::uint8_t(1), VAR_IS_ESSENTIAL, "8-bit code");
        REQUIRE(gpb.parseConfigFile(cfg));
    }
    {
        const json::value doc = Gem::Common::parseJsonFile(cfg);
        const json::object &o = doc.get_object();
        CHECK(o.at("code").at("value").to_number<int>() == 1);      // the number 1, not 49
        CHECK(o.at("code").at("default").to_number<int>() == 1);
    }
    {
        GParserBuilder reader;
        std::uint8_t code = 0;
        reader.registerFileParameter<std::uint8_t>("code", code, std::uint8_t(0), VAR_IS_ESSENTIAL, "8-bit code");
        REQUIRE(reader.parseConfigFile(cfg));
        CHECK(int(code) == 1);
    }

    // The earlier all-strings form ("1") must also read back as 1, not 49.
    {
        json::object t;
        t["code"] = json::object{{"default", "1"}, {"value", "1"}};
        Gem::Common::writeJsonFile(cfg, json::value(std::move(t)));
        GParserBuilder reader;
        std::uint8_t code = 0;
        reader.registerFileParameter<std::uint8_t>("code", code, std::uint8_t(0), VAR_IS_ESSENTIAL, "8-bit code");
        REQUIRE(reader.parseConfigFile(cfg));
        CHECK(int(code) == 1);
    }

    std::filesystem::remove(cfg);
}

// ---------------------------------------------------------------------------
// setEmitTimestamp: the header's creation-timestamp line can be suppressed for byte-stable,
// version-controlled output (the config-reference tree). Regression guard for that flag.

TEST_CASE("GParserBuilder::setEmitTimestamp toggles the header creation timestamp",
          "[common][parser-builder]") {
    auto with_ts    = scratch("ts_on");
    auto without_ts = scratch("ts_off");
    std::filesystem::remove(with_ts);
    std::filesystem::remove(without_ts);

    auto header_lines = [](std::filesystem::path const &p) {
        return Gem::Common::parseJsonFile(p)
            .as_object().at("header").as_object().at("comment").as_array().size();
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
    GParserBuilder::setEmitTimestamp(true); // restore the global default before any assertion can throw

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

TEST_CASE("GParserBuilder: comment-less registration honors is_essential",
          "[common][parser-builder]") {
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
