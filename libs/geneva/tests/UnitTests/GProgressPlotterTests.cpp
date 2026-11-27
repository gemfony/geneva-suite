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
 * The shipped monitors of GPluggableOptimizationMonitors.hpp on the recording framework
 * (Gem::Common::Record), each driven by a small, real GEvolutionaryAlgorithm: every monitor declares
 * its stream at INFOINIT and appends at INFOPROCESSING; the CSV file holds the rows as they come, and
 * the plot of a GPlotSink (a ROOT macro) is written when the stream ends -- here at closeAll(), as
 * courtier's launcher does at the end of a run. The checks are on the files: the columns of the
 * stream, that rows were recorded, and the ROOT macro of the plot.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "common/record/GRecorder.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GMonitorStream.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"

namespace gen = Gem::Geneva::Genome;
namespace oa = Gem::Geneva::OptimizationAlgorithms;
using Gem::Common::Record::recorder;

namespace {

/******************************************************************************/
/**
 * A tiny flat-genome sphere: three constrained doubles in [-5, 5), sharing one Gauss group, started at
 * 2.0. Enough to give the monitors a profileable parameter (index 0) and a real, evaluated fitness.
 */
class Sphere3 : public gen::GGenomeT<Sphere3> {
public:
    using genome_only =
        void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
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
        auto const v = this->template parameterValues<double>();
        double s = 0.;
        for(double const x : v) {
            s += x * x;
        }
        return {s};
    }
};

/** @brief A fresh directory of the recorder per test case; the streams are closed and the
 *  directory removed at its end */
class RecordDirectory {
public:
    explicit RecordDirectory(const std::string &tag)
      : dir_(
            std::filesystem::temp_directory_path() / "geneva_monitor_tests" /
            (tag + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
        ) {
        recorder().setDirectory(dir_);
    }
    RecordDirectory(const RecordDirectory &) = delete;
    RecordDirectory &operator=(const RecordDirectory &) = delete;
    RecordDirectory(RecordDirectory &&) = delete;
    RecordDirectory &operator=(RecordDirectory &&) = delete;
    ~RecordDirectory() {
        recorder().closeAll();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    [[nodiscard]] std::filesystem::path operator/(const std::string &file) const {
        return dir_ / file;
    }

private:
    std::filesystem::path dir_;
};

/** @brief Slurps a whole text file into a string. */
std::string readFile(const std::filesystem::path &p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/** @brief The lines of a file */
std::vector<std::string> linesOf(const std::filesystem::path &file) {
    std::ifstream in(file);
    std::vector<std::string> lines;
    for(std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

/**
 * @brief Runs a small EA on Sphere3 under one monitor, reporting every iteration, and ends the
 * streams as the launcher does at the end of a run
 */
void runUnder(
    const std::shared_ptr<oa::GBasePluggableOM> &monitor,
    std::size_t population = 12,
    std::size_t parents = 4,
    Gem::Geneva::sortingMode sorting = Gem::Geneva::sortingMode::MUPLUSNU_SINGLEEVAL
) {
    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(population, parents);
    p->setMaxIteration(4);
    p->setReportIteration(1);
    p->setSortingScheme(sorting);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(monitor);
    REQUIRE_NOTHROW(p->optimize());
    recorder().closeAll();
}

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE(
    "GProgressPlotterT records its stream and draws a valid ROOT macro",
    "[monitor][plot][oa]"
) {
    RecordDirectory const dir("progress");
    // One profiled floating-point variable (parameter index 0) -> a 2D "fitness vs. parameter" graph.
    auto monitor = std::make_shared<Gem::Geneva::GProgressPlotterT<double>>();
    monitor->setProfileSpec("d(0, -10, 10, 100)");
    REQUIRE(monitor->nProfileVars() == 1);
    REQUIRE(monitor->getStreamName() == "progress");
    runUnder(monitor);

    const auto rows = linesOf(dir / "progress.csv");
    REQUIRE(rows.size() > 1);
    CHECK(rows[0] == "algorithm,iteration,chain_iteration,var0,fitness");
    CHECK(rows[1].starts_with("Evolutionary Algorithm,0,0,"));

    const std::string content = readFile(dir / "progress.C");
    // The 1-variable case sets this plot title, and the curve is emitted as a ROOT TGraph.
    CHECK(content.contains("Fitness as a function of a parameter value"));
    CHECK(content.contains("TGraph"));
}

/******************************************************************************/

TEST_CASE(
    "GProgressPlotterT names its axes and draws a 2-d profile over a heat map of the function",
    "[monitor][plot][oa]"
) {
    RecordDirectory const dir("landscape");
    auto monitor = std::make_shared<Gem::Geneva::GProgressPlotterT<double>>();
    monitor->setProfileSpec("d(0, -5, 5), d(1, -5, 5)");
    monitor->setVariableNames({"x", "y"});
    monitor->setLandscapeResolution(7);
    REQUIRE(monitor->getVariableNames() == std::vector<std::string>{"x", "y"});

    // The settings belong to the monitor's configuration, so a copy carries them.
    Gem::Geneva::GProgressPlotterT<double> const copy(*monitor);
    CHECK(copy.getVariableNames() == monitor->getVariableNames());
    CHECK(copy.getLandscapeResolution() == 7);

    runUnder(monitor);

    // The columns are named after the variables; the landscape is part of the plot only.
    CHECK(linesOf(dir / "progress.csv").at(0) == "algorithm,iteration,chain_iteration,x,y,fitness");
    const std::string content = readFile(dir / "progress.C");
    // The axes carry the given names instead of "variable id N" ...
    CHECK(content.contains("SetTitle(\"x\")"));
    CHECK(content.contains("SetTitle(\"y\")"));
    CHECK_FALSE(content.contains("variable id"));
    // ... the function, sampled on the 7 x 7 grid, is a heat map on the floor (the floor cells) without
    // its surface, and the candidates lie above it with their shadows on the floor.
    CHECK(content.contains("TGraph2D *graph__0 = new TGraph2D(49,"));
    CHECK(content.contains("TPolyLine"));
    CHECK_FALSE(content.contains("SURF1"));
    CHECK(content.contains("evaluated candidates"));
    CHECK(content.contains("TPolyMarker3D"));
}

/******************************************************************************/

TEST_CASE(
    "GProgressPlotterT shows the canvas label as the one title of its one pad",
    "[monitor][plot][oa]"
) {
    // One, two (without a landscape) and three profiled variables: the canvas label is the title of
    // the figure's one pad and is not drawn above it as well, so the figure has one title.
    RecordDirectory const dir("title");
    const std::string label = "Progress of the title check";
    for(const std::string spec :
        {"d(0, -5, 5)", "d(0, -5, 5), d(1, -5, 5)", "d(0, -5, 5), d(1, -5, 5), d(2, -5, 5)"}) {
        auto monitor = std::make_shared<Gem::Geneva::GProgressPlotterT<double>>();
        monitor->setProfileSpec(spec);
        monitor->setCanvasLabel(label);
        monitor->setStreamName("title");
        runUnder(monitor, 8, 2);

        const std::string content = readFile(dir / "title.C");
        INFO("profile " << spec);
        CHECK(content.contains("n->SetTitle(\"" + label + "\");"));
        CHECK_FALSE(content.contains("TPaveLabel"));
    }
}

/******************************************************************************/

TEST_CASE(
    "GProgressPlotterT with no profiled variable records the fitness without a plot",
    "[monitor][plot][oa]"
) {
    RecordDirectory const dir("unprofiled");
    auto monitor = std::make_shared<Gem::Geneva::GProgressPlotterT<double>>();
    REQUIRE(monitor->nProfileVars() == 0);
    runUnder(monitor, 8, 2);

    const auto rows = linesOf(dir / "progress.csv");
    REQUIRE(rows.size() > 1);
    CHECK(rows[0] == "algorithm,iteration,chain_iteration,fitness");
    CHECK_FALSE(std::filesystem::exists(dir / "progress.C"));
}

/******************************************************************************/

TEST_CASE("GFitnessMonitor records the best fitness per rank", "[monitor][oa]") {
    RecordDirectory const dir("fitness");
    auto monitor = std::make_shared<Gem::Geneva::GFitnessMonitor>();
    monitor->setNMonitorIndividuals(2);
    runUnder(monitor);

    const auto rows = linesOf(dir / "best_fitness.csv");
    CHECK(rows.at(0) == "algorithm,iteration,chain_iteration,rank,global_best,iteration_best");
    // Iterations 0 to 3, two ranks each
    CHECK(rows.size() == 1 + 4 * 2);
}

/******************************************************************************/

TEST_CASE("GFitnessMonitor survives a Pareto run (varying best count)", "[monitor][oa][pareto]") {
    // A Pareto sorting scheme can vary the number of best individuals per iteration; the monitor
    // records as many ranks as there are, up to the number it monitors.
    RecordDirectory const dir("pareto");
    auto monitor = std::make_shared<Gem::Geneva::GFitnessMonitor>();
    monitor->setNMonitorIndividuals(3);
    runUnder(monitor, 16, 5, Gem::Geneva::sortingMode::MUPLUSNU_PARETO);

    const auto rows = linesOf(dir / "best_fitness.csv");
    CHECK(rows.size() > 1);
    CHECK(rows.size() <= 1 + 4 * 3);
}

/******************************************************************************/

TEST_CASE(
    "GNAdpationsLogger records the adaptions and draws a histogram + fitness ROOT macro (all "
    "individuals)",
    "[monitor][plot][oa]"
) {
    // Monitor all: the n-adaptions plot is a 2-d histogram, one bin per recorded iteration and per
    // number of adaptions; the fitness plot is a curve.
    RecordDirectory const dir("nadaptions_all");
    auto monitor = std::make_shared<Gem::Geneva::GNAdpationsLogger>();
    monitor->setMonitorBestOnly(false);
    runUnder(monitor);

    const auto rows = linesOf(dir / "n_adaptions.csv");
    CHECK(rows.at(0) == "algorithm,iteration,chain_iteration,individual,n_adaptions,best_fitness");
    CHECK(rows.size() == 1 + 4 * 12);
    const std::string content = readFile(dir / "n_adaptions.C");
    CHECK(content.contains("Number of parameter adaptions"));
    CHECK(content.contains("TH2D"));   // the 2-d histogram
    CHECK(content.contains("TGraph")); // the fitness curve
}

/******************************************************************************/

TEST_CASE(
    "GNAdpationsLogger records the best individual and draws a curve + fitness ROOT macro",
    "[monitor][plot][oa]"
) {
    RecordDirectory const dir("nadaptions_best");
    auto monitor = std::make_shared<Gem::Geneva::GNAdpationsLogger>();
    monitor->setMonitorBestOnly(true);
    runUnder(monitor);

    CHECK(linesOf(dir / "n_adaptions.csv").size() == 1 + 4);
    const std::string content = readFile(dir / "n_adaptions.C");
    // Best-only -> the n-adaptions plot is a curve (no histogram).
    CHECK(content.contains("Number of parameter adaptions"));
    CHECK(content.contains("TGraph"));
    CHECK(!content.contains("TH2D"));
}

/******************************************************************************/

TEST_CASE(
    "GAdaptorPropertyLoggerT records sigma and draws a property histogram + fitness ROOT macro",
    "[monitor][plot][oa]"
) {
    // The default adaptor "GDoubleGaussAdaptor" and property "sigma": a column "sigma", a 2-d
    // histogram of it over the iterations, a fitness curve beside it. Its default stream name is not
    // that of GNAdpationsLogger (both once defaulted to NAdaptions.C).
    RecordDirectory const dir("adaptor_property");
    auto monitor = std::make_shared<Gem::Geneva::GAdaptorPropertyLoggerT<double>>();
    CHECK(monitor->getStreamName() != Gem::Geneva::GNAdpationsLogger().getStreamName());
    runUnder(monitor);

    const auto rows = linesOf(dir / "adaptor_properties.csv");
    CHECK(rows.at(0) == "algorithm,iteration,chain_iteration,sigma,best_fitness");
    CHECK(rows.size() > 1);
    const std::string content = readFile(dir / "adaptor_properties.C");
    CHECK(content.contains("Property: sigma")); // the dynamic y-axis label
    CHECK(content.contains("TH2D"));            // the property histogram
    CHECK(content.contains("TGraph"));          // the fitness curve
}

/******************************************************************************/

TEST_CASE(
    "GProcessingTimesLogger records the times and draws 1-d and 2-d timing histograms",
    "[monitor][plot][oa]"
) {
    // One plot: four auto-ranged 1-d timing histograms (TH1D) and four 2-d timing-vs-iteration
    // histograms (TH2D).
    RecordDirectory const dir("processing_times");
    auto monitor = std::make_shared<Gem::Geneva::GProcessingTimesLogger>(20, 20);
    runUnder(monitor);

    const auto rows = linesOf(dir / "processing_times.csv");
    CHECK(rows.at(0) == "algorithm,iteration,chain_iteration,individual,pre,main,post,total");
    CHECK(rows.size() == 1 + 4 * 12);
    const std::string content = readFile(dir / "processing_times.C");
    CHECK(content.contains("TH1D"));
    CHECK(content.contains("Pre-processing time [s]"));
    CHECK(content.contains("TH2D"));
    CHECK(content.contains("Iteration"));
}

/******************************************************************************/

TEST_CASE("GAllSolutionFileLogger records every candidate with its parameters", "[monitor][oa]") {
    RecordDirectory const dir("all_solutions");
    auto monitor = std::make_shared<Gem::Geneva::GAllSolutionFileLogger>();
    monitor->setPrintInitial();
    runUnder(monitor);

    const auto rows = linesOf(dir / "all_solutions.csv");
    CHECK(
        rows.at(0) ==
        "algorithm,iteration,chain_iteration,individual,var0,var1,var2,fitness_0,valid"
    );
    // The initial population (unevaluated: fitness nan, not valid) and four iterations of twelve
    CHECK(rows.size() == 1 + 12 + 4 * 12);
    CHECK(rows.at(1).ends_with(",nan,false"));
    CHECK(rows.at(13).ends_with(",true"));
}

/******************************************************************************/

TEST_CASE(
    "GAllSolutionFileLogger records only the raw or the transformed fitness when asked",
    "[monitor][oa]"
) {
    // With setFitnessOnly() the stream holds the individual and its fitness values only (what the
    // former GIterationResultsFileLogger recorded). And setUseTrueFitness() is honoured: with a
    // sigmoid evaluation policy the transformed fitness of a valid individual differs from its raw
    // fitness, so two monitors of one run, one raw and one transformed, record different values.
    RecordDirectory const dir("fitness_only");
    auto raw = std::make_shared<Gem::Geneva::GAllSolutionFileLogger>();
    raw->setFitnessOnly();
    raw->setUseTrueFitness(true);
    raw->setStreamName("raw");
    auto transformed = std::make_shared<Gem::Geneva::GAllSolutionFileLogger>();
    transformed->setFitnessOnly();
    transformed->setUseTrueFitness(false);
    transformed->setStreamName("transformed");
    CHECK(transformed->getFitnessOnly());

    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(8, 2);
    p->setMaxIteration(2);
    p->setReportIteration(1);
    Sphere3 src;
    src.setEvaluationPolicy(Gem::Geneva::evaluationPolicy::USESIGMOID);
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(raw);
    p->registerPluggableOM(transformed);
    REQUIRE_NOTHROW(p->optimize());
    recorder().closeAll();

    const auto raw_rows = linesOf(dir / "raw.csv");
    const auto transformed_rows = linesOf(dir / "transformed.csv");
    CHECK(raw_rows.at(0) == "algorithm,iteration,chain_iteration,individual,fitness_0");
    REQUIRE(raw_rows.size() == 1 + 2 * 8);
    REQUIRE(transformed_rows.size() == raw_rows.size());
    CHECK(raw_rows[1] != transformed_rows[1]);
}

/******************************************************************************/

TEST_CASE("Two monitors of one class need different stream names", "[monitor][oa]") {
    RecordDirectory const dir("twice");
    auto p = std::make_shared<oa::GEvolutionaryAlgorithm>();
    p->setPopulationSizes(8, 2);
    p->setMaxIteration(2);
    p->setReportIteration(1);
    Sphere3 const src;
    p->push_back(src.clone());
    p->setAdaptionConfig(src.buildAdaptionConfig());
    p->registerPluggableOM(std::make_shared<Gem::Geneva::GFitnessMonitor>());
    p->registerPluggableOM(std::make_shared<Gem::Geneva::GFitnessMonitor>());
    try {
        p->optimize();
        FAIL("the second monitor's stream was taken");
    }
    catch(const Gem::Common::gem_exception &e) {
        CHECK(std::string(e.what()).contains("two objects of one class"));
    }
}

/******************************************************************************/

TEST_CASE("A copy of a monitor's stream holder is empty", "[monitor][oa]") {
    // The holder lets every monitor keep a defaulted copy constructor: copying a monitor copies its
    // configuration, never its stream, so a clone declares a stream of its own.
    RecordDirectory const dir("copy");
    oa::GEvolutionaryAlgorithm const ea;
    oa::GMonitorStream original;
    REQUIRE_FALSE(original.isOpen());
    original
        .declare("copied", {{.name = "n", .type = Gem::Common::Record::columnType::UNSIGNED}}, ea);
    REQUIRE(original.isOpen());
    oa::GMonitorStream copy(original);
    CHECK_FALSE(copy.isOpen());
    CHECK(original.isOpen());
    copy = original;
    CHECK_FALSE(copy.isOpen());
    CHECK(original.isOpen());
}

/******************************************************************************/

TEST_CASE("GProcessingTimesLogger refuses histograms without bins", "[monitor][oa]") {
    // Regression: setNBinsX(0) and setNBinsY(0) once refused 0, lost the refusal when the monitor
    // moved onto streams, and so asked the plot for a histogram of zero bins. They refuse it again,
    // and so does the constructor.
    Gem::Geneva::GProcessingTimesLogger monitor;
    CHECK_THROWS_AS(monitor.setNBinsX(0), Gem::Common::gem_exception);
    CHECK_THROWS_AS(monitor.setNBinsY(0), Gem::Common::gem_exception);
    CHECK(monitor.getNBinsX() > 0);
    CHECK(monitor.getNBinsY() > 0);
    CHECK_THROWS_AS(Gem::Geneva::GProcessingTimesLogger(0, 10), Gem::Common::gem_exception);
    CHECK_THROWS_AS(Gem::Geneva::GProcessingTimesLogger(10, 0), Gem::Common::gem_exception);
    CHECK_NOTHROW(Gem::Geneva::GProcessingTimesLogger(10, 10));
}

/******************************************************************************/
// The test-access point of GProgressPlotterT: its landscape sink is private, and no public function
// shows which sink an assigned plotter draws on.

namespace Gem::Geneva {
struct GProgressPlotterTestAccess {
    template <typename fp_type>
    static std::shared_ptr<detail::GLandscapePlotSink> &
    landscapeSink(GProgressPlotterT<fp_type> &p) {
        return p.landscape_sink_;
    }
};
} // namespace Gem::Geneva

TEST_CASE(
    "GProgressPlotter: an assigned plotter keeps its own landscape sink",
    "[geneva][monitors]"
) {
    // Regression: the implicit assignment took the source's sink, so a chain drew its landscape there
    using Gem::Geneva::GProgressPlotterTestAccess;
    Gem::Geneva::GProgressPlotter source;
    GProgressPlotterTestAccess::landscapeSink(source) =
        std::make_shared<Gem::Geneva::detail::GLandscapePlotSink>(
            Gem::Dietrich::GDataLog("landscape", 1, 1),
            Gem::Dietrich::GDataLog::SeriesId{0}
        );
    Gem::Geneva::GProgressPlotter assigned;
    assigned = source;
    CHECK(
        GProgressPlotterTestAccess::landscapeSink(assigned) !=
        GProgressPlotterTestAccess::landscapeSink(source)
    );
}

/******************************************************************************/
