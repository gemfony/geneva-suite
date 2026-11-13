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

// Standard heders go here

// Boosrt headers go here

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named / archive_named_base (named member / base emitters)
#include "common/GExceptions.hpp"
#include "common/GFactoryT.hpp"
#include "common/GLogger.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GPostProcessorT.hpp"

// Forward declarations (kept light so this header carries no OA / flat-genome dependency): the base
// getAdaptionConfig() hook below refers to them only by shared_ptr / const-ref.
namespace Gem::Geneva::OptimizationAlgorithms {
class GAdaptionConfigBase;
} // namespace Gem::Geneva::OptimizationAlgorithms

namespace Gem::Geneva::Genome {

class GGenome;



/******************************************************************************/
/**
 * This class facilitates handling of factories for GGenome-derivatives.
 * In particular it allows to register pre- and post-procesing objects
 */
class GGenomeFactory // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GFactoryT<GGenome> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        Gem::Common::archive_named_base<Gem::Common::GFactoryT<GGenome>>(
            ar, "GFactoryT_GGenome", *this);
        archive_named(ar, "pre_processor_", pre_processor_);
        archive_named(ar, "post_processor_", post_processor_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /***************************************************************************/
    /**
	  * The standard constructor
	  *
	  * @param configFile path object of a configuration file holding information about objects of type T
	  */
    explicit GGenomeFactory(std::filesystem::path const &configFile)
      : Gem::Common::GFactoryT<GGenome>(configFile) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The copy constructor
	  *
	  * @param cp A constant reference to another GGenomeFactory object to be copied
	  */
    GGenomeFactory(const GGenomeFactory &cp)
      : Gem::Common::GFactoryT<GGenome>(cp) {
        Gem::Common::copyCloneableSmartPointer(cp.pre_processor_, pre_processor_);
        Gem::Common::copyCloneableSmartPointer(cp.post_processor_, post_processor_);
    }

    /***************************************************************************/
    // Defaulted and deleted functions
    ~GGenomeFactory() override = default;

    /***************************************************************************/
    /**
	  * Registration of pre-processor function objects
	  *
	  * @param p A shared pointer to a pre-processor function object to be cloned into each produced individual; must not be empty
	  */
    void registerPreProcessor(
        const std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>>& p
    ) {
        if(p) {
            pre_processor_ = p;
        }
        else {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenomeFactory::registerPreProcessor(): Error!" << '\n'
                << "Got empty pre-processor" << '\n'
            );
        }
    }

    /***************************************************************************/
    /**
	  * Registration of post-processor function objects
	  *
	  * @param p A shared pointer to a post-processor function object to be cloned into each produced individual; must not be empty
	  */
    void registerPostProcessor(
        const std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>>& p
    ) {
        if(p) {
            post_processor_ = p;
        }
        else {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GGenomeFactory::registerPostProcessor(): Error!" << '\n'
                << "Got empty post-processor" << '\n'
            );
        }
    }

    /***************************************************************************/
    /**
     * @brief Returns the OA-owned adaption configuration for a genome this factory produces.
     *
     * This lets an adapting algorithm be configured WITHOUT the caller knowing the concrete individual
     * type -- essential when the individual is supplied at runtime through a plugin (a generic launcher
     * pulls the config from the loaded factory through this base interface). The base returns a null
     * pointer (no adaption config); GIndividualFactory overrides it to delegate to the individual's
     * @c buildAdaptionConfig hook.
     *
     * @param sample A sample genome produced by this factory (passed to the individual's hook)
     * @return The OA-owned adaption configuration, or a null pointer if the individual provides none
     */
    virtual std::shared_ptr<Gem::Geneva::OptimizationAlgorithms::GAdaptionConfigBase>
    getAdaptionConfig([[maybe_unused]] const GGenome &sample) const {
        return {};
    }

protected:
    /** @brief A pre-processor for GGenome-derivatives */
    std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>> pre_processor_;

    /** @brief A post-processor for GGenome-derivatives */
    std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<GGenome>> post_processor_;

    /***************************************************************************/
    /**
     * Production of GGenome-derivatives
     *
     * @return A shared pointer to a newly produced GGenome-derivative, with any registered pre- and post-processor cloned and attached
     */
    std::shared_ptr<GGenome> get_() override {
        std::shared_ptr<GGenome> p = GFactoryT<GGenome>::get_();

        if(pre_processor_) {
            p->registerPreProcessor(pre_processor_->clone());
        }

        if(post_processor_) {
            p->registerPostProcessor(post_processor_->clone());
        }

        return p;
    }

private:
    // Only needed for (de-)serialization purposes, hence private
    GGenomeFactory() = default;
};

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
