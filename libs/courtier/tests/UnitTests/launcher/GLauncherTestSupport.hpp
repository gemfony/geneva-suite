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

/**
 * @file
 * What the tests of the launcher (GCourtierLauncherTests.cpp) share with their consumer modules:
 * a consumer family "needy" that records the configuration it was built with and misses a
 * prerequisite until a test supplies it -- the shape of the GPU consumer, which needs a
 * marshaller, without a device. Its statics are shared by the test program and the module because
 * the program is linked with exported symbols.
 */

// Standard header files go here
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Geneva header files go here
#include "courtier/GConsumerProviderT.hpp"
#include "courtier/consumers/GStdThreadConsumerConfig.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

namespace Gem::Courtier::Tests {

/******************************************************************************/
/** @brief The configuration of the family "needy" */
struct GNeedyConsumerConfig {
    /// The mnemonic of the family
    static constexpr std::string_view MNEMONIC = "needy";
    /// A number, set with --needy_threads
    unsigned int nThreads = 1;
    /// A text, set with --needy_label
    std::string label = "plain";
};

/** @brief The configuration the last "needy" consumer was built with @return It */
inline std::optional<GNeedyConsumerConfig> &lastNeedyConfig() {
    static std::optional<GNeedyConsumerConfig> config;
    return config;
}

/** @brief Whether the prerequisite of "needy" was supplied @return The flag */
inline bool &needySupplied() {
    static bool supplied = false;
    return supplied;
}

/******************************************************************************/
/**
 * @brief A provider that builds a thread pool, records its configuration and needs something
 * the program must supply (needySupplied())
 * @tparam processable_type The item type
 */
template <ProcessableItem processable_type>
class GNeedyConsumerProviderT final
  : public GConfiguredConsumerProviderT<processable_type, GNeedyConsumerConfig> {
    using base_type = GConfiguredConsumerProviderT<processable_type, GNeedyConsumerConfig>;

public:
    [[nodiscard]] std::string getName() const override {
        return "GNeedyConsumer";
    }
    [[nodiscard]] bool needsClient() const override {
        return false;
    }
    [[nodiscard]] std::string missingPrerequisite() const override {
        return needySupplied() ? std::string() : std::string("the test has supplied nothing");
    }
    typename base_type::consumer_ptr setup(const ConsumerSpec &spec) override {
        const auto config = configOf<GNeedyConsumerConfig>(spec);
        lastNeedyConfig() = config;
        return std::make_shared<GStdThreadConsumerT<processable_type>>(
            GStdThreadConsumerConfig{.nThreads = config.nThreads}
        );
    }
    [[nodiscard]] typename base_type::client_ptr buildClient(const ConsumerSpec &) const override {
        return nullptr;
    }

protected:
    void addOptions_(Gem::Common::GParserBuilder &gpb) override {
        const GNeedyConsumerConfig defaults;
        this->option(
            gpb,
            "needy_threads",
            &GNeedyConsumerConfig::nThreads,
            defaults.nThreads,
            "[needy] threads"
        );
        this->option(
                gpb,
                "needy_label",
                &GNeedyConsumerConfig::label,
                defaults.label,
                "[needy] a label"
        )
            .setHidden();
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier::Tests */
