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
// compile. (Every Asio header defines BOOST_ASIO_VERSION, every Beast header
// BOOST_BEAST_VERSION, mpi.h MPI_VERSION.)
#include "courtier/consumers/GAsioConsumerConfig.hpp"
#include "courtier/consumers/GMPIConsumerConfig.hpp"
#include "courtier/consumers/GStdThreadConsumerConfig.hpp"
#include "courtier/consumers/GWebsocketConsumerConfig.hpp"

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
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessingContainerT.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

using Gem::Courtier::GAsioConsumerConfig;
using Gem::Courtier::GStdThreadConsumerConfig;
using Gem::Courtier::GWebsocketConsumerConfig;
using Gem::Courtier::Consumers::MPIConsumerConfig;

/******************************************************************************/
// The structs are plain, copyable aggregates and name the mnemonic of their family.
static_assert(std::is_aggregate_v<GStdThreadConsumerConfig>);
static_assert(std::is_aggregate_v<GWebsocketConsumerConfig>);
static_assert(std::is_aggregate_v<GAsioConsumerConfig>);
static_assert(std::copyable<GStdThreadConsumerConfig>);
static_assert(std::copyable<GWebsocketConsumerConfig>);
static_assert(std::copyable<GAsioConsumerConfig>);
static_assert(GStdThreadConsumerConfig::MNEMONIC == "stc");
static_assert(GWebsocketConsumerConfig::MNEMONIC == "beast");
static_assert(GAsioConsumerConfig::MNEMONIC == "asio");
static_assert(std::is_aggregate_v<MPIConsumerConfig>);
static_assert(std::copyable<MPIConsumerConfig>);
static_assert(MPIConsumerConfig::MNEMONIC == "mpi");

namespace {

/******************************************************************************/
/**
 * A work item that records the thread it was processed on, in a set shared by all items, and
 * then registers a result.
 */
class ThreadProbe : public Gem::Courtier::GProcessingContainerT<ThreadProbe, double> {
    ///////////////////////////////////////////////////////////////////////

    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
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
    CHECK(beast.nThreads == 0);
    CHECK(beast.pingInterval == Gem::Courtier::GBEASTCONSUMERPINGINTERVAL);
    CHECK_FALSE(beast.verboseControlFrames);
    CHECK(beast.clientPrefetchDepth == 1);
    CHECK(beast.ip == Gem::Courtier::GCONSUMERDEFAULTSERVER);

    const GAsioConsumerConfig asio{};
    CHECK(asio.ip == "localhost");
    CHECK(asio.port == 0);
    CHECK(asio.serializationMode == Gem::Common::serializationMode::GEM_BINARY);
    CHECK(asio.nThreads == 0);
    CHECK(asio.maxReconnects == 0);
    CHECK(asio.clientPrefetchDepth == 1);
    CHECK(asio.ip == Gem::Courtier::GCONSUMERDEFAULTSERVER);
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
    }

    SECTION("built without arguments") {
        ThreadProbe::Seen seen;
        auto consumer = std::make_shared<Gem::Courtier::GStdThreadConsumerT<ThreadProbe>>();
        CHECK(runBatch(*consumer, seen, 16) == 16);
        CHECK_FALSE(seen.ids.empty());
    }
}
