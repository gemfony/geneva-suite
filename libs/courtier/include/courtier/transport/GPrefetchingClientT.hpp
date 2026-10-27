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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>

// Boost headers
#include <boost/asio.hpp>

// Geneva headers
#include "common/GLogger.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "courtier/GBaseClientT.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GWireSerializationContext.hpp"

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * The shared prefetch/compute pipeline of the socket clients (websocket and ASIO). Both keep up to
 * prefetch_depth_ work items in flight (requested-but-unanswered pulls + items computing on the
 * compute pool), overlap network transfer with evaluation, back off on NO_WORK and poll halt() on a
 * timer. That whole pipeline -- the in-flight bookkeeping, the compute-pool dispatch with its
 * work-guard discipline, the shared result prologue, the jittered NO_WORK refill timer and the
 * halt-poll timer -- lives here ONCE, as a CRTP base between GBaseClientT and the concrete client.
 * A concrete client contributes only its transport specifics through three hooks it befriends the
 * base for:
 *
 *  - refill_()                       -- issue pulls until the pipeline is at depth (transport send)
 *  - sendResultAndRefill_(container) -- transmit a finished RETURN and top the pipeline back up
 *  - haltShutdown_()                 -- tear the transport down when halt() is reached
 *
 * @tparam Derived The concrete client type (CRTP; must also inherit enable_shared_from_this)
 * @tparam processable_type The work-item type exchanged with the server (must be processable)
 */
template <typename Derived, typename processable_type>
class GPrefetchingClientT : public Gem::Courtier::GBaseClientT<processable_type> {
protected:
    //-------------------------------------------------------------------------
    /** @brief Initializes the pipeline state.
     *
     *  @param client_name The concrete client's name, used in diagnostics
     *  @param prefetch_depth Maximum number of work items kept in flight at once (0 is treated as 1) */
    GPrefetchingClientT(std::string_view client_name, std::size_t prefetch_depth)
      : client_name_(client_name)
      , prefetch_depth_(prefetch_depth == 0 ? 1 : prefetch_depth)
      , compute_pool_(prefetch_depth_) { /* nothing */ }

    //-------------------------------------------------------------------------
    /** @brief The destructor. Logs a shutdown summary (items processed, NO_WORK count, prefetch depth). */
    ~GPrefetchingClientT() override {
        glogger << '\n'
                << client_name_ << " is shutting down. Processed " << this->getNProcessed()
                << " items in total" << '\n'
                << "\"no data\" was received " << n_nodata_ << " times" << '\n'
                << "prefetch depth was " << prefetch_depth_ << '\n'
                << '\n'
                << GLOGGING;
    }

    //-------------------------------------------------------------------------
    /**
     * @brief Hands the given work item to the compute pool for evaluation, keeping the io thread free
     * for transport work (pings / connection exchanges). A work guard pins io_context::run() open from
     * here until finish_compute_() has run, so the posted result is always delivered -- no premature
     * drain and no leftover-handler leak on shutdown. Each evaluation owns its OWN container (moved
     * into the worker lambda), so several items compute concurrently without sharing state; the
     * container travels back to the io thread for the result transmission (all connection state must
     * only be touched there).
     *
     * @param container The frame holding the WORK item to evaluate (moved into the worker)
     */
    void dispatch_compute_(
        GCommandContainerT<processable_type> container
    ) {
        auto self = derived().shared_from_this();
        auto guard = boost::asio::make_work_guard(io_context_);
        boost::asio::post(
            compute_pool_,
            [self, container = std::move(container), guard = std::move(guard)]() mutable {
                // A failure in the user's processing code surfaces as a g_processing_exception, with the
                // work item already flagged (EXCEPTION_CAUGHT). We must NOT let that kill the client:
                // catch it and return the flagged item like any other result, so it is accounted for.
                try {
                    container.process();
                }
                catch(const g_processing_exception &e) {
                    glogger << "In " << self->client_name_ << "::dispatch_compute_():" << '\n'
                            << "The work item flagged a processing exception:" << '\n'
                            << e.what() << '\n'
                            << "It is returned to the server flagged; the client keeps running."
                            << '\n'
                            << GWARNING;
                }
                // Hop back onto the io thread for the result transmission -- connection state must
                // only be touched there.
                boost::asio::post(
                    self->io_context_,
                    [self, container = std::move(container), guard = std::move(guard)]() mutable {
                        self->finish_compute_(std::move(container));
                    }
                );
            }
        );
    }

    //-------------------------------------------------------------------------
    /** @brief Runs on the io thread once an evaluation has completed: accounts for the finished item,
     *  rewrites the frame into the RETURN the using library wants (makeReturnFrame -- this is where the
     *  library, not courtier, decides whether the whole item or a payload of its own travels back) and
     *  hands it to the transport-specific sendResultAndRefill_() hook. A RETURN doubles as a pull: the
     *  server answers it with the next item. The work guard captured by the posting lambda is released
     *  when this returns.
     *
     *  @param container The frame holding the just-evaluated work item, rewritten into a RETURN */
    void finish_compute_(
        GCommandContainerT<processable_type> container
    ) {
        this->incrementProcessingCounter();
        if(computing_ > 0) {
            --computing_;
        }
        Gem::Courtier::makeReturnFrame(container);
        ++pending_pulls_; // the RETURN we are about to send is a pull (the server replies with an item)
        derived().sendResultAndRefill_(std::move(container));
    }

    //-------------------------------------------------------------------------
    /** @brief After a NO_WORK reply, waits a short randomized backoff and then tops the pipeline back
     *  up. A single timer suffices: refill_() covers the whole deficit at once. */
    void schedule_refill_() {
        std::uniform_int_distribution<> dist(50, 200);
        nodata_timer_.expires_after(std::chrono::milliseconds(dist(rng_engine_)));
        auto self = derived().shared_from_this();
        nodata_timer_.async_wait([self](boost::system::error_code ec) {
            if(ec) { // cancelled during teardown
                return;
            }
            if(not self->halt()) {
                self->refill_();
            }
        });
    }

    //-------------------------------------------------------------------------
    /** @brief Arms a periodic timer that polls the base-class halt() condition (max runtime / stop
     *  request / error flag). A prefetching client can sit idle with all items computing and no
     *  exchange active (or with only a read outstanding), so a timer is needed to notice a stop
     *  promptly; on halt the transport-specific haltShutdown_() hook tears the connection down. */
    void start_halt_timer() {
        halt_timer_.expires_after(std::chrono::seconds(1));
        auto self = derived().shared_from_this();
        halt_timer_.async_wait([self](boost::system::error_code ec) { self->on_halt_timer(ec); });
    }

    //-------------------------------------------------------------------------
    /** @brief Timer callback: tears the transport down once a halt condition is reached, otherwise
     *  re-arms the poll.
     *
     *  @param ec The timer error code (non-zero means the timer was cancelled during teardown) */
    void on_halt_timer(boost::system::error_code ec) {
        if(ec) { // the timer was cancelled during teardown -- stop polling
            return;
        }
        if(this->halt()) {
            derived().haltShutdown_();
            return;
        }
        start_halt_timer(); // keep polling
    }

    //-------------------------------------------------------------------------
    // Data (all touched on the io thread only, except where noted)

    boost::asio::io_context
        io_context_; ///< The io-service object handling the asynchronous processing

    std::random_device nondet_rng_; ///< Source of non-deterministic random numbers
    std::mt19937 rng_engine_{
        nondet_rng_()
    }; ///< The actual random number engine, seeded by nondet_rng_

    std::string client_name_; ///< The concrete client's name (diagnostics)

    /// Maximum number of work items the client keeps in flight at once (requested-but-not-yet-answered
    /// pulls + currently computing). 1 == serial (one item at a time, the classic behaviour); a larger
    /// depth overlaps network transfer with computation.
    std::size_t prefetch_depth_ = 1;

    /// In-flight bookkeeping, touched on the io thread only (no locking needed): pulls (PULL/RETURN)
    /// sent but not yet answered, and items currently being evaluated on the compute pool. The client
    /// keeps pending_pulls_ + computing_ == prefetch_depth_ whenever work is available.
    std::size_t pending_pulls_ = 0;
    std::size_t computing_ = 0;

    std::uint64_t n_nodata_ = 0; ///< How often a NO_WORK reply was received (reported at shutdown)

    GCommandContainerT<processable_type> command_container_{
        GFrameKind::NONE
    }; ///< The read/parse target; a WORK item is moved out of it onto the compute pool

    /// Per-client cache of received blobs (keyed by content id), and the wire scope installed around
    /// every (de)serialisation so an id-referenced blob resolves locally (blob send-once). The
    /// concrete client configures the scope (and any cache-miss fetch) in its constructor.
    Gem::Courtier::GWireBlobRegistry wire_registry_;
    Gem::Courtier::GWireSerializationContext wire_ctx_;

    boost::asio::steady_timer halt_timer_{
        io_context_
    }; ///< Periodically polls halt() so a stop is noticed even while all items are computing
    boost::asio::steady_timer nodata_timer_{
        io_context_
    }; ///< Backoff timer that retries a PULL top-up after a NO_WORK reply (async, never blocks)

    /// The compute pool's reservation in the process-wide thread budget: Fixed, because the pool
    /// must hold exactly prefetch_depth_ workers to keep prefetch_depth_ items computing. Declared
    /// before the pool so it is released only after the pool's threads are joined.
    Gem::Common::Concurrency::GThreadBudget::Reservation compute_budget_{
        Gem::Common::Concurrency::threadBudget().reserve(
            "client:compute",
            static_cast<unsigned int>(prefetch_depth_),
            Gem::Common::Concurrency::ThreadElasticity::Fixed
        )
    };

    /// A thread pool that runs the (possibly long, unbounded) work-item evaluations OFF the io thread,
    /// so the io thread stays free for transport work while items are computed. Sized to the prefetch
    /// depth so all in-flight items can compute concurrently. Declared last so it is destroyed (and its
    /// threads joined) before io_context_ and the other pipeline state.
    boost::asio::thread_pool compute_pool_;

private:
    //-------------------------------------------------------------------------
    /** @brief The CRTP downcast to the concrete client.
     *  @return A reference to *this as the concrete client type */
    Derived &derived() { return static_cast<Derived &>(*this); }
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
