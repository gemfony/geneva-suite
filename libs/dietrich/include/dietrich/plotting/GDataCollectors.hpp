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
#include <tuple>
#include <type_traits>

#include "common/GReflectiveInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp" // getMinMax, used by project<I>()
#include "dietrich/plotting/GBasePlotter.hpp"

namespace Gem::Dietrich {


// Only forward-declared here: project<I>() returns freshly filled 1-d histograms,
// and GHistogram1D (GHistogramPlots.hpp) itself derives from a collector defined
// in this header. The complete type is available wherever project<I>() is
// instantiated (every user of the histogram header).
class GHistogram1D;

/******************************************************************************/
/**
 * A variadic data collector for N-d data of user-defined component types. It is
 * the single, generic implementation backing the named GDataCollector{1T,2T,2ET,
 * 3T,4T} collectors (see the alias declarations below). The data is stored in
 * columnar (struct-of-arrays) form: one std::vector per axis, i.e.
 * std::tuple<std::vector<Ts>...>. This matches the per-axis C arrays the emitter
 * builds and removes the std::get<>() churn of the previous array-of-tuples
 * layout. A logical data item is the cross-section of all columns at one index;
 * the per-axis machinery (the narrowing operator& overloads, the projections,
 * sorting and min/max extraction) is generated from the parameter pack via fold
 * expressions / std::index_sequence.
 *
 * The class is assumed to be movable, hence we use all defaulted constructors and
 * assignment operators.
 *
 * @tparam Ts The numeric component types of each stored data item (one per axis)
 */
template <typename... Ts>
class GDataCollectorT : public Gem::Common::GReflectiveInterfaceBaseT<GDataCollectorT<Ts...>, GBasePlotter> {
    ///////////////////////////////////////////////////////////////////////
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceBaseT base reach this class's
    // private localMembers_(); this abstract collector is never reconstructed on load.
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief The number of axes / components of each stored data item */
    static constexpr std::size_t n_axes = sizeof...(Ts);
    /** @brief The component type of axis I (e.g. axis_t<0> is the x-component type) */
    template <std::size_t I>
    using axis_t = std::tuple_element_t<I, std::tuple<Ts...>>;
    /** @brief The logical type of a single data item (the cross-section of all columns) */
    using item_t = std::tuple<Ts...>;
    /** @brief The columnar storage type: one std::vector per axis */
    using columns_t = std::tuple<std::vector<Ts>...>;

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDataCollectorT<Ts...>";

    /***************************************************************************/
    // Defaulted constructors and destructors

    GDataCollectorT() = default;
    GDataCollectorT(GDataCollectorT<Ts...> const &) = default;
    GDataCollectorT(GDataCollectorT<Ts...> &&) = default;

    ~GDataCollectorT() override = default;

    GDataCollectorT<Ts...> &operator=(GDataCollectorT<Ts...> const &) = default;
    GDataCollectorT<Ts...> &operator=(GDataCollectorT<Ts...> &&) = default;

    /***************************************************************************/
    /**
	  * Allows to retrieve information about the amount of data sets stored in
	  * this object
	  *
	  * @return The number of data items currently stored in this collector
	  */
    [[nodiscard]] std::size_t currentSize() const {
        return std::get<0>(columns_).size();
    }

    /***************************************************************************/
    /**
	  * Reserves capacity in every column, so a known number of subsequent
	  * insertions does not repeatedly reallocate.
	  *
	  * @param n The number of data items to reserve room for
	  */
    void reserve(std::size_t n) {
        reserveImpl(n, std::make_index_sequence<n_axes>{});
    }

    /***************************************************************************/
    /**
	  * Alias for reserve(), provided for call sites that pass a non-binding
	  * size hint.
	  *
	  * @param n The number of data items to reserve room for
	  */
    void reserveHint(std::size_t n) {
        this->reserve(n);
    }

    /***************************************************************************/
    /**
	  * This very simple functions allows derived classes
	  * to add data easily to their data sets, when called through a
	  * pointer. I.e., this makes "object_ptr->add(data)" instead of
	  * "*object_ptr & data" possible.
	  *
	  * @tparam data_type The type of the data item being added
	  * @param item The data item to be added to the collection
	  */
    template <typename data_type>
    void add(const data_type &item) {
        *this &item;
    }

    /***************************************************************************/
    /**
	  * For multi-axis (1<) collectors: convenience add() taking one argument per
	  * axis, assembling them into a tuple before forwarding to operator&.
	  *
	  * @param items One value per axis, to be combined into a single data item
	  */
    template <typename... Args>
        requires((n_axes > 1) && sizeof...(Args) == n_axes)
    void add(const Args &...items) {
        *this &std::make_tuple(items...);
    }

    /***************************************************************************/
    // The data-insertion operators. For a single-axis collector (n_axes == 1)
    // values are passed bare; for multi-axis collectors they are passed as a
    // std::tuple. Both the exactly-typed and the convertible ("undetermined")
    // forms are provided, the latter performing a checked narrow<> conversion.

    /**
	  * Adds a single, exactly-typed data item.
	  *
	  * @param item The data item to be added to the collection
	  */
    void operator&(const item_t &item) {
        pushItem(item, std::make_index_sequence<n_axes>{});
    }

    /**
	  * Adds a single data item of undetermined component type(s), provided it can
	  * be converted safely to the target component type(s).
	  *
	  * @tparam Us The source component type(s), narrowed to the target type(s)
	  * @param item_undet The data item to be added to the collection
	  */
    template <typename... Us>
        requires(sizeof...(Us) == sizeof...(Ts))
    void operator&(const std::tuple<Us...> &item_undet) {
        pushItem(narrowItem(item_undet), std::make_index_sequence<n_axes>{});
    }

    /**
	  * For a single-axis collector only: adds a bare value of undetermined type,
	  * provided it can be converted safely to the target component type.
	  *
	  * @tparam U The source type of the data item, narrowed to the target type
	  * @param x_undet The data item to be added to the collection
	  */
    template <typename U>
        requires(n_axes == 1)
    void operator&(const U &x_undet) {
        axis_t<0> x = axis_t<0>(0);

        // Make sure the data can be converted to doubles
        try {
            x = Gem::Common::narrow<axis_t<0>>(x_undet);
        }
        catch(std::overflow_error &e) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GDataCollectorT::operator&(const T&): Error!" << '\n'
                << "Encountered invalid cast with Gem::Common::narrow," << '\n'
                << "with the message " << '\n'
                << e.what() << '\n'
            );
        }

        // Add the converted data to our collection
        std::get<0>(columns_).push_back(x);
    }

    /**
	  * Adds a collection of exactly-typed data items in one go.
	  *
	  * @param cnt A vector of data items to be added to the collection
	  */
    void operator&(const std::vector<item_t> &cnt) {
        this->reserve(this->currentSize() + cnt.size());
        for(auto const &item : cnt) {
            // Add the data item to our collection
            pushItem(item, std::make_index_sequence<n_axes>{});
        }
    }

    /**
	  * Adds a contiguous range of exactly-typed data items in one go. This span
	  * overload sits alongside the std::vector overload above and lets callers
	  * pass arrays / sub-ranges without first materializing a std::vector.
	  *
	  * @param cnt A span of data items to be added to the collection
	  */
    void operator&(std::span<const item_t> cnt) {
        this->reserve(this->currentSize() + cnt.size());
        for(auto const &item : cnt) {
            // Add the data item to our collection
            pushItem(item, std::make_index_sequence<n_axes>{});
        }
    }

    /**
	  * Adds a collection of data items of undetermined component type(s) in one
	  * go, provided they can be converted safely into the target component type(s).
	  *
	  * @tparam Us The source component type(s), narrowed to the target type(s)
	  * @param cnt_undet A vector of data items of undetermined type, to be added
	  */
    template <typename... Us>
        requires(sizeof...(Us) == sizeof...(Ts))
    void operator&(const std::vector<std::tuple<Us...>> &cnt_undet) {
        this->reserve(this->currentSize() + cnt_undet.size());
        for(auto const &item_undet : cnt_undet) {
            pushItem(narrowItem(item_undet), std::make_index_sequence<n_axes>{});
        }
    }

    /**
	  * Adds a contiguous range of data items of undetermined component type(s) in
	  * one go (span counterpart of the std::vector overload above), provided they
	  * can be converted safely into the target component type(s).
	  *
	  * @tparam Us The source component type(s), narrowed to the target type(s)
	  * @param cnt_undet A span of data items of undetermined type, to be added
	  */
    template <typename... Us>
        requires(sizeof...(Us) == sizeof...(Ts))
    void operator&(std::span<const std::tuple<Us...>> cnt_undet) {
        this->reserve(this->currentSize() + cnt_undet.size());
        for(auto const &item_undet : cnt_undet) {
            pushItem(narrowItem(item_undet), std::make_index_sequence<n_axes>{});
        }
    }

    /**
	  * For a single-axis collector only: adds a collection of bare values of
	  * undetermined type, provided they can be converted safely to the target type.
	  *
	  * @tparam U The source element type of the vector, narrowed to the target type
	  * @param x_cnt_undet A collection of data items of undetermined type, to be added
	  */
    template <typename U>
        requires(n_axes == 1)
    void operator&(const std::vector<U> &x_cnt_undet) {
        axis_t<0> x = axis_t<0>(0);

        std::get<0>(columns_).reserve(std::get<0>(columns_).size() + x_cnt_undet.size());
        for(auto const &src : x_cnt_undet) {
            // Make sure the data can be converted to doubles
            try {
                x = Gem::Common::narrow<axis_t<0>>(src);
            }
            catch(std::overflow_error &e) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GDataCollectorT::operator&(const std::vector<T>&): Error!" << '\n'
                    << "Encountered invalid cast with Gem::Common::narrow," << '\n'
                    << "with the message " << '\n'
                    << e.what() << '\n'
                );
            }

            // Add the converted data to our collection
            std::get<0>(columns_).push_back(x);
        }
    }

    /**
	  * For a single-axis collector only: adds a contiguous range of bare values of
	  * undetermined type (span counterpart of the std::vector overload above),
	  * provided they can be converted safely to the target type.
	  *
	  * @tparam U The source element type of the span, narrowed to the target type
	  * @param x_cnt_undet A span of data items of undetermined type, to be added
	  */
    template <typename U>
        requires(n_axes == 1)
    void operator&(std::span<const U> x_cnt_undet) {
        axis_t<0> x = axis_t<0>(0);

        std::get<0>(columns_).reserve(std::get<0>(columns_).size() + x_cnt_undet.size());
        for(auto const &src : x_cnt_undet) {
            // Make sure the data can be converted to doubles
            try {
                x = Gem::Common::narrow<axis_t<0>>(src);
            }
            catch(std::overflow_error &e) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GDataCollectorT::operator&(std::span<const T>): Error!" << '\n'
                    << "Encountered invalid cast with Gem::Common::narrow," << '\n'
                    << "with the message " << '\n'
                    << e.what() << '\n'
                );
            }

            // Add the converted data to our collection
            std::get<0>(columns_).push_back(x);
        }
    }

    /**
	  * Adds a collection of exactly-typed data items in one go, stealing the
	  * incoming buffer for a single-axis collector (where the buffer matches the
	  * sole column directly). For multi-axis collectors there is no single column
	  * to move into, so the items are appended element-wise.
	  *
	  * @param cnt A vector of data items to be moved into the collection
	  */
    void operator&(std::vector<item_t> &&cnt) {
        if constexpr(n_axes == 1) {
            auto &col = std::get<0>(columns_);
            if(col.empty()) {
                // Steal the incoming buffer wholesale (item_t is a 1-tuple of axis_t<0>;
                // it is layout-equivalent to its sole element, but we move element-wise
                // to stay strictly within well-defined behavior).
                col.reserve(cnt.size());
                for(auto &item : cnt) {
                    col.push_back(std::move(std::get<0>(item)));
                }
            }
            else {
                col.reserve(col.size() + cnt.size());
                for(auto &item : cnt) {
                    col.push_back(std::move(std::get<0>(item)));
                }
            }
        }
        else {
            this->reserve(this->currentSize() + cnt.size());
            for(auto &item : cnt) {
                pushItem(std::move(item), std::make_index_sequence<n_axes>{});
            }
        }
    }

    /***************************************************************************/
    /**
	  * Projects the data onto axis I, returning a 1-d histogram of that axis'es
	  * values. Only available for all-double collectors (any other element type
	  * is rejected at compile time). This one generic implementation replaces
	  * the nine per-arity/per-axis specializations that used to be copy-pasted
	  * across the histogram / graph headers.
	  *
	  * In case of a default-constructed range, suitable histogram boundaries are
	  * derived from the minimum and maximum values of the projected data.
	  *
	  * The hist_type parameter exists purely to defer the use of GHistogram1D
	  * (only forward-declared here; complete at every call site, which includes
	  * the histogram header) to instantiation time -- do not pass it explicitly.
	  *
	  * @tparam I The axis to project onto
	  * @param nBins The desired number of bins of the resulting histogram
	  * @param range The lower and upper boundary of the resulting histogram
	  * @return A 1-d histogram of the data projected onto axis I
	  */
    template <std::size_t I, typename hist_type = GHistogram1D>
    [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] std::shared_ptr<GDataCollectorT<axis_t<I>>>
    project(std::size_t nBins, std::tuple<axis_t<I>, axis_t<I>> range) const {
        static_assert(
            I < n_axes,
            "GDataCollectorT<>::project<I>(): axis index I exceeds this collector's arity"
        );
        static_assert(
            (std::is_same_v<Ts, double> && ...),
            "GDataCollectorT<>::project<I>() is only implemented for all-double collectors"
        );

        // Derive the histogram boundaries from the data unless a range was given
        std::tuple<double, double> my_range = range;
        if(range == std::tuple<axis_t<I>, axis_t<I>>{}) {
            const auto extremes = Gem::Common::getMinMax(this->asTuples());
            my_range = std::tuple<double, double>(
                std::get<2 * I>(extremes), std::get<2 * I + 1>(extremes)
            );
        }

        // Construct the result object
        std::shared_ptr<hist_type> result(new hist_type(nBins, my_range));
        if constexpr(I == 0) {
            result->setXAxisLabel(this->xAxisLabel());
        } else if constexpr(I == 1) {
            result->setXAxisLabel(this->yAxisLabel());
        } else if constexpr(I == 2) {
            result->setXAxisLabel(this->zAxisLabel());
        } else {
            result->setXAxisLabel("w"); // GBasePlotter labels only three axes
        }
        result->setYAxisLabel("Number of entries");
        constexpr char axis_letters[] = "xyzw";
        result->setPlotLabel(this->plotLabel() + " / " + axis_letters[I] + "-projection");

        // Add data to the object
        for(auto const &v : this->template column<I>()) {
            (*result) & v;
        }

        return result;
    }

    /***************************************************************************/
    // Named projection wrappers, preserved from the original per-arity API. They
    // are only available for the axes actually present in this collector.

    // The named wrappers are templated on their (fixed-by-default) axis index so
    // that the axis_t<I> appearing in their signature is a dependent type, only
    // instantiated when the wrapper is actually called -- otherwise a low-arity
    // collector (e.g. the single-axis histogram base) would hard-error merely by
    // forming projectY/Z/W's parameter type. The requires-clause then keeps each
    // wrapper available only for collectors that actually have the requested axis.

    /** @brief Projects the data onto the x-axis (see project<0>()) */
    template <std::size_t I = 0>
        requires((I == 0) && (n_axes >= 1))
    [[nodiscard]] auto projectX(std::size_t nBins, std::tuple<axis_t<I>, axis_t<I>> range) const {
        return this->template project<I>(nBins, range);
    }

    /** @brief Projects the data onto the y-axis (see project<1>()) */
    template <std::size_t I = 1>
        requires((I == 1) && (n_axes >= 2))
    [[nodiscard]] auto projectY(std::size_t nBins, std::tuple<axis_t<I>, axis_t<I>> range) const {
        return this->template project<I>(nBins, range);
    }

    /** @brief Projects the data onto the z-axis (see project<2>()) */
    template <std::size_t I = 2>
        requires((I == 2) && (n_axes >= 3))
    [[nodiscard]] auto projectZ(std::size_t nBins, std::tuple<axis_t<I>, axis_t<I>> range) const {
        return this->template project<I>(nBins, range);
    }

    /** @brief Projects the data onto the w-axis (see project<3>()) */
    template <std::size_t I = 3>
        requires((I == 3) && (n_axes >= 4))
    [[nodiscard]] auto projectW(std::size_t nBins, std::tuple<axis_t<I>, axis_t<I>> range) const {
        return this->template project<I>(nBins, range);
    }

    /***************************************************************************/
    /**
	  * Sorts the data according to its x-component (axis 0)
	  */
    void sortX() {
        const std::size_t n = this->currentSize();
        if(n < 2) {
            return;
        }

        // Reproduce the previous behaviour byte-for-byte: the old storage was a
        // std::vector<item_t> and was sorted with std::sort comparing only the
        // x-component (axis 0). std::sort is not stable, so the exact order of
        // x-ties is an artefact of the algorithm operating on that very element
        // sequence. We therefore materialize the item tuples, run the identical
        // sort, and scatter the result back into the columns -- this co-sorts all
        // axes (a row stays together) and matches the old ordering exactly.
        std::vector<item_t> items = asTuples();
        std::ranges::sort(items, [](const item_t &a, const item_t &b) -> bool {
            return std::get<0>(a) < std::get<0>(b);
        });
        assignFromTuples(items, std::make_index_sequence<n_axes>{});
    }

    /***************************************************************************/
    /**
	  * Retrieves the per-axis minimum and maximum values in data_, interleaved as
	  * (min_0, max_0, min_1, max_1, ...). For a single-axis collector this is the
	  * familiar (min, max); for a two-axis collector it is (min_x, max_x, min_y,
	  * max_y), matching the original per-arity return type.
	  *
	  * @return A tuple holding the per-axis (min, max) pairs of the stored data
	  */
    [[nodiscard]] auto getMinMaxElements() const {
        if(this->currentSize() == 0) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GDataCollectorT::getMinMaxElements(): Error!" << '\n'
                << "Cannot determine the data range of an empty collector." << '\n'
            );
        }
        return minMaxImpl(std::make_index_sequence<n_axes>{});
    };

    /***************************************************************************/
    /**
	  * Const, read-only access to the per-axis value vector (the column) for axis
	  * I. This exposes the columnar data to out-of-hierarchy consumers (the
	  * pluggable plot emitters) without granting any mutation path; the writable
	  * column() overload remains protected / internal to the collector hierarchy.
	  *
	  * @tparam I The axis whose value vector should be returned
	  * @return A const reference to the std::vector holding axis I's values
	  */
    template <std::size_t I>
    [[nodiscard]] [[nodiscard]] [[nodiscard]] [[nodiscard]] const std::vector<axis_t<I>> &column() const {
        return std::get<I>(columns_);
    }

    /***************************************************************************/
    /**
	  * Reports this collector's columns generically, as a storage-order list of
	  * type-tagged (float64 / int32) read-only columns, so the plot backends can read
	  * the data without knowing the concrete plotter type (they switch on
	  * plotSpec().kind instead). A collector whose every axis is double or int32 exports
	  * its columns with their true dtype; a collector with any other axis type reports
	  * nothing, exactly as the base default does -- such data is ROOT-only.
	  *
	  * @return Type-tagged views of every column, in storage order (empty if an axis is
	  *         neither double nor int32)
	  */
    [[nodiscard]] std::vector<GPlotColumn> dataColumns() const override {
        if constexpr (((std::is_same_v<Ts, double> || std::is_same_v<Ts, std::int32_t>) && ...)) {
            return dataColumnsImpl_(std::make_index_sequence<n_axes>{});
        } else {
            return {};
        }
    }

    /***************************************************************************/
    /**
	  * Appends one data row (one value per axis, in column order) generically -- the
	  * inverse of dataColumns(), used by GDataLog to fill a plotter built from a
	  * GPlotSpec without knowing its concrete type. Any collector whose axes are double
	  * or int32 accepts rows this way (the same set dataColumns() exposes); an int32 axis
	  * checked-narrows its value (range-checked, fraction truncated -- the same semantics
	  * as the tuple insertion operators), so e.g. the integer histogram (hist_1i) is
	  * fillable through GDataLog rather than throwing at realize time. A row of the wrong
	  * width, an out-of-range value for an int32 axis, and any other axis type are rejected.
	  *
	  * @param row One value per axis, in column order (size must equal the axis count)
	  */
    void appendRow(std::span<const double> row) override {
        if constexpr (((std::is_same_v<Ts, double> || std::is_same_v<Ts, std::int32_t>) && ...)) {
            if(row.size() != n_axes) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GDataCollectorT::appendRow(): Error!" << '\n'
                    << "row size " << row.size() << " != axis count " << n_axes << '\n'
                );
            }
            appendRowImpl_(row, std::make_index_sequence<n_axes>{});
        } else {
            GBasePlotter::appendRow(row); // unsupported axis type: rejected with a clear message
        }
    }

    /***************************************************************************/
    /**
	  * Sorts the data rows by the first column (axis 0), generically -- forwards to the
	  * collector's existing sortX(), so GDataLog can reproduce GGraph2D::sortX() without
	  * the concrete type.
	  */
    void sortByFirstColumn() override {
        this->sortX();
    }

protected:
    /***************************************************************************/
    /**
	  * Single declaration of this class'es local data members, used by load_() and compare_()
	  *
	  * @return A tuple of named members (the data vector) of this object
	  */
    template <typename Self>
    auto localMembers_(this Self &self) {
        // Expose one named member per column, so each column is compared via the
        // existing sequence-container comparison path (exactly as the former single
        // data_ vector was). The columns are the serialized / compared state.
        return localMembersImpl_(self, std::make_index_sequence<n_axes>{});
    }

    /** @brief Stable per-axis member name ("column_0", "column_1", ...) for diagnostics */
    template <std::size_t I>
    static constexpr const char *columnName_() {
        constexpr const char *names[] = {
            "column_0", "column_1", "column_2", "column_3",
            "column_4", "column_5", "column_6", "column_7"
        };
        static_assert(I < (sizeof(names) / sizeof(names[0])), "GDataCollectorT: too many axes for column naming");
        return names[I];
    }

    template <typename Self, std::size_t... Is>
    static auto localMembersImpl_(Self &self, std::index_sequence<Is...>) {
        return std::make_tuple(make_member(columnName_<Is>(), std::get<Is>(self.columns_))...);
    }

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is abstract)
    // from class_name and the localMembers_() declaration above.

    /***************************************************************************/
    /**
	  * Returns the column (the per-axis value vector) for axis I, for use by the
	  * concrete plotters' emission code. Both const and non-const overloads are
	  * provided; they are protected, i.e. internal to the collector hierarchy.
	  *
	  * @tparam I The axis whose value vector should be returned
	  * @return A reference to the std::vector holding axis I's values
	  */
    template <std::size_t I>
    std::vector<axis_t<I>> &column() {
        return std::get<I>(columns_);
    }

    /***************************************************************************/

    columns_t columns_; ///< Holds the actual data in columnar (struct-of-arrays) form

private:
    /***************************************************************************/
    /**
	  * Collects type-tagged views of every column, in storage order, for dataColumns().
	  * Instantiated only when every axis is double or int32 (guarded by the caller's
	  * if constexpr), so each &column pointer is a valid GPlotColumn alternative.
	  */
    template <std::size_t... Is>
    [[nodiscard]] std::vector<GPlotColumn> dataColumnsImpl_(std::index_sequence<Is...>) const {
        return {GPlotColumn{&std::get<Is>(columns_)}...};
    }

    /***************************************************************************/
    /**
	  * Pushes row[I] into column I for every axis, checked-narrowing to the axis type
	  * (identity for a double axis; range-and-integrality-checked for an int32 axis).
	  */
    template <std::size_t... Is>
    void appendRowImpl_(std::span<const double> row, std::index_sequence<Is...>) {
        try {
            (std::get<Is>(columns_).push_back(Gem::Common::narrow<axis_t<Is>>(row[Is])), ...);
        }
        catch(std::overflow_error &e) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GDataCollectorT::appendRow(): Error!" << '\n'
                << "Encountered invalid cast with Gem::Common::narrow," << '\n'
                << "with the message " << '\n'
                << e.what() << '\n'
            );
        }
    }

    /***************************************************************************/
    /**
	  * Appends one logical item (one value per axis) to the columns.
	  */
    template <typename Tuple, std::size_t... Is>
    void pushItem(Tuple &&item, std::index_sequence<Is...>) {
        (std::get<Is>(columns_).push_back(std::get<Is>(std::forward<Tuple>(item))), ...);
    }

    /***************************************************************************/
    /**
	  * Reserves capacity n in every column.
	  */
    template <std::size_t... Is>
    void reserveImpl(std::size_t n, std::index_sequence<Is...>) {
        (std::get<Is>(columns_).reserve(n), ...);
    }

    /***************************************************************************/
    /**
	  * Materializes the columnar data into a vector of item tuples (an
	  * array-of-tuples view). Used by the few legacy call sites (project<>() and
	  * the GGraph4D footer) that drive the free getMinMax(vector-of-tuples) helper
	  * and by sortX().
	  *
	  * @return A vector of item tuples reconstructed from the columns
	  */
    [[nodiscard]] std::vector<item_t> asTuples() const {
        return asTuplesImpl(std::make_index_sequence<n_axes>{});
    }

    template <std::size_t... Is>
    [[nodiscard]] std::vector<item_t> asTuplesImpl(std::index_sequence<Is...>) const {
        const std::size_t n = this->currentSize();
        std::vector<item_t> out;
        out.reserve(n);
        for(std::size_t i = 0; i < n; ++i) {
            out.emplace_back(std::get<Is>(columns_)[i]...);
        }
        return out;
    }

    /***************************************************************************/
    /**
	  * Replaces the columns with the contents of a vector of item tuples,
	  * de-interleaving each axis into its own column.
	  */
    template <std::size_t... Is>
    void assignFromTuples(const std::vector<item_t> &items, std::index_sequence<Is...>) {
        (std::get<Is>(columns_).clear(), ...);
        (std::get<Is>(columns_).reserve(items.size()), ...);
        for(const auto &item : items) {
            (std::get<Is>(columns_).push_back(std::get<Is>(item)), ...);
        }
    }

    /***************************************************************************/
    /**
	  * Narrows a tuple of undetermined component types into the target item type,
	  * wrapping every per-component narrow<> in the original try/catch contract.
	  */
    template <typename... Us>
    item_t narrowItem(const std::tuple<Us...> &src) const {
        return narrowItemImpl(src, std::make_index_sequence<n_axes>{});
    }

    template <typename Src, std::size_t... Is>
    item_t narrowItemImpl(const Src &src, std::index_sequence<Is...>) const {
        try {
            return item_t(Gem::Common::narrow<axis_t<Is>>(std::get<Is>(src))...);
        }
        catch(std::overflow_error &e) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GDataCollectorT::operator&(tuple): Error!" << '\n'
                << "Encountered invalid cast with Gem::Common::narrow," << '\n'
                << "with the message " << '\n'
                << e.what() << '\n'
            );
        }
    }

    /***************************************************************************/
    /**
	  * Computes the per-axis (min, max) of a single axis I across the data vector.
	  */
    template <std::size_t I>
    [[nodiscard]] [[nodiscard]] std::pair<axis_t<I>, axis_t<I>> minMaxAxis() const {
        const auto &col = std::get<I>(columns_);
        auto [min_it, max_it] = std::ranges::minmax_element(col);
        return {*min_it, *max_it};
    }

    template <std::size_t... Is>
    [[nodiscard]] [[nodiscard]] auto minMaxImpl(std::index_sequence<Is...>) const {
        // Interleave the per-axis (min, max) pairs into a single flat tuple
        return std::tuple_cat(
            std::make_tuple(minMaxAxis<Is>().first, minMaxAxis<Is>().second)...
        );
    }

    /***************************************************************************/
};

/******************************************************************************/
/**
 * The five named data collectors are thin aliases over the single variadic
 * GDataCollectorT. They preserve the historical names, arities and the exact
 * stored layout (std::tuple<std::vector<Ts>...>) the concrete plotters rely on.
 *
 * - GDataCollector1T<X>        : 1-d data (e.g. for histograms)
 * - GDataCollector2T<X,Y>      : 2-d data (e.g. for a TGraph)
 * - GDataCollector2ET<X,Y>     : 2-d data with x- and y-errors (x, error_x, y, error_y)
 * - GDataCollector3T<X,Y,Z>    : 3-d data
 * - GDataCollector4T<X,Y,Z,W>  : 4-d data
 */
template <typename x_type>
using GDataCollector1T = GDataCollectorT<x_type>;

template <typename x_type, typename y_type>
using GDataCollector2T = GDataCollectorT<x_type, y_type>;

template <typename x_type, typename y_type, typename z_type>
using GDataCollector3T = GDataCollectorT<x_type, y_type, z_type>;

template <typename x_type, typename y_type, typename z_type, typename w_type>
using GDataCollector4T = GDataCollectorT<x_type, y_type, z_type, w_type>;

/******************************************************************************/
/**
 * The 2-d data collector with error bars stores a 4-tuple (x, error_x, y,
 * error_y). Its tuple layout (X, X, Y, Y) would otherwise be indistinguishable
 * from a four-axis GDataCollector4T<X, X, Y, Y>, so it is realized as a distinct,
 * trivial subclass of the variadic base rather than as a bare alias. This keeps
 * its identity (and its is_abstract<> abstract-type marker) separate while
 * inheriting all of the shared collector machinery unchanged.
 *
 * @tparam x_type The numeric type of the x-component and its error
 * @tparam y_type The numeric type of the y-component and its error
 */
template <typename x_type, typename y_type>
class GDataCollector2ET
  : public Gem::Common::GReflectiveInterfaceBaseT<
        GDataCollector2ET<x_type, y_type>,
        GDataCollectorT<x_type, x_type, y_type, y_type>
    > {
    ///////////////////////////////////////////////////////////////////////
    // GReflectiveInterfaceAccess lets the GReflectiveInterfaceBaseT base reach this class's
    // (empty) localMembers_(); this abstract collector is never reconstructed on load.
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
	  * @brief This class adds no own members; the *explicit* empty declaration is
	  * required (a missing one would inherit the parent's and serialize its
	  * columns a second time -- rejected at compile time by GReflectiveInterfaceBaseT).
	  * @return An empty member tuple
	  */
    template <typename Self>
    auto localMembers_(this Self &) {
        return std::make_tuple();
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDataCollector2ET<x_type, y_type>";

    /***************************************************************************/
    // Defaulted constructors and destructors -- all machinery is inherited

    GDataCollector2ET() = default;
    GDataCollector2ET(GDataCollector2ET<x_type, y_type> const &) = default;
    GDataCollector2ET(GDataCollector2ET<x_type, y_type> &&) = default;

    ~GDataCollector2ET() override = default;

    GDataCollector2ET<x_type, y_type> &operator=(GDataCollector2ET<x_type, y_type> const &) = default;
    GDataCollector2ET<x_type, y_type> &operator=(GDataCollector2ET<x_type, y_type> &&) = default;

    // load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceBaseT base (clone_ stays pure -- this is abstract)
    // from class_name and the empty localMembers_() declaration above.
};


} /* namespace Gem::Dietrich */
