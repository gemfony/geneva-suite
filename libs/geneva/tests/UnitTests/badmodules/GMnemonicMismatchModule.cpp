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

/**
 * @file
 * @brief A deliberately broken module: its manifest entry states one consumer mnemonic, its
 * provider another.
 *
 * Part of the loader's negative-path coverage (see GGenomeTests.cpp, "[plugin][reject]"). The
 * mnemonic a program finds in the manifest and the one the provider registers under must be the
 * same; a module in which they differ is refused, and its provider is not registered under either.
 */

// Standard headers go here
#include <any>
#include <memory>
#include <string>
#include <string_view>

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp"
#include "common/GParserBuilder.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GConsumerPlugin.hpp"
#include "courtier/GConsumerProviderT.hpp"
#include "geneva/genome/GGenome.hpp"

namespace {

using item_type = Gem::Geneva::Genome::GGenome;

/** @brief A consumer provider that builds nothing; only its mnemonic matters. */
class GMnemonicMismatchProvider final : public Gem::Courtier::GConsumerProviderT<item_type> {
public:
    /** @brief The mnemonic the manifest states (consumerManifest() takes it from here) */
    static constexpr std::string_view MNEMONIC = "manifest-says";

    /** @brief Not the mnemonic the manifest states. @return "provider-says" */
    [[nodiscard]] std::string getMnemonic() const override {
        return "provider-says";
    }
    /** @brief The name of this provider. @return "GMnemonicMismatchModule" */
    [[nodiscard]] std::string getName() const override {
        return "GMnemonicMismatchModule";
    }
    /** @brief Registers no options. */
    void registerOptions(Gem::Common::GParserBuilder & /*gpb*/) override {
    }
    /** @brief Reads nothing. @return An empty configuration */
    [[nodiscard]] std::any configuration(
        const Gem::Common::GParserBuilder & /*gpb*/,
        const std::any & /*chosen*/
    ) const override {
        return {};
    }
    /** @brief Builds no consumer. @return nullptr */
    consumer_ptr setup(const Gem::Courtier::ConsumerSpec & /*spec*/) override {
        return nullptr;
    }
    /** @brief Builds no client. @return nullptr */
    [[nodiscard]] client_ptr
    buildClient(const Gem::Courtier::ConsumerSpec & /*spec*/) const override {
        return nullptr;
    }
    /** @brief No client. @return false */
    [[nodiscard]] bool needsClient() const override {
        return false;
    }
};

} // anonymous namespace

// The entry point's name is fixed by the module scheme (common/GModuleManifest.hpp).
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest();
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GemModuleManifest *gem_module_manifest() {
    return Gem::Courtier::consumerManifest<GMnemonicMismatchProvider>();
}
