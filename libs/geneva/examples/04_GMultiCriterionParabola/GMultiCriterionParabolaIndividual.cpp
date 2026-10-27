/**
 * @file GMultiCriterionParabolaIndividual.cpp
 */

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

#include "GMultiCriterionParabolaIndividual.hpp"

#include "geneva/genome/GProblemStoreT.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::GMultiCriterionParabolaIndividual) // NOLINT

//! [problem-store#1]
namespace {
/**
 * @brief The module's load-once store of per-criterion minima -- this problem's hardware-independent
 * constant data. Filled once from the config (in applyConfig) and read by the free evaluator. A
 * function-local static keeps its initialization order well-defined across translation units.
 */
Gem::Geneva::Genome::GProblemStoreT<std::vector<double>> &minimaStore() {
    static Gem::Geneva::Genome::GProblemStoreT<std::vector<double>> store;
    return store;
}
} // namespace
//! [problem-store#1]

namespace Gem::Geneva {
/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
     * Provide an easy way to print the individual's content
     */
std::ostream &operator<<(std::ostream &s, const Gem::Geneva::GMultiCriterionParabolaIndividual &f) {
    std::vector<double> parVec;
    f.streamline(parVec);

    for(std::size_t i = 0; i < f.getNStoredResults(); i++) {
        std::cout << "Raw fitness " << i << ": " << f.raw_fitness(i) << '\n';
    }

    std::vector<double>::iterator it;
    for(it = parVec.begin(); it != parVec.end(); ++it) {
        std::cout << std::distance(parVec.begin(), it) << ": " << *it << '\n';
    }

    return s;
}

/******************************************************************************/
/**
     * Provide an easy way to print the individual's content through a smart-pointer
     */
std::ostream &operator<<(
    std::ostream &s,
    const std::unique_ptr<Gem::Geneva::GMultiCriterionParabolaIndividual> &f_ptr
) {
    return operator<<(s, *f_ptr);
}

/******************************************************************************/
/**
     * The evaluation hook: one parabola per criterion around its own minimum, with the minima read from the
     * module's load-once store. The first entry is the main result.
     *
     * @return The per-criterion raw results (size == the number of minima)
     */
//! [problem-multicriterion#2]
std::vector<double> GMultiCriterionParabolaIndividual::evaluate() {
    //! [problem-store#3]
    const std::vector<double> &minima = minimaStore().get();
    //! [problem-store#3]

    std::vector<double> parVec; // Will hold the individual parameters
    this->streamline(parVec);   // Retrieve the (external) parameters

    std::vector<double> results(parVec.size());
    for(std::size_t i = 0; i < parVec.size(); i++) {
        results[i] = Gem::Common::gsquared(parVec[i] - minima[i]);
    }
    return results;
}
//! [problem-multicriterion#2]

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Registers the config-file options (parabola bounds and the list of minima), binding them to the
 * passed Config.
 */
void GMultiCriterionParabolaIndividual::describeConfig(Gem::Common::GParserBuilder &gpb, Config &c) {
    std::string comment;

    comment = "";
    comment += "The lower boundary of the parabola;";
    gpb.registerFileParameter<double>(
        "par_min", c.par_min, c.par_min, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "The upper boundary of the parabola;";
    gpb.registerFileParameter<double>(
        "par_max", c.par_max, c.par_max, Gem::Common::VAR_IS_ESSENTIAL, comment
    );

    comment = "";
    comment += "A list of optima, encoded as a string;";
    gpb.registerFileParameter<std::string>(
        "minima", c.minima, c.minima, Gem::Common::VAR_IS_ESSENTIAL, comment
    );
}

/******************************************************************************/
/**
 * Builds the flat genome's STRUCTURE only: one constrained double per minimum, in
 * [par_min, par_max]. The start value is the lower perimeter; the optimization algorithm
 * random-initialises within bounds. The Gauss adaptor settings live on the OA-owned config (see
 * getAdaptionConfig()), not in the structure-only genome layout.
 */
gen::GenomeData GMultiCriterionParabolaIndividual::buildGenome(const Config &c) {
    const std::vector<double> minima = Gem::Common::stringToDoubleVec(c.minima);

    gen::GGenomeBuilder b;
    for(std::size_t npar = 0; npar < minima.size(); npar++) {
        // structure only; the adaptor lives on the OA config (see getAdaptionConfig())
        b.addDouble(c.par_min, c.par_min, c.par_max);
    }
    return b.build();
}

/******************************************************************************/
/**
 * Per-object post-config hook: the number of evaluation criteria equals the number of parabolas
 * (= the number of minima), and the per-criterion minima are stored in the module's load-once store for
 * evaluate().
 */
void GMultiCriterionParabolaIndividual::applyConfig(
    GMultiCriterionParabolaIndividual &ind,
    const Config &c
) {
    //! [problem-multicriterion#1]
    const std::vector<double> minima = Gem::Common::stringToDoubleVec(c.minima);
    ind.setNStoredResults(minima.size());
    //! [problem-multicriterion#1]
    // Load the minima once into the module's store; the free evaluator reads them from there. The
    // number of criteria is genome structure (per-instance), so it stays on the individual.
    //! [problem-store#2]
    minimaStore().ensureLoaded([&minima]() { return minima; });
    //! [problem-store#2]
}

/******************************************************************************/
/**
 * Builds the OA-owned adaption configuration for a genome produced by this factory: each double
 * parameter is its own Gauss group, configured with the default GDoubleGaussAdaptor settings. The
 * adaptor settings live solely on the returned (OA-owned) config -- none reside on the individual.
 *
 * @param sample A sample flat genome whose group structure the config mirrors
 * @param c The Config (unused: the adaptor settings are fixed defaults)
 * @return A shared pointer to the populated OA-owned adaption config
 */
std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
GMultiCriterionParabolaIndividual::buildAdaptionConfig(const gen::GGenome &sample, [[maybe_unused]] const Config &c) {
    auto cfg = OptimizationAlgorithms::makeAdaptionConfig<OptimizationAlgorithms::GAdaptionConfigBase>(sample);
    for(std::size_t npar = 0; npar < cfg->doubleGroups().size(); npar++) {
        cfg->groupDouble(npar).gauss(
            DEFAULTSIGMA, DEFAULTSIGMASIGMA, DEFAULTMINSIGMA, DEFAULTMAXSIGMA, DEFAULTADPROB
        );
    }
    return cfg;
}

/******************************************************************************/
} // namespace Gem::Geneva
