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

// Standard headers
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

// Boost headers
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

// Geneva headers
#include "common/GCommonEnums.hpp" // serializationMode
#include "common/GProviderStoreT.hpp"
#include "courtier/GBaseConsumerT.hpp"
#include "geneva/genome/GGenome.hpp"

namespace Gem::Courtier {
template <typename processable_type>
class GBaseClientT; // the networked client base (wire-compatible with the courtier socket servers)
} /* namespace Gem::Courtier */

namespace Gem::Geneva {

/******************************************************************************/
/** @brief The mnemonic of the consumer a process falls back to when none was chosen -- the local
 *  thread-pool consumer. Stated once, here, because this layer owns the consumer catalog: it is both the
 *  command-line default (Go2's GO2_DEF_CONSUMER binds to it) and the consumer ensureProcessConsumer()
 *  builds for a standalone algorithm that was never given one. */
const std::string GDEFAULTCONSUMERMNEMONIC{"stc"}; // NOLINT

/******************************************************************************/
/**
 * A transport-agnostic description of the courtier consumer to build, assembled from configuration
 * / command-line parameters by the caller (Go2 or a standalone example). Keeps the caller free of the
 * concrete courtier consumer types -- those are known only to buildConsumerSetup().
 */
struct ConsumerSpec {
    std::string mnemonic;            ///< "stc" | "asio" | "beast" | "mpi" (serial = "stc" with n_threads == 1)
    unsigned int n_threads = 0;      ///< local thread-pool / networked IO-thread count (0 == hardware concurrency)
    unsigned short port = 0;         ///< listening / target port (networked socket consumers)
    Gem::Common::serializationMode serialization_mode =
        Gem::Common::serializationMode::GEM_BINARY; ///< wire serialization (networked)
    // --- client-side fields (networked socket consumers; ignored by the server build) ---
    std::string ip = "localhost";        ///< server address the client connects to
    std::size_t max_reconnects = 0;      ///< [asio] client reconnect attempts before giving up
    bool verbose_control_frames = false; ///< [beast] client: log ping/pong/close frames
    std::size_t client_prefetch_depth = 1; ///< [asio/beast] client: max work items held concurrently
                                           ///< (1 == serial; >1 overlaps fetch/compute/return)
    // --- [mpi] config fields (ignored by the other consumers). Defaults mirror MPIConsumerConfig so the
    //     default behaviour is unchanged; serialization_mode (above) carries mpi_serializationMode. ---
    bool mpi_async_req = true;                    ///< [mpi] clients prefetch the next work item
    unsigned int mpi_n_handler_threads = 0;       ///< [mpi] request-handler threads (0 == hardware concurrency)
    unsigned int mpi_clean_sess_interval = 1000;  ///< [mpi] ms between master session-completion checks
};

/******************************************************************************/
/**
 * The result of building a courtier setup for the current process.
 */
struct ConsumerSetup {
    /** @brief The ready consumer (clone function set, server started for networked consumers), also
     *  registered as the process's single consumer in GConsumerRegistry. Null when this process is not a
     *  submitter -- e.g. an MPI worker rank. */
    std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> consumer;
    /** @brief When this process must serve as a worker (an MPI worker rank), the loop to run; null
     *  otherwise. The caller invokes it instead of submitting. */
    std::move_only_function<void()> run_worker;
};

/******************************************************************************/
/**
 * @brief A provider for one courtier consumer family, registered under its mnemonic.
 *
 * Extends the shared @c Gem::Common::GProviderT with the consumer-specific build surface (@c setup /
 * @c specFromCommandLine / @c buildClient / @c needsClient) so the whole consumer catalog lives as data
 * in a @c Gem::Common::GProviderStoreT rather than a hard-coded table + switch. Each consumer registers
 * one provider at static init (see GConsumerSetup.cpp); the free functions below resolve a mnemonic
 * against the store instead of dispatching on it. A runtime-loaded consumer module would register its
 * provider into the same store (deferred; the store makes it a drop-in later).
 *
 * @c provide() (inherited) is unused for consumers: building a consumer has side effects (binding a port,
 * starting a server), so a consumer is built through @c setup(), never handed out prototype-style -- it
 * returns nullptr.
 */
class GConsumerProviderT
  : public Gem::Common::GProviderT<Gem::Courtier::GBaseConsumerT<gen::GGenome>> {
public:
    /** @brief Builds this consumer (and, for an MPI worker rank, a worker loop) for the current process.
     *  @param spec The consumer specification. @return The consumer and/or worker loop for this process. */
    virtual ConsumerSetup setup(const ConsumerSpec &spec) = 0;
    /** @brief Reads this consumer's command-line options out of @p vm into a spec (mnemonic filled by the
     *  caller). @param vm The parsed program-options map. @return The populated (partial) ConsumerSpec. */
    [[nodiscard]] virtual ConsumerSpec specFromCommandLine(
        const boost::program_options::variables_map &vm) const = 0;
    /** @brief Builds the networked client matching @p spec, or nullptr for a local-only consumer.
     *  @param spec The consumer specification. @return The client, or nullptr. */
    [[nodiscard]] virtual std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>>
    buildClient(const ConsumerSpec &spec) const = 0;
    /** @brief Whether a process selecting this consumer can run as a networked client.
     *  @return true for networked consumers (asio/beast/mpi), false otherwise. */
    [[nodiscard]] virtual bool needsClient() const = 0;
    /** @brief Whether this consumer binds a listening socket, so a second build reuses the existing one
     *  rather than binding again. @return true for the socket-server consumers (asio/beast). */
    [[nodiscard]] virtual bool bindsListeningPort() const { return false; }
    /** @brief Whether this consumer assigns each process its client/server role from the RUNTIME
     *  environment (e.g. an MPI process rank) rather than from the --client switch. Such a consumer is
     *  built on every process, and the role is not known until that build runs: the build hands a worker
     *  process a run_worker loop (and a null consumer) and a submitter process the consumer. A caller must
     *  therefore build the setup before it can know whether this process is a client, must not reject
     *  --client up front (the role is not yet decided), and reads the resulting role from
     *  ConsumerSetup.run_worker. The default (false) is the ordinary case: the role comes from --client
     *  and is known before the consumer is built.
     *  @return true if the client/server role is determined at runtime by the consumer, false otherwise. */
    [[nodiscard]] virtual bool determinesRoleAtRuntime() const { return false; }

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
 * @brief Builds a courtier setup (consumer and/or worker loop) for the current process from a spec.
 *
 * Resolves @c spec.mnemonic against @c consumerProviderStore() and invokes the matching provider's
 * @c setup(): it constructs the consumer, sets the polymorphic GGenome clone function
 * (required by clone-on-partial-return), and -- for networked consumers -- starts the server (for MPI
 * only on the master rank; a worker rank yields a run_worker loop and a null consumer instead). The
 * freshly-built consumer is registered in GConsumerRegistry as the process's single consumer. A socket
 * consumer whose process already holds a consumer reuses it rather than binding the port again.
 *
 * @param spec The transport-agnostic description of the consumer to build (mnemonic, ports, threads,
 *   serialization, client-side fields).
 * @return A ConsumerSetup whose consumer is registered in GConsumerRegistry (or null when this
 *   process is a worker rather than a submitter), and whose run_worker holds the worker loop when this
 *   process must serve as a worker. An unknown mnemonic yields an empty setup (both fields null).
 */
ConsumerSetup buildConsumerSetup(const ConsumerSpec &spec);

/******************************************************************************/
/**
 * @brief Returns the process's single consumer, building and registering the default one if none was
 * established yet.
 *
 * This is where the "what is the default consumer" decision lives -- with the layer that owns the
 * consumer catalog, not with the optimization algorithms (which are transport-agnostic and never build or
 * select a consumer) and not with courtier (whose registry knows only that there must be exactly one
 * consumer, never which). A process that went through Go2, through buildConsumerSetup(), or through
 * GConsumerRegistry::setConsumer() already has its consumer and gets it back unchanged; a standalone
 * algorithm that was never given one gets @c GDEFAULTCONSUMERMNEMONIC built from the provider store,
 * so a bare alg->optimize() works out of the box. Idempotent and race-free: the registry runs the build
 * at most once per process.
 *
 * @return The process's single consumer (never null)
 */
std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> ensureProcessConsumer();

/******************************************************************************/
/**
 * @brief Builds a ConsumerSpec for a mnemonic from an already-parsed command line.
 *
 * This is the single place that maps the consumer command-line options (asio_port,
 * beast_serializationMode, nWorkerThreads, ...) onto the transport-agnostic spec, keeping callers
 * (Go2, the standalone examples) free of per-consumer option knowledge.
 *
 * @param mnemonic The consumer mnemonic to build a spec for ("stc"|"asio"|"beast"|"mpi").
 * @param vm The parsed program-options variables map to read consumer option values from.
 * @return The populated ConsumerSpec. Options absent from @p vm fall back to the spec's defaults; an
 *   unknown mnemonic yields a spec carrying only the mnemonic.
 */
ConsumerSpec specFromCommandLine(
    const std::string &mnemonic, const boost::program_options::variables_map &vm);

/******************************************************************************/
/**
 * @brief Builds the networked client for a spec, for a process running in client mode.
 *
 * The socket servers built by buildConsumerSetup() are wire-compatible with the existing client
 * classes, so this is the single place that maps a mnemonic onto the matching client (asio/beast).
 * The caller sets the maximum runtime and invokes run() on the returned client.
 *
 * @param spec The consumer description; its client-side fields (ip, port, serialization, reconnects,
 *   prefetch depth) drive the client that is constructed.
 * @return The constructed client, or null for mnemonics that have no socket client (stc is local;
 *   the mpi worker loop is obtained from buildConsumerSetup().run_worker instead).
 */
std::shared_ptr<Gem::Courtier::GBaseClientT<gen::GGenome>>
buildConsumerClient(const ConsumerSpec &spec);

/******************************************************************************/
/**
 * @brief Registers the command-line options of every supported courtier consumer.
 *
 * Registers the options for asio/beast/stc (and mpi when built). This is the single place that owns
 * the consumer option surface, so callers (Go2) register them without iterating a consumer store.
 * specFromCommandLine() reads the matching values back out of the parsed map.
 *
 * @param visible The options group that user-facing (help-listed) consumer options are added to.
 * @param hidden The options group that internal / non-listed consumer options are added to.
 */
void addConsumerOptions(
    boost::program_options::options_description &visible,
    boost::program_options::options_description &hidden);

/******************************************************************************/
/**
 * @brief Whether a mnemonic names a consumer this layer can build (stc/asio/beast/mpi).
 * @param mnemonic The consumer mnemonic to test.
 * @return true if the mnemonic is a known/buildable consumer, false otherwise.
 */
bool isKnownConsumer(const std::string &mnemonic);

/******************************************************************************/
/**
 * @brief Whether a process selecting the given mnemonic can run as a networked client.
 * @param mnemonic The consumer mnemonic to test.
 * @return true if the mnemonic denotes a consumer with a client role (asio/beast/mpi), false otherwise.
 */
bool consumerNeedsClient(const std::string &mnemonic);

/**
 * @brief Whether the given consumer determines each process's client/server role at runtime (e.g. from an
 * MPI rank) rather than from --client.
 *
 * Generic drivers use this instead of naming a specific consumer: a consumer that answers true must be
 * built on every process to discover its role (so a client's role is only known once the setup runs), and
 * --client must not be rejected up front for it. See GConsumerProviderT::determinesRoleAtRuntime().
 *
 * @param mnemonic The consumer mnemonic to test.
 * @return true if the consumer self-assigns the role at runtime, false otherwise (incl. unknown mnemonics).
 */
bool consumerDeterminesRoleAtRuntime(const std::string &mnemonic);

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

} /* namespace Gem::Geneva */
