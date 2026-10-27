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
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Initialization with number of bins and automatic range detection
 *
 * @param n_bins_x The number of bins along the x-axis (the range is detected automatically from the data)
 */
GHistogram1D::GHistogram1D(const std::size_t &n_bins_x)
  : n_bins_x_(n_bins_x) { /* nothing */
}

/******************************************************************************/
/**
 * Initialization with the number of bins and an explicit x-range
 *
 * @param n_bins_x The number of bins along the x-axis
 * @param min_x The lower boundary of the x-axis range
 * @param max_x The upper boundary of the x-axis range
 */
GHistogram1D::GHistogram1D(const std::size_t &n_bins_x, const double &min_x, const double &max_x)
  : n_bins_x_(n_bins_x)
  , min_x_(min_x)
  , max_x_(max_x) { /* nothing */
}

/******************************************************************************/
/**
 * Initialization with a range in the form of a tuple
 *
 * @param n_bins_x The number of bins along the x-axis
 * @param range_x A tuple holding the lower (get<0>) and upper (get<1>) boundary of the x-axis range
 */
GHistogram1D::GHistogram1D(const std::size_t &n_bins_x, const std::tuple<double, double> &range_x)
  : n_bins_x_(n_bins_x)
  , min_x_(std::get<0>(range_x))
  , max_x_(std::get<1>(range_x)) { /* nothing */
}

/******************************************************************************/
/**
 * Retrieve specific header settings for this plot
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The header code declaring the ROOT TH1D histogram (using explicit or auto-detected range)
 */
std::string
GHistogram1D::headerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream header_data; // NOLINT(cppcoreguidelines-init-variables)

    const std::string comment = dsMarkerComment(ds_marker_);

    std::string const hist_name = "histD" + suffix(is_secondary, p_id, own_id);

    if(min_x_ != max_x_) {
        header_data << indent << "TH1D *" << hist_name << " = new TH1D(\"" << hist_name << "\", \""
                    << hist_name << "\"," << n_bins_x_ << ", " << min_x_ << ", " << max_x_ << ");"
                    << comment << '\n'
                    << '\n';
    }
    else { // automatic range detection
        std::tuple<double, double> minmax = this->getMinMaxElements();
        header_data << indent << "TH1D *" << hist_name << " = new TH1D(\"" << hist_name << "\", \""
                    << hist_name << "\"," << n_bins_x_ << ", " << std::get<0>(minmax) << ", "
                    << std::get<1>(minmax) << ");" << comment << '\n'
                    << '\n';
    }

    return header_data.str();
}

/******************************************************************************/
/**
 * Retrieves the actual data sets
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The body code filling the ROOT TH1D histogram with this plotter's data values
 */
std::string
GHistogram1D::bodyData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream body_data; // NOLINT(cppcoreguidelines-init-variables)

    const std::string comment = dsMarkerComment(ds_marker_);

    std::string const hist_name = "histD" + suffix(is_secondary, p_id, own_id);

    const auto &x_col = this->column<0>();
    const std::size_t n = this->currentSize();
    for(std::size_t pos_counter = 0; pos_counter < n; ++pos_counter) {
        body_data << indent << hist_name << "->Fill(" << std::showpoint << x_col[pos_counter] << ");"
                  << (pos_counter == 0 ? comment : ("")) << '\n';
    }
    body_data << '\n';

    return body_data.str();
}

/******************************************************************************/
/**
 * Retrieves specific draw commands for this plot
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The footer code setting titles and drawing the ROOT TH1D histogram
 */
std::string
GHistogram1D::footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream footer_data; // NOLINT(cppcoreguidelines-init-variables)

    std::string const hist_name = "histD" + suffix(is_secondary, p_id, own_id);

    emitRootTitle(footer_data, indent, hist_name, plot_label_);

    std::string const comment; // NOLINT(cppcoreguidelines-init-variables)
    if(!ds_marker_.empty()) {
        footer_data << "// " + rootEscape(ds_marker_) << '\n';
    }

    // Check whether custom drawing arguments have been set
    std::string const d_a = this->drawingArguments(is_secondary);

    footer_data << indent << hist_name << "->GetXaxis()->SetTitle(\"" << rootEscape(xAxisLabel()) << "\");"
                << '\n'
                << indent << hist_name << "->GetYaxis()->SetTitle(\"" << rootEscape(yAxisLabel()) << "\");"
                << '\n'
                << indent << hist_name << "->Draw(\"" << d_a << "\");" << '\n'
                << '\n';

    return footer_data.str();
}

/******************************************************************************/
/**
 * Retrieve the current drawing arguments
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @return The ROOT draw-option string; secondary plotters get the "same" option appended
 */
std::string GHistogram1D::drawingArguments(bool is_secondary) const {
    std::string d_a;

    if(!drawing_arguments_.empty()) {
        d_a = drawing_arguments_;
    }
    else {
        if(is_secondary) {
            if(d_a.empty()) {
                d_a = "same";
            }
            else {
                d_a = d_a + ",same";
            }
        }
    }

    return d_a;
}

/******************************************************************************/
/**
 * Retrieve the number of bins in x-direction
 *
 * @return The number of bins in x-direction
 */
std::size_t GHistogram1D::getNBinsX() const {
    return n_bins_x_;
}

/******************************************************************************/
/**
 * Retrieve the lower boundary of the plot
 *
 * @return The lower boundary of the plot
 */
double GHistogram1D::getMinX() const {
    return min_x_;
}

/******************************************************************************/
/**
 * Retrieve the upper boundary of the plot
 *
 * @return The upper boundary of the plot
 */
double GHistogram1D::getMaxX() const {
    return max_x_;
}

/******************************************************************************/
/**
 * Retrieves a unique name for this plotter
 *
 * @return A unique name identifying this plotter type
 */
std::string GHistogram1D::getPlotterName() const {
    return "GHistogram1D";
}

/******************************************************************************/
/**
 * Reports this plotter's choice as a GPlotSpec (a 1-d histogram of double samples).
 * The single column holds the raw (unbinned) sample values; n_bins_x carries the
 * histogram's bin count.
 *
 * @return A GPlotSpec describing this GHistogram1D
 */
GPlotSpec GHistogram1D::plotSpec() const {
    GPlotSpec spec = GBasePlotter::plotSpec();
    spec.kind = plotKind::hist_1d;
    spec.role = defaultRole(spec.kind);
    spec.columns = {"value"};
    spec.n_bins_x = n_bins_x_;
    // A fixed value range is signalled by min != max (min == max means auto-range from data).
    if(min_x_ != max_x_) {
        spec.range_x = std::make_tuple(min_x_, max_x_);
    }
    return spec;
}

// name_(), compare_(), clone_() and load_() for GHistogram1D are generated by
// Gem::Common::GReflectiveInterfaceT from class_name and localMembers_().

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The standard constructor
 *
 * @param n_bins_x The number of bins along the x-axis
 * @param min_x The lower boundary of the x-axis range
 * @param max_x The upper boundary of the x-axis range
 */
GHistogram1I::GHistogram1I(const std::size_t &n_bins_x, const double &min_x, const double &max_x)
  : n_bins_x_(n_bins_x)
  , min_x_(min_x)
  , max_x_(max_x) { /* nothing */
}

/******************************************************************************/
/**
 * Initialization with a range in the form of a tuple
 *
 * @param n_bins_x The number of bins along the x-axis
 * @param range_x A tuple holding the lower (get<0>) and upper (get<1>) boundary of the x-axis range
 */
GHistogram1I::GHistogram1I(const std::size_t &n_bins_x, const std::tuple<double, double> &range_x)
  : n_bins_x_(n_bins_x)
  , min_x_(std::get<0>(range_x))
  , max_x_(std::get<1>(range_x)) { /* nothing */
}

/******************************************************************************/
/**
 * Retrieve specific header settings for this plot
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The header code declaring the ROOT TH1I integer histogram
 */
std::string
GHistogram1I::headerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream header_data; // NOLINT(cppcoreguidelines-init-variables)

    const std::string comment = dsMarkerComment(ds_marker_);

    std::string const hist_name = "histI" + suffix(is_secondary, p_id, own_id);

    header_data << indent << "TH1I *" << hist_name << " = new TH1I(\"" << hist_name << "\", \""
                << hist_name << "\"," << n_bins_x_ << ", " << min_x_ << ", " << max_x_ << ");"
                << comment << '\n'
                << '\n';

    return header_data.str();
}

/******************************************************************************/
/**
 * Retrieves the actual data sets
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The body code filling the ROOT TH1I histogram with this plotter's integer data values
 */
std::string
GHistogram1I::bodyData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream body_data; // NOLINT(cppcoreguidelines-init-variables)

    const std::string comment = dsMarkerComment(ds_marker_);

    std::string const hist_name = "histI" + suffix(is_secondary, p_id, own_id);

    const auto &x_col = this->column<0>();
    const std::size_t n = this->currentSize();
    for(std::size_t pos_counter = 0; pos_counter < n; ++pos_counter) {
        body_data << indent << hist_name << "->Fill(" << x_col[pos_counter] << ");"
                  << (pos_counter == 0 ? comment : ("")) << '\n';
    }

    body_data << '\n';

    return body_data.str();
}

/******************************************************************************/
/**
 * Retrieves specific draw commands for this plot
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The footer code setting titles and drawing the ROOT TH1I histogram
 */
std::string
GHistogram1I::footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream footer_data; // NOLINT(cppcoreguidelines-init-variables)

    std::string const hist_name = "histI" + suffix(is_secondary, p_id, own_id);

    emitRootTitle(footer_data, indent, hist_name, plot_label_);

    std::string const comment; // NOLINT(cppcoreguidelines-init-variables)
    if(!ds_marker_.empty()) {
        footer_data << "// " + rootEscape(ds_marker_) << '\n';
    }

    // Check whether custom drawing arguments have been set
    std::string const d_a = this->drawingArguments(is_secondary);

    footer_data << indent << hist_name << "->GetXaxis()->SetTitle(\"" << rootEscape(xAxisLabel()) << "\");"
                << '\n'
                << indent << hist_name << "->GetYaxis()->SetTitle(\"" << rootEscape(yAxisLabel()) << "\");"
                << '\n'
                << indent << hist_name << "->Draw(\"" << d_a << "\");" << '\n'
                << '\n';

    return footer_data.str();
}

/******************************************************************************/
/**
 * Retrieve the current drawing arguments
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @return The ROOT draw-option string; secondary plotters get the "same" option appended
 */
std::string GHistogram1I::drawingArguments(bool is_secondary) const {
    std::string d_a;

    if(!drawing_arguments_.empty()) {
        d_a = drawing_arguments_;
    }
    else {
        if(is_secondary) {
            if(d_a.empty()) {
                d_a = "same";
            }
            else {
                d_a = d_a + ",same";
            }
        }
    }

    return d_a;
}

/******************************************************************************/
/**
 * Retrieve the number of bins in x-direction
 *
 * @return The number of bins in x-direction
 */
std::size_t GHistogram1I::getNBinsX() const {
    return n_bins_x_;
}

/******************************************************************************/
/**
 * Retrieve the lower boundary of the plot
 *
 * @return The lower boundary of the plot
 */
double GHistogram1I::getMinX() const {
    return min_x_;
}

/******************************************************************************/
/**
 * Retrieve the upper boundary of the plot
 *
 * @return The upper boundary of the plot
 */
double GHistogram1I::getMaxX() const {
    return max_x_;
}

/******************************************************************************/
/**
 * Retrieves a unique name for this plotter
 *
 * @return A unique name identifying this plotter type
 */
std::string GHistogram1I::getPlotterName() const {
    return "GHistogram1I";
}

/******************************************************************************/
/**
 * Reports this plotter's choice as a GPlotSpec (a 1-d histogram of integer samples).
 *
 * @return A GPlotSpec describing this GHistogram1I
 */
GPlotSpec GHistogram1I::plotSpec() const {
    GPlotSpec spec = GBasePlotter::plotSpec();
    spec.kind = plotKind::hist_1i;
    spec.role = defaultRole(spec.kind);
    spec.columns = {"value"};
    spec.n_bins_x = n_bins_x_;
    // An integer histogram has no auto-range ctor; emit its range whenever it is fixed
    // (min != max), which lets makePlotter reconstruct it.
    if(min_x_ != max_x_) {
        spec.range_x = std::make_tuple(min_x_, max_x_);
    }
    return spec;
}

// name_(), compare_(), clone_() and load_() for GHistogram1I are generated by
// Gem::Common::GReflectiveInterfaceT from class_name and localMembers_().

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The standard constructor
 *
 * @param n_bins_x The number of bins along the x-axis
 * @param n_bins_y The number of bins along the y-axis
 * @param min_x The lower boundary of the x-axis range
 * @param max_x The upper boundary of the x-axis range
 * @param min_y The lower boundary of the y-axis range
 * @param max_y The upper boundary of the y-axis range
 */
GHistogram2D::GHistogram2D(
    const std::size_t &n_bins_x,
    const std::size_t &n_bins_y,
    const double &min_x,
    const double &max_x,
    const double &min_y,
    const double &max_y
)
  : n_bins_x_(n_bins_x)
  , n_bins_y_(n_bins_y)
  , min_x_(min_x)
  , max_x_(max_x)
  , min_y_(min_y)
  , max_y_(max_y)
  , dropt_(tddropt::TDEMPTY) { /* nothing */
}

/******************************************************************************/
/**
 * Initialization with ranges
 *
 * @param n_bins_x The number of bins along the x-axis
 * @param n_bins_y The number of bins along the y-axis
 * @param range_x A tuple holding the lower (get<0>) and upper (get<1>) boundary of the x-axis range
 * @param range_y A tuple holding the lower (get<0>) and upper (get<1>) boundary of the y-axis range
 */
GHistogram2D::GHistogram2D(
    const std::size_t &n_bins_x,
    const std::size_t &n_bins_y,
    const std::tuple<double, double> &range_x,
    const std::tuple<double, double> &range_y
)
  : n_bins_x_(n_bins_x)
  , n_bins_y_(n_bins_y)
  , min_x_(std::get<0>(range_x))
  , max_x_(std::get<1>(range_x))
  , min_y_(std::get<0>(range_y))
  , max_y_(std::get<1>(range_y))
  , dropt_(tddropt::TDEMPTY) { /* nothing */
}

/******************************************************************************/
/**
 * Initialization with automatic range detection
 *
 * @param n_bins_x The number of bins along the x-axis (the x-range is detected automatically)
 * @param n_bins_y The number of bins along the y-axis (the y-range is detected automatically)
 */
GHistogram2D::GHistogram2D(const std::size_t &n_bins_x, const std::size_t &n_bins_y)
  : n_bins_x_(n_bins_x)
  , n_bins_y_(n_bins_y)
  , min_x_(0)
  , max_x_(min_x_)
  , min_y_(0)
  , max_y_(min_y_)
  , dropt_(tddropt::TDEMPTY) { /* nothing */
}

/******************************************************************************/
/**
 * Retrieve specific header settings for this plot
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The header code declaring the ROOT TH2D histogram (using explicit or auto-detected ranges)
 */
std::string
GHistogram2D::headerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream header_data; // NOLINT(cppcoreguidelines-init-variables)

    const std::string comment = dsMarkerComment(ds_marker_);

    std::string const hist_name = "hist2D" + suffix(is_secondary, p_id, own_id);

    if(min_x_ != max_x_ && min_y_ != max_y_) {
        header_data << indent << "TH2D *" << hist_name << " = new TH2D(\"" << hist_name << "\", \""
                    << hist_name << "\"," << n_bins_x_ << ", " << min_x_ << ", " << max_x_ << ","
                    << n_bins_y_ << ", " << min_y_ << ", " << max_y_ << ");"
                    << comment << '\n'
                    << '\n';
    }
    else { // // automatic range detection
        std::tuple<double, double, double, double> minmax = this->getMinMaxElements();

        header_data << indent << "TH2D *" << hist_name << " = new TH2D(\"" << hist_name << "\", \""
                    << hist_name << "\"," << n_bins_x_ << ", " << std::get<0>(minmax) << ", "
                    << std::get<1>(minmax) << "," << n_bins_y_ << ", " << std::get<2>(minmax) << ", "
                    << std::get<3>(minmax) << ");" << comment << '\n'
                    << '\n';
    }

    return header_data.str();
}

/******************************************************************************/
/**
 * Retrieves the actual data sets
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The body code filling the ROOT TH2D histogram with this plotter's (x, y) data values
 */
std::string
GHistogram2D::bodyData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream body_data; // NOLINT(cppcoreguidelines-init-variables)

    const std::string comment = dsMarkerComment(ds_marker_);

    std::string const hist_name = "hist2D" + suffix(is_secondary, p_id, own_id);

    const auto &x_col = this->column<0>();
    const auto &y_col = this->column<1>();
    const std::size_t n = this->currentSize();
    for(std::size_t pos_counter = 0; pos_counter < n; ++pos_counter) {
        body_data << indent << hist_name << "->Fill(" << std::showpoint << x_col[pos_counter] << ", "
                  << y_col[pos_counter] << ");" << (pos_counter == 0 ? comment : ("")) << '\n';
    }

    body_data << '\n';

    return body_data.str();
}

/******************************************************************************/
/**
 * Retrieves specific draw commands for this plot
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, used to build a unique histogram name for secondary plotters
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @param indent The indentation string prepended to every emitted line
 * @return The footer code setting titles and drawing the ROOT TH2D histogram
 */
std::string
GHistogram2D::footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const {
    EmitStream footer_data; // NOLINT(cppcoreguidelines-init-variables)

    std::string const hist_name = "hist2D" + suffix(is_secondary, p_id, own_id);

    emitRootTitle(footer_data, indent, hist_name, plot_label_);

    std::string const comment; // NOLINT(cppcoreguidelines-init-variables)
    if(!ds_marker_.empty()) {
        footer_data << "// " + rootEscape(ds_marker_) << '\n';
    }

    // Check whether custom drawing arguments have been set
    std::string const d_a = this->drawingArguments(is_secondary);

    footer_data << indent << hist_name << "->GetXaxis()->SetTitle(\"" << rootEscape(xAxisLabel()) << "\");"
                << '\n'
                << indent << hist_name << "->GetYaxis()->SetTitle(\"" << rootEscape(yAxisLabel()) << "\");"
                << '\n'
                << indent << hist_name << "->Draw(\"" << d_a << "\");" << '\n'
                << '\n';

    return footer_data.str();
}

/******************************************************************************/
/**
 * Retrieve the current drawing arguments
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @return The ROOT draw-option string derived from the 2D drawing option; secondary plotters get
 * the "same" option appended
 */
std::string GHistogram2D::drawingArguments(bool is_secondary) const {
    std::string d_a;

    if(!drawing_arguments_.empty()) {
        d_a = drawing_arguments_;
    }
    else {
        switch(dropt_) {
            using enum tddropt;
        case TDEMPTY:
            d_a = "";
            break;

        case SURFONE:
            d_a = "SURF1";
            break;

        case SURFTWOZ:
            d_a = "SURF2Z";
            break;

        case SURFTHREE:
            d_a = "SURF3";
            break;

        case SURFFOUR:
            d_a = "SURF4";
            break;

        case CONTZ:
            d_a = "CONTZ";
            break;

        case CONTONE:
            d_a = "CONT1";
            break;

        case CONTTWO:
            d_a = "CONT2";
            break;

        case CONTTHREE:
            d_a = "CONT3";
            break;

        case TEXT:
            d_a = "TEXT";
            break;

        case SCAT:
            d_a = "SCAT";
            break;

        case BOX:
            d_a = "BOX";
            break;

        case ARR:
            d_a = "ARR";
            break;

        case COLZ:
            d_a = "COLZ";
            break;

        case LEGO:
            d_a = "LEGO";
            break;

        case LEGOONE:
            d_a = "LEGO1";
            break;

        case SURFONEPOL:
            d_a = "SURF1POL";
            break;

        case SURFONECYL:
            d_a = "SURF1CYL";
            break;
        }

        if(is_secondary) {
            d_a = d_a + ",same";
        }
    }

    return d_a;
}

/******************************************************************************/
/**
 * Allows to specify 2d-drawing options
 *
 * @param dropt The 2D drawing option (e.g. surface, contour, lego, ...) to be used when drawing the histogram
 */
void GHistogram2D::set2DOpt(tddropt dropt) {
    dropt_ = dropt;
}

/******************************************************************************/
/**
 * Allows to retrieve 2d-drawing options
 *
 * @return The currently configured 2D drawing option
 */
tddropt GHistogram2D::get2DOpt() const {
    return dropt_;
}

/******************************************************************************/
/**
 * Retrieve the number of bins in x-direction
 *
 * @return The number of bins in x-direction
 */
std::size_t GHistogram2D::getNBinsX() const {
    return n_bins_x_;
}

/******************************************************************************/
/**
 * Retrieve the number of bins in y-direction
 *
 * @return The number of bins in y-direction
 */
std::size_t GHistogram2D::getNBinsY() const {
    return n_bins_y_;
}

/******************************************************************************/
/**
 * Retrieve the lower boundary of the plot in x-direction
 *
 * @return The lower boundary of the plot in x-direction
 */
double GHistogram2D::getMinX() const {
    return min_x_;
}

/******************************************************************************/
/**
 * Retrieve the upper boundary of the plot in x-direction
 *
 * @return The upper boundary of the plot in x-direction
 */
double GHistogram2D::getMaxX() const {
    return max_x_;
}

/******************************************************************************/
/**
 * Retrieve the lower boundary of the plot in y-direction
 *
 * @return The lower boundary of the plot in y-direction
 */
double GHistogram2D::getMinY() const {
    return min_y_;
}

/******************************************************************************/
/**
 * Retrieve the upper boundary of the plot in y-direction
 *
 * @return The upper boundary of the plot in y-direction
 */
double GHistogram2D::getMaxY() const {
    return max_y_;
}

/******************************************************************************/
/**
 * Retrieves a unique name for this plotter
 *
 * @return A unique name identifying this plotter type
 */
std::string GHistogram2D::getPlotterName() const {
    return "GHistogram2D";
}

/******************************************************************************/
/**
 * Reports this plotter's choice as a GPlotSpec (a 2-d histogram). The columns hold
 * the raw (unbinned) x/y sample values; n_bins_x / n_bins_y carry the bin counts.
 *
 * @return A GPlotSpec describing this GHistogram2D
 */
GPlotSpec GHistogram2D::plotSpec() const {
    GPlotSpec spec = GBasePlotter::plotSpec();
    spec.kind = plotKind::hist_2d;
    spec.role = defaultRole(spec.kind);
    spec.columns = {"x", "y"};
    spec.n_bins_x = n_bins_x_;
    spec.n_bins_y = n_bins_y_;
    // Fixed per-axis ranges are signalled by min != max (min == max means auto-range).
    if(min_x_ != max_x_) {
        spec.range_x = std::make_tuple(min_x_, max_x_);
    }
    if(min_y_ != max_y_) {
        spec.range_y = std::make_tuple(min_y_, max_y_);
    }
    return spec;
}

// name_(), compare_(), clone_() and load_() for GHistogram2D are generated by
// Gem::Common::GReflectiveInterfaceT from class_name and localMembers_().


/******************************************************************************/
} /* namespace Gem::Dietrich */
