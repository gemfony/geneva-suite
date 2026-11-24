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
#include "dietrich/plotting/GDataCollectors.hpp"
#include "dietrich/plotting/GDataLog.hpp"
#include "dietrich/plotting/GGraphPlots.hpp"
#include "dietrich/plotting/GHistogramPlots.hpp"
#include "dietrich/plotting/GPlotEmitter.hpp"

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
  : public Gem::Common::
        GReflectiveInterfaceT<GPlotDesigner, Gem::Common::GCommonInterfaceT<GPlotDesigner>> {
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
    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_cloneable_container_member("plotters_cnt_", self.plotters_cnt_),
            Gem::Common::make_member("c_x_div_", self.c_x_div_),
            Gem::Common::make_member("c_y_div_", self.c_y_div_),
            Gem::Common::make_member("c_x_dim_", self.c_x_dim_),
            Gem::Common::make_member("c_y_dim_", self.c_y_dim_),
            Gem::Common::make_member("canvas_label_", self.canvas_label_),
            Gem::Common::make_member("add_print_command_", self.add_print_command_),
            Gem::Common::make_member("show_canvas_label_", self.show_canvas_label_),
            Gem::Common::make_member("n_indention_spaces_", self.n_indention_spaces_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GPlotDesigner";

    /**
     * @brief The standard constructor
     */
    GPlotDesigner(
        const std::string &canvas_label,
        const std::size_t &c_x_div,
        const std::size_t &c_y_div
    );

    /**
     * @brief Copy constructor
     */
    GPlotDesigner(GPlotDesigner const &cp);
    /**
     * @brief Assignment operator
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
     */
    [[nodiscard]] std::string
    plot(const std::filesystem::path &plot_name = std::filesystem::path("empty")) const;
    /**
     * @brief Writes the plot to a file; throws if the file cannot be opened or written
     */
    void writeToFile(const std::filesystem::path &file_name);

    /**
     * @brief Selects the backend the designer emits through
     *
     * Installs the standard emitter for the requested backend (ROOT, GNUPLOT,
     * MATPLOTLIB, OCTAVE, or DATA). plot() then delegates to the selected emitter.
     */
    void setPlotBackend(plotBackend backend);

    /**
     * @brief Installs a custom plot emitter
     *
     * Overrides the backend selected via setPlotBackend() with a caller-supplied
     * emitter. plot() delegates to it.
     */
    void setEmitter(std::shared_ptr<IPlotEmitter> emitter);

    /**
     * @brief Selects the DATA backend in the requested export format
     *
     * A convenience for `setEmitter(std::make_shared<GDataEmitter>(format))`: installs a
     * GDataEmitter that exports the raw series data (CSV text or a numpy .npz archive)
     * rather than a rendered plot. plot() then delegates to it.
     */
    void setDataFormat(dataFormat format);

    /**
     * @brief Allows to add a new plotter object
     */
    void registerPlotter(const std::shared_ptr<GBasePlotter> &plotter_ptr);

    /**
     * @brief Set the dimensions of the output canvas
     */
    void setCanvasDimensions(const std::uint32_t &c_x_dim, const std::uint32_t &c_y_dim);
    /**
     * @brief Set the dimensions of the output canvas
     */
    void setCanvasDimensions(const std::tuple<std::uint32_t, std::uint32_t> &c_dim);
    /**
     * @brief Allows to retrieve the canvas dimensions
     */
    std::tuple<std::uint32_t, std::uint32_t> getCanvasDimensions() const;

    /**
     * @brief Allows to set the canvas label
     */
    void setCanvasLabel(const std::string &canvas_label);
    /**
     * @brief Allows to retrieve the canvas label
     */
    std::string getCanvasLabel() const;

    /**
     * @brief Allows to add a "Print" command to the end of the script so that picture files are created
     */
    void setAddPrintCommand(bool add_print_command);
    /**
     * @brief Allows to retrieve the current value of the add_print_command_ variable
     */
    bool getAddPrintCommand() const;

    /**
     * @brief Whether the canvas label is shown as the title of the whole figure. It is by
     * default; a figure that carries its title elsewhere (a caption in a book) leaves it out.
     */
    void setShowCanvasLabel(bool show_canvas_label);
    /**
     * @brief Whether the canvas label is shown
     */
    bool getShowCanvasLabel() const;

    /** @brief Resets the plotters */
    void resetPlotters();

    /**
     * @brief Allows to set the number of spaces used for indention
     */
    void setNIndentionSpaces(const std::size_t &n_indention_spaces);

    /**
     * @brief Allows to retrieve the number spaces used for indention
     */
    std::size_t getNIndentionSpaces() const;

    /**
     * @brief Returns the current number of indention spaces as a string
     */
    std::string indent() const;

protected:
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

public:
    /** @brief Changes this object for the standard tests (common/testing/GStandardTests.hpp).
     *  Mutates the canvas configuration and registers a plotter so the plot list is non-empty.
     *  It is never invoked on rendered objects, so it does not affect output. */
    bool modifyForTest() {
        canvas_label_ += "_m";
        c_x_dim_ += 1;
        c_y_dim_ += 1;
        add_print_command_ = not add_print_command_;
        show_canvas_label_ = not show_canvas_label_;
        this->registerPlotter(std::make_shared<GGraph2D>());
        return true;
    }

protected:
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

    bool show_canvas_label_ = true; ///< Whether the canvas label is shown as the figure's title

    std::size_t n_indention_spaces_ = (DEFNINDENTIONSPACES);

    // --- Backend selection (transient; not part of the serialized / compared state) ---

    /** @brief transient: the emitter plot() delegates to, a choice of the running program (lazily
     *  defaulted to a GRootEmitter) */
    std::shared_ptr<IPlotEmitter> emitter_;
    /** @brief transient: the plot name threaded from plot() to the emitter for the print command,
     *  valid during one plot() */
    mutable std::filesystem::path pending_plot_name_ = std::filesystem::path("empty");
};

/******************************************************************************/

} /* namespace Gem::Dietrich */
