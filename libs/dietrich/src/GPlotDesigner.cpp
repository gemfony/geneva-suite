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
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
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

GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GMarker<short>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GMarker<std::int32_t>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GMarker<std::uint32_t>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GMarker<float>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GMarker<double>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_2D<short>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_2D<std::int32_t>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_2D<std::uint32_t>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_2D<float>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_2D<double>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_3D<short>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_3D<std::int32_t>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_3D<std::uint32_t>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_3D<float>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GDecoratorContainer_3D<double>) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GHistogram1D) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GHistogram1I) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GHistogram2D) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GGraph2D) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GGraph2ED) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GGraph3D) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GGraph4D) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GFunctionPlotter1D) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GFunctionPlotter2D) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Dietrich::GPlotDesigner) // NOLINT

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
// The plotting enums' stream operators are supplied by the shared
// numeric_enum_io_v machinery declared in GCommonEnums.hpp / GPlotEnums.hpp.

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The copy constructor
 *
 * @param cp A copy of another GBasePlotter object
 */
GBasePlotter::GBasePlotter(const GBasePlotter &cp)
  : Gem::Common::GReflectiveInterfaceBaseT<GBasePlotter, Gem::Common::GCommonInterfaceT<GBasePlotter>>(cp)
  , drawing_arguments_(cp.drawing_arguments_)
  , x_axis_label_(cp.x_axis_label_)
  , y_axis_label_(cp.y_axis_label_)
  , z_axis_label_(cp.z_axis_label_)
  , plot_label_(cp.plot_label_)
  , ds_marker_(cp.ds_marker_)
  , id_(cp.id_) {
    // Note: Explicit scope needed for name resolution of clone -- compare
    // https://isocpp.org/wiki/faq/templates#nondependent-name-lookup-members

    // Copy secondary plot data over
    for(auto const &plotter_ptr : cp.secondary_plotter_) {
        secondary_plotter_.push_back(plotter_ptr->GCommonInterfaceT<GBasePlotter>::clone());
    }
}

/******************************************************************************/
/**
 * Assignment operator
 *
 * @param cp A constant reference to another GBasePlotter object to copy from
 * @return A reference to this object, to allow chaining
 */
GBasePlotter &GBasePlotter::operator=(GBasePlotter const &cp) {
    if(this == &cp) {
        return *this;
    }
    GCommonInterfaceT<GBasePlotter>::operator=(cp);

    drawing_arguments_ = cp.drawing_arguments_;
    x_axis_label_ = cp.x_axis_label_;
    y_axis_label_ = cp.y_axis_label_;
    z_axis_label_ = cp.z_axis_label_;
    plot_label_ = cp.plot_label_;
    ds_marker_ = cp.ds_marker_;
    id_ = cp.id_;

    Gem::Common::copyCloneableSmartPointerContainer(cp.secondary_plotter_, secondary_plotter_);

    return *this;
}

/******************************************************************************/
/**
 * Allows to set the drawing arguments for this plot
 *
 * @param drawing_arguments The drawing arguments for this plot
 */
void GBasePlotter::setDrawingArguments(std::string drawing_arguments) {
    drawing_arguments_ = std::move(drawing_arguments);
}

/******************************************************************************/
/**
 * Sets the label for the x-axis
 *
 * @param x_axis_label The label to be assigned to the x-axis
 * */
void GBasePlotter::setXAxisLabel(std::string x_axis_label) {
    x_axis_label_ = std::move(x_axis_label);
}

/******************************************************************************/
/**
 * Retrieve the x-axis label
 *
 * @return The label currently assigned to the x-axis
 */
std::string GBasePlotter::xAxisLabel() const {
    return x_axis_label_;
}

/******************************************************************************/
/**
 * Sets the label for the y-axis
 *
 * @param y_axis_label The label to be assigned to the y-axis
 */
void GBasePlotter::setYAxisLabel(std::string y_axis_label) {
    y_axis_label_ = std::move(y_axis_label);
}

/******************************************************************************/
/**
 * Retrieve the y-axis label
 *
 * @return The label currently assigned to the y-axis
 */
std::string GBasePlotter::yAxisLabel() const {
    return y_axis_label_;
}

/******************************************************************************/
/**
 * Sets the label for the z-axis
 *
 * @param z_axis_label The label to be assigned to the z-axis
 */
void GBasePlotter::setZAxisLabel(std::string z_axis_label) {
    z_axis_label_ = std::move(z_axis_label);
}

/******************************************************************************/
/**
 * Retrieve the z-axis label
 *
 * @return The label currently assigned to the z-axis
 */
std::string GBasePlotter::zAxisLabel() const {
    return z_axis_label_;
}

/******************************************************************************/
/**
 * Allows to assign a label to the entire plot
 *
 * @param p_l A label to be assigned to the entire plot
 */
void GBasePlotter::setPlotLabel(std::string p_l) {
    plot_label_ = std::move(p_l);
}

/******************************************************************************/
/**
 * Allows to retrieve the plot label
 *
 * @return The label that has been assigned to the plot
 */
std::string GBasePlotter::plotLabel() const {
    return plot_label_;
}

/******************************************************************************/
/**
 * Reports this plotter's choice as a GPlotSpec value. The base fills the common
 * fields (name, labels, drawing args) and a default kind; concrete plotters override
 * this to set their kind, their column names and (for histograms) the bin counts.
 * This is a pure const reporter and does not touch serialize()/load_()/compare_().
 *
 * @return A GPlotSpec describing this plotter
 */
GPlotSpec GBasePlotter::plotSpec() const {
    GPlotSpec spec; // kind defaults to graph_2d, role defaulted from it
    spec.name = plot_label_;
    spec.x_label = x_axis_label_;
    spec.y_label = y_axis_label_;
    spec.z_label = z_axis_label_;
    spec.drawing_args = drawing_arguments_;
    return spec;
}

/******************************************************************************/
/**
 * The base reports no exportable columns; the columnar collectors override this (see
 * GDataCollectorT::dataColumns()). A plotter that stores no exportable sample columns
 * -- a function plotter -- keeps this default.
 *
 * @return An empty list of columns
 */
std::vector<GPlotColumn> GBasePlotter::dataColumns() const {
    return {};
}

/******************************************************************************/
/**
 * The base cannot accept a generic data row (a function plotter holds no sample
 * columns); the columnar collectors override this. See GDataCollectorT::appendRow().
 *
 * @param row Unused in the base; always rejected
 */
void GBasePlotter::appendRow([[maybe_unused]] std::span<const double> row) {
    throw geneva_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In GBasePlotter::appendRow(): Error!" << '\n'
        << getPlotterName() << " does not support generic row append." << '\n'
    );
}

/******************************************************************************/
/**
 * The base has no sortable columns to order; the columnar collectors override this
 * (see GDataCollectorT::sortByFirstColumn()).
 */
void GBasePlotter::sortByFirstColumn() { /* nothing */ }

/******************************************************************************/
/**
 * Constructs an empty plotter of the kind described by a GPlotSpec (the inverse of
 * GBasePlotter::plotSpec()), applying the spec's labels, drawing arguments and (for
 * histograms) bin counts. The function plotters cannot be reconstructed from a spec
 * and therefore throw. See the declaration in GPlots.hpp for the full contract.
 *
 * @param spec The plot specification describing the plotter to build
 * @return A newly-allocated, empty plotter matching the spec
 */
std::unique_ptr<GBasePlotter> makePlotter(const GPlotSpec &spec) {
    std::unique_ptr<GBasePlotter> p;
    switch(spec.kind) {
        case plotKind::graph_2d:
            p = std::make_unique<GGraph2D>();
            break;
        case plotKind::graph_2d_err:
            p = std::make_unique<GGraph2ED>();
            break;
        case plotKind::graph_3d:
            p = std::make_unique<GGraph3D>();
            break;
        case plotKind::graph_4d:
            p = std::make_unique<GGraph4D>();
            break;
        case plotKind::hist_1d:
            // With a fixed range use the (n_bins, min, max) ctor; otherwise the single-bin-count
            // ctor auto-determines the value range from the data.
            if(spec.range_x.has_value()) {
                p = std::make_unique<GHistogram1D>(spec.n_bins_x.value_or(10), *spec.range_x);
            }
            else {
                p = std::make_unique<GHistogram1D>(spec.n_bins_x.value_or(10));
            }
            break;
        case plotKind::hist_2d:
            // A fixed range needs BOTH per-axis ranges (there is no mixed ctor); otherwise auto-range.
            if(spec.range_x.has_value() && spec.range_y.has_value()) {
                p = std::make_unique<GHistogram2D>(
                    spec.n_bins_x.value_or(10), spec.n_bins_y.value_or(10), *spec.range_x, *spec.range_y
                );
            }
            else {
                p = std::make_unique<GHistogram2D>(
                    spec.n_bins_x.value_or(10), spec.n_bins_y.value_or(10)
                );
            }
            break;
        case plotKind::hist_1i:
            // GHistogram1I has no auto-range ctor, so it can only be reconstructed when the spec
            // carries an explicit value range.
            if(spec.range_x.has_value()) {
                p = std::make_unique<GHistogram1I>(spec.n_bins_x.value_or(10), *spec.range_x);
            }
            else {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In makePlotter(): Error!" << '\n'
                    << "an integer histogram (kind \"" << to_string(spec.kind)
                    << "\") without a fixed value range cannot be reconstructed from a "
                    << "GPlotSpec -- an integer histogram has no auto-range form." << '\n'
                );
            }
            break;
        case plotKind::function_1d:
        case plotKind::function_2d:
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In makePlotter(): Error!" << '\n'
                << "a function plotter (kind \"" << to_string(spec.kind)
                << "\") cannot be reconstructed from a GPlotSpec -- its formula and "
                << "sampling range are not part of the spec." << '\n'
            );
    }

    p->setPlotLabel(spec.name);
    p->setXAxisLabel(spec.x_label);
    p->setYAxisLabel(spec.y_label);
    p->setZAxisLabel(spec.z_label);
    p->setDrawingArguments(spec.drawing_args);

    // The plot mode (scatter vs. curve) is a GGraph2D / GGraph2ED render choice; apply it
    // when the spec carries one and the built plotter supports it.
    if(spec.plot_mode.has_value()) {
        if(auto *g2d = dynamic_cast<GGraph2D *>(p.get())) {
            g2d->setPlotMode(*spec.plot_mode);
        }
        else if(auto *g2ed = dynamic_cast<GGraph2ED *>(p.get())) {
            g2ed->setPlotMode(*spec.plot_mode);
        }
        else if(auto *g3d = dynamic_cast<GGraph3D *>(p.get())) {
            // For a 3-d graph a curve is a line through the points, in the plot box's own frame.
            g3d->setDrawLines(*spec.plot_mode == graphPlotMode::CURVE);
        }
    }

    // A surface projection is a 3-d-graph render choice; on any other kind it would silently mean
    // nothing, so it is refused.
    if(spec.projection.has_value() && *spec.projection != surfaceProjection::NONE) {
        if(auto *g3d = dynamic_cast<GGraph3D *>(p.get())) {
            g3d->setSurfaceProjection(*spec.projection);
        }
        else {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In makePlotter(): Error!" << '\n'
                << "a surface projection was requested for a plot of kind \"" << to_string(spec.kind)
                << "\" -- only a 3-d graph (graph_3d) can be drawn as a projected surface." << '\n'
            );
        }
    }

    // Map-only surfaces and floor shadows are 3-d-graph render choices too.
    if(spec.surface.has_value() || spec.floor_shadow.has_value()) {
        auto *g3d = dynamic_cast<GGraph3D *>(p.get());
        if(g3d == nullptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In makePlotter(): Error!" << '\n'
                << "\"surface\" or \"floor_shadow\" was given for a plot of kind \"" << to_string(spec.kind)
                << "\" -- only a 3-d graph (graph_3d) has a surface or a floor." << '\n'
            );
        }
        g3d->setDrawSurface(spec.surface.value_or(true));
        g3d->setFloorShadow(spec.floor_shadow.value_or(false));
    }
    return p;
}

/******************************************************************************/
/**
 * Allows to assign a marker to data structures in the output file
 *
 * @param ds_marker A marker that has been assigned to the output data structures
 */
void GBasePlotter::setDataStructureMarker(std::string ds_marker) {
    ds_marker_ = std::move(ds_marker);
}

/******************************************************************************/
/**
 * Allows to retrieve the data structure marker
 *
 * @return The marker that has been assigned to the output data structures
 */
std::string GBasePlotter::dsMarker() const {
    return ds_marker_;
}

/******************************************************************************/
/**
 * Allows to add secondary plots to be added to the same sub-canvas
 *
 * @param sp A shared pointer to the secondary plotter to register; must be
 * non-empty and compatible with this plotter, otherwise an exception is thrown
 */
void GBasePlotter::registerSecondaryPlotter(const std::shared_ptr<GBasePlotter>& sp) {
    // Check that the secondary plot isn't empty
    if(not sp) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GBasePlotter::registerSecondaryPlotter(): Error!" << '\n'
            << "Got empty secondary plot" << '\n'
        );
    }

    // Check that the secondary plotter is compatible with us
    if(not this->isCompatible(sp)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GBasePlotter::registerSecondaryPlotter(): Error!" << '\n'
            << "Received incompatible secondary plotter" << '\n'
            << sp->getPlotterName() << " in plotter " << this->getPlotterName() << '\n'
        );
    }

    // Add the plotter to our collection
    secondary_plotter_.push_back(sp);
}

/******************************************************************************/
/**
 * Check that a given plotter is compatible with us. By default we only
 * check that the names of both plotters match. If other plot types are
 * compatible with this plotter, you need to overload this function.
 *
 * @param other A shared pointer to the other plotter whose compatibility is checked
 * @return true if the other plotter is compatible (same plotter name by default), false otherwise
 */
bool GBasePlotter::isCompatible(std::shared_ptr<GBasePlotter> other) const {
    return (this->getPlotterName() == other->getPlotterName());
}

/******************************************************************************/
/**
 * calculate a suffix from id and parent ids
 *
 * @param is_secondary Whether this plotter is a secondary plotter (true) or a primary one (false)
 * @param p_id The id of the parent plotter, only used when is_secondary is true
 * @param own_id This plotter's own emit index, threaded in by the caller (replaces the former mutated id_)
 * @return A suffix string built from the parent id (if secondary) and this object's own id
 */
std::string GBasePlotter::suffix(bool is_secondary, std::size_t p_id, std::size_t own_id) const {
    std::string result; // NOLINT(cppcoreguidelines-init-variables)

    if(not is_secondary) {
        result = std::string("_") + to_string(own_id);
    }
    else {
        result = std::string("_") + to_string(p_id) + std::string("_") + to_string(own_id);
    }

    return result;
}

/******************************************************************************/
/**
 * Allows to retrieve the id of this object
 *
 * @return The id currently assigned to this object
 */
std::size_t GBasePlotter::id() const {
    return id_;
}

/******************************************************************************/
/**
 * Read-only access to the secondary plotters sharing this plotter's pad
 *
 * @return A const reference to the list of registered secondary plotters
 */
const std::vector<std::shared_ptr<GBasePlotter>> &GBasePlotter::secondaryPlotters() const {
    return secondary_plotter_;
}

/******************************************************************************/
/**
 * Sets the id of the object
 *
 * @param id The id to be assigned to this object
 */
void GBasePlotter::setId(const std::size_t &id) {
    id_ = id;
}

// GBasePlotter::name_(), compare_() and load_() are generated by the
// Gem::Common::GReflectiveInterfaceBaseT base from class_name and localMembers_().

/******************************************************************************/
/**
 * Retrieve header settings for this plot (and any sub-plots)
 *
 * @param indent The indentation string prepended to every emitted line
 * @return The combined header data of this primary plotter and any secondary plotters
 */
std::string GBasePlotter::headerData(const std::string &indent) const {
    EmitStream header_data; // NOLINT(cppcoreguidelines-init-variables)

    // Add this plot's data
    header_data << indent << "// Header data for primary plotter" << '\n'
                << this->headerData_(false, 0, this->id(), indent);

    // Extract data from the secondary plotters, if any. The secondary plotter's own
    // emit index (pos) is threaded through the call instead of being stashed in its
    // id_, so this const emit no longer mutates the shared child state.
    std::size_t pos = 0;
    for(auto const &plotter_ptr : secondary_plotter_) {
        // We parent id 0 is reserved for primary plotters
        header_data << indent << "// Header data for secondary plotter " << pos << " of "
                    << this->getPlotterName() << '\n'
                    << plotter_ptr->headerData_(true, this->id(), pos, indent) << '\n';

        pos++;
    }

    return header_data.str();
}

/******************************************************************************/
/**
 * Retrieves body / data settings for this plot (and any sub-plots)
 *
 * @param indent The indentation string prepended to every emitted line
 * @return The combined body data of this primary plotter and any secondary plotters
 */
std::string GBasePlotter::bodyData(const std::string &indent) const {
    EmitStream body_data; // NOLINT(cppcoreguidelines-init-variables)

    // Add this plot's data
    body_data << indent << "// Body data for primary plotter" << '\n'
              << this->bodyData_(false, 0, this->id(), indent);

    // Extract data from the secondary plotters, if any. The secondary plotter's own
    // emit index (pos) is threaded through the call instead of being stashed in its
    // id_, so this const emit no longer mutates the shared child state.
    std::size_t pos = 0;
    for(auto const &plotter_ptr : secondary_plotter_) {
        body_data << indent << "// Body data for secondary plotter " << pos << " of "
                  << this->getPlotterName() << '\n'
                  << plotter_ptr->bodyData_(true, this->id(), pos, indent) << '\n';

        pos++;
    }

    return body_data.str();
}

/******************************************************************************/
/**
 * Retrieves footer / drawing settings for this plot (and any sub-plots)
 *
 * @param indent The indentation string prepended to every emitted line
 * @return The combined footer data of this primary plotter and any secondary plotters
 */
std::string GBasePlotter::footerData(const std::string &indent) const {
    EmitStream footer_data; // NOLINT(cppcoreguidelines-init-variables)

    // Add this plot's data
    footer_data << indent << "// Footer data for primary plotter" << '\n'
                << this->footerData_(false, 0, this->id(), indent);

    // Extract data from the secondary plotters, if any. The secondary plotter's own
    // emit index (pos) is threaded through the call instead of being stashed in its
    // id_, so this const emit no longer mutates the shared child state.
    std::size_t pos = 0;
    for(auto const &plotter_ptr : secondary_plotter_) {
        footer_data << indent << "// Footer data for secondary plotter " << pos << " of "
                    << this->getPlotterName() << '\n'
                    << plotter_ptr->footerData_(true, this->id(), pos, indent) << '\n';

        pos++;
    }

    return footer_data.str();
}


/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The standard constructor. Note that some variables are initialized in the
 * class body.
 *
 * @param canvas_label The label of the canvas
 * @param c_x_div The number of plots in x-direction
 * @param c_y_div The number of plots in y-direction
 */
GPlotDesigner::GPlotDesigner(
    const std::string &canvas_label,
    const std::size_t &c_x_div,
    const std::size_t &c_y_div
)
  : c_x_div_(c_x_div)
  , c_y_div_(c_y_div)
  , canvas_label_(canvas_label) { /* nothing */
}

/******************************************************************************/
/**
 * The copy constructor
 *
 * @param cp A constant reference to another GPlotDesigner object to copy from
 */
GPlotDesigner::GPlotDesigner(const GPlotDesigner &cp)
  : c_x_div_(cp.c_x_div_)
  , c_y_div_(cp.c_y_div_)
  , c_x_dim_(cp.c_x_dim_)
  , c_y_dim_(cp.c_y_dim_)
  , canvas_label_(cp.canvas_label_)
  , add_print_command_(cp.add_print_command_)
  , n_indention_spaces_(cp.n_indention_spaces_) {
    // Copy any secondary plotters over
    copyCloneableSmartPointerContainer<GBasePlotter>(cp.plotters_cnt_, plotters_cnt_);
}

/******************************************************************************/
/**
 * The assignment operator
 *
 * @param cp A constant reference to another GPlotDesigner object to copy from
 * @return A reference to this object, to allow chaining
 */
GPlotDesigner &GPlotDesigner::operator=(GPlotDesigner const &cp) {
    if(this == &cp) {
        return *this;
    }
    c_x_div_ = cp.c_x_div_;
    c_y_div_ = cp.c_y_div_;
    c_x_dim_ = cp.c_x_dim_;
    c_y_dim_ = cp.c_y_dim_;
    canvas_label_ = cp.canvas_label_;
    add_print_command_ = cp.add_print_command_;
    n_indention_spaces_ = cp.n_indention_spaces_;

    // Copy any secondary plotters over
    copyCloneableSmartPointerContainer<GBasePlotter>(cp.plotters_cnt_, plotters_cnt_);

    return *this;
}

/******************************************************************************/
/**
 * Writes the plot to a file
 *
 * @param file_name The name of the file to which the data can be written
 */
void GPlotDesigner::writeToFile(const std::filesystem::path &file_name) {
    // Some backends (the DATA backend's NPZ mode) emit a BINARY document that may contain
    // embedded NUL bytes; a text-mode `<<` would truncate / mangle it. Render once, then
    // write the full byte length binary-safe so every backend round-trips intact (the text
    // backends are unaffected -- their bytes are the same in either mode).
    const std::string document = plot(file_name);
    std::ofstream result(file_name, std::ios::binary);
    result.write(document.data(), static_cast<std::streamsize>(document.size()));
    result.close();
}

/******************************************************************************/
/**
 * Selects the backend the designer emits through, installing the standard emitter
 * for the requested backend. plot() then delegates to it.
 *
 * @param backend The plotting backend to use (ROOT, the default; GNUPLOT; MATPLOTLIB; OCTAVE; DATA)
 */
void GPlotDesigner::setPlotBackend(plotBackend backend) {
    switch(backend) {
        case plotBackend::ROOT:       emitter_ = std::make_shared<GRootEmitter>(); break;
        case plotBackend::GNUPLOT:    emitter_ = std::make_shared<GnuplotEmitter>(); break;
        case plotBackend::MATPLOTLIB: emitter_ = std::make_shared<MatplotlibEmitter>(); break;
        case plotBackend::OCTAVE:     emitter_ = std::make_shared<OctaveEmitter>(); break;
        case plotBackend::DATA:       emitter_ = std::make_shared<GDataEmitter>(); break;
    }
}

/******************************************************************************/
/**
 * Selects the DATA backend in the requested export format, installing a GDataEmitter.
 * A convenience for setEmitter(std::make_shared<GDataEmitter>(format)); plot() then
 * delegates to it, exporting the raw series data rather than a rendered plot.
 *
 * @param format The on-disk format to export (CSV, the default, or NPZ)
 */
void GPlotDesigner::setDataFormat(dataFormat format) {
    emitter_ = std::make_shared<GDataEmitter>(format);
}

/******************************************************************************/
/**
 * Installs a custom plot emitter, overriding the backend selected via
 * setPlotBackend(). plot() delegates to it.
 *
 * @param emitter The emitter to install (must not be empty)
 */
void GPlotDesigner::setEmitter(std::shared_ptr<IPlotEmitter> emitter) {
    if(not emitter) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GPlotDesigner::setEmitter(): Error!" << '\n'
            << "Got empty emitter" << '\n'
        );
    }
    emitter_ = std::move(emitter);
}

/******************************************************************************/
/**
 * Emits the overall plot by delegating to the selected backend emitter. The ROOT
 * backend is the default; its output is byte-identical to the historical generator.
 *
 * @param plot_name The output file name; used to derive the png file name in the emitted print command
 * (the literal string "empty" or an empty path suppresses the print command)
 * @return The complete backend document for the canvas and all registered plotters
 */
std::string GPlotDesigner::plot(const std::filesystem::path &plot_name) const {
    // Thread the plot name to the emitter (used by the ROOT print command).
    pending_plot_name_ = plot_name;

    // Lazily default to the ROOT backend so the historical behaviour is unchanged.
    if(not emitter_) {
        const_cast<GPlotDesigner *>(this)->emitter_ = std::make_shared<GRootEmitter>();
    }

    return emitter_->emitDocument(*this);
}

/******************************************************************************/
/**
 * Realizes the log and writes it to a file through the given backend.
 *
 * @param path The output file path
 * @param backend The rendering backend
 */
void GDataLog::writeToFile(const std::filesystem::path &path, plotBackend backend) const {
    GPlotDesigner gpd = this->toDesigner();
    gpd.setPlotBackend(backend);
    gpd.writeToFile(path);
}

/******************************************************************************/
/**
 * The number of declared series (primaries + overlays).
 *
 * @return The series count
 */
std::size_t GDataLog::nSeries() const {
    return series_.size();
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/******************************************************************************/
/**
 * A default header for a ROOT file
 *
 * @param indent The indentation string prepended to every emitted line
 * @return The ROOT macro source code setting up the canvas, title and graph pad
 */
std::string GPlotDesigner::staticHeader(const std::string &indent) const {
    EmitStream result; // NOLINT(cppcoreguidelines-init-variables)

    result << indent << "gROOT->Reset();" << '\n'
           << indent << "gStyle->SetCanvasColor(0);" << '\n'
           << indent << "gStyle->SetStatBorderSize(1);" << '\n'
           << indent << "gStyle->SetOptStat(0);" << '\n'
           << '\n'
           << indent << R"(TCanvas *cc = new TCanvas("cc", "cc",0,0,)" << c_x_dim_ << ","
           << c_y_dim_ << ");" << '\n'
           << '\n'
           << indent << "TPaveLabel* canvasTitle = new TPaveLabel(0.2,0.95,0.8,0.99, \""
           << rootEscape(canvas_label_) << "\");" << '\n'
           << indent << "canvasTitle->Draw();" << '\n'
           << '\n'
           << indent << R"(TPad* graphPad = new TPad("Graphs", "Graphs", 0.01, 0.01, 0.99, 0.94);)"
           << '\n'
           << indent << "graphPad->Draw();" << '\n'
           << indent << "graphPad->Divide(" << c_x_div_ << "," << c_y_div_ << ");" << '\n'
           << '\n';

    return result.str();
}

/******************************************************************************/
/**
 * Allows to add a new plotter object
 *
 * @param plotter_ptr A pointer to a plotter
 */
void GPlotDesigner::registerPlotter(const std::shared_ptr<GBasePlotter>& plotter_ptr) {
    if(plotter_ptr) {
        plotter_ptr->setId(plotters_cnt_.size());
        plotters_cnt_.push_back(plotter_ptr);
    }
    else {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "GPlotDesigner::registerPlotter(): Error!" << '\n'
            << "Got empty plotter" << '\n'
        );
    }
}

/******************************************************************************/
/**
 * Set the dimensions of the output canvas
 *
 * @param c_x_dim The x-dimension of the output canvas
 * @param c_y_dim The y-dimension of the output canvas
 */
void GPlotDesigner::setCanvasDimensions(
    const std::uint32_t &c_x_dim,
    const std::uint32_t &c_y_dim
) {
    c_x_dim_ = c_x_dim;
    c_y_dim_ = c_y_dim;
}

/******************************************************************************/
/**
 * Set the dimensions of the output canvas
 *
 * @param c_dim A tuple holding the x-dimension (get<0>) and y-dimension (get<1>) of the output canvas
 */
void GPlotDesigner::setCanvasDimensions(const std::tuple<std::uint32_t, std::uint32_t> &c_dim) {
    this->setCanvasDimensions(std::get<0>(c_dim), std::get<1>(c_dim));
}

/******************************************************************************/
/**
 * Allows to retrieve the canvas dimensions
 *
 * @return A std::tuple holding the canvas dimensions
 */
std::tuple<std::uint32_t, std::uint32_t> GPlotDesigner::getCanvasDimensions() const {
    return std::tuple<std::uint32_t, std::uint32_t>{c_x_dim_, c_y_dim_};
}

/******************************************************************************/
/**
 * Allows to set the canvas label
 *
 * @param canvas_label The label to be assigned to the output canvas
 */
void GPlotDesigner::setCanvasLabel(const std::string &canvas_label) {
    canvas_label_ = canvas_label;
}

/******************************************************************************/
/**
 * Allows to retrieve the canvas label
 *
 * @return The label currently assigned to the output canvas
 */
std::string GPlotDesigner::getCanvasLabel() const {
    return canvas_label_;
}

/******************************************************************************/
/**
 * Allows to add a "Print" command to the end of the script so that picture files are created
 *
 * @param add_print_command If true, a print command writing a png file is appended to the emitted script
 */
void GPlotDesigner::setAddPrintCommand(bool add_print_command) {
    add_print_command_ = add_print_command;
}

/******************************************************************************/
/**
 * Allows to retrieve the current value of the add_print_command_ variable
 *
 * @return true if a print command is appended to the emitted script, false otherwise
 */
bool GPlotDesigner::getAddPrintCommand() const {
    return add_print_command_;
}

/******************************************************************************/
/**
 * Allows to set the number of spaces used for indention
 *
 * @param n_indention_spaces The number of space characters used for one level of indentation
 */
void GPlotDesigner::setNIndentionSpaces(const std::size_t &n_indention_spaces) {
    n_indention_spaces_ = n_indention_spaces;
}

/******************************************************************************/
/**
 * Allows to retrieve the number spaces used for indention
 *
 * @return The number of space characters used for one level of indentation
 */
std::size_t GPlotDesigner::getNIndentionSpaces() const {
    return n_indention_spaces_;
}

/******************************************************************************/
/**
 * Returns the current number of indention spaces as a string
 *
 * @return A string consisting of the configured number of space characters
 */
std::string GPlotDesigner::indent() const {
    return std::string(n_indention_spaces_, ' ');
}

/******************************************************************************/
/**
 * Resets the plotters
 */
void GPlotDesigner::resetPlotters() {
    plotters_cnt_.clear();
}

// name_(), compare_(), clone_() and load_() for GPlotDesigner are generated by
// Gem::Common::GReflectiveInterfaceT from class_name and localMembers_().

} /* namespace Gem::Dietrich */
