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

#include "GRandomSearch_PersonalityTraits.hpp"

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp" // Gem::Common::condnotset
#include "common/GExpectationChecksT.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/** @brief The mnemonic by which Go2 resolves this algorithm on the command line. */
const std::string GRandomSearch_PersonalityTraits::nickname = "rsearch"; // NOLINT

/******************************************************************************/
/**
 * @brief Retrieves the mnemonic of the optimization algorithm.
 *
 * @return The short identifier of the random-search personality ("rsearch")
 */
std::string GRandomSearch_PersonalityTraits::getMnemonic() const {
    return GRandomSearch_PersonalityTraits::nickname;
}

/******************************************************************************/
/**
 * @brief Emits a name for this class / object.
 *
 * @return The string "GRandomSearch_PersonalityTraits"
 */
std::string GRandomSearch_PersonalityTraits::name_() const {
    return std::string("GRandomSearch_PersonalityTraits");
}

/******************************************************************************/
/**
 * @brief Creates a deep clone of this object.
 *
 * @return A newly allocated clone of this object, camouflaged as a GPersonalityTraits (caller owns it)
 */
GPersonalityTraits *GRandomSearch_PersonalityTraits::clone_() const {
    return new GRandomSearch_PersonalityTraits(*this);
}

/******************************************************************************/
/**
 * @brief Loads the data of another GRandomSearch_PersonalityTraits object.
 *
 * @param cp A pointer to another object of this type, camouflaged as a GPersonalityTraits
 */
void GRandomSearch_PersonalityTraits::load_(const GPersonalityTraits *cp) {
    // Check that we are dealing with a GRandomSearch_PersonalityTraits reference independent of this object.
    const GRandomSearch_PersonalityTraits *p_load =
        Gem::Common::g_convert_and_compare<GPersonalityTraits, GRandomSearch_PersonalityTraits>(cp, this);

    // Load the parent class'es data (this personality carries no local data of its own).
    GPersonalityTraits::load_(cp);
    (void)p_load;
}

/******************************************************************************/
/**
 * @brief Searches for compliance with expectations with respect to another object of the same type.
 *
 * @param cp The other object to be compared against (as a GPersonalityTraits)
 * @param e The expectation for this object, e.g. equality
 * @param limit The limit for allowed deviations of floating point types (unused: no local data)
 */
void GRandomSearch_PersonalityTraits::compare_(
    const GPersonalityTraits &cp,
    const Gem::Common::expectation &e,
    [[maybe_unused]] const double &limit
) const {
    using namespace Gem::Common;

    const GRandomSearch_PersonalityTraits *p_load =
        Gem::Common::g_convert_and_compare<GPersonalityTraits, GRandomSearch_PersonalityTraits>(cp, this);

    GToken token("GRandomSearch_PersonalityTraits", e);

    // Compare our parent data (this personality adds no local data of its own).
    Gem::Common::compare_base_t<GPersonalityTraits>(*this, *p_load, token);

    token.evaluate();
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
