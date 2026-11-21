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
    GDataLog layout("Adaptions", 2, 1);
    auto const own = layout.declareSeries(curve("own", {"a", "b"}));
    GPlotSpec hist(plotKind::hist_2d);
    hist.columns = {"iteration", "n"};
    layout.declareSeries(hist);
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
    CHECK(contentOf(dir / "adaptions.C").find("1234.5") != std::string::npos);
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
