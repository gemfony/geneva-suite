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

#include "common/GReflectiveInterfaceT.hpp"
#include "dietrich/plotting/GDataCollectors.hpp"
#include "common/GSelfTestable.hpp"


namespace Gem::Dietrich {


/******************************************************************************/
/**
 * A wrapper for the ROOT TGraph class (2d data and curve-like structures). It
 * also adds the option to draw arrows between consecutive points. This results
 * in a 2D plot.
 */
class GGraph2D
  : public Gem::Common::GReflectiveInterfaceT<GGraph2D, GDataCollector2T<double, double>>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	 * @brief Single declaration of this class'es local data members, used by the
	 * GReflectiveInterfaceT-generated serialize()/load_()/compare_()
	 * @return A tuple of named local members of this object
	 */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            make_member("p_m_", self.p_m_),
            make_member("draw_arrows_", self.draw_arrows_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GGraph2D";

    /**********************************************************************/
    // Defaulted constructors, destructor and assignment operators

    /** @brief Applies test-only modifications: the base members plus this graph's plot mode and a
     *  data point. Never invoked on rendered objects. */
    bool modify_GUnitTests_() override {
        GBasePlotter::modify_GUnitTests_();
        this->setPlotMode(graphPlotMode::SCATTER);
        this->add(1.0, 2.0);
        return true;
    }

    GGraph2D() = default;
    GGraph2D(GGraph2D const &) = default;
    GGraph2D(GGraph2D &&) = default;
    ~GGraph2D() override = default;

    GGraph2D &operator=(GGraph2D const &) = default;
    GGraph2D &operator=(GGraph2D &&) = default;

    /**********************************************************************/

    /**
	 * @brief Adds arrows to the plots between consecutive points
	 * @param drawArrows Whether arrows should be drawn between consecutive points (default true)
	 */
    void setDrawArrows(bool d_a = true);
    /**
	 * @brief Retrieves the value of the draw_arrows_ variable
	 * @return Whether arrows are drawn between consecutive points
	 */
    [[nodiscard]] bool getDrawArrows() const;

    /**
	 * @brief Determines whether a scatter plot or a curve is created
	 * @param pm The plotting mode (scatter plot or connected curve) to be used
	 */
    void setPlotMode(graphPlotMode p_m);
    /**
	 * @brief Allows to retrieve the current plotting mode
	 * @return The currently set plotting mode
	 */
    [[nodiscard]] graphPlotMode getPlotMode() const;

    /**
	 * @brief Retrieves a unique name for this plotter
	 * @return A unique name for this plotter
	 */
    [[nodiscard]] std::string getPlotterName() const override;

    /**
	 * @brief Reports this plotter's choice (kind, columns, bins) as a GPlotSpec value
	 * @return A GPlotSpec describing this plotter
	 */
    [[nodiscard]] GPlotSpec plotSpec() const override;

protected:
    /**
	 * @brief Retrieve specific header settings for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The header section of the ROOT code for this plot
	 */
    [[nodiscard]] std::string headerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieves the actual data sets
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The body section of the ROOT code, holding the actual data
	 */
    [[nodiscard]] std::string bodyData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieves specific draw commands for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The footer section of the ROOT code, holding the draw commands
	 */
    [[nodiscard]] std::string footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieve the current drawing arguments
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @return The drawing arguments to be passed to ROOT's Draw() call
	 */
    [[nodiscard]] std::string drawingArguments(bool is_secondary) const override;

private:
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    graphPlotMode p_m_ =
        DEFPLOTMODE;          ///< Whether to create scatter plots or a curve, connected by lines
    bool draw_arrows_ = false; ///< When set to true, arrows will be drawn between consecutive points
};

/******************************************************************************/
/**
 * A wrapper for the ROOT TGraphErrors class (2d data and curve-like structures).
 * This results in a 2D plot.
 */
class GGraph2ED
  : public Gem::Common::GReflectiveInterfaceT<GGraph2ED, GDataCollector2ET<double, double>>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	 * @brief Single declaration of this class'es local data members, used by the
	 * GReflectiveInterfaceT-generated serialize()/load_()/compare_()
	 * @return A tuple of named local members of this object
	 */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(make_member("p_m_", self.p_m_));
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GGraph2ED";

    /**********************************************************************/
    // Defaulted constructors, destructor and assignment operators

    /** @brief Applies test-only modifications: the base members plus an (x, ex, y, ey) data point.
     *  Never invoked on rendered objects. */
    bool modify_GUnitTests_() override {
        GBasePlotter::modify_GUnitTests_();
        this->add(1.0, 0.1, 2.0, 0.2);
        return true;
    }

    GGraph2ED() = default;
    GGraph2ED(GGraph2ED const &) = default;
    GGraph2ED(GGraph2ED &&) = default;
    ~GGraph2ED() override = default;

    GGraph2ED &operator=(GGraph2ED const &) = default;
    GGraph2ED &operator=(GGraph2ED &&) = default;

    /**********************************************************************/

    /**
	 * @brief Determines whether a scatter plot or a curve is created
	 * @param pm The plotting mode (scatter plot or connected curve) to be used
	 */
    void setPlotMode(graphPlotMode p_m);
    /**
	 * @brief Allows to retrieve the current plotting mode
	 * @return The currently set plotting mode
	 */
    [[nodiscard]] graphPlotMode getPlotMode() const;

    /**
	 * @brief Retrieves a unique name for this plotter
	 * @return A unique name for this plotter
	 */
    [[nodiscard]] std::string getPlotterName() const override;

    /**
	 * @brief Reports this plotter's choice (kind, columns, bins) as a GPlotSpec value
	 * @return A GPlotSpec describing this plotter
	 */
    [[nodiscard]] GPlotSpec plotSpec() const override;

protected:
    /**
	 * @brief Retrieve specific header settings for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The header section of the ROOT code for this plot
	 */
    [[nodiscard]] std::string headerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieves the actual data sets
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The body section of the ROOT code, holding the actual data
	 */
    [[nodiscard]] std::string bodyData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieves specific draw commands for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The footer section of the ROOT code, holding the draw commands
	 */
    [[nodiscard]] std::string footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieve the current drawing arguments
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @return The drawing arguments to be passed to ROOT's Draw() call
	 */
    [[nodiscard]] std::string drawingArguments(bool is_secondary) const override;

private:
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    graphPlotMode p_m_ =
        DEFPLOTMODE; ///< Whether to create scatter plots or a curve, connected by lines
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A wrapper for the ROOT TGraph2D class (3d data). It
 * also adds the option to draw lines between consecutive points. This class
 * only allows a single plot mode. This results in a 3D plot.
 */
class GGraph3D
  : public Gem::Common::GReflectiveInterfaceT<GGraph3D, GDataCollector3T<double, double, double>>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	 * @brief Single declaration of this class'es local data members, used by the
	 * GReflectiveInterfaceT-generated serialize()/load_()/compare_()
	 * @return A tuple of named local members of this object
	 */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            make_member("draw_lines_", self.draw_lines_), make_member("projection_", self.projection_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GGraph3D";

    /*********************************************************************/
    // Defaulted constructors, destructor and assignment operators

    /** @brief Applies test-only modifications: the base members plus an (x, y, z) data point.
     *  Never invoked on rendered objects. */
    bool modify_GUnitTests_() override {
        GBasePlotter::modify_GUnitTests_();
        this->add(1.0, 2.0, 3.0);
        return true;
    }

    GGraph3D() = default;
    GGraph3D(GGraph3D const &) = default;
    GGraph3D(GGraph3D &&) = default;
    ~GGraph3D() override = default;

    GGraph3D &operator=(GGraph3D const &) = default;
    GGraph3D &operator=(GGraph3D &&) = default;

    /*********************************************************************/

    /**
	 * @brief Adds lines to the plots between consecutive points
	 * @param drawLines Whether lines should be drawn between consecutive points (default true)
	 */
    void setDrawLines(bool d_l = true);
    /**
	 * @brief Retrieves the value of the draw_lines_ variable
	 * @return Whether lines are drawn between consecutive points
	 */
    [[nodiscard]] bool getDrawLines() const;

    /**
	 * @brief Draws the data as a colour-mapped surface and shows its colour map on the floor or the
	 * ceiling of the plot box as well (surfaceProjection::NONE draws the data as before).
	 *
	 * The data must sample the surface, i.e. hold one z value per (x, y) point, ideally on a grid.
	 * The z range of the plot box is pinned to the data's own minimum and maximum, so an overlay
	 * whose points have z equal to that minimum (FLOOR) or maximum (CEILING) is drawn on the map.
	 * Only a primary plotter can be projected, and a projection decides the drawing itself: it
	 * cannot be combined with custom drawing arguments.
	 *
	 * @param projection Where the colour map is shown
	 */
    void setSurfaceProjection(surfaceProjection projection);
    /**
	 * @brief Retrieves where the colour map of the surface is shown
	 * @return The surface projection
	 */
    [[nodiscard]] surfaceProjection getSurfaceProjection() const;

    /**
	 * @brief Retrieves a unique name for this plotter
	 * @return A unique name for this plotter
	 */
    [[nodiscard]] std::string getPlotterName() const override;

    /**
	 * @brief Reports this plotter's choice (kind, columns, bins) as a GPlotSpec value
	 * @return A GPlotSpec describing this plotter
	 */
    [[nodiscard]] GPlotSpec plotSpec() const override;

protected:
    /**
	 * @brief Retrieve specific header settings for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The header section of the ROOT code for this plot
	 */
    [[nodiscard]] std::string headerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieves the actual data sets
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The body section of the ROOT code, holding the actual data
	 */
    [[nodiscard]] std::string bodyData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieves specific draw commands for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The footer section of the ROOT code, holding the draw commands
	 */
    [[nodiscard]] std::string footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieve the current drawing arguments
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @return The drawing arguments to be passed to ROOT's Draw() call
	 */
    [[nodiscard]] std::string drawingArguments(bool is_secondary) const override;

private:
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    /**
	 * @brief Emits the ROOT drawing of a projected surface (see setSurfaceProjection())
	 * @param is_secondary Whether this plotter is a secondary plotter (a projection must be primary)
	 * @param graph_name The name of the TGraph2D the footer has created
	 * @param indent The indentation string prepended to every emitted line
	 * @return The ROOT code drawing the projected surface
	 */
    [[nodiscard]] std::string
    rootProjectedSurface_(bool is_secondary, const std::string &graph_name, const std::string &indent) const;

    bool draw_lines_ = false; ///< When set to true, lines will be drawn between consecutive points
    surfaceProjection projection_ = surfaceProjection::NONE; ///< where a surface shows its colour map
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A wrapper for the ROOT TPolyMarker3D class, intended for 4D data. The fourth
 * data component is represented as the size of the markers. The class will by
 * default only draw a selection of items. This results in a 3D plot.
 */
class GGraph4D
  : public Gem::Common::GReflectiveInterfaceT<GGraph4D, GDataCollector4T<double, double, double, double>>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	 * @brief Single declaration of this class'es local data members, used by the
	 * GReflectiveInterfaceT-generated serialize()/load_()/compare_()
	 * @return A tuple of named local members of this object
	 */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            make_member("min_marker_size_", self.min_marker_size_),
            make_member("max_marker_size_", self.max_marker_size_),
            make_member("small_w_large_marker_", self.small_w_large_marker_),
            make_member("n_best_", self.n_best_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GGraph4D";

    /*********************************************************************/
    // Defaulted constructors, destructor and assignment operators

    /** @brief Applies test-only modifications: the base members plus an (x, y, z, w) data point.
     *  Never invoked on rendered objects. */
    bool modify_GUnitTests_() override {
        GBasePlotter::modify_GUnitTests_();
        this->add(1.0, 2.0, 3.0, 4.0);
        return true;
    }

    GGraph4D() = default;
    GGraph4D(const GGraph4D &) = default;
    GGraph4D(GGraph4D &&) = default;
    ~GGraph4D() override = default;

    GGraph4D &operator=(GGraph4D const &) = default;
    GGraph4D &operator=(GGraph4D &&) = default;

    /*********************************************************************/

    /**
	 * @brief Allows to set the minimum marker size
	 * @param minMarkerSize The minimum marker size to be used when drawing the w-component
	 */
    void setMinMarkerSize(const double &min_marker_size);
    /**
	 * @brief Allows to set the maximum marker size
	 * @param maxMarkerSize The maximum marker size to be used when drawing the w-component
	 */
    void setMaxMarkerSize(const double &max_marker_size);

    /**
	 * @brief Allows to retrieve the minimum marker size
	 * @return The currently set minimum marker size
	 */
    [[nodiscard]] double getMinMarkerSize() const;
    /**
	 * @brief Allows to retrieve the maximum marker size
	 * @return The currently set maximum marker size
	 */
    [[nodiscard]] double getMaxMarkerSize() const;

    /**
	 * @brief Allows to specify whether small w yield large markers
	 * @param smallWLargeMarker If true, small w-values are mapped to large markers
	 */
    void setSmallWLargeMarker(const bool &swlm);
    /**
	 * @brief Allows to check whether small w yield large markers
	 * @return Whether small w-values are mapped to large markers
	 */
    [[nodiscard]] bool getSmallWLargeMarker() const;

    /**
	 * @brief Allows to set the number of solutions the class should show
	 * @param nBest The number of (best) solutions to display; 0 means all
	 */
    void setNBest(const std::size_t &n_best);
    /**
	 * @brief Allows to retrieve the number of solutions the class should show
	 * @return The number of (best) solutions to display
	 */
    [[nodiscard]] std::size_t getNBest() const;

    /**
	 * @brief Retrieves a unique name for this plotter
	 * @return A unique name for this plotter
	 */
    [[nodiscard]] std::string getPlotterName() const override;

    /**
	 * @brief Reports this plotter's choice (kind, columns, bins) as a GPlotSpec value
	 * @return A GPlotSpec describing this plotter
	 */
    [[nodiscard]] GPlotSpec plotSpec() const override;

protected:
    /**
	 * @brief Retrieve specific header settings for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The header section of the ROOT code for this plot
	 */
    [[nodiscard]] std::string headerData_(bool isSecondary, std::size_t pId, std::size_t ownId, const std::string &indention) const override;

    /**
	 * @brief Retrieves the actual data sets
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The body section of the ROOT code, holding the actual data
	 */
    [[nodiscard]] std::string bodyData_(bool isSecondary, std::size_t pId, std::size_t ownId, const std::string &indention) const override;

    /**
	 * @brief Retrieves specific draw commands for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The footer section of the ROOT code, holding the draw commands
	 */
    [[nodiscard]] std::string footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieve the current drawing arguments
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @return The drawing arguments to be passed to ROOT's Draw() call
	 */
    [[nodiscard]] std::string drawingArguments(bool isSecondary) const override;

private:
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    double min_marker_size_ = DEFMINMARKERSIZE; ///< The minimum allowed size of the marker
    double max_marker_size_ = DEFMAXMARKERSIZE; ///< The maximum allowed size of the marker

    bool small_w_large_marker_ = true; ///< Indicates whether a small w value yields a large marker

    std::size_t n_best_ = 0; ///< Determines the number of items the class should show
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A wrapper for the ROOT TF1 1d-function plotter.
 * TODO: Add ability to add markers!
 */
class GFunctionPlotter1D
  : public Gem::Common::GReflectiveInterfaceT<GFunctionPlotter1D, GBasePlotter>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	 * @brief Single declaration of this class'es local data members, used by the
	 * GReflectiveInterfaceT-generated serialize()/load_()/compare_()
	 * @return A tuple of named local members of this object
	 */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            make_member("function_description_", self.function_description_),
            make_member("x_extremes_", self.x_extremes_),
            make_member("n_samples_x_", self.n_samples_x_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GFunctionPlotter1D";

    /** @brief Applies test-only modifications. This plotter adds no state of its own that the
     *  standard tests could perturb, so it opts into the self-test facet purely to keep the base
     *  plotter's perturbation -- and with it the generic round-trip check -- in effect. */
    bool modify_GUnitTests_() override { return GBasePlotter::modify_GUnitTests_(); }

    /**
	 * @brief The standard constructor
	 * @param fD A textual description of the 1-d function to be plotted (in ROOT TF1 syntax)
	 * @param xExtremes The minimum and maximum value of the x-axis, as a tuple
	 */
    GFunctionPlotter1D(const std::string &f_d, const std::tuple<double, double> &x_extremes);

    /*********************************************************************/
    // Defaulted constructors, destructor and assignment operators

    // Defaulted default constructor in private section

    GFunctionPlotter1D(GFunctionPlotter1D const &) = default;
    GFunctionPlotter1D(GFunctionPlotter1D &&) = default;
    ~GFunctionPlotter1D() override = default;

    GFunctionPlotter1D &operator=(GFunctionPlotter1D const &) = default;
    GFunctionPlotter1D &operator=(GFunctionPlotter1D &&) = default;

    /*********************************************************************/

    /**
	 * @brief Allows to set the number of sampling points in x-direction
	 * @param nSamplesX The number of sampling points used to evaluate the function in x-direction
	 */
    void setNSamplesX(std::size_t n_samples_x);

    /**
	 * @brief Retrieves a unique name for this plotter
	 * @return A unique name for this plotter
	 */
    [[nodiscard]] std::string getPlotterName() const override;

    /**
	 * @brief Reports this plotter's choice (kind, columns, bins) as a GPlotSpec value
	 * @return A GPlotSpec describing this plotter
	 */
    [[nodiscard]] GPlotSpec plotSpec() const override;

protected:
    /**
	 * @brief Retrieve specific header settings for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The header section of the ROOT code for this plot
	 */
    [[nodiscard]] std::string headerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieves the actual data sets
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The body section of the ROOT code, holding the actual data
	 */
    [[nodiscard]] std::string bodyData_(bool isSecondary, std::size_t pId, std::size_t ownId, const std::string &indention) const override;

    /**
	 * @brief Retrieves specific draw commands for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The footer section of the ROOT code, holding the draw commands
	 */
    [[nodiscard]] std::string footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieve the current drawing arguments
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @return The drawing arguments to be passed to ROOT's Draw() call
	 */
    [[nodiscard]] std::string drawingArguments(bool is_secondary) const override;

private:
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    GFunctionPlotter1D() =
        default; ///< The default constructor. Intentionally private, as it is only needed for (de-)serialization

    std::string function_description_; ///< A textual description of the function to be plotted

    std::tuple<double, double> x_extremes_; ///< Minimum and maximum values for the x-axis
    std::size_t n_samples_x_ = DEFNSAMPLES;  ///< The number of sampling points of the function
};

/******************************************************************************/
/**
 * A wrapper for the ROOT TF2 2d-function plotter
 */
class GFunctionPlotter2D
  : public Gem::Common::GReflectiveInterfaceT<GFunctionPlotter2D, GBasePlotter>
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	 * @brief Single declaration of this class'es local data members, used by the
	 * GReflectiveInterfaceT-generated serialize()/load_()/compare_()
	 * @return A tuple of named local members of this object
	 */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            make_member("function_description_", self.function_description_),
            make_member("x_extremes_", self.x_extremes_),
            make_member("y_extremes_", self.y_extremes_),
            make_member("n_samples_x_", self.n_samples_x_),
            make_member("n_samples_y_", self.n_samples_y_)
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GFunctionPlotter2D";

    /** @brief Applies test-only modifications. This plotter adds no state of its own that the
     *  standard tests could perturb, so it opts into the self-test facet purely to keep the base
     *  plotter's perturbation -- and with it the generic round-trip check -- in effect. */
    bool modify_GUnitTests_() override { return GBasePlotter::modify_GUnitTests_(); }

    /**
	 * @brief The standard constructor
	 * @param fD A textual description of the 2-d function to be plotted (in ROOT TF2 syntax)
	 * @param xExtremes The minimum and maximum value of the x-axis, as a tuple
	 * @param yExtremes The minimum and maximum value of the y-axis, as a tuple
	 */
    GFunctionPlotter2D(
        const std::string &f_d,
        const std::tuple<double, double> &x_extremes,
        const std::tuple<double, double> &y_extremes
    );

    /*********************************************************************/
    // Defaulted constructors, destructor and assignment operators

    // Defaulted default constructor in private section

    GFunctionPlotter2D(GFunctionPlotter2D const &) = default;
    GFunctionPlotter2D(GFunctionPlotter2D &&) = default;
    ~GFunctionPlotter2D() override = default;

    GFunctionPlotter2D &operator=(GFunctionPlotter2D const &) = default;
    GFunctionPlotter2D &operator=(GFunctionPlotter2D &&) = default;

    /*********************************************************************/

    /**
	 * @brief Allows to set the number of sampling points in x-direction
	 * @param nSamplesX The number of sampling points used to evaluate the function in x-direction
	 */
    void setNSamplesX(std::size_t n_samples_x);
    /**
	 * @brief Allows to set the number of sampling points in y-direction
	 * @param nSamplesY The number of sampling points used to evaluate the function in y-direction
	 */
    void setNSamplesY(std::size_t n_samples_y);

    /**
	 * @brief Retrieves a unique name for this plotter
	 * @return A unique name for this plotter
	 */
    [[nodiscard]] std::string getPlotterName() const override;

    /**
	 * @brief Reports this plotter's choice (kind, columns, bins) as a GPlotSpec value
	 * @return A GPlotSpec describing this plotter
	 */
    [[nodiscard]] GPlotSpec plotSpec() const override;

protected:
    /**
	 * @brief Retrieve specific header settings for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The header section of the ROOT code for this plot
	 */
    [[nodiscard]] std::string headerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieves the actual data sets
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The body section of the ROOT code, holding the actual data
	 */
    [[nodiscard]] std::string bodyData_(bool isSecondary, std::size_t pId, std::size_t ownId, const std::string &indention) const override;

    /**
	 * @brief Retrieves specific draw commands for this plot
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @param pId The id of this plotter, used to build unique variable names
	 * @param indention The indention string prepended to each emitted line
	 * @return The footer section of the ROOT code, holding the draw commands
	 */
    [[nodiscard]] std::string footerData_(bool is_secondary, std::size_t p_id, std::size_t own_id, const std::string &indent) const override;

    /**
	 * @brief Retrieve the current drawing arguments
	 * @param isSecondary Whether this is a secondary plot drawn into an existing pad
	 * @return The drawing arguments to be passed to ROOT's Draw() call
	 */
    [[nodiscard]] std::string drawingArguments(bool is_secondary) const override;

private:
    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    GFunctionPlotter2D() =
        default; ///< The default constructor -- intentionally private, as it is only needed for (de-)serialization

    std::string function_description_; ///< A textual description of the function to be plotted

    std::tuple<double, double> x_extremes_; ///< Minimum and maximum values for the x-axis
    std::tuple<double, double> y_extremes_; ///< Minimum and maximum values for the y-axis

    std::size_t n_samples_x_ = DEFNSAMPLES; ///< The number of sampling points of the function
    std::size_t n_samples_y_ = DEFNSAMPLES; ///< The number of sampling points of the function
};

/******************************************************************************/
/**
 * Constructs an empty plotter of the kind described by a GPlotSpec, the inverse of
 * GBasePlotter::plotSpec(). The returned plotter carries the spec's labels, drawing
 * arguments and (for histograms) bin counts, but holds NO data -- the caller fills it
 * via the usual add() / operator& path. This makes plot-choice a value a caller can
 * round-trip (plotter -> plotSpec() -> makePlotter() reproduces the same plotSpec()),
 * the groundwork for monitors that declare a GPlotSpec instead of hard-coding a
 * concrete plotter type.
 *
 * The function plotters (function_1d / function_2d) cannot be reconstructed from a
 * spec -- their formula and sampling range are not part of the GPlotSpec value -- so
 * those kinds throw a geneva_exception.
 *
 * @param spec The plot specification describing the plotter to build
 * @return A newly-allocated, empty plotter matching the spec
 */
[[nodiscard]] std::unique_ptr<GBasePlotter> makePlotter(const GPlotSpec &spec);

/******************************************************************************/

} /* namespace Gem::Dietrich */
