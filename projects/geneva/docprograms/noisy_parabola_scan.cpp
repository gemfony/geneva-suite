/**
 * @file noisy_parabola_scan.cpp
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
 * The two pictures of one optimization run, drawn by dietrich.
 *
 * A two-dimensional Berlich noisy parabola is first scanned exhaustively on a regular grid --
 * every point of the landscape, at the cost of one evaluation per grid node -- and then searched
 * by a small evolutionary algorithm, which evaluates a few hundred points instead of a few
 * thousand. Both go into ONE plot: the scan as the surface, every individual the algorithm ever
 * looked at as a red marker on top of it. That contrast (the whole landscape versus the guided
 * walk across it) is what the figure exists to show.
 *
 * A second canvas shows the same run as an optimizer sees it: every individual it evaluated,
 * scattered over the iterations, with the best result known so far drawn through the cloud.
 *
 * The programme writes ROOT macros; the manual's `make figures` target renders them. It is
 * registered as a test, because a documentation programme that no longer compiles -- or no longer
 * produces the figure the manual prints -- is worse than none.
 *
 * NOTE on reproducibility: the scan is deterministic, the search is NOT. Geneva's random numbers
 * come from concurrent producer threads seeded from entropy and are not reproducible from a seed
 * (DEVELOPMENT-INVARIANTS.md, Invariant 18), so a rerun draws a different -- but statistically
 * equivalent -- path across the same landscape. The final check below is therefore a statistical
 * bar, not a fixed expectation.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
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
#include "geneva/oa/GBasePluggableOM.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"

using namespace Gem::Geneva;
using namespace Gem::Dietrich;

namespace {

/** The square that is both scanned and searched: [-SCAN_RANGE, SCAN_RANGE] in each coordinate. */
constexpr double SCAN_RANGE = 4.;
/** Grid nodes per axis. 81 x 81 = 6561 evaluations -- the "exhaustive" half of the picture. */
constexpr std::size_t SCAN_NODES = 81;

/** The searching half: a deliberately small population over few iterations (Invariant 22). */
constexpr std::size_t POPULATION_SIZE = 18;
constexpr std::size_t N_PARENTS = 6;
constexpr std::uint32_t MAX_ITERATIONS = 40;

/**
 * @brief The coordinate of scan-grid node @p i along one axis.
 * @param i The node index, 0 .. SCAN_NODES-1
 * @return The parameter value that node stands for
 */
double gridNode(std::size_t i) {
    return -SCAN_RANGE + 2. * SCAN_RANGE * double(i) / double(SCAN_NODES - 1);
}

/**
 * @brief The plotted fitness: base-10 logarithm, floored so an exact zero cannot plot at -infinity.
 * @param fitness A raw fitness value (non-negative for this problem)
 * @return log10 of the fitness, floored at -12
 */
double logFitness(double fitness) {
    return std::log10(std::max(fitness, 1.e-12));
}

/******************************************************************************/
/**
 * A pluggable optimization monitor that feeds a dietrich data log while the algorithm runs.
 *
 * Geneva ships GProgressPlotterT for the common case, but that monitor owns its canvas and writes
 * its own file. Here the individuals have to land in a series of a log this programme already
 * holds -- next to the scanned landscape they are drawn on top of -- so the monitor is written out
 * in full. It is also the shortest honest answer to "what does a custom monitor look like?".
 */
class GSearchPathMonitor final
  : public Gem::Common::GReflectiveInterfaceT<GSearchPathMonitor, oa::GBasePluggableOM> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief The monitor's own state is the (non-owning) plot wiring below, which is run state and
     *  not configuration; the *explicit* empty member list is what the CRTP base requires. */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GSearchPathMonitor";

    GSearchPathMonitor() = default;
    GSearchPathMonitor(const GSearchPathMonitor &) = default;
    ~GSearchPathMonitor() override = default;

    /**
     * @brief Wires the monitor to the series it fills.
     *
     * @param path_log The log holding the scanned landscape; the individuals are appended to it
     * @param path The (overlay) series that receives every individual as an (x0, x1, fitness) row
     * @param progress_log The log holding the run's progress plot
     * @param best The series that receives one (iteration, best fitness so far) row per iteration
     * @param spread The series that receives one (iteration, fitness) row per individual
     */
    GSearchPathMonitor(
        GDataLog &path_log,
        GDataLog::SeriesId path,
        GDataLog &progress_log,
        GDataLog::SeriesId best,
        GDataLog::SeriesId spread
    )
      : path_log_(&path_log)
      , path_(path)
      , progress_log_(&progress_log)
      , best_(best)
      , spread_(spread) { /* nothing */ }

    /** @brief The number of individuals recorded so far (what the figure's red cloud contains) */
    [[nodiscard]] std::size_t nRecorded() const { return n_recorded_; }

private:
    /**
     * @brief Records the whole population of every iteration, plus the running best fitness.
     *
     * @param im The information mode (initialization, per-iteration cycle, or finalization)
     * @param goa A non-owning pointer to the algorithm being monitored
     */
    //! [dietrich-search-monitor]
    void
    informationFunction_(infoMode im, oa::GOptimizationAlgorithmBase const *const goa) override {
        if(im != infoMode::INFOPROCESSING) {
            return; // nothing to record before the first iteration or after the last
        }

        std::vector<double> parameters;
        for(const auto &ind_ptr : *goa) {
            ind_ptr->streamline<double>(parameters); // the genome's doubles, in genome order
            const double fitness = ind_ptr->raw_fitness(0);

            path_log_->append(path_, parameters[0], parameters[1], fitness);
            progress_log_->append(spread_, double(goa->getIteration()), logFitness(fitness));
            ++n_recorded_;
        }

        // The algorithm's own book-keeping of the best result seen so far -- (raw, transformed).
        progress_log_->append(
            best_,
            double(goa->getIteration()),
            logFitness(std::get<0>(goa->getBestKnownPrimaryFitness()))
        );
    }
    //! [dietrich-search-monitor]

    GDataLog *path_log_ = nullptr;     ///< the log carrying the scanned landscape
    GDataLog::SeriesId path_ = 0;      ///< the overlay series drawn on top of it
    GDataLog *progress_log_ = nullptr; ///< the log carrying the progress plot
    GDataLog::SeriesId best_ = 0;      ///< the "best so far" curve
    GDataLog::SeriesId spread_ = 0;    ///< the scatter of every evaluated individual
    std::size_t n_recorded_ = 0;       ///< how many individuals were recorded
};

/******************************************************************************/
/**
 * @brief Builds one individual of the two-dimensional noisy-parabola problem.
 *
 * This is what GFunctionIndividualFactory does for a caller who has a configuration file; the two
 * static hooks are public, so a programme that wants no config file of its own can call them
 * directly.
 *
 * @param c The problem configuration (dimension, bounds, demo function)
 * @return A freshly built individual with randomly initialized parameters
 */
std::unique_ptr<gind::GFunctionIndividual>
makeIndividual(const gind::GFunctionIndividual::Config &c) {
    auto ind = std::make_unique<gind::GFunctionIndividual>();
    ind->setGenome(gind::GFunctionIndividual::buildGenome(c));
    gind::GFunctionIndividual::applyConfig(*ind, c);
    ind->randomInit(activityMode::ALLPARAMETERS);
    return ind;
}

} // anonymous namespace

/******************************************************************************/

int main() {
    GenevaInitializer const gi;

    // ---------------------------------------------------------------------------------------
    // The problem: the Berlich noisy parabola, f(x) = (cos(|x|^2) + 2) |x|^2, in two dimensions.
    gind::GFunctionIndividual::Config problem;
    problem.par_dim = 2;
    problem.min_var = -SCAN_RANGE;
    problem.max_var = SCAN_RANGE;
    problem.demo_function = gind::solverFunction::NOISYPARABOLA;

    //! [dietrich-scan-overlay#1]
    // ---------------------------------------------------------------------------------------
    // One pad, two series: the scanned landscape, and the search drawn on top of it. Both are
    // 3-d graphs -- only same-kind series may share a pad -- so the overlay's markers sit at the
    // very fitness values the surface shows.
    GDataLog scan_log("A landscape scanned exhaustively, and searched", 1, 1);

    GPlotSpec landscape(plotKind::graph_3d);
    landscape.name = "Berlich noisy parabola";
    landscape.x_label = "x_{0}";
    landscape.y_label = "x_{1}";
    landscape.z_label = "fitness";
    landscape.columns = {"x", "y", "z"};
    landscape.drawing_args = "SURF2Z"; // a colour-mapped surface with a fitness scale
    const auto landscape_id = scan_log.declareSeries(landscape);

    GPlotSpec search(plotKind::graph_3d);
    search.name = "individuals evaluated by the algorithm";
    search.columns = {"x", "y", "z"};
    const auto search_id = scan_log.overlaySeries(landscape_id, search);
    //! [dietrich-scan-overlay#1]

    // ---------------------------------------------------------------------------------------
    // The exhaustive scan: one evaluation per grid node, through the same entry point the
    // individual's evaluate() uses, so the surface is exactly the landscape the search walks on.
    //! [dietrich-scan-overlay#2]
    const int function = static_cast<int>(problem.demo_function);
    double scan_sum = 0.;
    for(std::size_t i = 0; i < SCAN_NODES; ++i) {
        for(std::size_t j = 0; j < SCAN_NODES; ++j) {
            const std::array<double, 2> point{gridNode(i), gridNode(j)};
            const double fitness = gbm::eval(function, point.data(), 2);

            scan_log.append(landscape_id, point[0], point[1], fitness);
            scan_sum += fitness;
        }
    }
    //! [dietrich-scan-overlay#2]

    // ---------------------------------------------------------------------------------------
    // The second canvas: the same run seen as an optimizer sees it.
    // One pad again, and the other kind of overlay: a SCATTER of every individual the run
    // evaluated, with the CURVE of the best result known so far drawn through it. The fitness is
    // plotted logarithmically -- a converging run crosses several decades, and a linear axis shows
    // the last three of them as one line on the floor.
    GDataLog progress_log("The same run, seen from the optimizer's side", 1, 1);

    GPlotSpec spread(plotKind::graph_2d);
    spread.plot_mode = graphPlotMode::SCATTER; // the backend-independent intent ...
    spread.drawing_args = "A*";                // ... and ROOT's own refinement of it: star markers
    spread.name = "every individual the run evaluated";
    spread.x_label = "iteration";
    spread.y_label = "log_{10}(fitness)";
    spread.columns = {"x", "y"};
    const auto spread_id = progress_log.declareSeries(spread);

    GPlotSpec best_so_far(plotKind::graph_2d);
    best_so_far.plot_mode = graphPlotMode::CURVE;
    best_so_far.drawing_args = "L,same"; // a bare line, into the pad the scatter already framed
    best_so_far.name = "best fitness known so far";
    best_so_far.columns = {"x", "y"};
    const auto best_id = progress_log.overlaySeries(spread_id, best_so_far);

    // ---------------------------------------------------------------------------------------
    // The search: a small evolutionary algorithm on the very same problem, watched by the monitor.
    auto monitor = std::make_shared<GSearchPathMonitor>(
        scan_log, search_id, progress_log, best_id, spread_id
    );

    auto parent = makeIndividual(problem);

    auto pop = std::make_shared<oa::GEvolutionaryAlgorithm>();
    pop->setPopulationSizes(POPULATION_SIZE, N_PARENTS);
    pop->setMaxIteration(MAX_ITERATIONS);
    pop->setMaxStallIteration(MAX_ITERATIONS); // the figure wants the whole run, stalls included
    // The reporting interval gates the whole information cycle, pluggable monitors included: at 0
    // the monitor below would never be called, and the figure would have no red markers at all.
    pop->setReportIteration(1);
    pop->push_back(parent->clone());
    for(std::size_t p = 1; p < N_PARENTS; ++p) {
        pop->push_back(makeIndividual(problem));
    }
    pop->setAdaptionConfig(gind::GFunctionIndividual::buildAdaptionConfig(*parent, problem));
    pop->registerPluggableOM(monitor);

    // One consumer per process (Invariant 5); a single thread is plenty for 720 evaluations.
    ConsumerSpec consumer;
    consumer.mnemonic = "stc";
    consumer.n_threads = 1;
    buildConsumerSetup(consumer);

    pop->optimize();

    // ---------------------------------------------------------------------------------------
    // Write both canvases. The manual renders the macros; nothing here needs ROOT to be present.
    scan_log.writeToFile("noisy-parabola-scan.C", plotBackend::ROOT);
    progress_log.writeToFile("noisy-parabola-progress.C", plotBackend::ROOT);

    const double best = std::get<0>(pop->getBestKnownPrimaryFitness());
    const double scan_mean = scan_sum / double(SCAN_NODES * SCAN_NODES);

    std::cout << "noisy_parabola_scan: scanned " << (SCAN_NODES * SCAN_NODES)
              << " grid nodes, recorded " << monitor->nRecorded()
              << " individuals over " << MAX_ITERATIONS << " iterations\n"
              << "noisy_parabola_scan: best fitness found " << best << " (scan mean " << scan_mean
              << ")\n"
              << "noisy_parabola_scan: wrote noisy-parabola-{scan,progress}.C\n";

    // A statistical bar rather than a fixed expectation (Invariant 18): a search of several hundred
    // guided evaluations that did NOT beat the average of the whole landscape has not searched.
    if(monitor->nRecorded() == 0 || not(best < scan_mean)) {
        std::cerr << "noisy_parabola_scan: the search did not improve on the scanned average\n";
        return 1;
    }
    return 0;
}
