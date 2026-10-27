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
#include "dietrich/plotting/GPlotEnums.hpp"
#include "common/GSelfTestable.hpp"

namespace Gem::Dietrich {


/******************************************************************************/
/**
 * This is the base class of a hierarchy of "decorator" classes that allow to
 * add features like markers, lines or text to plots. Plotters simply create a
 * container class of decorators, which in turn emit the code necessary to add the
 * desired decorations to the plots. Decorators (and their containers) are ordered
 * according to the plot dimension which they are supposed to cater for. The
 * dimension is provided as a template argument, so that it is not possible
 * to accidentally "mix" decorators for different dimensions. "Storage" of the
 * dimension (in the form of a compile-time template argument) is the main
 * purpose of this class. NOTE: As different access functions are needed for different
 * dimensions, some code duplication is unavoidable. C++ does not allow to add
 * "just" an additional function to a template specialization, unfortunately.
 *
 * @tparam dim The plot dimension (e.g. dimensions::Dim2, dimensions::Dim3) this decorator caters for
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <dimensions dim, Gem::Common::arithmetic coordinate_type>
class GDecorator { /* nothing */
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This is the specialization of GDecorator for 2D-plots (e.g. histograms, graphs, ...)
 *
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <Gem::Common::arithmetic coordinate_type>
class GDecorator<dimensions::Dim2, coordinate_type>
  : public Gem::Common::GReflectiveInterfaceBaseT<
        GDecorator<dimensions::Dim2, coordinate_type>,
        GCommonInterfaceT<GDecorator<dimensions::Dim2, coordinate_type>>
    > {
    ///////////////////////////////////////////////////////////////////////
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceBaseT base reach this class's
    // (empty) localMembers_(); this abstract root is never reconstructed on load.
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief This data-less base declares an *explicit* empty member list (a missing
     * one would inherit a parent's and is rejected at compile time by GReflectiveInterfaceBaseT).
     * @return An empty member tuple
     */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

    // coordinate_type is constrained to be arithmetic via the Gem::Common::arithmetic
    // concept on the template parameter (clearer diagnostics than the former static_assert).

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDecorator<Dim2, coordinate_type>";

    /***************************************************************************/
    // Defaulted constructors, destructor and assignment operators

    GDecorator() = default;
    GDecorator(GDecorator<dimensions::Dim2, coordinate_type> const &cp) = default;
    GDecorator(GDecorator<dimensions::Dim2, coordinate_type> &&cp) noexcept = default;
    ~GDecorator() override = default;

    GDecorator<dimensions::Dim2, coordinate_type> &
    operator=(GDecorator<dimensions::Dim2, coordinate_type> const &) = default;
    GDecorator<dimensions::Dim2, coordinate_type> &
    operator=(GDecorator<dimensions::Dim2, coordinate_type> &&) noexcept = default;

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data. Plot boundaries are not taken into account.
	  *
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @param pos A running index that disambiguates the names of the generated plot objects
	  * @return A string holding the plotting code that renders this decoration
	  */
    [[nodiscard]] virtual std::string
    decoratorData(const std::string &indent, const std::size_t &pos) const = 0;

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data, taking into account externally supplied
	  * plot boundaries. Decorators will usually not be drawn if they would "live" outside
	  * of the plot boundaries. Lines will be cut at the boundaries. Text, however, will
	  * not be affected by the boundaries. This function needs to be implemented by derived
	  * classes.
	  *
	  * @param x_axis_range A (min, max) tuple delimiting the plot range along the x-axis
	  * @param y_axis_range A (min, max) tuple delimiting the plot range along the y-axis
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @param pos A running index that disambiguates the names of the generated plot objects
	  * @return A string holding the plotting code that renders this decoration within the boundaries
	  */
    [[nodiscard]] virtual std::string decoratorData(
        const std::tuple<coordinate_type, coordinate_type> &x_axis_range,
        const std::tuple<coordinate_type, coordinate_type> &y_axis_range,
        const std::string &indent,
        const std::size_t &pos
    ) const = 0;

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is abstract)
    // from class_name and the empty localMembers_() declaration above.

    /***************************************************************************/
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Allows to add markers of different types to a plot. Note that this class
 * may only be used for 2D-plots.
 *
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <Gem::Common::arithmetic coordinate_type>
class GMarker
  : public Gem::Common::GReflectiveInterfaceT<
        GMarker<coordinate_type>,
        GDecorator<dimensions::Dim2, coordinate_type>
        // clone_() returns the hierarchy root (GDecorator<Dim2>). A covariant return to the
        // CRTP-self would need GMarker complete at the base's clone_ declaration, which it
        // is not; clone_ is private, so the wider return is not observable.
    >
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    // Gem::Weft::access default-constructs this concrete type on load;
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceT base reach this class's localMembers_().
    friend struct Gem::Common::GReflectiveInterfaceAccess;

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GMarker<coordinate_type>";

    /***************************************************************************/
    /**
	  * @brief The standard constructor, which takes all essential data for this
	  * decorator type.
	  *
	  * @param coordinates The (x, y) position at which the marker is drawn
	  * @param marker The type/shape of the marker to be drawn (e.g. a closed circle)
	  * @param color The color of the marker
	  * @param size The size of the marker
	  */
    GMarker(
        const std::tuple<coordinate_type, coordinate_type> &coordinates,
        const gMarker &marker,
        const gColor &color,
        const double &size
    )
      : coordinates_(coordinates)
      , marker_(marker)
      , color_(color)
      , size_(size) { /* nothing */
    }

    /***************************************************************************/
    // Defaulted constructos, destructor and assignment operators. The default-
    // constructor is private, as it is only needed for (de)serialization.

    GMarker(GMarker<coordinate_type> const &cp) = default;
    GMarker(GMarker<coordinate_type> &&cp) noexcept = default;
    ~GMarker() override = default;

    GMarker<coordinate_type> &operator=(GMarker<coordinate_type> const &) = default;
    GMarker<coordinate_type> &operator=(GMarker<coordinate_type> &&) noexcept = default;

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data. Plot boundaries are not taken into account.
	  *
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @param pos A running index that disambiguates the names of the generated plot objects
	  * @return A string holding the plotting code that draws this marker
	  */
    [[nodiscard]] std::string decoratorData(const std::string &indent, const std::size_t &pos) const override {
        std::ostringstream data; // NOLINT(cppcoreguidelines-init-variables)

        data << indent << "TMarker * tm_" << pos << " = new TMarker("
             << Gem::Common::narrow<double>(std::get<0>(coordinates_)) << ", "
             << Gem::Common::narrow<double>(std::get<1>(coordinates_)) << ", " << marker_ << ");"
             << '\n'
             << indent << "tm_" << pos << "->SetMarkerColor(" << color_ << ");" << '\n'
             << indent << "tm_" << pos << "->SetMarkerSize(" << size_ << ");" << '\n'
             << indent << "tm_" << pos << "->Draw();" << '\n'
             << '\n';

        return data.str();
    }

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data. Plot boundaries are taken into account.
	  *
	  * The marker is drawn only when its coordinates lie inside the supplied axis
	  * ranges (clipping); an out-of-range marker yields an empty string.
	  *
	  * @param x_axis_range A (min, max) tuple delimiting the plot range along the x-axis
	  * @param y_axis_range A (min, max) tuple delimiting the plot range along the y-axis
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @param pos A running index that disambiguates the names of the generated plot objects
	  * @return Plotting code for the marker, or an empty string if it falls outside the boundaries
	  */
    [[nodiscard]] std::string decoratorData(
        const std::tuple<coordinate_type, coordinate_type> &x_axis_range,
        const std::tuple<coordinate_type, coordinate_type> &y_axis_range,
        const std::string &indent,
        const std::size_t &pos
    ) const override {
        coordinate_type marker_x = std::get<0>(coordinates_);
        coordinate_type marker_y = std::get<1>(coordinates_);
        coordinate_type x_min = std::get<0>(x_axis_range);
        coordinate_type x_max = std::get<1>(x_axis_range);
        coordinate_type y_min = std::get<0>(y_axis_range);
        coordinate_type y_max = std::get<1>(y_axis_range);

        // Clip: a marker outside the axis ranges is not drawn
        if(marker_x < x_min || marker_x > x_max || marker_y < y_min || marker_y > y_max) {
            return {};
        }
        return this->decoratorData(indent, pos);
    }

protected:
    /***************************************************************************/
    /**
     * @brief The single declaration of this class'es local data members. load_() and
     * compare_() are derived from it, so the member list lives in one place.
     *
     * @return A tuple of named handles to this object's local data members
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            make_member("coordinates_", self.coordinates_),
            make_member("marker_", self.marker_),
            make_member("color_", self.color_),
            make_member("size_", self.size_)
        );
    }

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and localMembers_().

    /***************************************************************************/
    /** @brief Applies test-only modifications: this marker's coordinates and size. Never invoked on
     *  rendered objects. */
    bool modify_GUnitTests_() override {
        coordinates_ = std::tuple<coordinate_type, coordinate_type>(
            static_cast<coordinate_type>(1), static_cast<coordinate_type>(2)
        );
        size_ += 1.0;
        return true;
    }

private:
    /***************************************************************************/
    /**
	  * @brief The default constructor -- intentionally private, as it is only needed
	  * for de-serialization.
	  */
    GMarker() = default;

    /***************************************************************************/
    // Local data ...

    std::tuple<coordinate_type, coordinate_type> coordinates_; ///< The coordinates of the marker

    gMarker marker_ = gMarker::closedCircle; ///< Denotes the type of markers to be drawn
    gColor color_ = gColor::black;           ///< The color of the marker
    double size_ = 0.05;                     ///< The size of the marker
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This is the specialization of GDecorator for 3D-plots (e.g. 2D-histograms, 3D-graphs, ...)
 *
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <Gem::Common::arithmetic coordinate_type>
class GDecorator<dimensions::Dim3, coordinate_type>
  : public Gem::Common::GReflectiveInterfaceBaseT<
        GDecorator<dimensions::Dim3, coordinate_type>,
        GCommonInterfaceT<GDecorator<dimensions::Dim3, coordinate_type>>
    > {
    ///////////////////////////////////////////////////////////////////////
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceBaseT base reach this class's
    // (empty) localMembers_(); this abstract root is never reconstructed on load.
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief This data-less base declares an *explicit* empty member list (a missing
     * one would inherit a parent's and is rejected at compile time by GReflectiveInterfaceBaseT).
     * @return An empty member tuple
     */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

    // coordinate_type is constrained to be arithmetic via the Gem::Common::arithmetic
    // concept on the template parameter (clearer diagnostics than the former static_assert).

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDecorator<dimensions::Dim3, coordinate_type>";

    /***************************************************************************/
    // Defaulted constructors, destructor and assignment operators.

    GDecorator() = default;
    GDecorator(GDecorator<dimensions::Dim3, coordinate_type> const &cp) = default;
    GDecorator(GDecorator<dimensions::Dim3, coordinate_type> &&cp) noexcept = default;
    ~GDecorator() override = default;

    GDecorator<dimensions::Dim3, coordinate_type> &
    operator=(GDecorator<dimensions::Dim3, coordinate_type> const &) = default;
    GDecorator<dimensions::Dim3, coordinate_type> &
    operator=(GDecorator<dimensions::Dim3, coordinate_type> &&) noexcept = default;

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data. Plot boundaries are not taken into account.
	  *
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @param pos A running index that disambiguates the names of the generated plot objects
	  * @return A string holding the plotting code that renders this decoration
	  */
    [[nodiscard]] virtual std::string decoratorData(const std::string &indent, const std::size_t &pos) const = 0;

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data, taking into account externally supplied
	  * plot boundaries. Decorators will usually not be drawn if they would "live" outside
	  * of the plot boundaries. Lines will be cut at the boundaries. Text, however, will
	  * not be affected by the boundaries. This function needs to be implemented by derived
	  * classes.
	  *
	  * @param x_axis_range A (min, max) tuple delimiting the plot range along the x-axis
	  * @param y_axis_range A (min, max) tuple delimiting the plot range along the y-axis
	  * @param z_axis_range A (min, max) tuple delimiting the plot range along the z-axis
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @param pos A running index that disambiguates the names of the generated plot objects
	  * @return A string holding the plotting code that renders this decoration within the boundaries
	  */
    [[nodiscard]] virtual std::string decoratorData(
        const std::tuple<coordinate_type, coordinate_type> &x_axis_range,
        const std::tuple<coordinate_type, coordinate_type> &y_axis_range,
        const std::tuple<coordinate_type, coordinate_type> &z_axis_range,
        const std::string &indent,
        const std::size_t &pos
    ) const = 0;

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is abstract)
    // from class_name and the empty localMembers_() declaration above.

    /***************************************************************************/
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class acts as a container of decorator objects. In its specializations,
 * it is derived from GPtrContainerT and may thus be treated like a
 * std::vector of std::shared_ptr<GDecorator<dim>> . Note that the actual work
 * is done in the specializations for different dimensions. Hence some code
 * duplications for the different template specializations cannot be avoided.
 *
 * @tparam dim The plot dimension (e.g. dimensions::Dim2, dimensions::Dim3) of the held decorators
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <dimensions dim, Gem::Common::arithmetic coordinate_type>
class GDecoratorContainer { /* nothing */
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Specialization of GDecoratorContainer for 2D-plots
 *
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <Gem::Common::arithmetic coordinate_type>
class GDecoratorContainer<dimensions::Dim2, coordinate_type>
  : public Gem::Common::GReflectiveInterfaceBaseT<
        GDecoratorContainer<dimensions::Dim2, coordinate_type>,
        GCommonInterfaceT<GDecoratorContainer<dimensions::Dim2, coordinate_type>>
    >
  , public GPtrContainerT<GDecorator<dimensions::Dim2, coordinate_type>> {
    ///////////////////////////////////////////////////////////////////////
    // This class multiply-inherits GPtrContainerT for its container BEHAVIOUR, but
    // that base's entire serialized state is its data_cnt_ vector -- tied into
    // localMembers_ below via make_cloneable_container_member. So the quartet folds
    // onto the single-Parent GReflectiveInterfaceBaseT (Parent = the GCommonInterfaceT root)
    // exactly as the hand-written serialize (base_object<GPtrContainerT>), load_
    // (GPtrContainerT::operator=) and compare_ (compare_t on data_cnt_) did.
    // GReflectiveInterfaceAccess lets the mixin reach localMembers_(); the boost and GArchive
    // access shims both call the one-line serialize() below.
    friend struct Gem::Weft::access;
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief The GPtrContainerT base's sole state (its data_cnt_ vector) is declared here
     * so the GReflectiveInterfaceBaseT-generated load_()/compare_()/name_()/clone_() handle it uniformly.
     * @return A tuple tying the container's data vector as a cloneable-pointer container member
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_cloneable_container_member("data_cnt_", self.data_cnt_)
        );
    }

    /**
     * @brief One-line serialize(): required only to DISAMBIGUATE the two inherited serialize()s
     * (GReflectiveInterfaceBaseT's generated one and GPtrContainerT's) -- a member declared here hides both.
     * data_cnt_ rides in localMembers_, so this ties the same single vector the base_object<GPtrContainerT>
     * form used to (no base_object needed; the GCommonInterfaceT root is stateless). load_/compare_/name_/
     * clone_ still fold onto the mixin.
     * @tparam Archive The GArchive codec type
     * @param ar The archive to serialize to / from
     * @param version The (unused) serialization format version
     */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] unsigned int const version) {
        Gem::Common::serialize_members(ar, this->localMembers_());
    }
    ///////////////////////////////////////////////////////////////////////

    // coordinate_type is constrained to be arithmetic via the Gem::Common::arithmetic
    // concept on the template parameter (clearer diagnostics than the former static_assert).

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDecoratorContainer<dimensions::Dim2, coordinate_type>";

    /***************************************************************************/
    // Defaulted constructors, destructor and assignment operators

    GDecoratorContainer() = default;
    GDecoratorContainer(GDecoratorContainer<dimensions::Dim2, coordinate_type> const &cp) = default;
    GDecoratorContainer(GDecoratorContainer<dimensions::Dim2, coordinate_type> &&cp) noexcept =
        default;

    GDecoratorContainer<dimensions::Dim2, coordinate_type> &
    operator=(GDecoratorContainer<dimensions::Dim2, coordinate_type> const &) = default;
    GDecoratorContainer<dimensions::Dim2, coordinate_type> &
    operator=(GDecoratorContainer<dimensions::Dim2, coordinate_type> &&) noexcept = default;

    ~GDecoratorContainer() override = default;

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data of all decorators. Plot boundaries are
	  * not taken into account.
	  *
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @return The concatenated plotting code of all contained decorators
	  */
    [[nodiscard]] virtual std::string decoratorData(const std::string &indent) const {
        std::string result; // NOLINT(cppcoreguidelines-init-variables)

        std::size_t pos = 0;
        for(auto const &decorator_ptr : *this) {
            result += decorator_ptr->decoratorData(indent, pos++);
        }

        return result;
    }

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data of all decorators, taking into account externally supplied
	  * plot boundaries. Decorators will usually not be drawn if they would "live" outside
	  * of the plot boundaries. Lines will be cut at the boundaries. Text, however, will
	  * not be affected by the boundaries. This function needs to be implemented by derived
	  * classes.
	  *
	  * @param x_axis_range A (min, max) tuple delimiting the plot range along the x-axis
	  * @param y_axis_range A (min, max) tuple delimiting the plot range along the y-axis
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @return The concatenated plotting code of all contained decorators, clipped to the boundaries
	  */
    [[nodiscard]] virtual std::string decoratorData(
        const std::tuple<coordinate_type, coordinate_type> &x_axis_range,
        const std::tuple<coordinate_type, coordinate_type> &y_axis_range,
        const std::string &indent
    ) const {
        std::string result; // NOLINT(cppcoreguidelines-init-variables)

        std::size_t pos = 0;
        for(auto const &decorator_ptr : *this) {
            result += decorator_ptr->decoratorData(x_axis_range, y_axis_range, indent, pos++);
        }

        return result;
    }

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is abstract)
    // from class_name and the data_cnt_-tied localMembers_() declaration above.

    /***************************************************************************/
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Specialization for 2D decorators
 *
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <Gem::Common::arithmetic coordinate_type>
class GDecoratorContainer_2D
  : public Gem::Common::GReflectiveInterfaceT<
        GDecoratorContainer_2D<coordinate_type>,
        GDecoratorContainer<dimensions::Dim2, coordinate_type>
    >
  , public Gem::Common::GSelfTestable {
    ///////////////////////////////////////////////////////////////////////
    // Gem::Weft::access default-constructs this concrete type on load;
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceT base reach this class's (empty) localMembers_().
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	  * @brief This wrapper adds no own members; the *explicit* empty declaration is
	  * required (a missing one would inherit the parent's and is rejected at compile
	  * time by GReflectiveInterfaceT).
	  * @return An empty member tuple
	  */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDecoratorContainer_2D<coordinate_type>";

    // Relay to the parent class'es constructors (propagated down through the mixin's inherited ctors).
    using Gem::Common::GReflectiveInterfaceT<
        GDecoratorContainer_2D<coordinate_type>,
        GDecoratorContainer<dimensions::Dim2, coordinate_type>
    >::GReflectiveInterfaceT;

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and the empty localMembers_() declaration above.

protected:
    /***************************************************************************/
    /** @brief Applies test-only modifications: appends a marker so the decorator list is non-empty.
     *  Never invoked on rendered objects. */
    bool modify_GUnitTests_() override {
        this->push_back(std::make_shared<GMarker<coordinate_type>>(
            std::tuple<coordinate_type, coordinate_type>(
                static_cast<coordinate_type>(1), static_cast<coordinate_type>(2)
            ),
            gMarker::closedCircle,
            gColor::black,
            0.1
        ));
        return true;
    }

    /***************************************************************************/
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Specialization of GDecoratorContainer for 3D-plots
 *
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <Gem::Common::arithmetic coordinate_type>
class GDecoratorContainer<dimensions::Dim3, coordinate_type>
  : public Gem::Common::GReflectiveInterfaceBaseT<
        GDecoratorContainer<dimensions::Dim3, coordinate_type>,
        GCommonInterfaceT<GDecoratorContainer<dimensions::Dim3, coordinate_type>>
    >
  , public GPtrContainerT<GDecorator<dimensions::Dim3, coordinate_type>> {
    ///////////////////////////////////////////////////////////////////////
    // Multiply-inherits GPtrContainerT for container BEHAVIOUR; that base's sole
    // serialized state (data_cnt_) rides in localMembers_, so the quartet folds onto
    // the single-Parent GReflectiveInterfaceBaseT. GReflectiveInterfaceAccess lets the mixin reach
    // localMembers_(); the boost and GArchive access shims both call the one-line serialize() below.
    friend struct Gem::Weft::access;
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief The GPtrContainerT base's sole state (its data_cnt_ vector) is declared here
     * so the GReflectiveInterfaceBaseT-generated load_()/compare_()/name_()/clone_() handle it uniformly.
     * @return A tuple tying the container's data vector as a cloneable-pointer container member
     */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_cloneable_container_member("data_cnt_", self.data_cnt_)
        );
    }

    /**
     * @brief One-line serialize(): DISAMBIGUATES the two inherited serialize()s
     * (GReflectiveInterfaceBaseT's and GPtrContainerT's); data_cnt_ rides in localMembers_, so it ties the
     * same single vector base_object<GPtrContainerT> used to. load_/compare_/name_/clone_ fold onto the mixin.
     * @tparam Archive The GArchive codec type
     * @param ar The archive to serialize to / from
     * @param version The (unused) serialization format version
     */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] unsigned int const version) {
        Gem::Common::serialize_members(ar, this->localMembers_());
    }
    ///////////////////////////////////////////////////////////////////////

    // coordinate_type is constrained to be arithmetic via the Gem::Common::arithmetic
    // concept on the template parameter (clearer diagnostics than the former static_assert).

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDecoratorContainer<dimensions::Dim3, coordinate_type>";

    /***************************************************************************/
    // Defaulted constructors, destructor and assignment operators

    GDecoratorContainer() = default;
    GDecoratorContainer(GDecoratorContainer<dimensions::Dim3, coordinate_type> const &cp) = default;
    GDecoratorContainer(GDecoratorContainer<dimensions::Dim3, coordinate_type> &&cp) noexcept =
        default;
    ~GDecoratorContainer() override = default;

    GDecoratorContainer<dimensions::Dim3, coordinate_type> &
    operator=(GDecoratorContainer<dimensions::Dim3, coordinate_type> const &) = default;
    GDecoratorContainer<dimensions::Dim3, coordinate_type> &
    operator=(GDecoratorContainer<dimensions::Dim3, coordinate_type> &&) noexcept = default;

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data of all decorators. Plot boundaries are
	  * not taken into account.
	  *
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @return The concatenated plotting code of all contained decorators
	  */
    [[nodiscard]] virtual std::string decoratorData(const std::string &indent) const {
        std::string result; // NOLINT(cppcoreguidelines-init-variables)

        std::size_t pos = 0;
        for(auto const &decorator_ptr : *this) {
            result += decorator_ptr->decoratorData(indent, pos++);
        }

        return result;
    }

    /***************************************************************************/
    /**
	  * @brief Retrieves the decorator data of all decorators, taking into account externally supplied
	  * plot boundaries. Decorators will usually not be drawn if they would "live" outside
	  * of the plot boundaries. Lines will be cut at the boundaries. Text, however, will
	  * not be affected by the boundaries. This function needs to be implemented by derived
	  * classes.
	  *
	  * @param x_axis_range A (min, max) tuple delimiting the plot range along the x-axis
	  * @param y_axis_range A (min, max) tuple delimiting the plot range along the y-axis
	  * @param z_axis_range A (min, max) tuple delimiting the plot range along the z-axis
	  * @param indent The leading whitespace prepended to each emitted line of plotting code
	  * @return The concatenated plotting code of all contained decorators, clipped to the boundaries
	  */
    [[nodiscard]] virtual std::string decoratorData(
        const std::tuple<coordinate_type, coordinate_type> &x_axis_range,
        const std::tuple<coordinate_type, coordinate_type> &y_axis_range,
        const std::tuple<coordinate_type, coordinate_type> &z_axis_range,
        const std::string &indent
    ) const {
        std::string result; // NOLINT(cppcoreguidelines-init-variables)

        std::size_t pos = 0;
        for(auto const &decorator_ptr : *this) {
            result += decorator_ptr
                          ->decoratorData(x_axis_range, y_axis_range, z_axis_range, indent, pos++);
        }

        return result;
    }

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is abstract)
    // from class_name and the data_cnt_-tied localMembers_() declaration above.

    /***************************************************************************/
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Specialization for 3D decorators
 *
 * @tparam coordinate_type The arithmetic type used for plot coordinates
 */
template <Gem::Common::arithmetic coordinate_type>
class GDecoratorContainer_3D
  : public Gem::Common::GReflectiveInterfaceT<
        GDecoratorContainer_3D<coordinate_type>,
        GDecoratorContainer<dimensions::Dim3, coordinate_type>
    > {
    ///////////////////////////////////////////////////////////////////////
    // Gem::Weft::access default-constructs this concrete type on load;
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceT base reach this class's (empty) localMembers_().
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	  * @brief This wrapper adds no own members; the *explicit* empty declaration is
	  * required (a missing one would inherit the parent's and is rejected at compile
	  * time by GReflectiveInterfaceT).
	  * @return An empty member tuple
	  */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDecoratorContainer_3D<coordinate_type>";

    // Relay to the parent class'es constructors (propagated down through the mixin's inherited ctors).
    using Gem::Common::GReflectiveInterfaceT<
        GDecoratorContainer_3D<coordinate_type>,
        GDecoratorContainer<dimensions::Dim3, coordinate_type>
    >::GReflectiveInterfaceT;

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base from class_name and the empty localMembers_() declaration above.

protected:

    /***************************************************************************/
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////

} /* namespace Gem::Dietrich */
