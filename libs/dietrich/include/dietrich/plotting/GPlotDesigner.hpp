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

#pragma once

#include <memory>

#include "common/GReflectiveInterfaceT.hpp"
#include "dietrich/plotting/GDataLog.hpp"
#include "dietrich/plotting/GPlotEmitter.hpp"
#include "dietrich/plotting/GPlots.hpp"
#include "common/GSelfTestable.hpp"

namespace Gem::Dietrich {


/******************************************************************************/
/**
 * The central class of the @b dietrich plotting library: it collects data
 * providers (plotters) and renders them, through pluggable backend emitters, to a
 * ROOT macro (compare http://root.cern.ch), a gnuplot or matplotlib script, raw
 * CSV / .npz data, or a self-describing manifest for an external renderer.
 *
 * @par About the name
 * The library is named for the Dresden court painter @b Christian @b Wilhelm @b Ernst
 * @b Dietrich ("Dietricy", 1712-1774), celebrated for his stylistic versatility -- he
 * painted fluently in the manner of Rembrandt, Ostade, Salvator Rosa, Watteau and
 * Claude. One painter, many idioms mirrors one library, many output backends. (As a
 * bonus, a "Dietrich" is a skeleton key in German: one key, many locks.) See
 * dietrich/README.md.
 */
class GPlotDesigner
  : public Gem::Common::GReflectiveInterfaceT<GPlotDesigner, Gem::Common::GCommonInterfaceT<GPlotDesigner>>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    // Gem::Weft::access default-constructs this concrete type on load;
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceT base reach this class's private localMembers_().
    friend struct Gem::Common::GReflectiveInterfaceAccess;
    // The pluggable backend emitters read the designer's plotters, canvas
    // dimensions / divisions and label to compose their backend document.
    friend class GRootEmitter;
    friend class GnuplotEmitter;
    friend class MatplotlibEmitter;
    friend class OctaveEmitter;
    friend class GDataEmitter;

    /**
     * @brief Single declaration of this class'es local data members, driving the
     * GReflectiveInterfaceT-generated serialize()/load_()/compare_(). plotters_cnt_ (the plot
     * list) is included so a checkpointed monitor's accumulated plots survive a resume.
     * @return A tuple of named member references
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_cloneable_container_member("plotters_cnt_", self.plotters_cnt_),
            Gem::Common::make_member("c_x_div_", self.c_x_div_),
            Gem::Common::make_member("c_y_div_", self.c_y_div_),
            Gem::Common::make_member("c_x_dim_", self.c_x_dim_),
            Gem::Common::make_member("c_y_dim_", self.c_y_dim_),
            Gem::Common::make_member("canvas_label_", self.canvas_label_),
            Gem::Common::make_member("add_print_command_", self.add_print_command_),
            Gem::Common::make_member("n_indention_spaces_", self.n_indention_spaces_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GPlotDesigner";

    /**
     * @brief The standard constructor
     * @param canvas_label The label of the canvas
     * @param c_x_div The number of plot divisions (pads) in the x-direction
     * @param c_y_div The number of plot divisions (pads) in the y-direction
     */
    GPlotDesigner(const std::string &canvas_label, const std::size_t &c_x_div, const std::size_t &c_y_div);

    /**
     * @brief Copy constructor
     * @param cp The designer to copy from
     */
    GPlotDesigner(GPlotDesigner const &cp);
    /**
     * @brief Assignment operator
     * @param cp The designer to copy from
     * @return A reference to this object
     */
    GPlotDesigner &operator=(GPlotDesigner const &cp);

    /*********************************************************************/
    // Defaulted constructors, destructor and assignment operators

    // Defaulted default constructor in private section

    /** @brief The move constructor */
    GPlotDesigner(GPlotDesigner &&) = default;
    /** @brief The (defaulted) destructor */
    ~GPlotDesigner() override = default;

    /** @brief The move-assignment operator @return A reference to this object */
    GPlotDesigner &operator=(GPlotDesigner &&) = default;

    /*********************************************************************/

    /**
     * @brief Emits the overall plot through the selected backend emitter
     * @param plot_name The name used in the emitted script (and a warning context); defaults to "empty"
     * @return The complete backend-specific document as a string
     */
    [[nodiscard]] std::string
    plot(const std::filesystem::path & plot_name = std::filesystem::path("empty")) const;
    /**
     * @brief Writes the plot to a file
     * @param file_name The path of the file the emitted plot script is written to
     */
    void writeToFile(const std::filesystem::path & file_name);

    /**
     * @brief Selects the backend the designer emits through
     *
     * Installs the standard emitter for the requested backend (ROOT, GNUPLOT,
     * MATPLOTLIB, OCTAVE, or DATA). plot() then delegates to the selected emitter.
     *
     * @param backend The plotting backend to use
     */
    void setPlotBackend(plotBackend backend);

    /**
     * @brief Installs a custom plot emitter
     *
     * Overrides the backend selected via setPlotBackend() with a caller-supplied
     * emitter. plot() delegates to it.
     *
     * @param emitter The emitter to install (must not be empty)
     */
    void setEmitter(std::shared_ptr<IPlotEmitter> emitter);

    /**
     * @brief Selects the DATA backend in the requested export format
     *
     * A convenience for `setEmitter(std::make_shared<GDataEmitter>(format))`: installs a
     * GDataEmitter that exports the raw series data (CSV text or a numpy .npz archive)
     * rather than a rendered plot. plot() then delegates to it.
     *
     * @param format The on-disk format to export (CSV, the default, or NPZ)
     */
    void setDataFormat(dataFormat format);

    /**
     * @brief Allows to add a new plotter object
     * @param plotter_ptr The plotter to register with this designer
     */
    void registerPlotter(const std::shared_ptr<GBasePlotter>& plotter_ptr);

    /**
     * @brief Set the dimensions of the output canvas
     * @param c_x_dim The canvas width in pixels
     * @param c_y_dim The canvas height in pixels
     */
    void setCanvasDimensions(const std::uint32_t & c_x_dim, const std::uint32_t & c_y_dim);
    /**
     * @brief Set the dimensions of the output canvas
     * @param c_dim A tuple holding the canvas width and height (in pixels)
     */
    void setCanvasDimensions(const std::tuple<std::uint32_t, std::uint32_t> & c_dim);
    /**
     * @brief Allows to retrieve the canvas dimensions
     * @return A tuple holding the canvas width and height (in pixels)
     */
    std::tuple<std::uint32_t, std::uint32_t> getCanvasDimensions() const;

    /**
     * @brief Allows to set the canvas label
     * @param canvas_label The label to be assigned to the entire canvas
     */
    void setCanvasLabel(const std::string & canvas_label);
    /**
     * @brief Allows to retrieve the canvas label
     * @return The current canvas label
     */
    std::string getCanvasLabel() const;

    /**
     * @brief Allows to add a "Print" command to the end of the script so that picture files are created
     * @param add_print_command Whether a print command (for png creation) should be appended to the script
     */
    void setAddPrintCommand(bool add_print_command);
    /**
     * @brief Allows to retrieve the current value of the add_print_command_ variable
     * @return Whether a print command is appended to the emitted script
     */
    bool getAddPrintCommand() const;

    /** @brief Resets the plotters */
    void resetPlotters();

    /**
     * @brief Allows to set the number of spaces used for indention
     * @param n_indention_spaces The number of spaces used for indenting emitted lines
     */
    void setNIndentionSpaces(const std::size_t & n_indention_spaces);

    /**
     * @brief Allows to retrieve the number spaces used for indention
     * @return The number of spaces currently used for indention
     */
    std::size_t getNIndentionSpaces() const;

    /**
     * @brief Returns the current number of indention spaces as a string
     * @return A string consisting of the configured number of indention spaces
     */
    std::string indent() const;

protected:
    /**
     * @brief A header for static data in a ROOT file
     * @param indent The indentation string prepended to each emitted line
     * @return The static-data header section of the ROOT script
     */
    std::string staticHeader(const std::string & indent) const;

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    /** @brief Applies modifications to this object. This is needed for testing purposes.
     *  Mutates the canvas configuration and registers a plotter so the plot list is non-empty.
     *  As a test-only hook it is never invoked on rendered objects, so it does not affect output. */
    bool modify_GUnitTests_() override {
        canvas_label_ += "_m";
        c_x_dim_ += 1;
        c_y_dim_ += 1;
        add_print_command_ = not add_print_command_;
        this->registerPlotter(std::make_shared<GGraph2D>());
        return true;
    }

private:
    /** @brief The default constructor -- only needed for (de-)serialization */
    GPlotDesigner() = default;

    std::vector<std::shared_ptr<GBasePlotter>>
        plotters_cnt_; ///< A list of plots to be added to the diagram

    std::size_t c_x_div_ = 1, c_y_div_ = 1; ///< The number of divisions in x- and y-direction
    std::uint32_t c_x_dim_ = DEFCXDIM,
                  c_y_dim_ = DEFCYDIM; ///< Holds the number of pixels of the canvas

    std::string canvas_label_ =
        std::string("empty"); ///< A label to be assigned to the entire canvas

    bool add_print_command_ =
        false; ///< Indicates whether a print command for the creation of a png file should be added

    std::size_t n_indention_spaces_ = (DEFNINDENTIONSPACES);

    // --- Backend selection (transient; not part of the serialized / compared state) ---

    /** @brief The emitter plot() delegates to (lazily defaulted to a GRootEmitter) */
    std::shared_ptr<IPlotEmitter> emitter_;
    /** @brief The plot name threaded from plot() to the emitter for the print command */
    mutable std::filesystem::path pending_plot_name_ = std::filesystem::path("empty");
};

/******************************************************************************/

} /* namespace Gem::Dietrich */


