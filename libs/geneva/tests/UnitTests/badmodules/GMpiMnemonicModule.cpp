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
 * @brief A consumer module that claims the mnemonic "mpi", which Geneva's own optional MPI module
 * holds.
 *
 * Part of the loader's coverage (see GGenomeTests.cpp, "[optional-first]"). The module is good in
 * every respect but its mnemonic. Gem::Geneva::loadModule() loads Geneva's optional consumer
 * modules before the module it is given, so this one must be refused with "already registered", and
 * the consumer "mpi" must stay the one of Geneva's module. Built only where the MPI module is
 * built.
 */

// Standard headers go here
#include <memory>
#include <string>

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT
#include <boost/program_options.hpp>

// Geneva headers go here
#include "common/GModuleManifest.hpp"
#include "courtier/GBaseClientT.hpp"
#include "geneva/GConsumerPlugin.hpp"
#include "geneva/GConsumerSetup.hpp"

namespace {

/** @brief A consumer provider that builds nothing; only its mnemonic matters. */
class GMpiMnemonicProvider final : public Gem::Geneva::GConsumerProviderT {
public:
    /** @brief The mnemonic of Geneva's optional MPI consumer. @return "mpi" */
    [[nodiscard]] std::string getMnemonic() const override {
        return "mpi";
    }
    /** @brief The name of this provider. @return "GMpiMnemonicModule" */
    [[nodiscard]] std::string getName() const override {
        return "GMpiMnemonicModule";
    }
    /** @brief No client. @return false */
    [[nodiscard]] bool needsClient() const override {
        return false;
    }

    /** @brief Builds no consumer. @return An empty setup */
    Gem::Geneva::ConsumerSetup setup(const Gem::Geneva::ConsumerSpec & /*spec*/) override {
        return {};
    }

    /** @brief Reads nothing. @return An empty spec */
    [[nodiscard]] Gem::Geneva::ConsumerSpec
    specFromCommandLine(const boost::program_options::variables_map & /*vm*/) const override {
        return {};
    }

    /** @brief Builds no client. @return nullptr */
    [[nodiscard]] std::shared_ptr<Gem::Courtier::GBaseClientT<Gem::Geneva::Genome::GGenome>>
    buildClient(const Gem::Geneva::ConsumerSpec & /*spec*/) const override {
        return nullptr;
    }

    /** @brief Adds no options. */
    void addCLOptions(
        boost::program_options::options_description & /*visible*/,
        boost::program_options::options_description & /*hidden*/
    ) override {
        // no options
    }
};

} // anonymous namespace

// The entry point's name is fixed by the module scheme (common/GModuleManifest.hpp).
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return Gem::Geneva::consumerManifest<GMpiMnemonicProvider, "mpi">();
}
