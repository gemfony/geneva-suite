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

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

#include "common/GExceptions.hpp"
#include "common/record/GCsvSink.hpp"
#include "common/record/GRecorder.hpp"
#include "dietrich/plotting/GPlotSink.hpp"

using namespace Gem::Common::Record;
using namespace Gem::Dietrich;

namespace {

// A fresh directory per test case, set as the recorder's directory, and removed at its end
class ScratchDirectory {
public:
    explicit ScratchDirectory(std::string const &tag)
      : dir_(
            std::filesystem::temp_directory_path() / "gem_plot_sink_tests" /
            (tag + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
        ) {
        recorder().setDirectory(dir_);
    }
    ScratchDirectory(ScratchDirectory const &) = delete;
    ScratchDirectory &operator=(ScratchDirectory const &) = delete;
    ~ScratchDirectory() {
        recorder().closeAll();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path operator/(std::string const &name) const {
        return dir_ / name;
    }

private:
    std::filesystem::path dir_;
};

std::string contentOf(std::filesystem::path const &file) {
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

GPlotSpec curve(std::string name, std::vector<std::string> columns) {
    GPlotSpec spec(plotKind::graph_2d);
    spec.name = std::move(name);
    spec.columns = std::move(columns);
    return spec;
}

// A sink that shows what it is given at the end, and fills a series of its own
class GLookingSink final : public GPlotSink {
public:
    using GPlotSink::GPlotSink;
    std::size_t bins_x = 0;
    std::size_t bins_y = 0;
    std::size_t own_series = 0;

private:
    void completeLog_(GDataLog &log) override {
        bins_x = log.spec(1).n_bins_x.value_or(0);
        bins_y = log.spec(1).n_bins_y.value_or(0);
        log.append(own_series, 1234.5, 6789.25);
    }
};

} // namespace

/******************************************************************************/

TEST_CASE("a plot sink draws its series from the stream's columns and writes at the end", "[dietrich][record]") {
    ScratchDirectory const dir("plot");
    GDataLog layout("Progress", 1, 1);
    layout.declareSeries(curve("best", {"iteration", "fitness"}));
    layout.setAddPrintCommand(true);
    {
        auto s = recorder().stream<std::uint64_t, double, std::string>(
            "progress",
            {"iteration", "fitness", "algorithm"},
            withTime::none,
            GStreamOptions{.sinks = {std::make_shared<GCsvSink>(), std::make_shared<GPlotSink>(layout)}}
        );
        s.append(1, 123.25, "EA");
        s.append(2, 99.5, "EA");
        CHECK(std::filesystem::exists(dir / "progress.csv"));
        CHECK(not std::filesystem::exists(dir / "progress.C"));
    }
    std::string const macro = contentOf(dir / "progress.C");
    CHECK(macro.find("123.25") != std::string::npos);
    CHECK(macro.find("99.5") != std::string::npos);
    CHECK(macro.find("best") != std::string::npos);
    // Regression: the macro named its picture with the directory of the program that wrote it
    // ("<records dir>/progress.C.png"), so it failed when run from where it lies. It names the
    // picture by its own file name, beside wherever ROOT runs it.
    CHECK(macro.find("cc->Print(\"progress.C.png\");") != std::string::npos);
}

TEST_CASE("the backend is chosen per sink and names the extension", "[dietrich][record]") {
    ScratchDirectory const dir("backend");
    GDataLog layout("Curve", 1, 1);
    layout.declareSeries(curve("c", {"x", "y"}));
    {
        auto s = recorder().stream<double, double>(
            "curve",
            {"x", "y"},
            withTime::none,
            GStreamOptions{.sinks = {std::make_shared<GPlotSink>(layout, plotBackend::GNUPLOT)}}
        );
        s.append(1., 2.);
    }
    CHECK(std::filesystem::exists(dir / "curve.gp"));
    CHECK_THROWS_AS(GPlotSink(layout, plotBackend::DATA), gem_exception);
}

TEST_CASE("a histogram without bins gets one per distinct value; a derived sink completes the plot", "[dietrich][record]") {
    ScratchDirectory const dir("hist");
    GDataLog layout("Adaptions", 3, 1);
    auto const own = layout.declareSeries(curve("own", {"a", "b"}));
    GPlotSpec hist(plotKind::hist_2d);
    hist.columns = {"iteration", "n"};
    layout.declareSeries(hist);
    GPlotSpec hist_1d(plotKind::hist_1d);
    hist_1d.columns = {"n"};
    layout.declareSeries(hist_1d);
    auto const sink = std::make_shared<GLookingSink>(layout);
    sink->own_series = own;
    {
        auto s = recorder().stream<std::uint64_t, std::int64_t>(
            "adaptions", {"iteration", "n"}, withTime::none, GStreamOptions{.sinks = {sink}}
        );
        for(std::uint64_t it = 0; it < 4; ++it) {
            s.append(it, 1);
            s.append(it, 2);
            s.append(it, 7);
        }
    }
    CHECK(sink->bins_x == 4);
    CHECK(sink->bins_y == 3);
    const std::string macro = contentOf(dir / "adaptions.C");
    CHECK(macro.find("1234.5") != std::string::npos);

    // Regression: the automatic range ended at the largest value, which ROOT's exclusive upper edge
    // put into the overflow bin -- the last iteration and the largest count were not drawn. The
    // range now holds every value: the upper edges lie above 3 (the last iteration) and 7.
    std::smatch m;
    const std::regex th2d(R"re(new TH2D\("[^"]*", "[^"]*",(\d+), ([^,]+), ([^,]+),(\d+), ([^,]+), ([^)]+)\))re");
    REQUIRE(std::regex_search(macro, m, th2d));
    CHECK(std::stod(m[2].str()) <= 0.);
    CHECK(std::stod(m[3].str()) > 3.);
    CHECK(std::stod(m[5].str()) <= 1.);
    CHECK(std::stod(m[6].str()) > 7.);
    const std::regex th1d(R"re(new TH1D\("[^"]*", "[^"]*",(\d+), ([^,]+), ([^)]+)\))re");
    REQUIRE(std::regex_search(macro, m, th1d));
    CHECK(std::stod(m[2].str()) <= 1.);
    CHECK(std::stod(m[3].str()) > 7.);
}

TEST_CASE("a plot sink refuses text columns and series that are only partly the stream's", "[dietrich][record]") {
    ScratchDirectory const dir("refuse");
    {
        GDataLog layout("Text", 1, 1);
        layout.declareSeries(curve("t", {"x", "name"}));
        auto s = recorder().stream<double, std::string>(
            "text", {"x", "name"}, withTime::none, GStreamOptions{.sinks = {std::make_shared<GPlotSink>(layout)}}
        );
        CHECK_THROWS_AS(s.append(1., "a"), gem_exception);
    }
    {
        GDataLog layout("Partly", 1, 1);
        layout.declareSeries(curve("p", {"x", "missing"}));
        auto s = recorder().stream<double>(
            "partly", {"x"}, withTime::none, GStreamOptions{.sinks = {std::make_shared<GPlotSink>(layout)}}
        );
        CHECK_THROWS_AS(s.append(1.), gem_exception);
    }
}

TEST_CASE("a resumed plot reads the stream's CSV and draws the whole run", "[dietrich][record]") {
    // The earlier run recorded x = 0..4; the resumed run starts after the checkpoint of x = 2 and
    // records x = 3. The stream's CSV is cut back to x <= 2 and then holds the new row; the plot
    // of the resumed stream reads that CSV and draws all of it: 0, 1, 2 of the earlier run and the
    // new 3 -- not the earlier 3 and 4. Regression: it drew the rows after the resume only.
    ScratchDirectory const dir("plot_resume");
    GDataLog layout("Resume", 1, 1);
    layout.declareSeries(curve("run", {"x", "y"}));
    auto const declare = [&layout](bool resume, bool with_csv) {
        std::vector<std::shared_ptr<GSink>> sinks;
        if(with_csv) {
            sinks.push_back(std::make_shared<GCsvSink>());
        }
        sinks.push_back(std::make_shared<GPlotSink>(layout));
        return recorder().stream<double, double>(
            "run",
            {"x", "y"},
            withTime::none,
            GStreamOptions{
                .sinks = std::move(sinks), .resume = resume, .resumeColumn = "x", .resumeAfter = 2.
            }
        );
    };
    {
        auto s = declare(false, true);
        for(int x = 0; x < 5; ++x) {
            s.append(x, 7770.25 + x);
        }
    }
    {
        auto s = declare(true, true);
        s.append(3., 8883.25);
    }
    std::string macro = contentOf(dir / "run.C");
    for(const char *kept : {"7770.25", "7771.25", "7772.25", "8883.25"}) {
        CHECK(macro.find(kept) != std::string::npos);
    }
    for(const char *dropped : {"7773.25", "7774.25"}) {
        CHECK(macro.find(dropped) == std::string::npos);
    }

    // Without a CSV sink on the stream the plot holds the resumed rows only, as it did: the CSV
    // left in the directory is no file of this stream
    {
        auto s = declare(true, false);
        s.append(3., 9993.25);
    }
    macro = contentOf(dir / "run.C");
    CHECK(macro.find("9993.25") != std::string::npos);
    CHECK(macro.find("7770.25") == std::string::npos);
}

TEST_CASE("a plot added to a stream that another library declares draws it", "[dietrich][record]") {
    // As a program would draw courtier's end-of-run report, which courtier declares without
    // knowing dietrich: the plot sink is added by the stream's name beforehand.
    ScratchDirectory const dir("added_plot");
    GDataLog layout("End of run", 1, 1);
    layout.declareSeries(curve("returns", {"requeued", "dropped_returns"}));
    recorder().addSink("report", std::make_shared<GPlotSink>(layout));
    {
        auto s = recorder().stream<std::string, std::uint64_t, std::uint64_t>(
            "report", {"consumer", "requeued", "dropped_returns"}
        );
        s.append("beast", 4321, 8765);
    }
    CHECK(std::filesystem::exists(dir / "report.csv"));
    const std::string macro = contentOf(dir / "report.C");
    CHECK(macro.find("4321") != std::string::npos);
    CHECK(macro.find("8765") != std::string::npos);
}
