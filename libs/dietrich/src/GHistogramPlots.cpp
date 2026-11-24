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

#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "dietrich/GPlotDesigner.hpp"
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

namespace Gem::Dietrich {

// The plotting library builds on common's facilities (logging, serialization helpers,
// exception types, make_member, EmitStream, ...); make them visible here without
// per-name qualification. This affects lookup only within Gem::Dietrich.
using namespace Gem::Common;

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
 * @return The lower boundary of the histogram in x-direction
 */
double GHistogram1D::getMinX() const {
    return min_x_;
}

/******************************************************************************/
/**
 * Retrieve the upper boundary of the plot
 *
 * @return The upper boundary of the histogram in x-direction
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
 * @return The lower boundary of the histogram in x-direction
 */
double GHistogram1I::getMinX() const {
    return min_x_;
}

/******************************************************************************/
/**
 * Retrieve the upper boundary of the plot
 *
 * @return The upper boundary of the histogram in x-direction
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
 * The 2-d drawing option whose ROOT draw option is @p name: the inverse of its name (enum_names_v), for a
 * plot spec that reports a set2DOpt() choice as its drawing arguments.
 *
 * @param name A ROOT draw option
 * @return The option, or nothing if no option draws as @p name (the empty option included)
 */
std::optional<tddropt> GHistogram2D::drawingOptionNamed(const std::string &name) {
    if(name.empty()) {
        return std::nullopt;
    }
    return Gem::Common::enumFromName<tddropt>(name);
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
 * @return The lower boundary in x-direction
 */
double GHistogram2D::getMinX() const {
    return min_x_;
}

/******************************************************************************/
/**
 * Retrieve the upper boundary of the plot in x-direction
 *
 * @return The upper boundary in x-direction
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
    // How ROOT draws the histogram (its own drawing arguments, or the set2DOpt() choice), so that
    // the draw style, the legend sample and whether the pad is a 3-d box follow it.
    spec.drawing_args =
        drawing_arguments_.empty() ? Gem::Common::enumName(dropt_) : drawing_arguments_;
    return spec;
}

// name_(), compare_(), clone_() and load_() for GHistogram2D are generated by
// Gem::Common::GReflectiveInterfaceT from class_name and localMembers_().

/******************************************************************************/
} /* namespace Gem::Dietrich */
