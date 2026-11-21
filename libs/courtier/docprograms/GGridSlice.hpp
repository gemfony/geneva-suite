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

/**
 * @file
 * The item type of grid_minimum: a slice of a grid over an interval, whose processing finds the
 * smallest value of a function on the slice's points. It is all the program and its consumer
 * modules share: a courtier item type knows nothing of the consumer that carries it. What its
 * processing finds travels back as its results (registerResult()): a client returns those, not
 * the whole item.
 */

// Standard header files go here
#include <cstddef>
#include <vector>

// Geneva header files go here
#include "common/GArchiveNamed.hpp" // archive_named, archive_named_base
#include "courtier/GProcessingContainerT.hpp"

/******************************************************************************/
/** @brief A slice of the grid: n points from lo to hi; processed, the smallest value on them and
 *  its point (results 0 and 1) */
class GGridSlice final : public Gem::Courtier::GProcessingContainerT<GGridSlice, double> {
    friend struct Gem::Weft::access;

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<Gem::Courtier::GProcessingContainerT<GGridSlice, double>>(
            ar,
            "GProcessingContainerT_GGridSlice",
            *this
        );
        Gem::Common::archive_named(ar, "lo_", lo_);
        Gem::Common::archive_named(ar, "hi_", hi_);
        Gem::Common::archive_named(ar, "n_", n_);
    }

public:
    /** @brief The function whose smallest value is searched for @param x A point @return f(x) */
    static double f(double x) {
        return (x - 1.25) * (x - 1.25) + 0.5;
    }

    /** @brief A slice of @p n points from @p lo to @p hi */
    GGridSlice(double lo, double hi, std::size_t n)
      : Gem::Courtier::GProcessingContainerT<GGridSlice, double>(2)
      , lo_(lo)
      , hi_(hi)
      , n_(n) {
    }
    GGridSlice(const GGridSlice &) = default;
    GGridSlice &operator=(const GGridSlice &) = default;
    GGridSlice(GGridSlice &&) noexcept = default;
    GGridSlice &operator=(GGridSlice &&) noexcept = default;
    ~GGridSlice() override = default;

    /** @brief The smallest value, once processed @return It */
    [[nodiscard]] double fMin() const {
        return getStoredResult(0);
    }
    /** @brief The point with the smallest value, once processed @return It */
    [[nodiscard]] double xMin() const {
        return getStoredResult(1);
    }

private:
    GGridSlice() = default; ///< For deserialization

    /** @brief Evaluates f on the slice's points and keeps the smallest value and its point */
    void process_(const std::vector<double> & /*res_vec*/ = std::vector<double>()) override {
        double x_min = lo_;
        double f_min = f(lo_);
        for(std::size_t i = 1; i < n_; ++i) {
            const double x =
                lo_ + (hi_ - lo_) * static_cast<double>(i) / static_cast<double>(n_ - 1);
            if(const double fx = f(x); fx < f_min) {
                f_min = fx;
                x_min = x;
            }
        }
        registerResult(0, f_min);
        registerResult(1, x_min);
    }

    double lo_ = 0.;    ///< The first point
    double hi_ = 0.;    ///< The last point
    std::size_t n_ = 2; ///< The number of points
};
