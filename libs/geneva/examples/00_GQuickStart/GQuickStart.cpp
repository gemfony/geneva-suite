/**
 * @file GQuickStart.cpp
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
 * The quick start in one file: a small, complete optimization problem and the program that solves
 * it. This is the first example to read, and the one the manual's Quick Start chapter walks through
 * line by line (the //! [quickstart-tour-*] markers bracket the regions it quotes).
 *
 * The problem is a bowl, f(x, y) = (x - 1)^2 + (y + 2)^2, whose minimum f = 0 lies at (1, -2) -- a
 * known answer, so a reader can check what the run reports. The program hands the problem to
 * GOptimizer, lets the default algorithm (an evolutionary algorithm) search it on all cores,
 * records the search with a progress plotter (its stream records/progress.csv, and its plot
 * records/progress.C, a ROOT macro), and prints the best solution found.
 *
 * The example runs as a test. Geneva's random numbers are not reproducible from a seed
 * (DEVELOPMENT-INVARIANTS.md, Invariant 18), so the check at the end is a tolerance on the known
 * minimum, not an exact value.
 */

#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#include "geneva/GOptimizer.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"
#include "weft/GArchivePolymorphic.hpp"

namespace geneva = Gem::Geneva;                     // GOptimizer and the optimization monitors
namespace Genome = Gem::Geneva::Genome;             // the genome and its builder
namespace OA = Gem::Geneva::OptimizationAlgorithms; // algorithms and their adaption

namespace first {

//! [quickstart-tour-class#1]
/** A candidate: x and y in [-5, 5], judged by f(x, y) = (x - 1)^2 + (y + 2)^2 */
class GFirstProblem final : public Genome::GGenomeT<GFirstProblem> {
public:
    using genome_only = void; // the genome is all the state there is
    static constexpr std::string_view class_name = "GFirstProblem";

    /** @brief Declares the parameters: two doubles in [-5, 5], both starting at 4 */
    GFirstProblem() {
        Genome::GGenomeBuilder builder;
        builder.addDoubleGroup(2, -5., 5.).init(4.);
        this->setGenome(builder.build());
    }
    GFirstProblem(const GFirstProblem &) = default;

    /** @brief How the parameters are mutated: Gaussian steps, adapted during the run */
    [[nodiscard]] std::shared_ptr<OA::GAdaptionConfigBase> adaptionConfig() const {
        auto config = OA::makeAdaptionConfig<OA::GAdaptionConfigBase>(*this);
        config->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        return config;
    }

protected:
    /** @brief The evaluation: one number per criterion, here a single one */
    std::vector<double> evaluate() override {
        auto const [x, y] = this->parameters<double, 2>(); // in the units declared above
        const double dx = x - 1.;
        const double dy = y + 2.;
        return {dx * dx + dy * dy};
    }
};
//! [quickstart-tour-class#1]

} /* namespace first */

//! [quickstart-tour-class#2]
GEM_REGISTER_ARCHIVABLE(first::GFirstProblem) // NOLINT
//! [quickstart-tour-class#2]

/******************************************************************************/

// clang-format off
//! [quickstart-tour-main]
/** The search, on the process that submits the work; returns the exit code */
int solve(geneva::GOptimizer &go) {
    // One prototype candidate, and how the evolutionary algorithm mutates it.
    auto problem = std::make_shared<first::GFirstProblem>();
    go.registerAdaptionConfig("ea", problem->adaptionConfig());
    go.push_back(problem);

    // Record every evaluated candidate over x and y, above a heat map of f.
    auto plotter = std::make_shared<geneva::GProgressPlotter>();
    plotter->setProfileSpec("d(0, -5, 5), d(1, -5, 5)");
    plotter->setVariableNames({"x", "y"});
    plotter->setLandscapeResolution(40);
    go.registerPluggableOM(plotter);

    // Run, then read the best candidate found.
    auto best = go.optimize().getBestGlobalIndividual<first::GFirstProblem>();
    auto const [x, y] = best->parameters<double, 2>();
    std::cout << "best solution: x = " << x << ", y = " << y
              << ", f = " << best->raw_fitness(0) << '\n';

    // Check the answer against the known minimum at (1, -2) -- with a tolerance, as
    // the search is stochastic.
    const bool close_enough = std::abs(x - 1.) < 0.1 && std::abs(y + 2.) < 0.1;
    if(not close_enough) {
        std::cerr << "GQuickStart: the search did not get close to (1, -2)\n";
    }
    return close_enough ? 0 : 1;
}

int main(int argc, char **argv) {
    // Reads the command line and ./config/GOptimizer.json (written with defaults if absent).
    // Only the process that submits the work runs solve(); a worker of a networked run
    // evaluates what it is sent.
    return geneva::runOptimization(argc, argv, {
        .produce = [](geneva::GOptimizer &go, const auto &) { return solve(go); }
    });
}
//! [quickstart-tour-main]
// clang-format on
