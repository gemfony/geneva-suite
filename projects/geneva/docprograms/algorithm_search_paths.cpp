/**
 * @file algorithm_search_paths.cpp
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
 * How each optimization algorithm searches, drawn by dietrich.
 *
 * Each of the ten registered algorithms is run on a two-dimensional test function chosen to show
 * what that algorithm is good at, and watched by a monitor. One figure per algorithm: the
 * landscape as a colour map seen from above, every individual the algorithm held after each of
 * its iterations as a marker on top of it, and the position of the best individual known so far
 * as a line -- the path the search took.
 *
 * The programme writes ROOT macros, search-<mnemonic>.C; the manual's `make figures` target
 * renders them. It is registered as a test, because a documentation programme that no longer
 * compiles -- or no longer produces the figures the manual prints -- is worse than none.
 *
 * NOTE on reproducibility: the landscapes are deterministic, the searches are NOT. Geneva's random
 * numbers come from concurrent producer threads seeded from entropy and are not reproducible from
 * a seed (DEVELOPMENT-INVARIANTS.md, Invariant 18), so a rerun draws different -- but
 * statistically equivalent -- paths. The check at the end is therefore a statistical bar.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "common/GGlobalDefines.hpp"
#include "common/GReflectiveInterfaceT.hpp"
#include "dietrich/GPlotDesigner.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GenevaInitializer.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/individuals/GBenchmarkFunctions.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/oa/GAntColonyOptimization.hpp"
#include "geneva/oa/GBasePluggableOM.hpp"
#include "geneva/oa/GConjugateGradientDescent.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GGeneralizedSimulatedAnnealing.hpp"
#include "geneva/oa/GNelderMead.hpp"
#include "geneva/oa/GParameterScan.hpp"
#include "geneva/oa/GSepCmaEvolutionStrategy.hpp"
#include "geneva/oa/GSimulatedAnnealing.hpp"
#include "geneva/oa/GStandardPSO2011.hpp"
#include "geneva/oa/GSwarmAlgorithm.hpp"

using namespace Gem::Geneva;
using namespace Gem::Dietrich;

namespace {

/** Grid nodes per axis of the landscape map. 101 x 101 evaluations per figure. */
constexpr std::size_t MAP_NODES = 101;

/** Iterations per run: enough to show the character of a search, short by default (Invariant 22). */
constexpr std::uint32_t ITERATIONS = 40;

/** Where the landscape's colour map is shown; the search is drawn onto it. */
constexpr surfaceProjection MAP_PLANE = surfaceProjection::FLOOR;

/******************************************************************************/
/**
 * A pluggable optimization monitor that records where an algorithm searches.
 *
 * After every iteration it appends the position of each individual the algorithm holds to one
 * series, and the position of the best individual known so far to another. It reads only the
 * population and the individuals' first two parameters, so it works unchanged for every algorithm.
 */
class GSearchTraceMonitor final
  : public Gem::Common::GReflectiveInterfaceT<GSearchTraceMonitor, oa::GBasePluggableOM> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief The monitor's state is run state wired to a plot, not configuration; hence the
     *  explicit empty member list the CRTP base requires. */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GSearchTraceMonitor";

    GSearchTraceMonitor() = default;
    GSearchTraceMonitor(const GSearchTraceMonitor &) = default;
    ~GSearchTraceMonitor() override = default;

    /**
     * @brief Wires the monitor to the series it fills.
     *
     * @param log The log holding the landscape map
     * @param points The series that receives every individual as an (x0, x1) row
     * @param path The series that receives the best-so-far position after each iteration
     */
    GSearchTraceMonitor(GDataLog &log, GDataLog::SeriesId points, GDataLog::SeriesId path, double map_z)
      : log_(&log)
      , points_(points)
      , path_(path)
      , map_z_(map_z) { /* nothing */ }

    /** @brief The number of individuals recorded so far */
    [[nodiscard]] std::size_t nRecorded() const { return n_recorded_; }
    /** @brief The best (lowest) fitness seen by the monitor */
    [[nodiscard]] double bestFitness() const { return best_fitness_; }

private:
    /**
     * @brief Records the population of every iteration and the best position found so far.
     *
     * @param im The information mode (initialization, per-iteration cycle, or finalization)
     * @param goa A non-owning pointer to the algorithm being monitored
     */
    void
    informationFunction_(infoMode im, oa::GOptimizationAlgorithmBase const *const goa) override {
        if(im != infoMode::INFOPROCESSING) {
            return;
        }

        std::vector<double> parameters;
        for(const auto &ind_ptr : *goa) {
            if(not ind_ptr->is_processed()) {
                continue; // an individual without results says nothing about the search yet
            }
            ind_ptr->streamline<double>(parameters);
            const double fitness = ind_ptr->raw_fitness(0);
            log_->append(points_, parameters[0], parameters[1], map_z_);
            ++n_recorded_;

            if(fitness < best_fitness_) {
                best_fitness_ = fitness;
                best_position_ = {parameters[0], parameters[1]};
            }
        }

        if(best_fitness_ < std::numeric_limits<double>::max()) {
            log_->append(path_, best_position_[0], best_position_[1], map_z_);
        }
    }

    GDataLog *log_ = nullptr;          ///< the log carrying the landscape
    GDataLog::SeriesId points_ = 0;    ///< every individual held after an iteration
    GDataLog::SeriesId path_ = 0;      ///< the best position known so far, per iteration
    std::size_t n_recorded_ = 0;       ///< how many individuals were recorded
    double best_fitness_ = std::numeric_limits<double>::max(); ///< the best fitness seen
    std::array<double, 2> best_position_{0., 0.};                ///< where it was seen
    double map_z_ = 0.;                                          ///< the height of the colour map
};

/******************************************************************************/
/** One figure: an algorithm, the function it is shown on, and that function's domain. */
struct Showcase {
    std::string_view mnemonic;          ///< the algorithm's command-line name, also the file name
    std::string_view function_name;     ///< the test function, as printed in the figure
    gind::solverFunction function;      ///< the test function
    double lower;                       ///< lower bound of both parameters
    double upper;                       ///< upper bound of both parameters
    std::optional<std::array<double, 2>> start; ///< a fixed starting point (local methods), else random
    std::uint32_t iterations;           ///< how long the run lasts
    std::function<std::shared_ptr<oa::GOptimizationAlgorithmBase>(gind::GFunctionIndividual const &,
                                                                  gind::GFunctionIndividual::Config const &)>
        make; ///< builds the configured algorithm around one prototype individual
};

/**
 * @brief Builds one individual of a two-dimensional test problem with random starting values.
 * @param c The problem configuration (dimension, bounds, function)
 * @return A freshly built individual
 */
std::unique_ptr<gind::GFunctionIndividual>
makeIndividual(const gind::GFunctionIndividual::Config &c) {
    auto ind = std::make_unique<gind::GFunctionIndividual>();
    ind->setGenome(gind::GFunctionIndividual::buildGenome(c));
    gind::GFunctionIndividual::applyConfig(*ind, c);
    ind->randomInit(activityMode::ALLPARAMETERS);
    return ind;
}

/**
 * @brief The ten showcases: each algorithm on a function that shows its particular strength.
 * @return The showcases, in the order of the manual's list of algorithms
 */
std::vector<Showcase> showcases() {
    using F = gind::solverFunction;
    using Ind = gind::GFunctionIndividual;
    using Cfg = gind::GFunctionIndividual::Config;
    constexpr double pi = std::numbers::pi;

    return {
        {"ea", "Rastrigin", F::RASTRIGIN, -5.12, 5.12, std::nullopt, ITERATIONS,
         [](Ind const &proto, Cfg const &c) {
             auto oa = std::make_shared<oa::GEvolutionaryAlgorithm>();
             oa->setPopulationSizes(30, 6);
             oa->push_back(proto.clone());
             oa->setAdaptionConfig(Ind::buildAdaptionConfig(proto, c));
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"sepcma", "Ellipsoid", F::ELLIPSOID, -5., 5., std::nullopt, ITERATIONS,
         [](Ind const &proto, Cfg const &) {
             auto oa = std::make_shared<oa::GSepCmaEvolutionStrategy>();
             oa->push_back(proto.clone());
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"sa", "Styblinski-Tang", F::STYBLINSKI_TANG, -5., 5., std::array<double, 2>{2.75, 2.75}, 100,
         [](Ind const &proto, Cfg const &c) {
             auto oa = std::make_shared<oa::GSimulatedAnnealing>();
             oa->setPopulationSizes(30, 6);
             oa->push_back(proto.clone());
             oa->setAdaptionConfig(Ind::buildAdaptionConfig(proto, c));
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"gsa", "Schwefel", F::SCHWEFEL, -500., 500., std::nullopt, ITERATIONS,
         [](Ind const &proto, Cfg const &) {
             auto oa = std::make_shared<oa::GGeneralizedSimulatedAnnealing>();
             oa->setNChains(8);
             oa->push_back(proto.clone());
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"swarm", "Ackley", F::ACKLEY_CANONICAL, -32.768, 32.768, std::nullopt, ITERATIONS,
         [](Ind const &proto, Cfg const &) {
             auto oa = std::make_shared<oa::GSwarmAlgorithm>();
             oa->setSwarmSizes(4, 6); // 4 neighborhoods x 6 members
             oa->push_back(proto.clone());
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"spso2011", "Levy", F::LEVY, -10., 10., std::nullopt, ITERATIONS,
         [](Ind const &proto, Cfg const &) {
             auto oa = std::make_shared<oa::GStandardPSO2011>();
             oa->setSwarmSize(24);
             oa->push_back(proto.clone());
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"acor", "Michalewicz", F::MICHALEWICZ, 0., pi, std::nullopt, ITERATIONS,
         [](Ind const &proto, Cfg const &) {
             auto oa = std::make_shared<oa::GAntColonyOptimization>();
             oa->setArchiveSize(20);
             oa->setNAnts(10);
             oa->push_back(proto.clone());
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"cgd", "Rosenbrock", F::ROSENBROCK, -2., 2., std::array<double, 2>{-1.2, 1.}, 400,
         [](Ind const &proto, Cfg const &) {
             auto oa = std::make_shared<oa::GConjugateGradientDescent>();
             oa->setNStartingPoints(1);
             oa->push_back(proto.clone());
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"nm", "Zakharov", F::ZAKHAROV, -5., 10., std::array<double, 2>{8., 8.}, ITERATIONS,
         [](Ind const &proto, Cfg const &) {
             auto oa = std::make_shared<oa::GNelderMead>();
             oa->push_back(proto.clone());
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
        {"ps", "Berlich noisy parabola", F::NOISYPARABOLA, -4., 4., std::nullopt, ITERATIONS,
         [](Ind const &proto, Cfg const &c) {
             auto oa = std::make_shared<oa::GParameterScan>();
             oa->setScanRandomly(false); // a regular grid: the scan as a map
             oa->setParameterSpecs(
                 "d(0, " + std::to_string(c.min_var) + ", " + std::to_string(c.max_var) + ", 21), d(1, "
                 + std::to_string(c.min_var) + ", " + std::to_string(c.max_var) + ", 21)"
             );
             oa->push_back(proto.clone());
             return std::static_pointer_cast<oa::GOptimizationAlgorithmBase>(oa);
         }},
    };
}

/**
 * @brief Runs one showcase and writes its figure.
 * @param s The showcase
 * @return true if the search recorded individuals and beat the average of the landscape
 */
bool runShowcase(Showcase const &s) {
    gind::GFunctionIndividual::Config problem;
    problem.par_dim = 2;
    problem.min_var = s.lower;
    problem.max_var = s.upper;
    problem.demo_function = s.function;

    // The landscape, seen from above. The colour shows log10(1 + f - f_min) rather than f, so
    // that the structure stays visible on functions whose values span several decades.
    const int function = static_cast<int>(s.function);
    std::vector<std::array<double, 3>> grid;
    grid.reserve(MAP_NODES * MAP_NODES);
    double f_min = std::numeric_limits<double>::max();
    double f_sum = 0.;
    for(std::size_t i = 0; i < MAP_NODES; ++i) {
        for(std::size_t j = 0; j < MAP_NODES; ++j) {
            const std::array<double, 2> p{
                s.lower + (s.upper - s.lower) * double(i) / double(MAP_NODES - 1),
                s.lower + (s.upper - s.lower) * double(j) / double(MAP_NODES - 1)
            };
            const double f = gbm::eval(function, p.data(), 2);
            grid.push_back({p[0], p[1], f});
            f_min = std::min(f_min, f);
            f_sum += f;
        }
    }

    GDataLog log(std::string(s.mnemonic) + " on the " + std::string(s.function_name) + " function", 1, 1);

    GPlotSpec landscape(plotKind::graph_3d);
    landscape.name = std::string(s.function_name);
    landscape.x_label = "x_{0}";
    landscape.y_label = "x_{1}";
    landscape.z_label = "log_{10}(1 + f - f_{min})";
    landscape.columns = {"x", "y", "z"};
    landscape.projection = MAP_PLANE; // the surface, and its colour map on the floor of the box
    const auto landscape_id = log.declareSeries(landscape);
    double map_z = 0.; // the plotted height of f_min is log10(1) = 0, the floor of the box
    for(auto const &g : grid) {
        const double z = std::log10(1. + g[2] - f_min);
        log.append(landscape_id, g[0], g[1], z);
        if(MAP_PLANE == surfaceProjection::CEILING) {
            map_z = std::max(map_z, z);
        }
    }

    GPlotSpec points(plotKind::graph_3d);
    points.name = "individuals held after each iteration";
    points.columns = {"x", "y", "z"};
    const auto points_id = log.overlaySeries(landscape_id, points);

    GPlotSpec path(plotKind::graph_3d);
    path.plot_mode = graphPlotMode::CURVE; // a line through the points, in the box's own frame
    path.name = "best position known so far";
    path.columns = {"x", "y", "z"};
    const auto path_id = log.overlaySeries(landscape_id, path);

    auto monitor = std::make_shared<GSearchTraceMonitor>(log, points_id, path_id, map_z);

    auto proto = makeIndividual(problem);
    if(s.start.has_value()) {
        proto->assignValueVector<double>({(*s.start)[0], (*s.start)[1]});
    }
    auto oa = s.make(*proto, problem);
    oa->setMaxIteration(s.iterations);
    oa->setMaxStallIteration(s.iterations); // the figure wants the whole run, stalls included
    oa->setReportIteration(1);            // the monitor is only called on reporting iterations
    oa->registerPluggableOM(monitor);
    oa->optimize();

    log.writeToFile("search-" + std::string(s.mnemonic) + ".C", plotBackend::ROOT);

    const double f_mean = f_sum / double(grid.size());
    std::cout << "algorithm_search_paths: " << s.mnemonic << " on " << s.function_name << ": recorded "
              << monitor->nRecorded() << " individuals, best fitness " << monitor->bestFitness()
              << " (landscape mean " << f_mean << ", minimum " << f_min << ")\n";

    return monitor->nRecorded() > 0 && monitor->bestFitness() < f_mean;
}

} // anonymous namespace

/******************************************************************************/

int main() {
    GenevaInitializer const gi;

    // One consumer per process (Invariant 5); a single thread is plenty for these small runs.
    ConsumerSpec consumer;
    consumer.mnemonic = "stc";
    consumer.n_threads = 1;
    buildConsumerSetup(consumer);

    bool all_searched = true;
    for(auto const &s : showcases()) {
        // A statistical bar rather than a fixed expectation (Invariant 18): a search whose best
        // individual is no better than the average of its landscape has not searched.
        if(not runShowcase(s)) {
            std::cerr << "algorithm_search_paths: " << s.mnemonic
                      << " did not improve on the average of its landscape\n";
            all_searched = false;
        }
    }
    return all_searched ? 0 : 1;
}
