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
#include <string>

// Boost headers go here

// Geneva headers go here
#include "common/GExpectationChecksT.hpp"
#include "geneva/GPersonalityTraits.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The personality traits of the loadable random-search algorithm.
 *
 * A minimal GPersonalityTraits subclass: it carries no per-individual state, only the algorithm mnemonic
 * ("rsearch") by which Go2 resolves the algorithm. Its @c nickname is what self-registers / is looked up in
 * the algorithm store, so a loaded @c GRandomSearch is selectable as @c --optimizationAlgorithms rsearch.
 */
class GRandomSearch_PersonalityTraits // NOLINT(cppcoreguidelines-special-member-functions)
  : public GPersonalityTraits {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /** @brief Serializes this object (no local data beyond the base). Archive-generic: the same body
     *  serves a Boost archive or a GArchive codec (the backend branch lives inside archive_named_base).
     *  @tparam Archive The archive type used for (de-)serialization
     *  @param ar The archive to serialize to / from
     *  @param version The (unused) serialization format version */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GPersonalityTraits>(ar, "GPersonalityTraits", *this);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief An easy identifier for the class (the algorithm mnemonic). Initialized in the .cpp. */
    static const std::string nickname;

    /** @brief The default constructor */
    GRandomSearch_PersonalityTraits() = default;
    /** @brief The copy constructor
     *  @param cp Another GRandomSearch_PersonalityTraits object to be copied */
    GRandomSearch_PersonalityTraits(const GRandomSearch_PersonalityTraits &cp) = default;
    /** @brief The standard destructor */
    ~GRandomSearch_PersonalityTraits() override = default;

    /** @brief Retrieves the mnemonic of the optimization algorithm.
     *  @return The short mnemonic identifying the random-search algorithm ("rsearch") */
    std::string getMnemonic() const override;

protected:
    /** @brief Loads the data of another GRandomSearch_PersonalityTraits object.
     *  @param cp A pointer to another object of this type, camouflaged as a GPersonalityTraits */
    void load_(const GPersonalityTraits *cp) override;

    /** @brief Allow access to this class'es compare_ function */
    friend void Gem::Common::compare_base_t<GRandomSearch_PersonalityTraits>(
        GRandomSearch_PersonalityTraits const &, GRandomSearch_PersonalityTraits const &, Gem::Common::GToken &);

    /** @brief Searches for compliance with expectations with respect to another object of the same type.
     *  @param cp The other object to be compared against (as a GPersonalityTraits)
     *  @param e The expectation for this object, e.g. equality
     *  @param limit The limit for allowed deviations of floating point types */
    void compare_(
        const GPersonalityTraits &cp,
        const Gem::Common::expectation &e,
        const double &limit
    ) const override;

private:
    /** @brief Emits a name for this class / object.
     *  @return The class name of this object */
    std::string name_() const override;
    /** @brief Creates a deep clone of this object.
     *  @return A pointer to a freshly allocated deep copy of this object */
    GPersonalityTraits *clone_() const override;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

