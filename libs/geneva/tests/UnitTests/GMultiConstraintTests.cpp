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

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "geneva/genome/GMultiConstraintT.hpp"
#include "geneva/genome/GIndividualMultiConstraint.hpp"
#include "geneva/genome/GGenomeMultiConstraint.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/genome/GGenome.hpp"

namespace gen = Gem::Geneva::Genome;

/******************************************************************************/
/**
 * Round-trips a GCheckCombinerT through serialization and checks that its
 * contained validity_checks_ (held on the GValidityCheckContainerT base) survive.
 *
 * This pins the fix for a defect where GCheckCombinerT::serialize()/load_() serialized
 * the GRANDPARENT GPreEvaluationValidityCheckT base directly, bypassing the direct
 * GValidityCheckContainerT base that carries validity_checks_ -- so a combiner came back
 * with an EMPTY check vector (constraints silently dropped on the wire / in checkpoints).
 * Before the fix the EQUALITY compare below throws (size mismatch); after it, it passes.
 */
TEST_CASE("GCheckCombinerT: serialization preserves the contained validity checks", "[constraint]") {
    using Gem::Geneva::GCheckCombinerT;
    using Gem::Geneva::Individuals::GSphereConstraint;
    using Gem::Geneva::Individuals::GDoubleSumConstraint;
    using Gem::Geneva::validityCheckCombinerPolicy;

    GCheckCombinerT<gen::GGenome> original;
    original.setCombinerPolicy(validityCheckCombinerPolicy::ADDINVALID); // non-default, to also pin the policy
    original.addCheck(std::make_shared<GSphereConstraint>(2.5));
    original.addCheck(std::make_shared<GDoubleSumConstraint>(1.25));

    for(auto mode : {Gem::Common::serializationMode::GEM_BINARY,
                     Gem::Common::serializationMode::GEM_JSON}) {
        const std::string archived = original.toString(mode);

        GCheckCombinerT<gen::GGenome> restored;
        restored.fromString(archived, mode);

        // The restored combiner must equal the original -- including the two contained
        // checks. If serialize() dropped validity_checks_, restored would hold zero checks
        // and this comparison would throw a g_expectation_violation.
        CHECK_NOTHROW(restored.compare(
            original,
            Gem::Common::expectation::EQUALITY,
            Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
        ));
    }
}

/******************************************************************************/
