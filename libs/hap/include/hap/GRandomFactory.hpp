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

// Standard headers go here

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <type_traits>
#include <sstream>
#include <thread>

// Geneva headers go here

#include "common/concurrency/GMPMCQueueT.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GSingletonT.hpp"
#include "common/concurrency/GThreadBudget.hpp"
#include "common/concurrency/GThreadGroup.hpp"
#include "hap/GRandomDefines.hpp"
#include "hap/GXoshiro256pp.hpp"

/******************************************************************************/

namespace Gem::Hap {

// CPU engine: xoshiro256++ (local implementation in GXoshiro256pp.hpp).
// 64-bit output with a 32-byte state (vs ~2.5 KiB for the Mersenne twister),
// giving far better cache locality in the producer thread. Any
// std::uniform_random_bit_generator (e.g. std::mt19937_64) can be substituted
// here; every call site is engine-agnostic.
using G_CPU_BASE_GENERATOR = xoshiro256pp;

/**
 * @brief Name of the compiled-in public CPU engine (G_CPU_BASE_GENERATOR).
 *
 * For diagnostics / benchmark labelling; reflects exactly what consumers get.
 * A substituted engine (see G_CPU_BASE_GENERATOR above) is labelled "unknown"
 * rather than guessed at -- whoever substitutes an engine extends this label.
 *
 * @return A static string literal naming the engine ("xoshiro256++" or "unknown")
 */
inline const char *cpuEngineName() noexcept {
    if constexpr (std::is_same_v<G_CPU_BASE_GENERATOR, xoshiro256pp>) {
        return "xoshiro256++";
    } else {
        return "unknown";
    }
}

class GRandomFactory; // Forward declaration, so we can make random_container constructor private

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A fixed-size package of @f$B@f$ pre-generated raw random words, handed to a QUEUE proxy.
 *
 * A @c random_container is the unit of work the factory ships to a @c GRandomT<QUEUE> proxy: a flat
 * array of @f$B=@f$ @c DEFAULTARRAYSIZE 64-bit words plus a read cursor. The proxy consumes it one
 * word at a time and returns the empty shell for recycling. It does minimal error checking (it is
 * an internal hot-path type) and is @b not thread-safe: exactly one proxy owns a container at a
 * time, so no synchronization is needed on it -- all cross-thread coordination lives in the
 * factory's bounded buffers instead.
 *
 * @par Data structure
 * @verbatim
   random_container
   +-------------------------------------------+
   | r_ : array<uint64_t, B>   (B = DEFAULTARRAYSIZE)
   | current_pos_ : read cursor 0..B           |
   +-------------------------------------------+
     fill_from(rng,n): bulk-generate n words   (one SIMD/cuRAND call when rng supports it)
     next(): return r_[current_pos_++]         empty() == (current_pos_ >= B)
   @endverbatim
 *
 * @par Algorithm -- bulk fill, scalar serve
 * Filling is delegated to the engine's bulk path when available: @c fill_from() uses
 * @f$\texttt{rng.generate}(r\_,n)@f$ if the engine exposes it (the SIMD / cuRAND backends do --
 * one vectorised or device call fills many words), and otherwise draws @f$n@f$ words one at a time.
 * Serving is then a single array read per draw, @f$\texttt{next}()=r\_[\,p{+}{+}\,]@f$. Recycling
 * regenerates only the consumed prefix: @c refresh() refills @f$[0,\texttt{current\_pos\_})@f$ and
 * resets the cursor, so a returned container costs only as much as was actually used.
 */
class random_container {
    friend class
        GRandomFactory; // Needed so we can prevent construction of containers outside of the factory

public:
    /***************************************************************************/
    // Deleted constructors and assignment operators

    random_container() = delete; ///< The default constructor -- intentionally deleted
    random_container(const random_container &) =
        delete; ///< The copy constructor -- intentionally deleted
    random_container(random_container &&) =
        delete; ///< The move constructor -- intentionally deleted
    random_container &
    operator=(const random_container &) = delete; ///< intentionally deleted
    random_container &
    operator=(random_container &&) = delete; ///< Intentionally deleted

    /***************************************************************************/
    /** @brief The destructor */
    ~random_container() = default;

    /***************************************************************************/
    /**
	  * @brief Returns the size of the buffer.
	  *
	  * @return The fixed number of random numbers a full container holds (DEFAULTARRAYSIZE)
	  */
    static std::size_t size() {
        return DEFAULTARRAYSIZE;
    }

    /***************************************************************************/
    /**
	  * @brief Returns the current read position within the buffer.
	  *
	  * @return The index of the next random number to be handed out
	  */
    [[nodiscard]] std::size_t getCurrentPosition() const {
        return current_pos_;
    }

    /***************************************************************************/
    /**
	  * @brief Allows to check whether the buffer has run empty.
	  *
	  * @return true if every random number in the container has been consumed, false otherwise
	  */
    [[nodiscard]] bool empty() const {
        return (current_pos_ >= DEFAULTARRAYSIZE);
    }

    /***************************************************************************/
    /**
	  * @brief Returns the next random number from the package.
	  *
	  * Advances the internal read position. In DEBUG builds an exhausted
	  * container throws; in release builds the caller must ensure the container
	  * is not empty() beforehand.
	  *
	  * @return The next raw random value, then increments the read position
	  */
    G_CPU_BASE_GENERATOR::result_type next() {
#ifdef DEBUG
        if(empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In random_container::next(): Error!" << '\n'
                << "Invalid current_pos_: " << current_pos_ << " / " << DEFAULTARRAYSIZE
                << '\n'
            );
        }
#endif

        return r_[current_pos_++];
    }

private:
    /***************************************************************************/
    /**
	  * Fills the first n entries of the buffer. If the engine exposes a bulk
	  * generate(dst, n) method (the SIMD bulk-refill engines do), it is used —
	  * one vectorised call fills many words. Otherwise (plain
	  * std::uniform_random_bit_generator, e.g. the scalar xoshiro256++ or
	  * std::mt19937_64) values are drawn one at a time. Selected at compile
	  * time via a requires-expression; no virtual dispatch.
	  *
	  * @tparam RNG A std::uniform_random_bit_generator, or a bulk engine exposing generate(dst, n)
	  * @param rng The random number generator used to fill the buffer
	  * @param n The number of entries (from the front of the buffer) to fill
	  */
    template <typename RNG>
    void fill_from(RNG &rng, std::size_t n) {
        if constexpr (requires { rng.generate(r_.data(), n); }) {
            rng.generate(r_.data(), n);
        }
        else {
            std::generate(r_.begin(), r_.begin() + n, [&]() { return rng(); });
        }
    }

    /***************************************************************************/
    /**
	  * @brief Constructs the container and fills it completely from the generator.
	  *
	  * Private (only the friend GRandomFactory may construct containers). Wraps
	  * the fill in try/catch and rethrows allocation or unknown failures as a
	  * geneva_exception.
	  *
	  * @tparam RNG A std::uniform_random_bit_generator, or a bulk engine exposing generate(dst, n)
	  * @param rng A reference to an external random number generator used to fill the whole buffer
	  */
    template <typename RNG>
    explicit random_container(RNG &rng) {
        // fill_from writes into the in-object std::array -- it allocates nothing, so there is
        // no bad_alloc to guard against here (the producer's `new` has its own catch-all).
        fill_from(rng, r_.size());
    }

    /***************************************************************************/
    /**
	  * @brief Replaces "used" random numbers by new numbers and resets the read position.
	  *
	  * Only the already-consumed leading entries (up to current_pos_) are
	  * regenerated, then the read position is reset to zero so the container can
	  * be recycled.
	  *
	  * @tparam RNG A std::uniform_random_bit_generator, or a bulk refill engine exposing generate(dst, n)
	  * @param rng A reference to the random number generator used to refill the consumed entries
	  */
    template <typename RNG>
    void refresh(RNG &rng) {
        fill_from(rng, current_pos_);
        current_pos_ = 0;
    }
    /***************************************************************************/

    std::size_t current_pos_ = 0; ///< The current position in the array
    std::array<G_CPU_BASE_GENERATOR::result_type, DEFAULTARRAYSIZE>
        r_{}; ///< Holds the actual random numbers
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The process-global producer/consumer factory that backs the QUEUE source.
 *
 * Rather than seed a generator per consumer (the historical bottleneck), a single factory runs a
 * pool of producer threads that bulk-fill @c random_container packages and publish them on a
 * bounded "fresh" buffer; QUEUE proxies pop full packages and push the empty shells back on a
 * "return" buffer for recycling. Producers thus run ahead of demand and -- crucially for the
 * bursty EA workload -- keep refilling during the evaluation gaps between bursts, so a burst finds
 * packages already waiting. A single instance exists per process (a @c GSingletonT, reached via
 * @c randomFactory()); it also vends seeds (@c getSeed()) to the LOCAL / STAGED / QUARANTINE
 * sources.
 *
 * @par Data structure
 * @verbatim
   n producer threads                  bounded buffers                    QUEUE proxies
   +-----------------+                                                     +-----------+
   | GFillBackend    |  full pkg  ===> [ p_fresh_bfr_ : MPMC ] ==pop==>   |  proxy A  |
   | (GPU/SIMD/CPU)  |                                                     +-----------+
   |  fill package   |  <== reuse  <== [ p_ret_bfr_ : MPMC ] <==push==     |  proxy B  |
   +-----------------+   empty shells                                      +-----------+
        ^  refills while consumers are in their evaluation GAP
   @endverbatim
 *
 * @par Algorithm
 * Each producer thread loops: take a recycled shell from the return buffer (or allocate one),
 * fill it through the shared @c detail::GFillBackend (one SIMD or cuRAND bulk call per package),
 * and block-push it onto the fresh buffer; at shutdown the buffers close and the loop exits. A
 * producer never lets an exception escape (that would call @c std::terminate); it logs and the
 * remaining producers carry on. Consumers (@c getNewRandomContainer()) pop with a timeout and
 * retry, so the path is wait-free of the consumer's logic. The factory's getSeed() hands each
 * producer and each non-QUEUE source a distinct seed, so streams do not overlap. The raw engine is
 * xoshiro256++ (scalar in the header; SIMD / cuRAND inside @c GFillBackend) -- in an evolutionary
 * algorithm the geometry of the quality surface tolerates fast generators, so throughput is
 * favoured over cryptographic strength.
 */
class GRandomFactory {
public:
    /** @brief The default constructor. The single instance is created lazily by
     *  randomFactory() through the GSingletonT lifetime manager. */
    GRandomFactory() = default;
    /** @brief The destructor */
    ~GRandomFactory();

    /***************************************************************************/
    // Prevent copying and moving

    GRandomFactory(GRandomFactory const &) = delete;
    GRandomFactory(GRandomFactory &&) = delete;
    GRandomFactory &operator=(GRandomFactory const &) = delete;
    GRandomFactory &operator=(GRandomFactory &&) = delete;

    /***************************************************************************/

    /** @brief Finalization code for the GRandomFactory (stops producer threads and cleans up) */
    void finalize();

    /**
     * @brief Whether finalize() has run, i.e. the factory is permanently shut down.
     *
     * A finalized factory has closed (terminally) its producer/return buffers and joined its
     * producer threads, so getNewRandomContainer() will never hand out another container again -- the
     * distinction a consumer needs to tell a transient "buffer momentarily empty" (retry) apart from a
     * terminal "no more random numbers will ever come" (stop retrying). Consumers use this to fail
     * fast instead of spinning forever once the factory is down (e.g. during process teardown).
     *
     * @return true once finalize() has completed; false while the factory is live
     */
    [[nodiscard]] bool finalized() const { return finalized_.load(); }

    /**
     * @brief Sets the number of producer threads for this factory.
     *
     * @param n_producer_threads The desired number of threads producing random number packages
     */
    //! [hap-factory-tuning#1]
    void setNProducerThreads(const std::uint16_t &n_producer_threads);
    //! [hap-factory-tuning#1]

    /**
     * @brief Retrieves the current number of producer threads.
     *
     * Before the lazy thread start this is the count the first getNewRandomContainer() call
     * will launch; afterwards it equals the size of the running producer pool.
     *
     * @return The number of threads simultaneously producing random number packages
     */
    [[nodiscard]] std::uint16_t getNProducerThreads() const noexcept {
        return n_producer_threads_.load();
    }

    /**
     * @brief Allows to retrieve the size of the random number array held by each container.
     *
     * @return The number of random numbers in a full container
     */
    static std::size_t getCurrentArraySize();

    /**
     * @brief Allows to retrieve the size of the bounded buffer of containers.
     *
     * @return The maximum number of containers the factory's buffer can hold
     */
    static std::size_t getBufferSize();

    /**
     * @brief Delivers a new random number container with the current standard size to clients.
     *
     * @return A unique_ptr to a filled container, or a null pointer if none became
     *         available within the internal timeout
     */
    std::unique_ptr<random_container> getNewRandomContainer();
    /**
     * @brief Retrieval of a new seed for external or internal random number generators.
     *
     * Thread-safe; hands out a unique seed from the pre-calculated seed collection.
     *
     * @return A fresh seed value
     */
    seed_type getSeed();

    /**
     * @brief Allows recycling of partially used packages.
     *
     * Returns a (possibly partially consumed) container to the factory so its
     * used entries can be refilled and the container reused.
     *
     * @param p A unique_ptr (moved-from) to the container being returned for recycling
     */
    void returnUsedPackage(std::unique_ptr<random_container> &&p);

    /**
     * @brief The number of random-number packages the producer threads have delivered to the fresh
     * buffer over the factory's lifetime.
     *
     * A supply-throughput signal: it climbs continuously while producers keep the buffer fed. Read
     * together with getNGetTimeouts() to reason about whether production keeps up with demand.
     *
     * @return The lifetime count of packages delivered to the fresh buffer
     */
    //! [hap-factory-tuning#3]
    [[nodiscard]] std::uint64_t getNPackagesProduced() const noexcept { return n_packages_produced_.load(std::memory_order_relaxed); }
    //! [hap-factory-tuning#3]

    /**
     * @brief The number of times getNewRandomContainer() returned empty because no package became
     * available within the internal timeout -- i.e. production could not keep up with demand.
     *
     * This is the aggregate starvation signal: every consumer-side retry (a proxy waiting for a new
     * package) corresponds to one such timeout here. A non-zero and rising value indicates the
     * producer pool is under-supplying its consumers (consider more producer threads).
     *
     * @return The lifetime count of empty (timed-out) getNewRandomContainer() results
     */
    //! [hap-factory-tuning#4]
    [[nodiscard]] std::uint64_t getNGetTimeouts() const noexcept { return n_get_timeouts_.load(std::memory_order_relaxed); }
    //! [hap-factory-tuning#4]

private:
    /**
     * @brief The production of random number packages takes place here.
     *
     * Runs in a dedicated producer thread, continuously filling fresh containers
     * (and refilling recycled ones) until a stop is requested.
     *
     * @param seed The seed used to initialize this producer thread's engine
     */
    void producer(std::uint32_t seed);

    std::atomic<std::uint64_t> n_packages_produced_{0}; ///< Lifetime count of packages delivered to the fresh buffer (supply-throughput signal)
    std::atomic<std::uint64_t> n_get_timeouts_{0}; ///< Lifetime count of empty getNewRandomContainer() results (aggregate starvation signal)

    std::atomic<bool> finalized_{false};
    std::atomic<bool> threads_started_{false}; ///< Indicates whether threads were already started
    std::atomic<bool> threads_stop_requested_{false}; ///< Indicates whether all threads were requested to stop
    std::atomic<std::uint16_t> n_producer_threads_{
        autoProducerThreadCount()
    }; ///< The number of threads used to produce random numbers (hardware-derived by default)

    /// The producers' reservation in the process-wide thread budget (taken when the producer
    /// threads are lazily started, returned after they are joined in finalize()).
    Gem::Common::Concurrency::GThreadBudget::Reservation producer_budget_;

    Gem::Common::Concurrency::GThreadGroup
        producer_threads_; ///< A thread group that holds the raw-random-word producer threads

    /** @brief A bounded buffer holding the random number packages. The queue backend is selected at
     *  compile time by FACTORYQUEUEBACKEND (default: the std::deque-backed queue -- unchanged
     *  behaviour; switchable to the preallocated ring via GENEVA_HAP_FACTORY_QUEUE_PREALLOCATED). */
    Gem::Common::Concurrency::GMPMCQueueT<std::unique_ptr<random_container>, DEFAULTFACTORYBUFFERSIZE, FACTORYQUEUEBACKEND>
        p_fresh_bfr_; // Note: Absolutely needs to be defined after the thread group !!!
    /** @brief A bounded buffer holding random number packages ready for recycling */
    Gem::Common::Concurrency::GMPMCQueueT<std::unique_ptr<random_container>, DEFAULTFACTORYBUFFERSIZE, FACTORYQUEUEBACKEND>
        p_ret_bfr_;

    mutable std::mutex
        thread_creation_mutex_; ///< Synchronization of access to the threads_started_ variable

    /// Source of non-deterministic random numbers. getSeed() draws a fresh set of entropy words
    /// from it for every seed batch -- a std::seed_seq built once and kept would regenerate the
    /// same batch each time (see the comment in GRandomFactory::getSeed()).
    std::random_device nondet_rng_;

    mutable std::mutex seeding_mutex_; ///< Regulates start-up of the seeding process
    std::vector<seed_type> seed_collection_ =
        std::vector<seed_type>(DEFAULTSEEDVECTORSIZE); ///< Holds pre-calculated seeds
    std::vector<seed_type>::const_iterator seed_cit_ =
        seed_collection_.begin(); ///< Iterator over the seed_collection_
    std::atomic<bool> seeding_has_started_{false};
};

/******************************************************************************/
/**
 * A single, global GRandomFactory exists as a singleton. Access it through
 * randomFactory(), which forwards to the GSingletonT<GRandomFactory> lifetime
 * manager. This type-safe, namespaced function replaces the former GRANDOMFACTORY
 * macro. The factory is thread-safe and handed out by shared_ptr, so it is created
 * once on first access and shared for the life of the process (its destructor joins
 * the producer threads at static teardown); there is deliberately no reset facility.
 *
 * @return A shared_ptr to the single, global GRandomFactory instance
 */
[[nodiscard]] inline std::shared_ptr<GRandomFactory> randomFactory() {
    return Gem::Common::GSingletonT<GRandomFactory>::instance();
}

} /* namespace Gem::Hap */
