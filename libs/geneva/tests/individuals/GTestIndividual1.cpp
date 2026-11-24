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

#include "GTestIndividual1.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GConjugateGradientDescent_PersonalityTraits.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm_PersonalityTraits.hpp"
#include "geneva/oa/GSwarmAlgorithm_PersonalityTraits.hpp"
#include "hap/GRandomLeasePool.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::Individuals::GTestIndividual1) // NOLINT

namespace Gem::Geneva::Individuals {

/******************************************************************************/
/**
 * @brief The default constructor; builds a 100-double parabola genome and randomly initialises it.
 */
GTestIndividual1::GTestIndividual1() {
    using namespace Gem::Geneva;

    // 100 unbounded doubles (init perimeter [-10, 10]) sharing one Gauss adaptor. The builder's
    // default adapt_ad_prob == 0 prevents changes to adProb_. A small positive min_sigma (1e-3)
    // floors the self-adapting sigma so every adaption step stays well above ULP magnitude; this keeps
    // the "fitness changes after every customAdaptions()" unit test below reliable. With
    // min_sigma == 0 the shared sigma can collapse toward zero, the value steps fall back to a
    // one-ULP nudge, and 100 such nudges can round away in the sum-of-squares fitness.
    gen::GGenomeBuilder b;
    b.addDoublePlainGroup(100, -10., 10.); // structure only; the adaptor lives on the OA config
    this->setGenome(b.build());

    // Random per-parameter initialisation.
    this->randomInit(activityMode::ACTIVEONLY);
}

/******************************************************************************/
/**
 * @brief Builds the OA-owned adaption configuration: the single shared double group gets a Gauss adaptor
 * (sigma 0.025 / sigma_sigma 0.1 / [1e-3, 1] / ad_prob 1).
 *
 * @return A shared pointer to a freshly built adaption config whose double group carries the Gauss adaptor
 *         settings (sigma 0.025, sigma_sigma 0.1, min_sigma 1e-3, max_sigma 1.0, ad_prob 1.0)
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GTestIndividual1::getAdaptionConfig() const {
    namespace oa = Gem::Geneva::OptimizationAlgorithms;
    auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
    for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
        cfg->groupDouble(i).gauss(0.025, 0.1, 1e-3, 1., 1.);
    }
    return cfg;
}

/******************************************************************************/
/**
 * @brief The actual fitness calculation takes place here; computes a parabola (sum of squares) over the
 * flat double values.
 *
 * @return The fitness as a one-element vector (single criterion)
 */
std::vector<double> GTestIndividual1::evaluate() {
    // Read the flat double values and calculate the value of the parabola.
    std::vector<double> par_vec;
    this->streamline(par_vec);

    return {std::ranges::fold_left(
        par_vec | std::views::transform([](double x) { return x * x; }),
        0.,
        std::plus{}
    )};
}

/******************************************************************************/
/**
 * @brief Changes this individual for the standard tests: the genome's change and one adaption of its
 * values. The adaption state and logic are owned by the algorithms; a standalone individual drives them
 * with a self-owned scratch and configuration (StandaloneAdapter).
 *
 * @return Whether the individual was changed (always true)
 */
bool GTestIndividual1::modifyForTest() {
    gen::GGenome::modifyForTest();
    Gem::Geneva::OptimizationAlgorithms::StandaloneAdapter(*this, getAdaptionConfig()).adapt(*this);
    return true;
}

/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
