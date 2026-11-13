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

#include "dietrich/GPlotDesigner.hpp"
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <istream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>
#include "dietrich/plotting/detail/GPlotDetail.hpp"


namespace Gem::Dietrich {

// The plotting library builds on common's facilities (logging, serialization helpers,
// exception types, make_member, EmitStream, ...); make them visible here without
// per-name qualification. This affects lookup only within Gem::Dietrich.
using namespace Gem::Common;
// Dietrich declares its own to_string(plotKind), which would otherwise shadow common's
// numeric to_string(...) for unqualified calls; merge common's overloads back in.
using Gem::Common::to_string;

/******************************************************************************/
/**
 * The ROOT-macro file extension.
 *
 * @return The string ".C"
 */
std::string GRootEmitter::fileExtension() const {
    return std::string(".C");
}

/******************************************************************************/
/**
 * Emits the overall plot as a ROOT macro. This reproduces the historical
 * GPlotDesigner::plot() body verbatim, so the emitted text is unchanged.
 *
 * @param gpd The designer holding the plotters and canvas configuration
 * @return The complete ROOT macro source code for the canvas and all registered plotters
 */
std::string GRootEmitter::emitDocument(const GPlotDesigner &gpd) const {
    const std::filesystem::path &plot_name = gpd.pending_plot_name_;

    EmitStream result; // NOLINT(cppcoreguidelines-init-variables)
    std::size_t const max_plots = gpd.c_x_div_ * gpd.c_y_div_;

    warnPadOverflow(
        "GRootEmitter::emitDocument()", gpd.getCanvasLabel(), gpd.plotters_cnt_.size(), max_plots
    );

    result << "{" << '\n' << gpd.staticHeader(gpd.indent()) << '\n';

    // Plot all body sections up to the maximum allowed number
    result << gpd.indent() << "//===================  Header Section ====================" << '\n'
           << '\n';

    // Plot all headers up to the maximum allowed number
    for(const auto &p : gpd.plotters_cnt_ | std::views::take(max_plots)) {
        result << p->headerData(gpd.indent()) << '\n';
    }

    // Plot all body sections up to the maximum allowed number
    result << gpd.indent() << "//===================  Data Section ======================" << '\n'
           << '\n';

    for(const auto &p : gpd.plotters_cnt_ | std::views::take(max_plots)) {
        result << p->bodyData(gpd.indent()) << '\n';
    }

    // Plot all footer data up to the maximum allowed number
    result << gpd.indent() << "//===================  Plot Section ======================" << '\n'
           << '\n';

    for(const auto &[idx, p] :
        gpd.plotters_cnt_ | std::views::enumerate | std::views::take(max_plots)) {
        result << gpd.indent() << "graphPad->cd(" << static_cast<std::size_t>(idx) + 1 << ");"
               << '\n' /* cd starts at 1 */
               << p->footerData(gpd.indent()) << '\n';
    }

    result << gpd.indent() << "graphPad->cd();" << '\n' << gpd.indent() << "cc->cd();" << '\n';

    // Check if we are supposed to output a png file
    if(gpd.add_print_command_ && plot_name.string() != "empty" && not(plot_name.string()).empty()) {
        std::string plot_name_local = plot_name.string(); // Make sure there are no white spaces
        auto ltrim = plot_name_local.find_first_not_of(" \t\r\n");
        auto rtrim = plot_name_local.find_last_not_of(" \t\r\n");
        if(ltrim != std::string::npos) {
            plot_name_local = plot_name_local.substr(ltrim, rtrim - ltrim + 1);
        }
        else {
            plot_name_local.clear();
        }
        result << '\n'
               << gpd.indent() << "// Print out the data of this file to a png file" << '\n'
               << gpd.indent() << "cc->Print(\"" << plot_name_local << ".png\");" << '\n';
    }

    result << "}" << '\n';

    return result.str();
}


/******************************************************************************/
} /* namespace Gem::Dietrich */
