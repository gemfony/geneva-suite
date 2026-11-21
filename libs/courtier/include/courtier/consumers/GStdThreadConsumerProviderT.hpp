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
#include <memory>
#include <string>

// Geneva header files go here
#include "courtier/GConsumerProviderT.hpp"
#include "courtier/consumers/GStdThreadConsumerConfig.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief The provider of the local thread-pool consumer ("stc"), the default consumer of the
 * launcher. Serial execution is this consumer with one worker thread. It has no clients.
 * @tparam processable_type The item type
 */
template <ProcessableItem processable_type>
class GStdThreadConsumerProviderT final
  : public GConfiguredConsumerProviderT<processable_type, GStdThreadConsumerConfig> {
    using base_type = GConfiguredConsumerProviderT<processable_type, GStdThreadConsumerConfig>;

public:
    /** @brief The consumer's class @return Its name */
    [[nodiscard]] std::string getName() const override {
        return "GStdThreadConsumerT";
    }
    /** @brief The thread pool has no clients @return false */
    [[nodiscard]] bool needsClient() const override {
        return false;
    }

    /** @brief Builds the thread pool @param spec Mnemonic and configuration @return It */
    typename base_type::consumer_ptr setup(const ConsumerSpec &spec) override {
        return std::make_shared<GStdThreadConsumerT<processable_type>>(
            configOf<GStdThreadConsumerConfig>(spec)
        );
    }
    /** @brief There is no client @return nullptr */
    [[nodiscard]] typename base_type::client_ptr buildClient(const ConsumerSpec &) const override {
        return nullptr;
    }

protected:
    /** @brief --nWorkerThreads, listed under --showAll @param gpb The launcher's parser */
    void addOptions_(Gem::Common::GParserBuilder &gpb) override {
        this->option(
                gpb,
                "nWorkerThreads",
                &GStdThreadConsumerConfig::nThreads,
                GStdThreadConsumerConfig{}.nThreads,
                "[stc] The number of worker threads (0 == hardware concurrency)"
        )
            .setHidden();
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier */
