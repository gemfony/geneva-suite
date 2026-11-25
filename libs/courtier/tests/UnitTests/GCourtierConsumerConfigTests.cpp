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
 * @brief Tests of the configuration structs of the consumer families
 * (courtier/consumers/G*Config.hpp) and of the thread-pool consumer built from its struct.
 */

// The configuration structs must be usable without the consumers: a program names a consumer
// by its struct. If one of the headers pulls in Boost.Asio, Beast or MPI, this file does not
// compile. (Every Boost.Asio header defines BOOST_ASIO_VERSION, every Beast header
// BOOST_BEAST_VERSION, mpi.h MPI_VERSION.)
#include "courtier/consumers/GMPIConsumerConfig.hpp"
#include "courtier/consumers/GMqttConsumerConfig.hpp"
#include "courtier/consumers/GStdThreadConsumerConfig.hpp"
#include "courtier/consumers/GWebsocketConsumerConfig.hpp"
#include "courtier/mpi/GMPICascadeConfig.hpp" // read by programs and tests without MPI

#if defined(BOOST_ASIO_VERSION) || defined(BOOST_BEAST_VERSION)
#error "A consumer configuration header includes Boost.Asio or Boost.Beast"
#endif
#ifdef MPI_VERSION
#error "A consumer configuration header includes MPI"
#endif

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include <unistd.h>

#include "common/GExceptions.hpp"
#include "courtier/GConsumerSpec.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessingContainerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

using Gem::Courtier::GMqttConsumerConfig;
using Gem::Courtier::GStdThreadConsumerConfig;
using Gem::Courtier::GWebsocketConsumerConfig;
using Gem::Courtier::Consumers::MPIConsumerConfig;

/******************************************************************************/
// The structs are plain, copyable aggregates and name the mnemonic of their family.
static_assert(std::is_aggregate_v<GStdThreadConsumerConfig>);
static_assert(std::is_aggregate_v<GWebsocketConsumerConfig>);
static_assert(std::copyable<GStdThreadConsumerConfig>);
static_assert(std::copyable<GWebsocketConsumerConfig>);
static_assert(GStdThreadConsumerConfig::MNEMONIC == "stc");
static_assert(GWebsocketConsumerConfig::MNEMONIC == "beast");
static_assert(std::is_aggregate_v<MPIConsumerConfig>);
static_assert(std::copyable<MPIConsumerConfig>);
static_assert(MPIConsumerConfig::MNEMONIC == "mpi");
static_assert(std::is_aggregate_v<GMqttConsumerConfig>);
static_assert(std::copyable<GMqttConsumerConfig>);
static_assert(GMqttConsumerConfig::MNEMONIC == "mqtt");

namespace {

/******************************************************************************/
/**
 * A work item that records the thread it was processed on, in a set shared by all items, and
 * then registers a result.
 */
class ThreadProbe : public Gem::Courtier::GProcessingContainerT<ThreadProbe, double> {
    ///////////////////////////////////////////////////////////////////////

    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<Gem::Courtier::GProcessingContainerT<ThreadProbe, double>>(
            ar,
            "GProcessingContainerT_ThreadProbe",
            *this
        );
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief What all items of one test share: the ids of the threads that processed them */
    struct Seen {
        std::mutex mutex;              ///< Guards ids
        std::set<std::thread::id> ids; ///< The threads that processed an item
    };

    /** @brief An item due for processing that records into @p seen
     *  @param seen Where the thread is recorded */
    explicit ThreadProbe(Seen *seen)
      : seen_(seen) {
        this->mark_as_due_for_processing();
    }
    /** @brief For deserialization only */
    ThreadProbe() = default;

private:
    /** @brief Records the thread, takes a little time, and registers a result */
    void
    process_([[maybe_unused]] const std::vector<double> &res_vec = std::vector<double>()) final {
        {
            const std::scoped_lock lock(seen_->mutex);
            seen_->ids.insert(std::this_thread::get_id());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        this->registerResult(0, 1.);
    }

    Seen *seen_ = nullptr; ///< Where the thread is recorded (not owned)
};

using probe_ptr = std::unique_ptr<ThreadProbe>;

/** @brief Runs @p n items through @p consumer and returns how many were processed
 *  @param consumer The consumer
 *  @param seen Where the items record their threads
 *  @param n The number of items
 *  @return The number of processed items */
std::size_t runBatch(
    Gem::Courtier::GStdThreadConsumerT<ThreadProbe> &consumer,
    ThreadProbe::Seen &seen,
    std::size_t n
) {
    std::vector<probe_ptr> batch;
    batch.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        batch.push_back(std::make_unique<ThreadProbe>(&seen));
    }
    consumer.processBatch(
        std::span<probe_ptr>(batch.data(), batch.size()),
        Gem::Courtier::GSubmissionPolicy::full_success_or_fatal()
    );
    std::size_t n_processed = 0;
    for(const auto &item : batch) {
        if(item && item->is_processed()) {
            ++n_processed;
        }
    }
    return n_processed;
}

} // anonymous namespace

/******************************************************************************/
TEST_CASE("the defaults of the consumer configurations", "[courtier][config]") {
    const GStdThreadConsumerConfig stc{};
    CHECK(stc.nThreads == 0);

    const GWebsocketConsumerConfig beast{};
    CHECK(beast.ip == "localhost");
    CHECK(beast.port == 0);
    CHECK(beast.serializationMode == Gem::Common::serializationMode::GEM_BINARY);
    CHECK(beast.nThreads == Gem::Courtier::GCONSUMERLISTENERTHREADS);
    CHECK(beast.pingInterval == Gem::Courtier::GBEASTCONSUMERPINGINTERVAL);
    CHECK_FALSE(beast.verboseControlFrames);
    CHECK(beast.clientPrefetchDepth == 1);
    CHECK(beast.ip == Gem::Courtier::GCONSUMERDEFAULTSERVER);
    // The server listens on every IPv4 interface
    CHECK(beast.listenAddress == "0.0.0.0");
    CHECK(beast.listenAddress == Gem::Courtier::GCONSUMERDEFAULTLISTENADDRESS);

    // The MQTT consumer meets its clients on a broker on this host, under the prefix "gem"
    const GMqttConsumerConfig mqtt{};
    CHECK(mqtt.ip == "127.0.0.1");
    CHECK(mqtt.port == 1883);
    CHECK(mqtt.topic == "gem");
    CHECK(mqtt.pingInterval == Gem::Courtier::GBEASTCONSUMERPINGINTERVAL);
    CHECK(mqtt.clientPrefetchDepth == 1);
    CHECK(mqtt.clientParallelEvaluations == 1);

    // The MPI consumer's sessions run on two threads unless told otherwise (one more thread polls),
    // and its client fetches the next item while it evaluates one
    const MPIConsumerConfig mpi{};
    CHECK(mpi.nHandlerThreads == 2);
    CHECK(mpi.serializationMode == Gem::Common::serializationMode::GEM_BINARY);
    CHECK(mpi.clientPrefetchDepth == 2);
    CHECK(mpi.clientParallelEvaluations == 1);
    CHECK(mpi.clientFirstContactLimit == 0);
    CHECK(mpi.pingInterval == Gem::Courtier::GBEASTCONSUMERPINGINTERVAL);
}

/******************************************************************************/
TEST_CASE(
    "the thread-pool consumer runs as many worker threads as its configuration says",
    "[courtier][config]"
) {
    SECTION("one thread") {
        ThreadProbe::Seen seen;
        auto consumer = std::make_shared<Gem::Courtier::GStdThreadConsumerT<ThreadProbe>>(
            GStdThreadConsumerConfig{.nThreads = 1}
        );
        CHECK(runBatch(*consumer, seen, 16) == 16);
        CHECK(seen.ids.size() == 1);
        // A local consumer has no clients: GBaseConsumerT's answer
        CHECK(consumer->connectedClients() == std::optional<std::size_t>{0});
    }

    SECTION("built without arguments") {
        ThreadProbe::Seen seen;
        auto consumer = std::make_shared<Gem::Courtier::GStdThreadConsumerT<ThreadProbe>>();
        CHECK(runBatch(*consumer, seen, 16) == 16);
        CHECK_FALSE(seen.ids.empty());
    }
}

/******************************************************************************/
// A spec carries the configuration struct of its family in a std::any. configOf() is the one way a
// provider reads it: an empty spec means the family's defaults, and the struct of another family is
// refused with a message that says what was expected and what was found -- not with the
// std::bad_any_cast that std::any_cast would throw.
TEST_CASE("configOf() gives the defaults for an empty configuration", "[courtier][config]") {
    using Gem::Courtier::configOf;
    using Gem::Courtier::ConsumerSpec;

    const GWebsocketConsumerConfig reference{};
    const auto defaults = configOf<GWebsocketConsumerConfig>(ConsumerSpec{.mnemonic = "beast"});
    CHECK(defaults.ip == reference.ip);
    CHECK(defaults.port == reference.port);
    CHECK(defaults.nThreads == reference.nThreads);
    CHECK(defaults.pingInterval == reference.pingInterval);
    CHECK(defaults.clientPrefetchDepth == reference.clientPrefetchDepth);

    const ConsumerSpec carried{
        .mnemonic = "beast",
        .config = GWebsocketConsumerConfig{.ip = "server", .port = 4711}
    };
    CHECK(configOf<GWebsocketConsumerConfig>(carried).ip == "server");
    CHECK(configOf<GWebsocketConsumerConfig>(carried).port == 4711);
}

/******************************************************************************/
TEST_CASE("configOf() refuses the configuration of another family", "[courtier][config]") {
    const Gem::Courtier::ConsumerSpec mismatched{
        .mnemonic = "beast",
        .config = GStdThreadConsumerConfig{.nThreads = 2}
    };
    std::string message;
    try {
        (void)Gem::Courtier::configOf<GWebsocketConsumerConfig>(mismatched);
    }
    catch(const gem_exception &e) {
        message = e.what();
    }
    REQUIRE_FALSE(message.empty());
    CHECK(message.contains("\"beast\""));
    CHECK(message.contains("Gem::Courtier::GWebsocketConsumerConfig"));
    CHECK(message.contains("Gem::Courtier::GStdThreadConsumerConfig"));
}

/******************************************************************************/
// The default consumer is the thread-pool consumer, by the mnemonic of its struct; the family
// structs satisfy the concept the typed front door is constrained on, a plain value does not.
static_assert(Gem::Courtier::GDEFAULTCONSUMERMNEMONIC == GStdThreadConsumerConfig::MNEMONIC);
static_assert(Gem::Courtier::ConsumerConfig<GStdThreadConsumerConfig>);
static_assert(Gem::Courtier::ConsumerConfig<GWebsocketConsumerConfig>);
static_assert(Gem::Courtier::ConsumerConfig<MPIConsumerConfig>);
static_assert(not Gem::Courtier::ConsumerConfig<int>);

/******************************************************************************/
// The configuration of the MPI cascade (courtier/mpi/GMPICascadeConfig.hpp): its three keys, the
// group size and the two poll pauses, are read from a file that is written with their defaults if
// it is absent; a file that cannot be parsed is refused, never replaced by the defaults.

namespace {

/** @brief A directory of its own for one test, empty, below the system's temporary directory
 *  @param name The test's part of the directory's name @return The directory */
std::filesystem::path scratchDirectory(const std::string &name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("gem_cascade_config_" + name + "_" + std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::filesystem::create_directories(dir);
    return dir;
}

} // anonymous namespace

TEST_CASE(
    "the configuration of the MPI cascade is read from its file",
    "[courtier][config][cascade]"
) {
    const auto dir = scratchDirectory("load");

    SECTION("an absent file is written with the default group size") {
        const auto file = dir / "config" / "GMPICascade.json";
        Gem::Courtier::GMPICascadeConfig config;
        config.load(file.string());
        CHECK(config.groupSize == Gem::Courtier::GMPICASCADEDEFAULTGROUPSIZE);
        CHECK(config.groupSize == 4);
        REQUIRE(std::filesystem::exists(file));
        std::string text;
        {
            std::ifstream in(file);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        CHECK(text.contains("\"sub_client_group_size\""));
        CHECK(text.contains("\"sub_client_poll_pause_min_us\""));
        CHECK(text.contains("\"sub_client_poll_pause_max_us\""));
        Gem::Courtier::GMPICascadeConfig reread{.groupSize = 0};
        reread.load(file.string());
        CHECK(reread.groupSize == 4);
    }

    SECTION("a file with a group size of 3 gives 3") {
        const auto file = dir / "three.json";
        std::ofstream(file) << R"({ "sub_client_group_size": { "value": 3 } })";
        Gem::Courtier::GMPICascadeConfig config;
        config.load(file.string());
        CHECK(config.groupSize == 3);
        CHECK(config.pauseMin == Gem::Courtier::GMPIPEERPOLLPAUSEMIN);
        CHECK(config.pauseMax == Gem::Courtier::GMPIPEERPOLLPAUSEMAX);
    }

    SECTION("a file with the two poll pauses gives them") {
        const auto file = dir / "pauses.json";
        std::ofstream(file) << R"({ "sub_client_poll_pause_min_us": { "value": 20 },
                                    "sub_client_poll_pause_max_us": { "value": 400 } })";
        Gem::Courtier::GMPICascadeConfig config;
        config.load(file.string());
        CHECK(config.pauseMin == std::chrono::microseconds(20));
        CHECK(config.pauseMax == std::chrono::microseconds(400));
        CHECK(config.groupSize == Gem::Courtier::GMPICASCADEDEFAULTGROUPSIZE);
    }

    SECTION("a file that cannot be parsed is refused") {
        const auto file = dir / "broken.json";
        std::ofstream(file) << "{ this is not JSON";
        Gem::Courtier::GMPICascadeConfig config;
        CHECK_THROWS_AS(config.load(file.string()), gem_exception);
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST_CASE(
    "the configuration of the MPI cascade refuses what a cascade cannot use",
    "[courtier][config][cascade]"
) {
    using Gem::Courtier::GMPICascadeConfig;
    using std::chrono::microseconds;
    CHECK_NOTHROW(GMPICascadeConfig{}.check());
    CHECK_THROWS_AS(GMPICascadeConfig{.groupSize = 0}.check(), gem_exception);
    CHECK_THROWS_AS(GMPICascadeConfig{.pauseMin = microseconds(0)}.check(), gem_exception);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the value that is refused
    constexpr auto UNKNOWN_LAYOUT = static_cast<Gem::Courtier::GMPICascadeLayout>(7);
    CHECK_THROWS_AS((GMPICascadeConfig{.layout = UNKNOWN_LAYOUT}.check()), gem_exception);
    CHECK_THROWS_AS(
        (GMPICascadeConfig{.pauseMin = microseconds(20), .pauseMax = microseconds(10)}.check()),
        gem_exception
    );
    CHECK_NOTHROW(
        (GMPICascadeConfig{.groupSize = 1, .pauseMin = microseconds(1), .pauseMax = microseconds(1)}
             .check())
    );
}
