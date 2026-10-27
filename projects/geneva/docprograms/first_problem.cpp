/**
 * @file first_problem.cpp
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
 * The manual's quick start in one file: a complete, minimal optimization problem and the program
 * that solves it.
 *
 * The problem is a bowl, f(x, y) = (x - 1)^2 + (y + 2)^2, whose minimum f = 0 lies at (1, -2) -- a
 * known answer, so a reader can check what the run reports. The program hands the problem to Go2,
 * lets the default algorithm (an evolutionary algorithm) search it on all cores, records the search
 * with a progress plotter (written to progressScan.C, a ROOT macro the manual renders), and prints
 * the best solution found.
 *
 * Like every documentation program it runs as a test. Geneva's random numbers are not reproducible
 * from a seed (DEVELOPMENT-INVARIANTS.md, Invariant 18), so the check at the end is a tolerance on
 * the known minimum, not an exact value.
 */

#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

#include "geneva/Go2.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"
#include "weft/GArchivePolymorphic.hpp"

namespace gen = Gem::Geneva::Genome;                   // the genome and its builder
namespace oa = Gem::Geneva::OptimizationAlgorithms;    // algorithms and their adaption

namespace first {

//! [first-problem-class]
/**
 * A candidate solution of the problem: two floating-point parameters x and y, each between -5 and
 * 5, judged by how far f(x, y) = (x - 1)^2 + (y + 2)^2 lies above its minimum.
 */
class FirstProblem final : public gen::GGenomeT<FirstProblem> {
public:
    using gemfony_flat_individual = void; // the genome is all the state there is
    static constexpr std::string_view class_name = "FirstProblem";

    /** @brief Declares the parameters: two doubles in [-5, 5], both starting at 4 */
    FirstProblem() {
        gen::GGenomeBuilder builder;
        builder.addDoubleGroup(2, -5., 5.).init(4.);
        this->setGenome(builder.build());
    }
    FirstProblem(const FirstProblem &) = default;

    /** @brief How the parameters are mutated: Gaussian steps, adapted during the run */
    [[nodiscard]] std::shared_ptr<oa::GAdaptionConfigBase> adaptionConfig() const {
        auto config = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        config->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        return config;
    }

protected:
    /** @brief The evaluation: one number per criterion, here a single one */
    std::vector<double> evaluate() override {
        std::vector<double> p;
        this->streamline<double>(p); // the parameter values, in the units declared above
        const double dx = p[0] - 1.;
        const double dy = p[1] + 2.;
        return {dx * dx + dy * dy};
    }
};
//! [first-problem-class]

} // namespace first

GEM_REGISTER_ARCHIVABLE(first::FirstProblem) // NOLINT

/******************************************************************************/

//! [first-problem-main]
int main(int argc, char **argv) {
    using namespace Gem::Geneva;

    // Reads the command line and ./config/Go2.json (written with defaults on the first run).
    Go2 go(argc, argv, "./config/Go2.json");
    if(go.clientMode()) {
        return go.clientRun(); // a worker process in a networked run
    }

    // The problem: one prototype candidate, and how the evolutionary algorithm mutates it.
    auto problem = std::make_shared<first::FirstProblem>();
    go.registerAdaptionConfig("PERSONALITY_EA", problem->adaptionConfig());
    go.push_back(problem);

    // Record every candidate the search evaluates, plotted over the two parameters.
    auto plotter = std::make_shared<GProgressPlotter>();
    plotter->setProfileSpec("d(0, -5, 5), d(1, -5, 5)");
    go.registerPluggableOM(plotter);

    // Run, then read the best candidate found.
    auto best = go.optimize()->getBestGlobalIndividual<first::FirstProblem>();
    std::vector<double> xy;
    best->streamline<double>(xy);
    std::cout << "best solution: x = " << xy[0] << ", y = " << xy[1]
              << ", f = " << best->raw_fitness(0) << '\n';
    //! [first-problem-main]

    // A tolerance on the known minimum at (1, -2), not an exact value (Invariant 18).
    const bool found = std::abs(xy[0] - 1.) < 0.1 && std::abs(xy[1] + 2.) < 0.1;
    if(not found) {
        std::cerr << "first_problem: the search did not find the minimum at (1, -2)\n";
    }
    return found ? 0 : 1;
}
