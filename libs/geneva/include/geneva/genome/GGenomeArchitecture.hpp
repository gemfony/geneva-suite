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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <cstddef>
#include <string>
#include <vector>

// Geneva headers go here
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The semantic architecture view of an individual (DM §4). An architecture is a problem-specific,
 * *layout-agnostic* mapping from the individual's flat parameter values to the external structure a
 * problem cares about -- a 2D field, the layers of a neural network, etc. It reads the individual
 * exclusively through the genome-agnostic DM §2 channel accessors (streamline / streamlineFP), so it
 * never needs to know the underlying genome's storage layout: the very point of the seam is that
 * executors / clients / problem code consume the architecture without learning the concrete genome
 * implementation.
 *
 * A concrete individual exports its architecture (when it has structure -- a plain paraboloid needs
 * none) via a `static makeArchitecture()` factory, so the structure travels with the problem
 * definition rather than being hard-wired into any algorithm.
 *
 * This is the abstract base; GGridArchitecture below is a worked example.
 */
class GGenomeArchitecture {
public:
    /** @brief The (virtual) destructor */
    virtual ~GGenomeArchitecture() = default;

    /** @brief A human-readable name for this architecture.
     *  @return The architecture's name */
    [[nodiscard]] virtual std::string name() const = 0;

    /** @brief The number of floating-point genome values this architecture expects.
     *  @return The expected count of floating-point genome values */
    [[nodiscard]] virtual std::size_t expectedFPSize() const = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A worked example architecture: it views the floating-point genome as a rows x cols 2D field. The
 * same pattern extends to layered 1D arrays (a neural network: one row per layer) and plain 1D arrays
 * (rows == 1). It demonstrates the seam: every accessor takes a const GGenome& and reads
 * it via streamlineFP(), so it works unchanged over a tree genome and a flat genome alike.
 */
class GGridArchitecture : public GGenomeArchitecture {
public:
    /** @brief Initialization with the field dimensions.
     *  @param rows The number of rows in the 2D field
     *  @param cols The number of columns in the 2D field */
    GGridArchitecture(std::size_t rows, std::size_t cols)
      : rows_(rows)
      , cols_(cols) {
        /* nothing */
    }

    /** @brief @return The name of this architecture ("GGridArchitecture") */
    [[nodiscard]] std::string name() const override { return "GGridArchitecture"; }
    /** @brief @return The number of floating-point genome values expected (rows * cols) */
    [[nodiscard]] std::size_t expectedFPSize() const override { return rows_ * cols_; }

    /** @brief @return The number of rows in the field */
    [[nodiscard]] std::size_t rows() const { return rows_; }
    /** @brief @return The number of columns in the field */
    [[nodiscard]] std::size_t cols() const { return cols_; }

    /** @brief Extracts one row of the field from the individual's floating-point view.
     *  @param ind The individual to read the floating-point genome values from
     *  @param r The (zero-based) index of the row to extract
     *  @return A vector holding the cols_ values of row r */
    [[nodiscard]] std::vector<double> row(GGenome const &ind, std::size_t r) const {
        std::vector<double> all;
        ind.streamlineFP(all);
        checkSize(all.size());
        std::vector<double> out;
        out.reserve(cols_);
        for(std::size_t c = 0; c < cols_; ++c) {
            out.push_back(all[(r * cols_) + c]);
        }
        return out;
    }

    /** @brief Extracts a single field element (row r, column c).
     *  @param ind The individual to read the floating-point genome values from
     *  @param r The (zero-based) row index of the element
     *  @param c The (zero-based) column index of the element
     *  @return The floating-point value at field position (r, c) */
    [[nodiscard]] double at(GGenome const &ind, std::size_t r, std::size_t c) const {
        std::vector<double> all;
        ind.streamlineFP(all);
        checkSize(all.size());
        return all[(r * cols_) + c];
    }

private:
    /** @brief Verifies the individual supplies enough FP values for the field, throwing otherwise.
     *  @param n The number of floating-point values the individual provided */
    void checkSize(std::size_t n) const {
        if(n < rows_ * cols_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGridArchitecture: Error!" << '\n'
                << "Individual has " << n << " FP values, but the " << rows_ << "x" << cols_
                << " grid needs " << (rows_ * cols_) << '\n'
            );
        }
    }

    std::size_t rows_;
    std::size_t cols_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
