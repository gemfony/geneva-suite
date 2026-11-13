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

// Standard headers go here

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

// Hap headers go here
#include "hap/GHapEnums.hpp"
#include "hap/GRandomDefines.hpp"
#include "hap/GRandomFactory.hpp"

/******************************************************************************/

namespace Gem::Hap {

/******************************************************************************/
/**
 * This class defines ways of obtaining different random number distributions
 * from "raw" random numbers, which can be obtained in derived classes using
 * various different ways.
 */
class GRandomBase {
public:
    /** @brief Helps to use this object as a generator for C++11 std::distributions */
    using result_type = G_CPU_BASE_GENERATOR::result_type;

    /***************************************************************************/
    /** @brief The standard constructor */
    GRandomBase() = default;
    /** @brief A standard (virtual) destructor */
    virtual ~GRandomBase() = default;
    /**
     * @brief Retrieves a single "raw" uniformly distributed random item.
     *
     * Acts as the call operator required by the C++11 UniformRandomBitGenerator
     * interface, forwarding to the derived class' int_random() implementation.
     * Defined inline: this is the hot draw path of every distribution built on
     * a Geneva random proxy, so the former out-of-line forwarder hop is spared
     * (the virtual int_random() dispatch remains, by design).
     *
     * @return One raw random value drawn from the underlying generator
     */
    result_type operator()() {
        return this->int_random();
    }

    /***************************************************************************/
    // Prevent copying
    GRandomBase(const GRandomBase &) = delete;
    GRandomBase(const GRandomBase &&) = delete;
    GRandomBase &operator=(const GRandomBase &) = delete;
    GRandomBase &operator=(const GRandomBase &&) = delete;

    /***************************************************************************/
    /**
	  * This function is part of the standard interface of C++11 random number
	  * engines. It returns the minimum value returned by the generator. Since
	  * this class acts as a proxy for a wrapped generator or a generator running
	  * as a factory, we simply return the base generators min()-Value.
	  *
	  * @return The minimum value the underlying base generator can produce
	  */
    static constexpr result_type(min)() {
        return (G_CPU_BASE_GENERATOR::min)();
    }

    /***************************************************************************/
    /**
	  * This function is part of the standard interface of C++11 random number
	  * engines. It returns the maximum value returned by the generator. Since
	  * this class acts as a proxy for a wrapped generator or a generator running
	  * as a factory, we simply return the base generators max()-Value.
	  *
	  * @return The maximum value the underlying base generator can produce
	  */
    static constexpr result_type(max)() {
        return (G_CPU_BASE_GENERATOR::max)();
    }

private:
    /***************************************************************************/
    /**
     * @brief Produces a single uniformly distributed integer in the range [min(), max()].
     *
     * Pure virtual hook implemented by each derived flavour (proxy or local); it
     * is the sole source of raw randomness this base class draws upon.
     *
     * @return One raw uniformly distributed random value
     */
    virtual result_type int_random() = 0;
};

/******************************************************************************/

} /* namespace Gem::Hap */
