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

// Boost header files go here

// Geneva headers go here
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The sole CRTP base a concrete flat individual derives. It is a thin adaptor over the GReflectiveInterfaceT
 * mixin (with GGenome as the parent, so the generated clone_ returns the hierarchy root): clone_, name_,
 * load_, compare_ and serialize are all GENERATED, so a minimal flat individual is just a constructor that
 * builds its genome, an evaluate(), the opt-in flat tag, and one GEM_REGISTER_ARCHIVABLE(Derived).
 *
 * Because GGenome holds all genome state generically, a genome-only individual adds no members of its own.
 * It opts into GGenomeT's generated empty member list by declaring the marker
 * `using gemfony_flat_individual = void;`. An individual that genuinely carries extra (non-genome) members
 * instead declares its OWN localMembers_(), listing them. A leaf that does NEITHER is a compile error (see
 * localMembers_() below) -- so extra state can never be silently dropped from serialize/compare/load.
 *
 * Usage (genome-only):
 * @code
 *   class MyIndividual : public GGenomeT<MyIndividual> {
 *   public:
 *       using gemfony_flat_individual = void; // opt into the generated empty member list
 *       MyIndividual() {
 *           GGenomeBuilder b;
 *           b.addDouble(0., -10., 10.); // structure only; the adaptor lives on the OA-owned config
 *           this->setGenome(b.build());
 *       }
 *       std::vector<double> evaluate() override { ... }
 *   };
 * @endcode
 * plus, at file scope in the individual's own .cpp (never in a header: the macro's stringized argument
 * IS the wire/checkpoint tag, so it must be spelled fully qualified and emitted in exactly one
 * translation unit):
 * @code
 *   #include "weft/GArchivePolymorphic.hpp"
 *   GEM_REGISTER_ARCHIVABLE(Gem::Geneva::MyIndividual)
 * @endcode
 *
 * Usage (with extra state): declare localMembers_() listing the extra members (do NOT declare the marker),
 * e.g.
 * @code
 *   friend struct Gem::Common::GReflectiveInterfaceAccess;
 *   template <typename Self> auto localMembers_(this Self &self) {
 *       return std::make_tuple(Gem::Common::make_member("my_extra_", self.my_extra_));
 *   }
 * @endcode
 *
 * @tparam Derived The concrete flat individual type (CRTP), supplying its constructor and evaluate()
 */
template <class Derived>
class GGenomeT : public Gem::Common::GReflectiveInterfaceT<Derived, GGenome> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief A genome-only leaf carries no data beyond its genome, so it opts into this generated empty
     *  member list by declaring the public marker `using gemfony_flat_individual = void;` (which satisfies
     *  the requires-clause below). The GReflectiveInterfaceT mixin then generates clone_ / name_ / load_ / compare_ /
     *  serialize from it (serialize additionally emits base_object<GGenome>; clone_ returns GGenome*
     *  covariantly). A leaf that genuinely adds non-genome members declares its OWN localMembers_() instead
     *  (which hides this one) and MUST NOT declare the marker.
     *
     *  The requires-clause is the compile-time safety net: a stateful leaf that declares NEITHER the marker
     *  NOR its own localMembers_() finds no viable localMembers_() at all -- this one is constrained out and
     *  the GReflectiveInterfaceBaseT fallback is =deleted -- so GReflectiveInterfaceAccess::members() is ill-formed and the
     *  class fails to compile, rather than silently dropping that state from serialize/compare/load. The
     *  marker must be public: this base cannot see a derived-private nested type. Until C++26 member
     *  reflection can enumerate members automatically, this opt-in marker is the only compile-time guard. */
    template <typename Self>
        requires requires { typename Self::gemfony_flat_individual; }
    auto localMembers_(this Self &) { return std::make_tuple(); }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief Inherit the GGenome constructors (default + n-fitness-criteria). */
    using Gem::Common::GReflectiveInterfaceT<Derived, GGenome>::GReflectiveInterfaceT;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
