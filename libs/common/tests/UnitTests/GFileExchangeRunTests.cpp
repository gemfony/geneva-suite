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

// The run with file exchange (common/GFileExchangeRun.hpp): a directory per run, the inputs written,
// the result read only after a clean end, the directory removed on every path. The programs are sh
// with a script of their own, as in the tests of runExternalCommand().

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/GCommonHelperFunctions.hpp"
#include "common/GExceptions.hpp"
#include "common/GFileExchangeRun.hpp"
#include "common/GStopRequest.hpp"

#include "GChildProcess.hpp"

using namespace Gem::Common;
using namespace Gem::Common::Tests;

namespace {

/** @brief A fresh, empty parent directory for the runs of one test case */
std::filesystem::path freshParent(std::string const &tag) {
    auto const dir = std::filesystem::temp_directory_path() / "gem_file_exchange_tests" / tag;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/** @brief The number of entries in @p dir */
std::size_t entriesIn(std::filesystem::path const &dir) {
    return static_cast<std::size_t>(std::distance(
        std::filesystem::directory_iterator(dir),
        std::filesystem::directory_iterator()
    ));
}

/** @brief A spec that runs sh with @p script inside the run directory */
GFileExchangeSpec shInside(std::string const &script) {
    GFileExchangeSpec spec;
    spec.program = "sh";
    spec.arguments = {"-c", script};
    spec.run_inside = true;
    return spec;
}

} // namespace

TEST_CASE(
    "runWithFileExchange: writes the inputs and reads the result and removes the directory",
    "[common][file-exchange]"
) {
    auto const parent = freshParent("success");
    auto spec = shInside("echo working; tr a-z A-Z < in/parameters.txt > result.txt");
    spec.inputs = {{"in/parameters.txt", "x=1\n"}};
    spec.result = "result.txt";

    auto const run = runWithFileExchange(spec, parent);
    CHECK(run.program.end == ProgramEnd::EXITED);
    CHECK(run.program.code == 0);
    REQUIRE(run.result.has_value());
    CHECK(*run.result == "X=1\n");
    CHECK(run.output_tail == "working\n");
    CHECK(run.directory.parent_path() == std::filesystem::absolute(parent));
    CHECK(run.directory.filename().string().starts_with("sh-"));
    CHECK(not std::filesystem::exists(run.directory));
    CHECK(entriesIn(parent) == 0);
}

TEST_CASE(
    "runWithFileExchange: a failed program gives its output tail but no result and no directory",
    "[common][file-exchange]"
) {
    auto const parent = freshParent("failure");
    // 30 lines of output: the tail holds the last 20
    auto spec = shInside(
        "i=1; while [ $i -le 30 ]; do echo line $i; i=$((i+1)); done; "
        "echo partial > result.txt; exit 3"
    );
    spec.result = "result.txt";

    auto const run = runWithFileExchange(spec, parent);
    CHECK(run.program.end == ProgramEnd::EXITED);
    CHECK(run.program.code == 3);
    CHECK(not run.result.has_value());
    CHECK(run.output_tail.starts_with("line 11\n"));
    CHECK(run.output_tail.ends_with("line 30\n"));
    CHECK(entriesIn(parent) == 0);
}

TEST_CASE(
    "runWithFileExchange: the directory is removed when an exception leaves the run",
    "[common][file-exchange]"
) {
    auto const parent = freshParent("exception");
    GFileExchangeSpec spec;
    spec.program = "/gem-no-such-directory/gem-no-such-program";
    spec.inputs = {{"parameters.txt", "written before the start"}};
    CHECK_THROWS_AS(runWithFileExchange(spec, parent), gem_exception);
    CHECK(entriesIn(parent) == 0);

    // An absolute input path is refused before anything is written
    auto absolute = shInside("exit 0");
    absolute.inputs = {{"/tmp/gem-absolute-input.txt", "x"}};
    CHECK_THROWS_WITH(
        runWithFileExchange(absolute, parent),
        Catch::Matchers::ContainsSubstring("must be relative")
    );
    CHECK(entriesIn(parent) == 0);
}

TEST_CASE("runWithFileExchange: keep leaves the directory in place", "[common][file-exchange]") {
    auto const parent = freshParent("keep");
    auto spec = shInside("echo kept; exit 1");
    spec.inputs = {{"parameters.txt", "p"}};
    spec.keep = true;

    auto const run = runWithFileExchange(spec, parent);
    CHECK(run.program.code == 1);
    REQUIRE(std::filesystem::is_directory(run.directory));
    CHECK(std::filesystem::exists(run.directory / "parameters.txt"));
    CHECK(loadTextDataFromFile(run.directory / PROGRAM_OUTPUT_FILE) == "kept\n");
    std::filesystem::remove_all(parent);
}

TEST_CASE(
    "runWithFileExchange: outside the run directory RUN_DIRECTORY names it in the arguments",
    "[common][file-exchange]"
) {
    auto const parent = freshParent("outside");
    GFileExchangeSpec spec;
    spec.program = "sh";
    spec.arguments = {
        "-c",
        R"(pwd > "$3"; cat "$1" > "$2")",
        "sh",
        std::string(RUN_DIRECTORY) + "/in.txt",
        std::string(RUN_DIRECTORY) + "/result.txt",
        std::string(RUN_DIRECTORY) + "/where.txt"
    };
    spec.inputs = {{"in.txt", "content"}};
    spec.result = "result.txt";
    spec.keep = true;

    auto const run = runWithFileExchange(spec, parent);
    REQUIRE(run.result.has_value());
    CHECK(*run.result == "content");
    // The program ran in the caller's working directory
    CHECK(
        std::filesystem::path(trimWhitespace(loadTextDataFromFile(run.directory / "where.txt"))) ==
        std::filesystem::canonical(std::filesystem::current_path())
    );
    std::filesystem::remove_all(parent);
}

TEST_CASE(
    "runWithFileExchange: link_from entries are linked and the run's own directories are real",
    "[common][file-exchange]"
) {
    auto const parent = freshParent("link-from");
    auto const templ = parent / "template";
    std::filesystem::create_directories(templ / "config");
    std::filesystem::create_directories(templ / "pictures");
    {
        std::ofstream(templ / "pictures" / "picture.txt") << "picture";
        std::ofstream(templ / "config" / "template-only.json") << "{}";
        std::ofstream(templ / "module.txt") << "module";
    }
    auto spec = shInside(
        "test -L pictures && test -L module.txt && test -d config && test ! -L config && "
        "test ! -e config/template-only.json && cat module.txt pictures/picture.txt config/a.json "
        "> result.txt"
    );
    spec.inputs = {{"config/a.json", "{\"a\":1}"}};
    spec.link_from = templ;
    spec.result = "result.txt";

    auto const run = runWithFileExchange(spec, parent / "runs");
    CHECK(run.program.code == 0);
    REQUIRE(run.result.has_value());
    CHECK(*run.result == "modulepicture{\"a\":1}");
    // Nothing was written into the template, and the links are gone with the run directory
    CHECK(not std::filesystem::exists(templ / "config" / "a.json"));
    CHECK(std::filesystem::exists(templ / "pictures" / "picture.txt"));
    CHECK(entriesIn(parent / "runs") == 0);
    std::filesystem::remove_all(parent);
}

TEST_CASE(
    "runWithFileExchange: concurrent runs get directories and results of their own",
    "[common][file-exchange]"
) {
    auto const parent = freshParent("concurrent");
    constexpr std::size_t n_threads = 8;
    std::vector<std::string> results(n_threads);
    std::vector<std::filesystem::path> directories(n_threads);
    {
        std::vector<std::jthread> threads;
        for(std::size_t t = 0; t < n_threads; ++t) {
            threads.emplace_back([t, &parent, &results, &directories] {
                auto spec = shInside("sleep 0.1; cat parameters.txt > result.txt");
                spec.inputs = {{"parameters.txt", "thread " + std::to_string(t)}};
                spec.result = "result.txt";
                auto const run = runWithFileExchange(spec, parent);
                results[t] = run.result.value_or("no result");
                directories[t] = run.directory;
            });
        }
    }
    std::set<std::filesystem::path> const distinct(directories.begin(), directories.end());
    CHECK(distinct.size() == n_threads);
    for(std::size_t t = 0; t < n_threads; ++t) {
        CHECK(results[t] == "thread " + std::to_string(t));
    }
    CHECK(entriesIn(parent) == 0);
}

TEST_CASE(
    "runWithFileExchange: an interrupted run gives no result even if the program wrote one",
    "[common][file-exchange][stop-request]"
) {
    auto const parent = freshParent("interrupted");
    // The stop flag cannot be cleared: the run takes place in a child process. The program writes its
    // result, then asks for the stop (as a batch system's warning would reach it too) and exits with 0.
    CHECK(passed(inChild([&parent] {
        installStopHandlers();
        auto spec = shInside("echo truncated > result.txt; kill -USR1 $PPID; exit 0");
        spec.result = "result.txt";
        spec.limits.end_on_stop_request = true;
        auto const run = runWithFileExchange(spec, parent);
        bool const held = run.program.end == ProgramEnd::STOP_REQUEST and run.program.code == 0 and
                          not run.result.has_value();
        return held ? CHILD_PASSED : CHILD_FAILED;
    })));
    CHECK(entriesIn(parent) == 0);
}
