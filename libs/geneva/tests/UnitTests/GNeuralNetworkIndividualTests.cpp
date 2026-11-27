/**
 * @file GenevaStandardTests.cpp
 */

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
 * @brief Tests of GNeuralNetworkIndividual's output files: the C++ header of the trained network
 * (writeTrainedNetwork()) and the figure of the network's output (writeVisualizationFile()).
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <unistd.h> // getpid()

#include "GNeuralNetworkTestData.hpp"
#include "common/GExceptions.hpp"
#include "dietrich/plotting/GPlotDesigner.hpp"
#include "geneva/individuals/GNeuralNetworkIndividual.hpp"

using Gem::Geneva::Individuals::GNeuralNetworkIndividual;
namespace Individuals = Gem::Geneva::Individuals;

namespace {

/** @brief A network individual over the test's training data, with the default settings. */
GNeuralNetworkIndividual makeIndividual() {
    Individuals::Testing::provideNeuralNetworkTrainingData();
    return {
        Individuals::GNN_DEF_MINVAR,
        Individuals::GNN_DEF_MAXVAR,
        Individuals::GNN_DEF_SIGMA,
        Individuals::GNN_DEF_SIGMASIGMA,
        Individuals::GNN_DEF_MINSIGMA,
        Individuals::GNN_DEF_MAXSIGMA,
        Individuals::GNN_DEF_ADPROB,
        Individuals::GNN_DEF_ADAPTADPROB,
        Individuals::GNN_DEF_MINADPROB,
        Individuals::GNN_DEF_MAXADPROB
    };
}

} // namespace

/******************************************************************************/
// Both writers tested the emptiness of their file name twice (`name.empty() || name.empty()`, once
// `name == "" || name.empty()`), a duplicate, not a second condition. Pins the one refusal that is
// meant: no file name, no file.
TEST_CASE(
    "The writers of GNeuralNetworkIndividual refuse an empty file name",
    "[individuals][nn]"
) {
    GNeuralNetworkIndividual ind = makeIndividual();
    CHECK_THROWS_AS(ind.writeVisualizationFile(""), Gem::Common::gem_exception);
    CHECK_THROWS_AS(ind.writeTrainedNetwork(""), Gem::Common::gem_exception);

    // A file name that can be written is accepted.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("geneva-nn-writers-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    ind.writeTrainedNetwork((dir / "trainedNetwork.hpp").string());
    CHECK(std::filesystem::exists(dir / "trainedNetwork.hpp"));
    std::filesystem::remove_all(dir);
}

/******************************************************************************/
// writeVisualizationFile() wrote a C++ program, which in turn wrote a ROOT macro; the program did
// not compile as written (a Boost generator without its header), sampled y over the range of x,
// and its ROOT macro drew no axes when the lowest band of the output was empty. The figure is now
// drawn through dietrich: its data description -- the one every backend draws from -- holds one
// colour-mapped surface on a regular grid over the whole range of the training data.
TEST_CASE(
    "The figure of the network's output covers the range of its training data",
    "[individuals][nn]"
) {
    const GNeuralNetworkIndividual ind = makeIndividual();
    const auto series = Individuals::Testing::seriesOf(ind.outputPlot());

    REQUIRE(series.size() == 1);
    CHECK(series[0].header.contains("plotkind=graph_3d"));
    CHECK(series[0].header.contains("colour_map"));

    constexpr std::size_t N = GNeuralNetworkIndividual::OUTPUT_PLOT_POINTS_PER_AXIS;
    const auto &rows = series[0].rows;
    REQUIRE(rows.size() == N * N);

    // A grid over the range the training data report (+-0.5 in both inputs for the test's
    // hyper-cube), x and y each on their own range
    std::set<double> xs;
    std::set<double> ys;
    std::set<std::array<double, 2>> points;
    for(const auto &row : rows) {
        REQUIRE(row.size() == 3); // x, y and the output
        xs.insert(row[0]);
        ys.insert(row[1]);
        points.insert({row[0], row[1]});
        CHECK(row[2] >= 0.); // a sigmoid network's output
        CHECK(row[2] <= 1.);
    }
    const auto range = Individuals::nnTrainingDataStore()->getInitRange();
    REQUIRE(range.size() == 2);
    CHECK(xs.size() == N);
    CHECK(ys.size() == N);
    CHECK(points.size() == N * N);
    CHECK(*xs.begin() == Catch::Approx(std::get<0>(range[0])));
    CHECK(*xs.rbegin() == Catch::Approx(std::get<1>(range[0])));
    CHECK(*ys.begin() == Catch::Approx(std::get<0>(range[1])));
    CHECK(*ys.rbegin() == Catch::Approx(std::get<1>(range[1])));
}

/******************************************************************************/
// The figure is written through the backend the caller chooses, ROOT by default.
TEST_CASE(
    "The figure of the network's output is written through any backend",
    "[individuals][nn]"
) {
    const GNeuralNetworkIndividual ind = makeIndividual();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("geneva-nn-figure-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);

    const auto text_of = [](const std::filesystem::path &file) {
        std::ifstream in(file);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    ind.writeVisualizationFile((dir / "visualization.C").string());
    CHECK(text_of(dir / "visualization.C").contains("TGraph2D"));

    ind.writeVisualizationFile(
        (dir / "visualization.py").string(),
        Gem::Dietrich::plotBackend::MATPLOTLIB
    );
    CHECK(text_of(dir / "visualization.py").contains("matplotlib"));

    // A file that cannot be written is refused, not left unwritten in silence
    CHECK_THROWS_AS(
        ind.writeVisualizationFile((dir / "no-such-directory" / "visualization.C").string()),
        Gem::Common::gem_exception
    );
    std::filesystem::remove_all(dir);
}

/******************************************************************************/
// The C++ header writeTrainedNetwork() writes declared its loop counters `register`, a storage
// class removed in C++17: clang refused the header, gcc warned. The header is compiled here as a
// user's program would compile it, with the compiler of this build, as C++17 and with warnings as
// errors.
TEST_CASE("The header of the trained network compiles as C++17", "[individuals][nn]") {
    GNeuralNetworkIndividual ind = makeIndividual();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("geneva-nn-header-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path header = dir / "trainedNetwork.hpp";
    const std::filesystem::path log = dir / "compile.log";
    ind.writeTrainedNetwork(header.string());

    const std::string command = std::string("\"") + GENEVA_TEST_CXX_COMPILER +
                                "\" -std=c++17 -fsyntax-only -Werror -x c++ \"" + header.string() +
                                "\" > \"" + log.string() + "\" 2>&1";
    const int status =
        std::system(command.c_str()); // NOLINT(cert-env33-c): the build's own compiler
    std::ifstream in(log);
    INFO(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    CHECK(status == 0);
    std::filesystem::remove_all(dir);
}
