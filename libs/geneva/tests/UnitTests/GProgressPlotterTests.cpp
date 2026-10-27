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
 * End-to-end test of the GProgressPlotterT monitor AFTER its migration off the legacy plotter-object
 * API onto the modern, plotter-object-free GDataLog path. The monitor used to be exercisable only inside
 * a real optimization; here it is driven by a small, real GEvolutionaryAlgorithm so the whole monitor
 * lifecycle (INFOINIT declares the series, INFOPROCESSING appends one row per individual per iteration,
 * INFOEND sorts/realizes/writes the GDataLog) runs against genuine, evaluated iteration data.
 *
 * The byte-identical equivalence test in the common library already pins the GDataLog rendering path to
 * the former plotter-object output; this test pins the OTHER half -- that the migrated monitor still
 * EXTRACTS the right data from the algorithm and emits a valid, non-empty ROOT macro for it.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "geneva/oa/GPluggableOptimizationMonitors.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"

namespace gen = Gem::Geneva::Genome;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

namespace {

/******************************************************************************/
/**
 * A tiny flat-genome sphere: three constrained doubles in [-5, 5), sharing one Gauss group, started at
 * 2.0. Enough to give the monitor a profileable parameter (index 0) and a real, evaluated fitness.
 */
class Sphere3 : public gen::GGenomeT<Sphere3> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    Sphere3() {
        gen::GGenomeBuilder b;
        b.addDoubleGroup(3, -5., 5.).init(2.0);
        this->setGenome(b.build());
    }
    Sphere3(const Sphere3 &) = default;

    std::shared_ptr<oa::GAdaptionConfigBase> buildAdaptionConfig() const {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        cfg->groupDouble(0).gauss(0.05, 0.8, 1e-9, 5., 1.);
        return cfg;
    }

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->template streamline<double>(v);
        double s = 0.;
        for(double const x : v) {
            s += x * x;
        }
        return {s};
    }
};

/** @brief Slurps a whole text file into a string. */
std::string readFile(const std::filesystem::path &p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("GProgressPlotterT writes a valid ROOT macro when driven by a real EA", "[monitor][plot][oa]") {
    const auto out = std::filesystem::temp_directory_path() / "geneva_progress_plotter_e2e.C";
    std::filesystem::remove(out); // start from a clean slate

    // One profiled floating-point variable (parameter index 0) -> a 2D "fitness vs. parameter" graph.
    auto monitor = std::make_shared<Gem::Geneva::GProgressPlotterT<double>>();
    monitor->setProfileSpec("d(0, -10, 10, 100)");
    monitor->setFileName(out.string());
    REQUIRE(monitor->nProfileVars() == 1);

    // A small but genuine optimization run: the EA drives the monitor's INFOINIT / INFOPROCESSING /
    // INFOEND hooks over real, evaluated individuals.
    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(12, 4);
    p->setMaxIteration(5);
    p->setReportIteration(100000);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);

    REQUIRE_NOTHROW(p->optimize());

    // INFOEND must have realized the GDataLog and written the file.
    REQUIRE(std::filesystem::exists(out));
    REQUIRE(std::filesystem::file_size(out) > 0);

    const std::string content = readFile(out);
    // The 1-variable case sets this canvas/plot title, and the curve is emitted as a ROOT TGraph.
    CHECK(content.contains("Fitness as a function of a parameter value"));
    CHECK(content.contains("TGraph"));

    std::filesystem::remove(out);
}

/******************************************************************************/

TEST_CASE("GProgressPlotterT with no profiled variable still writes an (empty) canvas", "[monitor][plot][oa]") {
    // With zero profiled variables nProfileVars() is 0, so no renderable series is declared; the monitor
    // must still emit a valid, non-empty ROOT macro (an empty canvas), exactly as the legacy path did.
    const auto out = std::filesystem::temp_directory_path() / "geneva_progress_plotter_empty.C";
    std::filesystem::remove(out);

    auto monitor = std::make_shared<Gem::Geneva::GProgressPlotterT<double>>();
    monitor->setFileName(out.string());
    REQUIRE(monitor->nProfileVars() == 0);

    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(8, 2);
    p->setMaxIteration(3);
    p->setReportIteration(100000);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);

    REQUIRE_NOTHROW(p->optimize());

    REQUIRE(std::filesystem::exists(out));
    REQUIRE(std::filesystem::file_size(out) > 0);

    std::filesystem::remove(out);
}

/******************************************************************************/

TEST_CASE("GFitnessMonitor accumulates over a real EA without the legacy plotter API", "[monitor][plot][oa]") {
    // GFitnessMonitor records the global- and iteration-best fitness curves but never renders
    // them (no output file). After its migration onto GDataLog this exercises the new declare /
    // overlay / append path under a real optimization; the check is that a full run completes.
    auto monitor = std::make_shared<Gem::Geneva::GFitnessMonitor>();
    monitor->setNMonitorIndividuals(2);

    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(12, 4);
    p->setMaxIteration(5);
    p->setReportIteration(100000);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);

    REQUIRE_NOTHROW(p->optimize());
}

/******************************************************************************/

TEST_CASE("GFitnessMonitor survives a Pareto run (varying best count)", "[monitor][plot][oa][pareto]") {
    // A Pareto sorting scheme can vary the number of best individuals per iteration, exercising
    // the monitor's "reduce monitored series to 1" branch. It must not crash.
    auto monitor = std::make_shared<Gem::Geneva::GFitnessMonitor>();
    monitor->setNMonitorIndividuals(3);

    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(16, 5);
    p->setMaxIteration(5);
    p->setReportIteration(100000);
    p->setSortingScheme(Gem::Geneva::sortingMode::MUPLUSNU_PARETO);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);

    REQUIRE_NOTHROW(p->optimize());
}

/******************************************************************************/

TEST_CASE("GNAdpationsLogger writes a histogram + fitness ROOT macro (all individuals)", "[monitor][plot][oa]") {
    // Default (monitor all): the n-adaptions plot is a fixed-range 2-d histogram; the fitness plot
    // is a curve. This exercises the fixed-range-histogram path through GDataLog (the B0 gap).
    const auto out = std::filesystem::temp_directory_path() / "geneva_nadaptions_all.C";
    std::filesystem::remove(out);

    auto monitor = std::make_shared<Gem::Geneva::GNAdpationsLogger>();
    monitor->setFileName(out.string());
    monitor->setMonitorBestOnly(false);

    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(12, 4);
    p->setMaxIteration(5);
    p->setReportIteration(100000);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);

    REQUIRE_NOTHROW(p->optimize());

    REQUIRE(std::filesystem::exists(out));
    REQUIRE(std::filesystem::file_size(out) > 0);
    const std::string content = readFile(out);
    CHECK(content.contains("Number of parameter adaptions"));
    CHECK(content.contains("TH2D"));   // the fixed-range 2-d histogram
    CHECK(content.contains("TGraph")); // the fitness curve

    std::filesystem::remove(out);
}

/******************************************************************************/

TEST_CASE("GNAdpationsLogger writes a curve + fitness ROOT macro (best only)", "[monitor][plot][oa]") {
    const auto out = std::filesystem::temp_directory_path() / "geneva_nadaptions_best.C";
    std::filesystem::remove(out);

    auto monitor = std::make_shared<Gem::Geneva::GNAdpationsLogger>();
    monitor->setFileName(out.string());
    monitor->setMonitorBestOnly(true);

    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(12, 4);
    p->setMaxIteration(5);
    p->setReportIteration(100000);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);

    REQUIRE_NOTHROW(p->optimize());

    REQUIRE(std::filesystem::exists(out));
    REQUIRE(std::filesystem::file_size(out) > 0);
    const std::string content = readFile(out);
    // Best-only -> the n-adaptions plot is a curve (no histogram).
    CHECK(content.contains("Number of parameter adaptions"));
    CHECK(content.contains("TGraph"));
    CHECK(!content.contains("TH2D"));

    std::filesystem::remove(out);
}

/******************************************************************************/

TEST_CASE("GAdaptorPropertyLoggerT writes a property histogram + fitness ROOT macro", "[monitor][plot][oa]") {
    // Logs the sigma property of the double Gauss adaptor as a fixed-range 2-d histogram, with a
    // fitness curve below it. Exercises the fixed-range-histogram path through GDataLog (the B0 gap).
    const auto out = std::filesystem::temp_directory_path() / "geneva_adaptorproperty.C";
    std::filesystem::remove(out);

    auto monitor = std::make_shared<Gem::Geneva::GAdaptorPropertyLoggerT<double>>();
    monitor->setFileName(out.string());
    // Defaults: adaptor "GDoubleGaussAdaptor", property "sigma".

    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(12, 4);
    p->setMaxIteration(5);
    p->setReportIteration(100000);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);

    REQUIRE_NOTHROW(p->optimize());

    REQUIRE(std::filesystem::exists(out));
    REQUIRE(std::filesystem::file_size(out) > 0);
    const std::string content = readFile(out);
    CHECK(content.contains("Property: sigma")); // the dynamic y-axis label
    CHECK(content.contains("TH2D"));            // the fixed-range property histogram
    CHECK(content.contains("TGraph"));          // the fitness curve

    std::filesystem::remove(out);
}

/******************************************************************************/

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent test: a single optimize() run produces the 1-d, 2-d and text outputs together, all checked against that same run
TEST_CASE("GProcessingTimesLogger writes 1-d and 2-d timing ROOT macros + a text file", "[monitor][plot][oa]") {
    // Two designers: four auto-ranged 1-d timing histograms (TH1D) and four 2-d timing-vs-iteration
    // histograms (TH2D), plus an incremental plain-text dump. Exercises the two-transient-log path.
    const auto dir = std::filesystem::temp_directory_path();
    const auto pth = dir / "geneva_proctimes_1d.C";
    const auto pth2 = dir / "geneva_proctimes_2d.C";
    const auto txt = dir / "geneva_proctimes.txt";
    for(const auto &f : {pth, pth2, txt}) {
        std::filesystem::remove(f);
    }

    auto monitor = std::make_shared<Gem::Geneva::GProcessingTimesLogger>(
        pth.string(), pth2.string(), txt.string(), 20, 20
    );

    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(12, 4);
    p->setMaxIteration(5);
    p->setReportIteration(100000);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);

    REQUIRE_NOTHROW(p->optimize());

    REQUIRE(std::filesystem::exists(pth));
    REQUIRE(std::filesystem::exists(pth2));
    REQUIRE(std::filesystem::exists(txt));
    REQUIRE(std::filesystem::file_size(pth) > 0);
    REQUIRE(std::filesystem::file_size(pth2) > 0);

    const std::string c1 = readFile(pth);
    CHECK(c1.contains("TH1D"));
    CHECK(c1.contains("Pre-processing time [s]"));
    const std::string c2 = readFile(pth2);
    CHECK(c2.contains("TH2D"));
    CHECK(c2.contains("Iteration"));

    for(const auto &f : {pth, pth2, txt}) {
        std::filesystem::remove(f);
    }
}
