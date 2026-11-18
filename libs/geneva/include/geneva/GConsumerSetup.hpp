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

#include "common/GGlobalDefines.hpp"
#include "geneva/GNamespaceAliases.hpp"

// Standard headers
#include <any>
#include <cstddef>
#include <memory>
#include <string>

// Boost headers
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

// Geneva headers
#include "common/GProviderStoreT.hpp"
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GConsumerSpec.hpp" // ConsumerSpec, ConsumerConfig, configOf()
#include "courtier/GCourtierEnums.hpp" // processRole
#include "geneva/genome/GGenome.hpp"

namespace Gem::Courtier {
template <typename processable_type>
class GBaseClientT; // the networked client base (wire-compatible with the courtier socket servers)
} /* namespace Gem::Courtier */

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief A provider for one courtier consumer family, registered under its mnemonic.
 *
 * Extends the shared @c Gem::Common::GProviderT with the consumer-specific build surface
 * (@c setup / @c configFromCommandLine / @c buildClient / @c needsClient) so the whole consumer
 * catalog lives as data in a @c Gem::Common::GProviderStoreT rather than a hard-coded table +
 * switch. Each consumer registers one provider at static init (see GConsumerSetup.cpp); the free
 * functions below resolve a mnemonic against the store instead of dispatching on it. A consumer
 * module registers its provider into the same store (see consumerManifest()).
 *
 * A provider reads its family's configuration out of a spec with configOf() (an empty spec gives
 * the family's defaults) and states it from the command line with configFromCommandLine().
 *
 * @c provide() (inherited) is unused for consumers: building a consumer has side effects (binding a port,
 * starting a server), so a consumer is built through @c setup(), never handed out prototype-style -- it
 * returns nullptr.
 *
 * The role of a process (@c role()) decides which of the two builds applies: @c setup() is called
 * on a submitter only, @c buildClient() on a client only, and a bystander builds neither.
 */
class GConsumerProviderT
  : public Gem::Common::GProviderT<Gem::Courtier::GBaseConsumerT<gen::GGenome>> {
public:
    /** @brief Builds this consumer for the current process. Called on a submitter only (see
     *  role()).
     *  @param spec The consumer specification. @return The consumer for this process. */
    virtual std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>>
    setup(const Gem::Courtier::ConsumerSpec &spec) = 0;
    /** @brief Reads this consumer's command-line options out of @p vm into the configuration of its
     *  family. @param vm The parsed program-options map. @return The configuration struct of the
     *  family (in a std::any), or an empty std::any for a consumer that has no configuration. */
    [[nodiscard]] virtual std::any
    configFromCommandLine(const boost::program_options::variables_map &vm) const = 0;
    /** @brief Writes the configuration files this provider owns alone, for --update-configs. The
     *  default owns none. The timeout file of the networked consumers is not a provider's: it is
     *  applied and written by buildConsumer(). */
    virtual void materializeConfigFiles() const { /* owns no configuration file */
    }
    /** @brief Builds the networked client matching @p spec, or nullptr for a local-only consumer.
     *  Called on a client only (see role()).
     *  @param spec The consumer specification. @return The client, or nullptr. */
    [[nodiscard]] virtual std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>>
    buildClient(const Gem::Courtier::ConsumerSpec &spec) const = 0;
    /** @brief Whether a process selecting this consumer can run as a networked client.
     *  @return true for networked consumers (beast/mpi), false otherwise. */
    [[nodiscard]] virtual bool needsClient() const = 0;
    /** @brief Whether this consumer binds a listening socket, so a second build reuses the existing one
     *  rather than binding again. @return true for the socket-server consumers (beast). */
    [[nodiscard]] virtual bool bindsListeningPort() const { return false; }
    /** @brief Whether this consumer answers role() from its RUNTIME environment (e.g. the rank of
     *  the process in an MPI communicator) rather than from the --client switch. A caller must
     *  finalize its configuration (choose the consumer) before it can ask such a consumer for the
     *  role, and must not reject --client up front for it. The default (false) is the ordinary
     *  case: the role follows --client and is known as soon as the command line is parsed.
     *  @return true if the role is determined at runtime by the consumer, false otherwise. */
    [[nodiscard]] virtual bool determinesRoleAtRuntime() const { return false; }
    /** @brief The role of this process for this consumer. The default follows the client
     *  switch: CLIENT if a client was asked for and this consumer has one, otherwise SUBMITTER.
     *  @param spec The consumer specification
     *  @param client_requested Whether the program was started as a client (--client)
     *  @return The role of this process */
    [[nodiscard]] virtual Gem::Courtier::processRole
    role(const Gem::Courtier::ConsumerSpec &spec, bool client_requested) const {
        (void)spec;
        return client_requested && this->needsClient() ? Gem::Courtier::processRole::CLIENT
                                                       : Gem::Courtier::processRole::SUBMITTER;
    }

    /** @brief Unused for consumers (they are built via setup(), not handed out prototype-style).
     *  @return nullptr. */
    std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> provide() override {
        return nullptr;
    }
};

/******************************************************************************/
/**
 * @brief The process-global store of consumer providers, keyed by mnemonic.
 *
 * An instantiation of the shared @c Gem::Common::GProviderStoreT template (not a bespoke store); the
 * consumers register their providers into it at static init.
 *
 * @return The shared consumer-provider store singleton (never nullptr).
 */
[[nodiscard]] inline auto consumerProviderStore() {
    return Gem::Common::providerStore<Gem::Courtier::GBaseConsumerT<gen::GGenome>>();
}

/******************************************************************************/
/**
 * @brief Builds the consumer of the current process, a submitter, from a spec.
 *
 * Resolves @c spec.mnemonic against @c consumerProviderStore() and invokes the matching provider's
 * @c setup(): it constructs the consumer and -- for networked consumers -- starts the server. A
 * networked consumer (a GNetworkedConsumerT, built in or from a module) is given the timeout
 * configuration of ./config/GNetworkedConsumer.json. The freshly-built consumer is registered in
 * GConsumerRegistry as the process's single consumer. A socket consumer whose process already holds
 * a consumer reuses it rather than binding the port again. Call it on a submitter only (see
 * consumerRole()); a provider refuses to build on any other role.
 *
 * @param spec The consumer to build: its mnemonic and the configuration of its family
 * @return The consumer, registered in GConsumerRegistry; null for an unknown mnemonic
 * @throws geneva_exception if @c spec.config holds the configuration of another family
 */
std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>>
buildConsumer(const Gem::Courtier::ConsumerSpec &spec);

/******************************************************************************/
/**
 * @brief Builds the consumer of the current process, a submitter, from the configuration struct of
 * its family: buildConsumer(GStdThreadConsumerConfig{.nThreads = 1}).
 *
 * @tparam config_type The configuration struct; its MNEMONIC names the consumer
 * @param config The configuration of the consumer
 * @return As buildConsumer(const Gem::Courtier::ConsumerSpec &)
 */
template <Gem::Courtier::ConsumerConfig config_type>
std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>>
buildConsumer(const config_type &config) {
    return buildConsumer(
        Gem::Courtier::ConsumerSpec{
            .mnemonic = std::string(config_type::MNEMONIC),
            .config = config
        }
    );
}

/******************************************************************************/
/**
 * @brief Returns the process's single consumer, building and registering the default one if none was
 * established yet.
 *
 * This is where the "what is the default consumer" decision lives -- with the layer that owns the
 * consumer catalog, not with the optimization algorithms (which are transport-agnostic and never build or
 * select a consumer) and not with courtier (whose registry knows only that there must be exactly one
 * consumer, never which). A process that went through Go2, through buildConsumer(), or through
 * GConsumerRegistry::setConsumer() already has its consumer and gets it back unchanged; a standalone
 * algorithm that was never given one gets @c Gem::Courtier::GDEFAULTCONSUMERMNEMONIC built from the
 * provider store, so a bare alg->optimize() works out of the box. Idempotent and race-free: the
 * registry runs the build at most once per process.
 *
 * @return The process's single consumer (never null)
 */
std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> ensureProcessConsumer();

/******************************************************************************/
/**
 * @brief Builds a ConsumerSpec for a mnemonic from an already-parsed command line.
 *
 * Fills both fields: the mnemonic, and the configuration the mnemonic's provider reads out of
 * @p vm (GConsumerProviderT::configFromCommandLine()). Keeps callers (Go2, the standalone
 * examples) free of per-consumer option knowledge.
 */
Gem::Courtier::ConsumerSpec
specFromCommandLine(const std::string &mnemonic, const boost::program_options::variables_map &vm);

/******************************************************************************/
/**
 * @brief Builds the networked client for a spec, for a process running in client mode.
 *
 * The servers built by buildConsumer() are wire-compatible with the existing client classes,
 * so this is the single place that maps a mnemonic onto the matching client (beast/mpi). The
 * caller sets the maximum runtime and invokes run() on the returned client. Call it on a client
 * only (see consumerRole()); a provider may refuse to build on any other role.
 *
 * @param spec The consumer description; the client-side members of its configuration (ip, port,
 *   serialization, prefetch depth) drive the client that is constructed.
 * @return The constructed client (for mpi, the MPI worker node), or null for a consumer that has no
 *   client (stc is local).
 */
std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>>
buildConsumerClient(const Gem::Courtier::ConsumerSpec &spec);

/******************************************************************************/
/**
 * @brief Builds the networked client from the configuration struct of its family:
 * buildConsumerClient(GWebsocketConsumerConfig{.ip = "server", .port = 10000}).
 *
 * @tparam config_type The configuration struct; its MNEMONIC names the consumer
 * @param config The configuration of the consumer
 * @return As buildConsumerClient(const Gem::Courtier::ConsumerSpec &)
 */
template <Gem::Courtier::ConsumerConfig config_type>
std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>>
buildConsumerClient(const config_type &config) {
    return buildConsumerClient(
        Gem::Courtier::ConsumerSpec{
            .mnemonic = std::string(config_type::MNEMONIC),
            .config = config
        }
    );
}

/******************************************************************************/
/** @brief What buildClientFromCommandLine() gives a process: its role and, for a CLIENT, its
 *  client. */
struct GCommandLineClient {
    Gem::Courtier::processRole role; ///< The consumer's role of this process
    /// The client; null unless the role is CLIENT
    std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>> client;
};

/** @brief Builds the client of a process that runs without Go2 -- the lead of a group of an MPI
 *  cascade (courtier/mpi/GMPICascade.hpp) -- from the consumer options of its command line. The
 *  rework of the program launcher replaces this function. */
[[nodiscard]] GCommandLineClient buildClientFromCommandLine(int, char **);

/******************************************************************************/
/**
 * @brief Writes the configuration files that consumer providers own alone, for --update-configs.
 *
 * Calls GConsumerProviderT::materializeConfigFiles() on every registered provider, the providers of
 * loaded modules included. A provider that fails is reported as a warning; the others still run.
 */
void materializeConsumerConfigFiles();

/******************************************************************************/
/**
 * @brief Registers the command-line options of every supported courtier consumer.
 *
 * Registers the options for beast/stc (and mpi when built). This is the single place that owns
 * the consumer option surface, so callers (Go2) register them without iterating a consumer store.
 * specFromCommandLine() reads the matching values back out of the parsed map.
 */
void addConsumerOptions(
    boost::program_options::options_description &visible,
    boost::program_options::options_description &hidden);

/******************************************************************************/
/**
 * @brief Whether a mnemonic names a consumer this layer can build (stc/beast/mpi).
 */
bool isKnownConsumer(const std::string &mnemonic);

/******************************************************************************/
/**
 * @brief Whether a process selecting the given mnemonic can run as a networked client.
 */
bool consumerNeedsClient(const std::string &mnemonic);

/**
 * @brief Whether the given consumer determines each process's role at runtime (e.g. from an MPI
 * rank) rather than from --client.
 *
 * Generic drivers use this instead of naming a specific consumer: for a consumer that answers true,
 * the configuration must be finalized before consumerRole() can be asked, and --client must not be
 * rejected up front. See GConsumerProviderT::determinesRoleAtRuntime().
 */
bool consumerDeterminesRoleAtRuntime(const std::string &mnemonic);

/******************************************************************************/
/**
 * @brief The role of this process for the consumer of @p spec.
 *
 * Asks the provider of the mnemonic (GConsumerProviderT::role()): a submitter builds the consumer
 * (buildConsumer()), a client builds and runs the client (buildConsumerClient()), a bystander
 * takes no part in the consumer's protocol.
 *
 * @param spec The consumer specification
 * @param client_requested Whether the program was started as a client (--client)
 * @return The role of this process
 * @throws geneva_exception for a mnemonic no provider is registered for
 */
[[nodiscard]] Gem::Courtier::processRole
consumerRole(const Gem::Courtier::ConsumerSpec &spec, bool client_requested = false);

/******************************************************************************/
/**
 * @brief A "mnemonic:  human-readable-name" listing of the supported consumers, for help text.
 * @return A formatted multi-line string with one line per supported consumer.
 */
std::string consumerListing();

/******************************************************************************/
/**
 * @brief The number of supported consumers (for help text).
 * @return The count of supported consumers.
 */
std::size_t consumerCount();

/******************************************************************************/
/**
 * @brief Why consumers that live in optional modules (MPI) are missing, if their module is installed but
 * could not be loaded -- typically because a library it needs is absent on this machine.
 * @return One line per module that could not be loaded; empty if there is none.
 */
std::string optionalConsumerNotes();

/******************************************************************************/

} /* namespace Gem::Geneva */
