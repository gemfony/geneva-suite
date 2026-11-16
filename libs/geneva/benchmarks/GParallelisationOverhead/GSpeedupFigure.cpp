/**
 * @file GSpeedupFigure.cpp
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

// Draws the speed-up tables of several runs of GParallelisationOverhead (shortDelayResults.txt)
// into one figure, with linear axes: the speed-up over the evaluation time, one series per label,
// with error bars, and the ideal speed-up as a line. Tables that carry the same label are runs of
// the same kind; the figure shows their mean, with the error compareSpeedups.py gives it.
//
// Usage: GSpeedupFigure [--ideal S] [--figure F] [--backend B] [--zoom T]... [--means M] [--no-title]
//                       label=table...
//
// With linear axes the short evaluation times crowd at the left of the figure, so every --zoom
// adds a pad with the evaluation times up to T (default: up to 1 s and up to 0.1 s).

// Standard header files go here
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Boost header files go here
#include <boost/program_options.hpp>

// Geneva header files go here
#include "dietrich/GPlotDesigner.hpp"

namespace {

using Gem::Dietrich::GDataLog;
using Gem::Dietrich::GPlotSpec;
using Gem::Dietrich::graphPlotMode;
using Gem::Dietrich::legendPosition;
using Gem::Dietrich::plotBackend;
using Gem::Dietrich::plotKind;

/// The width of the drawn lines, and of their samples in the legend
constexpr std::uint32_t LINEWIDTH = 2;

/******************************************************************************/
/** One evaluation time of a speed-up series: the speed-up, its error, and how many runs gave it */
struct SpeedupPoint {
    double speedup = 0.;
    double error = 0.;
    std::size_t runs = 0;
};

/** The speed-up of one label over the evaluation time, ordered by the evaluation time */
using SpeedupSeries = std::map<double, SpeedupPoint>;

/******************************************************************************/
/**
 * Reads one speed-up table as GParallelisationOverhead writes it: a comment line, then per
 * evaluation time the evaluation time, the serial and the parallel run time with their errors, and
 * the speed-up with its error.
 *
 * @param file_name The table to read
 * @return The speed-up and its error per evaluation time (runs = 1)
 */
SpeedupSeries readTable(const std::string &file_name) {
    std::ifstream in(file_name);
    if(not in) {
        throw std::runtime_error("GSpeedupFigure: cannot read \"" + file_name + "\"");
    }
    SpeedupSeries table;
    std::string line;
    while(std::getline(in, line)) {
        std::istringstream fields(line);
        std::string first;
        if(not(fields >> first) or first.starts_with('#')) {
            continue;
        }
        double serial = 0.;
        double serial_error = 0.;
        double parallel = 0.;
        double parallel_error = 0.;
        SpeedupPoint point{.runs = 1};
        if(not(fields >> serial >> serial_error >> parallel >> parallel_error >> point.speedup >>
               point.error)) {
            throw std::runtime_error(
                std::format(
                    "GSpeedupFigure: \"{}\" has a line that is not a row of a speed-up table: "
                    "\"{}\"",
                    file_name,
                    line
                )
            );
        }
        table[std::stod(first)] = point;
    }
    if(table.empty()) {
        throw std::runtime_error("GSpeedupFigure: \"" + file_name + "\" holds no speed-up");
    }
    return table;
}

/******************************************************************************/
/**
 * Combines the tables of several runs of one kind, per evaluation time: the mean speed-up and the
 * error of that mean, which is the larger of what the scatter of the runs gives and what the errors
 * of the single runs give -- so that a few runs that agree by chance do not claim a precision they
 * do not have. This is the rule of compareSpeedups.py; a test checks that the two agree.
 *
 * @param tables The tables of the runs
 * @return The combined series
 */
SpeedupSeries combine(const std::vector<SpeedupSeries> &tables) {
    std::map<double, std::vector<SpeedupPoint>> by_time;
    for(const auto &table : tables) {
        for(const auto &[time, point] : table) {
            by_time[time].push_back(point);
        }
    }
    SpeedupSeries result;
    for(const auto &[time, points] : by_time) {
        const auto runs = static_cast<double>(points.size());
        double sum = 0.;
        double square_errors = 0.;
        for(const auto &point : points) {
            sum += point.speedup;
            square_errors += point.error * point.error;
        }
        const double mean = sum / runs;
        double error = std::sqrt(square_errors) / runs;
        if(points.size() > 1) {
            double square_deviations = 0.;
            for(const auto &point : points) {
                square_deviations += (point.speedup - mean) * (point.speedup - mean);
            }
            const double scatter = std::sqrt(square_deviations / (runs - 1.));
            error = std::max(error, scatter / std::sqrt(runs));
        }
        result[time] = SpeedupPoint{.speedup = mean, .error = error, .runs = points.size()};
    }
    return result;
}

/******************************************************************************/
/**
 * Splits "label=file" at the first '='.
 *
 * @param argument The argument to split
 * @return The label and the file name
 */
std::pair<std::string, std::string> splitLabel(const std::string &argument) {
    const auto pos = argument.find('=');
    if(pos == std::string::npos or pos == 0 or pos + 1 == argument.size()) {
        throw std::runtime_error(
            "GSpeedupFigure: \"" + argument + "\" is not of the form label=table"
        );
    }
    return {argument.substr(0, pos), argument.substr(pos + 1)};
}

/******************************************************************************/
/**
 * Translates a backend name of the command line.
 *
 * @param name root, gnuplot, matplotlib or octave
 * @return The dietrich backend
 */
plotBackend backendFromName(const std::string &name) {
    if(name == "root") {
        return plotBackend::ROOT;
    }
    if(name == "gnuplot") {
        return plotBackend::GNUPLOT;
    }
    if(name == "matplotlib") {
        return plotBackend::MATPLOTLIB;
    }
    if(name == "octave") {
        return plotBackend::OCTAVE;
    }
    throw std::runtime_error(
        "GSpeedupFigure: unknown backend \"" + name + "\" (root, gnuplot, matplotlib, octave)"
    );
}

/******************************************************************************/
/**
 * Translates a legend position of the command line.
 *
 * @param name free, bottom_right, center_right, top_left, bottom_left or top_right
 * @return The corner, or nothing for free (the freest corner of each pad)
 */
std::optional<legendPosition> legendFromName(const std::string &name) {
    if(name == "free") {
        return std::nullopt;
    }
    if(name == "bottom_right") {
        return legendPosition::BOTTOM_RIGHT;
    }
    if(name == "top_left") {
        return legendPosition::TOP_LEFT;
    }
    if(name == "bottom_left") {
        return legendPosition::BOTTOM_LEFT;
    }
    if(name == "top_right") {
        return legendPosition::TOP_RIGHT;
    }
    if(name == "center_right") {
        return legendPosition::CENTER_RIGHT;
    }
    throw std::runtime_error("GSpeedupFigure: unknown legend position \"" + name + "\"");
}

/******************************************************************************/
/**
 * Declares one pad of the figure: a series per label, with error bars, and the ideal speed-up as a
 * line over the range of the evaluation times shown. All series of a pad are of the same kind
 * (dietrich overlays only those), so the ideal line is a graph with error bars of zero. Every
 * series is named by its label alone; the pad has a title of its own, and its legend lies in a
 * corner, the same size in every pad.
 *
 * @param log The log the pad is declared in
 * @param title The title of the pad
 * @param series The combined series, per label, in the order of the legend
 * @param ideal The ideal speed-up; no line if it is not positive
 * @param up_to The longest evaluation time shown in this pad
 * @param corner Where the legend lies; without one the corner that covers the least of the curves
 */
void declarePad(
    GDataLog &log,
    const std::string &title,
    const std::vector<std::pair<std::string, SpeedupSeries>> &series,
    double ideal,
    double up_to,
    std::optional<legendPosition> corner
) {
    // What the pad shows: the points of every series up to its longest time, and the ideal line
    std::vector<std::vector<std::pair<double, double>>> curves;
    double shortest = std::numeric_limits<double>::max();
    double longest = 0.;
    for(const auto &[label, points] : series) {
        curves.emplace_back();
        for(const auto &[time, point] : points) {
            if(time <= up_to) {
                curves.back().emplace_back(time, point.speedup);
                shortest = std::min(shortest, time);
                longest = std::max(longest, time);
            }
        }
    }
    const bool ideal_line = ideal > 0. and longest > 0.;
    if(ideal_line) {
        curves.push_back({{shortest, ideal}, {longest, ideal}});
    }
    // Without a chosen corner: the one that covers the least of the curves. Speed-up curves rise
    // from the lower left and flatten under the ideal line, so the lower right, which the library
    // prefers, is usually free; a consumer that stays far below the others runs along the bottom,
    // and then the middle of the right side or a corner is.
    const legendPosition legend = corner.value_or(Gem::Dietrich::freestLegendCorner(curves, {}, curves.size()));

    std::optional<GDataLog::SeriesId> primary;
    const auto declare = [&](const std::string &name,
                             const std::vector<std::pair<double, double>> &points,
                             const SpeedupSeries *errors) {
        GPlotSpec spec(plotKind::graph_2d_err);
        spec.plot_mode = graphPlotMode::CURVE;
        spec.name = name;
        spec.line_width = LINEWIDTH;
        if(not primary) {
            spec.pad_title = title;
            spec.legend = legend;
        }
        spec.x_label = "Evaluation time [s]";
        spec.y_label = "Speed-up";
        spec.columns = {"x", "ex", "y", "ey"};
        const auto id = primary ? log.overlaySeries(*primary, spec) : log.declareSeries(spec);
        if(not primary) {
            primary = id;
        }
        for(const auto &[x, y] : points) {
            log.append(id, x, 0., y, errors ? errors->at(x).error : 0.);
        }
    };
    for(std::size_t i = 0; i < series.size(); i++) {
        declare(series[i].first, curves[i], &series[i].second);
    }
    if(ideal_line) {
        std::ostringstream name;
        name << "ideal (" << ideal << ")";
        declare(name.str(), curves.back(), nullptr);
    }
}

/******************************************************************************/
/** What the command line asks for */
struct Arguments {
    double ideal = 0.;                          ///< The ideal speed-up; no line if 0
    std::vector<double> zooms{1., 0.1};         ///< The further pads, by longest time shown
    std::string figure = "speedupComparison.C"; ///< The plot script to write
    std::string backend = "root";               ///< The dietrich backend, by name
    std::string meansFile;                      ///< Where the means go; nowhere if empty
    bool noTitle = false;                       ///< Leave the figure's title out
    std::string legend = "free";                ///< Where the legends lie
    std::vector<std::string> tables;            ///< label=table, once per run
};

/**
 * Parses the command line.
 *
 * @param argc The number of arguments
 * @param argv The arguments
 * @return The arguments, or nothing if the help was shown
 */
std::optional<Arguments> parseArguments(int argc, char **argv) {
    using boost::program_options::value;
    Arguments args;
    boost::program_options::options_description options("GSpeedupFigure [options] label=table...");
    options.add_options()("help,h", "Show this help")(
        "ideal",
        value<double>(&args.ideal)->default_value(args.ideal),
        "The ideal speed-up, drawn as a line (0: none)"
    )("figure", value<std::string>(&args.figure)->default_value(args.figure), "The plot script to write")(
        "backend",
        value<std::string>(&args.backend)->default_value(args.backend),
        "root, gnuplot, matplotlib or octave"
    )(
        "zoom",
        value<std::vector<double>>(&args.zooms)->multitoken(),
        "Each adds a pad with the evaluation times up to this one [s], which a linear axis crowds "
        "at the left of the first pad (default: 1 0.1; 0: no further pad)"
    )(
        "means",
        value<std::string>(&args.meansFile),
        "Also write the combined speed-up per label and evaluation time to this file"
    )(
        "legend",
        value<std::string>(&args.legend)->default_value(args.legend),
        "Where the legend of a pad lies: free (the corner, per pad, that covers the least of the "
        "curves, the lower right where it is free), bottom_right, center_right, "
        "top_left, bottom_left or top_right"
    )(
        "no-title",
        boost::program_options::bool_switch(&args.noTitle),
        "Leave out the title of the figure (a figure in a book carries it in its caption)"
    )("table", value<std::vector<std::string>>(&args.tables), "label=table, once per run");
    boost::program_options::positional_options_description positional;
    positional.add("table", -1);

    boost::program_options::variables_map vm;
    boost::program_options::store(
        boost::program_options::command_line_parser(argc, argv)
            .options(options)
            .positional(positional)
            .run(),
        vm
    );
    boost::program_options::notify(vm);
    if(vm.contains("help")) {
        std::cout << options << '\n';
        return std::nullopt;
    }
    if(args.tables.empty()) {
        std::cerr << options << '\n';
        throw std::runtime_error("GSpeedupFigure: no table given");
    }
    return args;
}

/******************************************************************************/
/**
 * Reads the tables and combines those of the same label.
 *
 * @param tables label=table, once per run
 * @return The combined series per label, in the order in which the labels first appear
 */
std::vector<std::pair<std::string, SpeedupSeries>>
readSeries(const std::vector<std::string> &tables) {
    std::vector<std::pair<std::string, std::vector<SpeedupSeries>>> runs;
    for(const auto &argument : tables) {
        const auto [label, file_name] = splitLabel(argument);
        auto it = std::ranges::find(runs, label, &decltype(runs)::value_type::first);
        if(it == runs.end()) {
            runs.emplace_back(label, std::vector<SpeedupSeries>{});
            it = std::prev(runs.end());
        }
        it->second.push_back(readTable(file_name));
    }
    std::vector<std::pair<std::string, SpeedupSeries>> series;
    series.reserve(runs.size());
    for(const auto &[label, label_tables] : runs) {
        series.emplace_back(label, combine(label_tables));
    }
    return series;
}

/******************************************************************************/
/**
 * Chooses the pads: the first shows all evaluation times, every further one those up to one of
 * the zooms. A further pad is worth drawing only if it leaves out some evaluation times, still
 * shows at least two, and differs from the pads before it.
 *
 * @param series The combined series
 * @param zooms The longest evaluation times of the further pads that were asked for
 * @return The longest evaluation time shown, per pad
 */
std::vector<double> choosePads(
    const std::vector<std::pair<std::string, SpeedupSeries>> &series,
    std::vector<double> zooms
) {
    std::vector<double> times;
    for(const auto &[label, points] : series) {
        for(const auto &[time, point] : points) {
            times.push_back(time);
        }
    }
    const double longest = std::ranges::max(times);
    std::ranges::sort(zooms, std::greater{});
    std::vector<double> pads{longest};
    for(const double zoom : zooms) {
        const auto shown = std::ranges::count_if(times, [zoom](double t) { return t <= zoom; });
        const auto shown_before =
            std::ranges::count_if(times, [&pads](double t) { return t <= pads.back(); });
        if(zoom > 0. and zoom < longest and shown >= 2 and shown < shown_before) {
            pads.push_back(zoom);
        }
    }
    return pads;
}

/******************************************************************************/
/**
 * Writes the combined speed-up per label and evaluation time.
 *
 * @param file_name The file to write
 * @param series The combined series
 */
void writeMeans(
    const std::string &file_name,
    const std::vector<std::pair<std::string, SpeedupSeries>> &series
) {
    std::ofstream out(file_name);
    out << "# label; evaluation time [s]; mean speed-up; error of the mean; runs\n"
        << std::setprecision(std::numeric_limits<double>::max_digits10);
    for(const auto &[label, points] : series) {
        for(const auto &[time, point] : points) {
            out << label << '\t' << time << '\t' << point.speedup << '\t' << point.error << '\t'
                << point.runs << '\n';
        }
    }
    if(not out) {
        throw std::runtime_error("GSpeedupFigure: could not write " + file_name);
    }
}

} /* namespace */

/******************************************************************************/

int main(int argc, char **argv) try {
    const auto args = parseArguments(argc, argv);
    if(not args) {
        return 0;
    }
    const auto series = readSeries(args->tables);
    const auto pads = choosePads(series, args->zooms);

    GDataLog log("Speed-up as a function of the evaluation time", 1, pads.size());
    for(const double up_to : pads) {
        declarePad(
            log,
            up_to == pads.front() ? std::string("All evaluation times")
                                  : std::format("Up to {:g} s", up_to),
            series,
            args->ideal,
            up_to,
            legendFromName(args->legend)
        );
    }
    log.setCanvasDimensions(1000, static_cast<std::uint32_t>(150 + (450 * pads.size())));
    if(args->noTitle) {
        log.setShowCanvasLabel(false);
    }
    log.writeToFile(args->figure, backendFromName(args->backend));
    std::cout << "GSpeedupFigure: wrote " << args->figure << '\n';

    if(not args->meansFile.empty()) {
        writeMeans(args->meansFile, series);
    }
    return 0;
}
catch(const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
