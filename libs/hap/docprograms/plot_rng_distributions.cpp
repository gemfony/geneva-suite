/**
 * @file plot_rng_distributions.cpp
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
 * Draws random numbers from the distributions the manual's hap chapter shows -- uniform real and
 * integer draws, one and two Gaussian peaks, boolean draws -- through a hap proxy, and writes one
 * CERN ROOT macro per distribution, rng-<name>.C, into the working directory. A macro builds the
 * canvas cc with a histogram of the draws and saves nothing; the renderer prints cc. ROOT is needed
 * only to render the macros, not to run this program.
 *
 * Every text a picture states about its distribution is formatted from the distribution object the
 * program draws with, so the legend and the draws cannot disagree. The program also checks what the
 * manual's captions say of the draws, and fails if they do not hold: a real-valued range is
 * half-open, an integer range closed (with this many draws both of its bounds occur), a boolean
 * draw is 0 or 1. Geneva's random numbers are not reproducible (DEVELOPMENT-INVARIANTS.md,
 * Invariant 18): every run draws other numbers.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Hap;

namespace {

/** The number of draws per distribution: some 60 per bin of a uniform histogram. */
constexpr std::size_t n_draws = 6000;

/** The range a uniform or boolean draw must lie in, and whether its upper bound belongs to it. */
struct DrawnRange {
    double low;
    double high;
    bool closed;
};

/** One picture: a histogram of the draws, the lines that state the distribution, and dashed lines
 *  at the positions the distribution is centred on. */
struct Figure {
    std::string name; // the macro is <name>.C
    bool integral;    // TH1I for integer draws, TH1F for real ones
    int nBins;
    double low;
    double high;
    std::string xTitle;
    std::vector<std::string> legend;
    std::vector<double> marks;
    std::optional<DrawnRange> range;
    std::vector<double> draws;
};

/** A value as the macros write it: the shortest text that reads back as the same value. */
std::string fmt(double value) {
    return std::format("{}", value);
}

template <typename distribution_type>
std::vector<double> draw(distribution_type &distribution, GRandomBase &gr) {
    std::vector<double> draws(n_draws);
    for(auto &d : draws) {
        d = static_cast<double>(distribution(gr));
    }
    return draws;
}

/** Real draws on [a, b): 100 bins span the range, with bounds on bin edges, and ten empty bins on
 *  either side show where the draws end. */
Figure uniformReal(
    const std::string &name,
    std::uniform_real_distribution<double> &distribution,
    GRandomBase &gr
) {
    const double a = distribution.a();
    const double b = distribution.b();
    const double margin = 10. * (b - a) / 100.;
    return {
        name,
        false,
        120,
        a - margin,
        b + margin,
        "x",
        {"Uniform double", std::format("on [{}, {})", fmt(a), fmt(b))},
        {},
        DrawnRange{a, b, false},
        draw(distribution, gr)
    };
}

/** Integer draws on [a, b]: one bin per integer, centred on it, with an empty bin on either side. */
Figure uniformInteger(
    const std::string &name,
    std::uniform_int_distribution<std::int32_t> &distribution,
    GRandomBase &gr
) {
    const double a = distribution.a();
    const double b = distribution.b();
    return {
        name,
        true,
        static_cast<int>(b - a) + 3,
        a - 1.5,
        b + 1.5,
        "x",
        {"Uniform integer", std::format("on [{}, {}]", fmt(a), fmt(b))},
        {},
        DrawnRange{a, b, true},
        draw(distribution, gr)
    };
}

/** Boolean draws, counted as 0 (false) and 1 (true). */
Figure
boolean(const std::string &name, std::bernoulli_distribution &distribution, GRandomBase &gr) {
    return {
        name,
        true,
        4,
        -1.5,
        2.5,
        "drawn value (0 = false, 1 = true)",
        {"Boolean", std::format("p(true) = {}", fmt(distribution.p()))},
        {},
        DrawnRange{0., 1., true},
        draw(distribution, gr)
    };
}

/** What is wrong with the draws of a figure that has a range, or nothing. */
std::string rangeProblem(const Figure &figure) {
    if(!figure.range) {
        return {};
    }
    const DrawnRange &r = *figure.range;
    bool low_drawn = false;
    bool high_drawn = false;
    for(const double d : figure.draws) {
        if(d < r.low || d > r.high || (!r.closed && d == r.high)) {
            return std::format("{}: the draw {} lies outside the range it states", figure.name, d);
        }
        low_drawn = low_drawn || d == r.low;
        high_drawn = high_drawn || d == r.high;
    }
    if(r.closed && !(low_drawn && high_drawn)) {
        return std::format(
            "{}: {} draws did not reach both bounds of a closed range",
            figure.name,
            n_draws
        );
    }
    return {};
}

/** Writes the macro of one figure; false if it could not be written completely. */
bool writeMacro(const Figure &figure) {
    std::ofstream ofs(figure.name + ".C");
    ofs << "{\n"
        << "  gStyle->SetOptTitle(0);\n"
        << "  gStyle->SetOptStat(0);\n"
        << "  gStyle->SetCanvasColor(0);\n"
        << "  TCanvas *cc = new TCanvas(\"cc\",\"cc\",0,0,1200,800);\n"
        << "  " << (figure.integral ? "TH1I" : "TH1F") << " *h = new "
        << (figure.integral ? "TH1I" : "TH1F") << "(\"" << figure.name << "\",\"" << figure.name
        << "\"," << figure.nBins << "," << fmt(figure.low) << "," << fmt(figure.high) << ");\n";
    // Each draw at full precision: one just below an upper bound must not be written as the bound.
    for(const double d : figure.draws) {
        ofs << "  h->Fill(" << fmt(d) << ");\n";
    }
    // The headroom above the histogram keeps the legend clear of it.
    ofs << "  h->GetXaxis()->SetTitle(\"" << figure.xTitle << "\");\n"
        << "  h->GetYaxis()->SetTitle(\"number of entries\");\n"
        << "  h->GetYaxis()->SetTitleOffset(1.2);\n"
        << "  h->SetMaximum(1.35 * h->GetMaximum());\n"
        << "  h->Draw();\n"
        << "  gPad->Update();\n";
    for(const double x : figure.marks) {
        ofs << "  { TLine *mark = new TLine(" << fmt(x) << ",0," << fmt(x) << ",gPad->GetUymax());"
            << " mark->SetLineStyle(2); mark->Draw(); }\n";
    }
    double y = 0.85;
    for(const auto &line : figure.legend) {
        ofs << "  { TLatex *text = new TLatex(0.14," << y << ",\"" << line << "\");"
            << " text->SetNDC(); text->SetTextSize(0.035); text->Draw(); }\n";
        y -= 0.05;
    }
    ofs << "}\n";
    ofs.close();
    return !ofs.fail();
}

} // namespace

int main() {
    randomFactory()->setNProducerThreads(4);
    GRandomT<randomSource::QUEUE> gr;

    std::normal_distribution<double> gauss(0., 0.5);
    bi_normal_distribution<double>
        dgauss(0. /*mean*/, 0.5 /*sigma1*/, 0.5 /*sigma2*/, 2. /*distance*/);
    std::uniform_real_distribution<double> even(0., 1.);
    std::uniform_real_distribution<double> evenwb(-3., 2.);
    std::uniform_int_distribution<std::int32_t> discrete(0, 10);
    std::uniform_int_distribution<std::int32_t> discretewb(-3, 10);
    std::bernoulli_distribution bitsimple(0.5);
    std::bernoulli_distribution bitprob(0.7);

    // One Gaussian, shown over mean +- 5.2 sigma; two, over mean +- (distance/2 + 6 sigma). The
    // dashed lines mark the means of the peaks.
    const double g_mean = gauss.mean();
    const double g_sigma = gauss.stddev();
    const double d_mean = dgauss.mean();
    const double d_half = dgauss.distance() / 2.;
    const double d_wide = std::max(dgauss.sigma1(), dgauss.sigma2());
    const std::vector<Figure> figures{
        {"rng-gauss",
         false,
         200,
         g_mean - 5.2 * g_sigma,
         g_mean + 5.2 * g_sigma,
         "x",
         {"Normal distribution", std::format("mean = {}, #sigma = {}", fmt(g_mean), fmt(g_sigma))},
         {g_mean},
         std::nullopt,
         draw(gauss, gr)},
        {"rng-dgauss",
         false,
         200,
         d_mean - d_half - 6. * d_wide,
         d_mean + d_half + 6. * d_wide,
         "x",
         {"Two normal distributions",
          std::format("mean = {}, distance = {}", fmt(d_mean), fmt(dgauss.distance())),
          std::format(
              "#sigma_{{1}} = {}, #sigma_{{2}} = {}",
              fmt(dgauss.sigma1()),
              fmt(dgauss.sigma2())
          )},
         {d_mean - d_half, d_mean + d_half},
         std::nullopt,
         draw(dgauss, gr)},
        uniformReal("rng-even", even, gr),
        uniformReal("rng-evenwb", evenwb, gr),
        uniformInteger("rng-discrete", discrete, gr),
        uniformInteger("rng-discretewb", discretewb, gr),
        boolean("rng-bitsimple", bitsimple, gr),
        boolean("rng-bitprob", bitprob, gr),
    };

    int result = 0;
    for(const auto &figure : figures) {
        if(const std::string problem = rangeProblem(figure); !problem.empty()) {
            std::cerr << problem << '\n';
            result = 1;
        }
        if(!writeMacro(figure)) {
            std::cerr << figure.name << ".C: could not be written\n";
            result = 1;
        }
    }
    return result;
}
