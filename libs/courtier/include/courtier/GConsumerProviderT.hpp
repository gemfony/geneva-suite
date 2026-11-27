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
#include <any>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

// Boost header files go here
#include <boost/core/demangle.hpp>

// Geneva header files go here
#include "common/GCommandLineParser.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GGlobalOptionsT.hpp"
#include "common/GModuleLoader.hpp"
#include "common/GModuleManifest.hpp"
#include "common/GParserBuilder.hpp"
#include "common/GSingletonT.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GConsumerPlugin.hpp"
#include "courtier/GConsumerSpec.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessable.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * @brief What a program learns about one consumer family for its item type, and how it builds
 * the family's consumer (on the submitter) or client (on a client).
 *
 * A provider is contributed by a consumer module (courtier/GConsumerPlugin.hpp) and held in the
 * catalogue of its item type (consumerCatalog()); the launcher (courtier/GLauncherT.hpp) asks it
 * for its options, its configuration, the role of the process and, when the consumer is chosen,
 * for anything it misses that the program must contribute (missingPrerequisite()). The shipped
 * consumers' providers are built like any other: GConfiguredConsumerProviderT below and a module
 * each.
 *
 * @tparam processable_type The item type the consumers of this provider distribute
 */
template <ProcessableItem processable_type>
class GConsumerProviderT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /** @brief The item type of this provider */
    using item_type = processable_type;
    /** @brief A consumer of this item type */
    using consumer_ptr = std::shared_ptr<GBaseConsumerT<processable_type>>;
    /** @brief A client of this item type */
    using client_ptr = std::shared_ptr<GBaseClientT<processable_type>>;

    /** @brief The (defaulted) destructor */
    virtual ~GConsumerProviderT() = default;

    /** @brief The mnemonic the consumer is chosen by (--consumer) @return The mnemonic */
    [[nodiscard]] virtual std::string getMnemonic() const = 0;
    /** @brief A human-readable name, for listings @return The name */
    [[nodiscard]] virtual std::string getName() const = 0;
    /** @brief Registers the family's command-line options, bound to values the provider keeps
     *  @param gpb The launcher's parser */
    virtual void registerOptions(Gem::Common::GParserBuilder &gpb) = 0;
    /**
     * @brief The family's configuration after the parse: @p chosen (a choice made in main(), the
     * family's configuration struct) with every option overridden that was given explicitly, or
     * the values of the command line (defaults included) if nothing was chosen.
     * @param gpb The parser the options were registered with, after its parse
     * @param chosen The configuration chosen in main(); empty if none was
     * @return The configuration struct of the family, in a std::any
     */
    [[nodiscard]] virtual std::any
    configuration(const Gem::Common::GParserBuilder &gpb, const std::any &chosen) const = 0;
    /** @brief Builds the consumer; called on a submitter only @param spec Mnemonic and
     *  configuration @return The consumer */
    virtual consumer_ptr setup(const ConsumerSpec &spec) = 0;
    /** @brief Builds the client; called on a client only @param spec Mnemonic and configuration
     *  @return The client, or nullptr for a consumer without clients */
    [[nodiscard]] virtual client_ptr buildClient(const ConsumerSpec &spec) const = 0;
    /** @brief Whether a process choosing this consumer can be its client (--client)
     *  @return true for a networked consumer */
    [[nodiscard]] virtual bool needsClient() const = 0;
    /** @brief Whether the consumer decides the role of a process at run time (from an MPI rank)
     *  instead of from --client @return false by default */
    [[nodiscard]] virtual bool determinesRoleAtRuntime() const {
        return false;
    }
    /**
     * @brief The role of this process: by default a client if one was asked for and the consumer
     * has clients, otherwise the submitter
     * @param spec The consumer's mnemonic and configuration
     * @param client_requested Whether --client (or setClientMode()) asked for a client
     * @return The role of this process
     */
    [[nodiscard]] virtual processRole role(const ConsumerSpec &spec, bool client_requested) const {
        (void)spec;
        return client_requested && this->needsClient() ? processRole::CLIENT
                                                       : processRole::SUBMITTER;
    }
    /** @brief Writes the configuration files this provider owns alone (--update-configs); none by
     *  default */
    virtual void materializeConfigFiles() const { /* owns none */
    }
    /**
     * @brief What the program has not contributed that this consumer needs, asked when the consumer
     * is chosen, after all modules are loaded (so the order of loading does not matter). Only the
     * GPU consumer needs something: a marshaller for the item type.
     * @return Empty if nothing is missing, else what is missing, for the refusal
     */
    [[nodiscard]] virtual std::string missingPrerequisite() const {
        return {};
    }
    /**
     * @brief The catalogue of this item type as the provider's own code sees it. A module that was
     * not linked into the program's instances (the program lacks exported symbols, CMake
     * ENABLE_EXPORTS) sees another one; the module's handler refuses it.
     * @return The address of the catalogue
     */
    [[nodiscard]] virtual const void *catalogWitness() const;
};

/******************************************************************************/
/** @brief A provider of consumers for @p processable_type, as a module hands it over */
template <ProcessableItem processable_type>
using GConsumerProviderPtrT = std::shared_ptr<GConsumerProviderT<processable_type>>;

/**
 * @brief The catalogue of consumer providers of one item type, keyed by mnemonic: one per process
 * and item type (a program that loads consumer modules is linked with exported symbols, so that a
 * module shares it). Filled by the consumer modules the launcher loads; never destroyed.
 */
template <ProcessableItem processable_type>
using GConsumerCatalogT =
    Gem::Common::GSingletonT<Gem::Common::GGlobalOptionsT<GConsumerProviderPtrT<processable_type>>>;

/** @brief The catalogue of consumer providers of @p processable_type @return It (never null) */
template <ProcessableItem processable_type>
[[nodiscard]] inline auto consumerCatalog() {
    return GConsumerCatalogT<processable_type>::instance();
}

template <ProcessableItem processable_type>
const void *GConsumerProviderT<processable_type>::catalogWitness() const {
    return consumerCatalog<processable_type>().get();
}

/******************************************************************************/
/**
 * @brief The base of a provider whose family has a configuration struct (courtier's consumers):
 * it takes the mnemonic from the struct, binds the family's options to a struct of its own and
 * merges a choice made in main() with the options given explicitly, option by option.
 *
 * A derived provider registers each option with option(), names the struct member it sets, and
 * builds consumer and client from configOf<config_type>(spec).
 *
 * @tparam processable_type The item type
 * @tparam config_type The configuration struct of the family (its MNEMONIC names the consumer)
 */
template <ProcessableItem processable_type, ConsumerConfig config_type>
class GConfiguredConsumerProviderT : public GConsumerProviderT<processable_type> {
public:
    /** @brief The mnemonic, stated once: by the configuration struct */
    static constexpr std::string_view MNEMONIC = config_type::MNEMONIC;

    /** @brief The mnemonic @return config_type::MNEMONIC */
    [[nodiscard]] std::string getMnemonic() const final {
        return std::string(MNEMONIC);
    }

    /** @brief Registers the family's options (option()), bound to this provider's struct
     *  @param gpb The launcher's parser */
    void registerOptions(Gem::Common::GParserBuilder &gpb) final {
        bound_ = config_type{};
        appliers_.clear();
        this->addOptions_(gpb);
    }

    /** @brief See GConsumerProviderT::configuration() @param gpb The parser after its parse
     *  @param chosen A configuration chosen in main(), or empty @return The configuration */
    [[nodiscard]] std::any
    configuration(const Gem::Common::GParserBuilder &gpb, const std::any &chosen) const final {
        if(not chosen.has_value()) {
            return bound_;
        }
        auto config =
            configOf<config_type>(ConsumerSpec{.mnemonic = getMnemonic(), .config = chosen});
        for(const auto &apply : appliers_) {
            apply(gpb, bound_, config);
        }
        return config;
    }

protected:
    /** @brief Registers the family's options with option() @param gpb The launcher's parser */
    virtual void addOptions_(Gem::Common::GParserBuilder &gpb) = 0;

    /**
     * @brief Registers one option, bound to a member of the family's struct
     * @tparam value_type The member's type
     * @param gpb The launcher's parser
     * @param name The option's name
     * @param member The member it sets
     * @param def_val Its default on the command line
     * @param comment Its help text
     * @return The registered option (to hide it, say)
     */
    template <typename value_type>
    Gem::Common::GCLParsableI &option(
        Gem::Common::GParserBuilder &gpb,
        const std::string &name,
        value_type config_type::*member,
        const value_type &def_val,
        const std::string &comment
    ) {
        appliers_.push_back([name, member](
                                const Gem::Common::GParserBuilder &parser,
                                const config_type &bound,
                                config_type &target
                            ) {
            if(parser.givenExplicitly(name)) {
                target.*member = bound.*member;
            }
        });
        if constexpr(std::is_same_v<value_type, bool>) { // --flag alone means true
            return gpb.registerCLParameter<bool>(
                name,
                bound_.*member,
                def_val,
                comment,
                Gem::Common::GCL_IMPLICIT_ALLOWED,
                true
            );
        }
        else {
            return gpb.registerCLParameter<value_type>(name, bound_.*member, def_val, comment);
        }
    }

private:
    /** @brief Copies a member from the parsed values into a chosen struct if it was given */
    using applier_type = std::function<
        void(const Gem::Common::GParserBuilder &, const config_type &, config_type &)>;

    config_type bound_;                  ///< The values of the command line
    std::vector<applier_type> appliers_; ///< One per option: the override if it was given
};

/******************************************************************************/
// Declared in courtier/GConsumerPlugin.hpp
template <ProcessableItem processable_type>
Gem::Common::GContributionHandler consumerContributionHandler() {
    auto refuse = [](const Gem::Common::GContributionContext &context, const std::string &why) {
        return Gem::Common::gem_exception(
            Gem::Common::g_error_streamer()
            << "The module " << context.description << " " << why << '\n'
        );
    };
    return {
        .kind = CONSUMER_CONTRIBUTION_KIND,
        .noun = "consumer",
        .check =
            [refuse](
                const GemContribution &contribution,
                const Gem::Common::GContributionContext &context
            ) {
                const char *expected = typeid(processable_type).name();
                if(contribution.subject != nullptr &&
                   std::string_view(contribution.subject) == expected) {
                    return;
                }
                throw refuse(
                    context,
                    "contributes a consumer for " +
                        (contribution.subject == nullptr
                             ? std::string("no item type (its subject is null)")
                             : "the item type " + boost::core::demangle(contribution.subject)) +
                        ", but this program distributes " + boost::core::demangle(expected) + "."
                );
            },
        .take =
            [refuse](
                const GemContribution &contribution,
                const Gem::Common::GContributionContext &context
            ) {
                auto provider =
                    Gem::Common::takeContribution<GConsumerProviderPtrT<processable_type>>(
                        contribution,
                        context,
                        "consumer"
                    );
                const std::string mnemonic = provider->getMnemonic();
                const std::string_view stated =
                    contribution.name_or_mnemonic != nullptr ? contribution.name_or_mnemonic : "";
                if(mnemonic != stated) {
                    throw refuse(
                        context,
                        "contributes a consumer under the mnemonic '" + std::string(stated) +
                            "', but its provider calls itself '" + mnemonic + "'."
                    );
                }
                auto catalog = consumerCatalog<processable_type>();
                if(provider->catalogWitness() != catalog.get()) {
                    throw refuse(
                        context,
                        "does not share this program's consumer catalogue: a program that loads "
                        "consumer modules must be linked with exported symbols (CMake: "
                        "ENABLE_EXPORTS ON)."
                    );
                }
                if(not catalog->setOnce(mnemonic, provider)) {
                    throw refuse(
                        context,
                        "contributes the consumer '" + mnemonic +
                            "', which another module has contributed already."
                    );
                }
            }
    };
}

/******************************************************************************/

} /* namespace Gem::Courtier */
