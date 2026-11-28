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

/**
 * @file
 * @brief Tests of GExternalEvaluatorIndividual against small sh scripts as the external program:
 * the time limit of an evaluation, the run directory of each exchange, the number of results, the
 * random start, the refusal of an inconsistent setup and the messages of a failed program. No test
 * needs Python.
 */

#include <sys/types.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "courtier/GProcessable.hpp"
#include "geneva/individuals/GExternalEvaluatorIndividual.hpp"

using namespace Gem::Geneva;
using Catch::Matchers::ContainsSubstring;

namespace {

namespace fs = std::filesystem;

/******************************************************************************/
/**
 * @brief A directory of the test's own that is the working directory while the object lives: the
 * evaluator puts its run directories into the working directory
 */
class GScratchDirectory {
public:
    /** @brief Creates the directory and makes it the working directory */
    explicit GScratchDirectory(std::string const &name)
      : previous_(fs::current_path())
      , path_(fs::temp_directory_path() / (name + "-" + Gem::Common::generate_uuid_v4())) {
        fs::create_directories(path_);
        fs::current_path(path_);
    }
    /** @brief Returns to the previous working directory and removes the directory */
    ~GScratchDirectory() {
        std::error_code ignored;
        fs::current_path(previous_, ignored);
        fs::remove_all(path_, ignored);
    }
    GScratchDirectory(GScratchDirectory const &) = delete;
    GScratchDirectory(GScratchDirectory &&) = delete;
    GScratchDirectory &operator=(GScratchDirectory const &) = delete;
    GScratchDirectory &operator=(GScratchDirectory &&) = delete;

    /** @brief The absolute path of the directory */
    [[nodiscard]] fs::path const &path() const {
        return path_;
    }
    /** @brief The names of the entries of the directory */
    [[nodiscard]] std::set<std::string> entries() const {
        std::set<std::string> names;
        for(auto const &entry : fs::directory_iterator(path_)) {
            names.insert(entry.path().filename().string());
        }
        return names;
    }

private:
    fs::path previous_; ///< the working directory before
    fs::path path_;     ///< the directory
};

/******************************************************************************/
/**
 * @brief What the sh script that stands in for the external program does in each step; an empty
 * text gives the standard behaviour
 */
struct GEvaluatorScript {
    std::size_t n_vars_declared = 2; ///< nVars of the setup document
    std::size_t n_vars_listed = 2;   ///< the number of entries of its vars
    std::size_t n_results = 1;       ///< n_results of the setup and result documents
    std::string setup;               ///< --setup; standard: the setup document
    std::string evaluate;            ///< --evaluate; standard: the results 1.5, 2.5, ...
    std::string archive = "exit 0";  ///< --archive
    std::string finalize = "exit 0"; ///< --finalize
};

/**
 * @brief Writes the script as dir/evaluator.sh and makes it executable
 * @return The absolute path of the script
 */
fs::path writeEvaluator(fs::path const &dir, GEvaluatorScript const &s) {
    std::string vars;
    for(std::size_t i = 0; i < s.n_vars_listed; ++i) {
        vars += std::string(i == 0 ? "" : ",") + R"({"name":"x)" + std::to_string(i) +
                R"(","type":"GConstrainedDoubleObject","values":[$v],)" +
                R"("lowerBoundary":-10,"upperBoundary":10,"initRandom":$r})";
    }
    std::string const setup_document =
        std::string(
            R"({"dataType":"setup_data","run_id":"0","n_individuals":1,"individuals":[{"type":"GGenome",)"
        ) +
        R"("nVars":)" + std::to_string(s.n_vars_declared) + R"(,"n_results":)" +
        std::to_string(s.n_results) + R"(,"nBounds":0,"vars":[)" + vars + "]}]}";

    std::string results;
    for(std::size_t i = 0; i < s.n_results; ++i) {
        results += std::string(i == 0 ? "" : ",") + R"({"rawResult":)" +
                   std::to_string(1.5 + static_cast<double>(i)) + "}";
    }
    std::string const result_document =
        std::string(
            R"({"dataType":"run_results","run_id":"0","n_individuals":1,"individuals":[{"isValid":true,)"
        ) +
        R"("n_results":)" + std::to_string(s.n_results) + R"(,"results":[)" + results + "]}]}";

    std::string const setup =
        s.setup.empty() ? "if [ \"$iv\" = random ]; then r=true; v=0; else r=false; v=-10; fi\n"
                          "cat > \"$out\" <<EOF\n" +
                              setup_document + "\nEOF\n"
                        : s.setup;
    std::string const evaluate =
        s.evaluate.empty() ? "cat > \"$out\" <<EOF\n" + result_document + "\nEOF\n" : s.evaluate;

    auto const program = dir / "evaluator.sh";
    {
        std::ofstream out(program);
        out << "#!/bin/sh\n"
               "mode=; in=; out=; iv=random\n"
               "for a in \"$@\"; do\n"
               "  case \"$a\" in\n"
               "    --init|--finalize|--setup|--evaluate|--archive) mode=\"${a#--}\" ;;\n"
               "    --input=*) in=\"${a#--input=}\" ;;\n"
               "    --output=*) out=\"${a#--output=}\" ;;\n"
               "    --initvalues=*) iv=\"${a#--initvalues=}\" ;;\n"
               "  esac\n"
               "done\n"
               "case \"$mode\" in\n"
               "init) exit 0 ;;\n"
               "setup)\n"
            << setup << "\n;;\nevaluate)\n"
            << evaluate << "\n;;\narchive)\n"
            << s.archive << "\n;;\nfinalize)\n"
            << s.finalize << "\n;;\nesac\n";
    }
    fs::permissions(program, fs::perms::owner_all, fs::perm_options::add);
    return program;
}

/**
 * @brief Writes the factory's configuration file dir/evaluator.json
 * @param more Further entries, each "key": { "value": ... }, separated by commas
 * @return The path of the file
 */
fs::path writeConfig(fs::path const &dir, fs::path const &program, std::string const &more = "") {
    auto const config = dir / "evaluator.json";
    std::ofstream out(config);
    out << R"({ "program_name": { "value": ")" << program.string() << R"(" })"
        << (more.empty() ? "" : ", " + more) << " }";
    return config;
}

/** @brief The message of the error that processing the individual ends with */
std::string processingError(Individuals::GExternalEvaluatorIndividual &ind) {
    try {
        ind.process();
    }
    catch(Gem::Courtier::g_processing_exception const &e) {
        return e.what();
    }
    return {};
}

} // namespace

/******************************************************************************/
// Regression (E1): a program that hung blocked its consumer thread for ever. With a time limit the
// evaluation ends as an error that names the limit, and the program is ended and reaped.
TEST_CASE(
    "GExternalEvaluatorIndividual ends an evaluation at its time limit",
    "[geneva][individuals][external]"
) {
    GScratchDirectory const scratch("gem-external-time-limit");
    auto const pid_file = scratch.path() / "pid";
    GEvaluatorScript s;
    s.evaluate = "echo $$ > " + pid_file.string() + "\nexec sleep 30";
    auto const config = writeConfig(
        scratch.path(),
        writeEvaluator(scratch.path(), s),
        R"("evaluation_time_limit": { "value": 1 })"
    );

    Individuals::GExternalEvaluatorIndividualFactory factory(config);
    auto ind = factory.get_as<Individuals::GExternalEvaluatorIndividual>();

    auto const start = std::chrono::steady_clock::now();
    std::string const error = processingError(*ind);
    auto const elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < std::chrono::seconds(10));
    CHECK(ind->has_errors());
    CHECK_THAT(error, ContainsSubstring("time limit of 1 s"));

    // The program is gone: not even a zombie answers a signal 0
    std::ifstream pid_in(pid_file);
    pid_t pid = 0;
    REQUIRE(pid_in >> pid);
    CHECK(::kill(pid, 0) == -1);
    CHECK(errno == ESRCH);
}

/******************************************************************************/
// Regression (E2): a program with more than one result failed every evaluation, because the
// individual stored only one result.
TEST_CASE(
    "GExternalEvaluatorIndividual stores every result of the program",
    "[geneva][individuals][external]"
) {
    GScratchDirectory const scratch("gem-external-results");
    GEvaluatorScript s;
    s.n_results = 2;
    auto const config = writeConfig(scratch.path(), writeEvaluator(scratch.path(), s));

    Individuals::GExternalEvaluatorIndividualFactory factory(config);
    auto ind = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
    CHECK(ind->getNExpectedResults() == 2);

    REQUIRE_NOTHROW(ind->process());
    REQUIRE(ind->getNStoredResults() == 2);
    CHECK(ind->raw_fitness(0) == 1.5);
    CHECK(ind->raw_fitness(1) == 2.5);
}

/******************************************************************************/
// Regression (E3): "init_values random" had no effect, every individual started at the values of the
// setup. A random start now gives each produced individual values of its own within the bounds; a
// start at the lower bound stays there.
TEST_CASE(
    "GExternalEvaluatorIndividual starts at random values when the setup asks for it",
    "[geneva][individuals][external]"
) {
    GScratchDirectory const scratch("gem-external-random-start");
    auto const program = writeEvaluator(scratch.path(), GEvaluatorScript{});

    SECTION("random") {
        Individuals::GExternalEvaluatorIndividualFactory factory(
            writeConfig(scratch.path(), program)
        );
        auto const a = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        auto const b = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        auto const va = a->parameterValues<double>();
        auto const vb = b->parameterValues<double>();
        REQUIRE(va.size() == 2);
        CHECK(va != vb);
        for(double const v : va) {
            CHECK((v >= -10. and v <= 10.));
        }
        for(double const v : vb) {
            CHECK((v >= -10. and v <= 10.));
        }
    }

    SECTION("min") {
        Individuals::GExternalEvaluatorIndividualFactory factory(
            writeConfig(scratch.path(), program, R"("init_values": { "value": "min" })")
        );
        auto const a = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        auto const b = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        CHECK(a->parameterValues<double>() == std::vector<double>{-10., -10.});
        CHECK(b->parameterValues<double>() == std::vector<double>{-10., -10.});
    }
}

/******************************************************************************/
// Regression (E4): the setup file stayed behind when the setup failed after writing it, and the
// archive's input file when --archive failed. Both now live in a run directory that is removed.
TEST_CASE(
    "GExternalEvaluatorIndividual leaves no files behind when the setup or the archive fails",
    "[geneva][individuals][external]"
) {
    GScratchDirectory const scratch("gem-external-leftovers");

    SECTION("setup") {
        GEvaluatorScript s;
        s.setup = "echo '{ not json' > \"$out\"";
        auto const config = writeConfig(scratch.path(), writeEvaluator(scratch.path(), s));
        auto const before = scratch.entries();

        Individuals::GExternalEvaluatorIndividualFactory factory(config);
        CHECK_THROWS(factory.get_as<Individuals::GExternalEvaluatorIndividual>());
        CHECK(scratch.entries() == before);
    }

    SECTION("archive") {
        GEvaluatorScript s;
        s.archive = "exit 1";
        auto const config = writeConfig(scratch.path(), writeEvaluator(scratch.path(), s));

        Individuals::GExternalEvaluatorIndividualFactory factory(config);
        auto const ind = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        std::vector<std::unique_ptr<Individuals::GExternalEvaluatorIndividual>> archive;
        archive.push_back(std::make_unique<Individuals::GExternalEvaluatorIndividual>(*ind));

        auto const before = scratch.entries();
        CHECK_THROWS(Individuals::GExternalEvaluatorIndividual::archive(archive));
        CHECK(scratch.entries() == before);
    }
}

/******************************************************************************/
// Regression (E5): a failing --finalize ended the whole process from the factory's destructor. It
// is now reported, and the program goes on.
TEST_CASE(
    "GExternalEvaluatorIndividual reports a failing finalize step and goes on",
    "[geneva][individuals][external]"
) {
    GScratchDirectory const scratch("gem-external-finalize");
    auto const marker = scratch.path() / "finalized";
    GEvaluatorScript s;
    s.finalize = "touch " + marker.string() + "\nexit 1";
    auto const config = writeConfig(scratch.path(), writeEvaluator(scratch.path(), s));

    {
        Individuals::GExternalEvaluatorIndividualFactory factory(config);
        (void)factory.get_as<Individuals::GExternalEvaluatorIndividual>();
    } // the destructor runs --finalize

    // Still here: the failure was reported, in the error log of the working directory
    CHECK(fs::exists(marker));
    std::ifstream log(scratch.path() / "GEM-EXCEPTION.log");
    std::string const text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    CHECK_THAT(text, ContainsSubstring("--finalize"));
}

/******************************************************************************/
// Regression (E6): a setup whose nVars and vars disagree was truncated silently.
TEST_CASE(
    "GExternalEvaluatorIndividual refuses a setup whose number of variables disagrees",
    "[geneva][individuals][external]"
) {
    GScratchDirectory const scratch("gem-external-nvars");
    GEvaluatorScript s;
    s.n_vars_declared = 3;
    s.n_vars_listed = 4;
    auto const config = writeConfig(scratch.path(), writeEvaluator(scratch.path(), s));

    Individuals::GExternalEvaluatorIndividualFactory factory(config);
    std::string error;
    try {
        (void)factory.get_as<Individuals::GExternalEvaluatorIndividual>();
    }
    catch(std::exception const &e) {
        error = e.what();
    }
    CHECK_THAT(error, ContainsSubstring("nVars 3"));
    CHECK_THAT(error, ContainsSubstring("4 variables"));
}

/******************************************************************************/
// Regression (E7, E8): a missing result named neither the command nor the program's output; a
// failed program copied its whole output into the error; a signal was reported as an error code.
TEST_CASE(
    "GExternalEvaluatorIndividual says how the program failed",
    "[geneva][individuals][external]"
) {
    GScratchDirectory const scratch("gem-external-messages");

    SECTION("no result after exit 0") {
        GEvaluatorScript s;
        s.evaluate = "echo 'the last words of the program'\nexit 0";
        Individuals::GExternalEvaluatorIndividualFactory factory(
            writeConfig(scratch.path(), writeEvaluator(scratch.path(), s))
        );
        auto ind = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        std::string const error = processingError(*ind);
        CHECK_THAT(error, ContainsSubstring("--evaluate"));
        CHECK_THAT(error, ContainsSubstring("the last words of the program"));
    }

    SECTION("a long output") {
        GEvaluatorScript s;
        s.evaluate = "seq 100000\nexit 1";
        Individuals::GExternalEvaluatorIndividualFactory factory(
            writeConfig(scratch.path(), writeEvaluator(scratch.path(), s))
        );
        auto ind = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        std::string const error = processingError(*ind);
        CHECK_THAT(error, ContainsSubstring("100000"));
        CHECK(error.size() < 10000);
    }

    SECTION("a signal") {
        GEvaluatorScript s;
        s.evaluate = "kill -KILL $$";
        Individuals::GExternalEvaluatorIndividualFactory factory(
            writeConfig(scratch.path(), writeEvaluator(scratch.path(), s))
        );
        auto ind = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        CHECK_THAT(processingError(*ind), ContainsSubstring("signal 9"));
    }
}

/******************************************************************************/
// Each evaluation exchanges its files in a run directory of its own in the working directory, so
// evaluations of other objects, processes or hosts that share the working directory never meet the
// same files; the program itself runs in the caller's working directory. The directories stay only
// when remove_exec_temporaries is false.
TEST_CASE(
    "GExternalEvaluatorIndividual exchanges the files of each evaluation in a run directory",
    "[geneva][individuals][external]"
) {
    GScratchDirectory const scratch("gem-external-run-directory");
    auto const cwd_file = scratch.path() / "cwd";
    GEvaluatorScript s;
    s.evaluate = "pwd > " + cwd_file.string() + "\n" + "cat > \"$out\" <<EOF\n" +
                 R"({"n_individuals":1,"individuals":[{"isValid":true,"n_results":1,)" +
                 R"("results":[{"rawResult":1.5}]}]})" + "\nEOF";
    auto const program = writeEvaluator(scratch.path(), s);

    SECTION("kept") {
        Individuals::GExternalEvaluatorIndividualFactory factory(writeConfig(
            scratch.path(),
            program,
            R"("parameter_file": { "value": "params" }, "remove_exec_temporaries": { "value": false })"
        ));
        auto ind = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        auto const before = scratch.entries();
        for(int i = 0; i < 2; ++i) {
            ind->mark_as_due_for_processing();
            REQUIRE_NOTHROW(ind->process());
        }

        std::vector<fs::path> runs;
        for(auto const &name : scratch.entries()) {
            if(not before.contains(name) and name != "cwd") {
                runs.push_back(scratch.path() / name);
            }
        }
        REQUIRE(runs.size() == 2);
        for(auto const &run : runs) {
            CHECK(fs::is_directory(run));
            CHECK(fs::exists(run / "params.json"));
            CHECK(fs::exists(run / "result.json"));
        }

        std::ifstream cwd_in(cwd_file);
        std::string cwd;
        std::getline(cwd_in, cwd);
        CHECK(fs::equivalent(cwd, scratch.path()));
    }

    SECTION("removed") {
        Individuals::GExternalEvaluatorIndividualFactory factory(
            writeConfig(scratch.path(), program)
        );
        auto ind = factory.get_as<Individuals::GExternalEvaluatorIndividual>();
        auto before = scratch.entries();
        REQUIRE_NOTHROW(ind->process());
        before.insert("cwd");
        CHECK(scratch.entries() == before);
    }
}
