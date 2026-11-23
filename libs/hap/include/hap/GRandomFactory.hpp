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
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <type_traits>

// Geneva headers go here

#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GSingletonT.hpp"
#include "common/concurrency/GCompletionLatchT.hpp"
#include "common/concurrency/GMPMCQueueT.hpp"
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
    if constexpr(std::is_same_v<G_CPU_BASE_GENERATOR, xoshiro256pp>) {
        return "xoshiro256++";
    }
    else {
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
    random_container &operator=(const random_container &) = delete; ///< intentionally deleted
    random_container &operator=(random_container &&) = delete;      ///< Intentionally deleted

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
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In random_container::next(): Error!" << '\n'
                << "Invalid current_pos_: " << current_pos_ << " / " << DEFAULTARRAYSIZE << '\n'
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
	  * @tparam rng_type A std::uniform_random_bit_generator, or a bulk engine exposing generate(dst, n)
	  * @param rng The random number generator used to fill the buffer
	  * @param n The number of entries (from the front of the buffer) to fill
	  */
    template <typename rng_type>
    void fill_from(rng_type &rng, std::size_t n) {
        if constexpr(requires { rng.generate(r_.data(), n); }) {
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
	  * gem_exception.
	  *
	  * @tparam rng_type A std::uniform_random_bit_generator, or a bulk engine exposing generate(dst, n)
	  * @param rng A reference to an external random number generator used to fill the whole buffer
	  */
    template <typename rng_type>
    explicit random_container(rng_type &rng) {
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
	  * @tparam rng_type A std::uniform_random_bit_generator, or a bulk refill engine exposing generate(dst, n)
	  * @param rng A reference to the random number generator used to refill the consumed entries
	  */
    template <typename rng_type>
    void refresh(rng_type &rng) {
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
    [[nodiscard]] bool finalized() const {
        return finalized_.load();
    }

    /**
     * @brief Sets the number of producer threads for this factory.
     *
     * Threads added while the producers run are started at once, and the call returns when each of them
     * has finished its start, as the first getNewRandomContainer() does: without a time limit, with a
     * warning while it waits. If only some of the added threads can be created, those run and are
     * counted; if none can, the call throws and the count stays as it was.
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
     */
    static std::size_t getCurrentArraySize();

    /**
     * @brief Allows to retrieve the size of the bounded buffer of containers.
     */
    static std::size_t getBufferSize();

    /**
     * @brief Delivers a new random number container with the current standard size to clients.
     *
     * The first call starts the producer threads and waits until each of them has finished its start
     * -- has built its engine and filled its first container, or has ended -- so that no producer is
     * still creating GPU resources when the program can end. That wait has no time limit; if it takes
     * longer than PRODUCERSTARTFIRSTWARNING, a warning names what is waited for, and again every
     * PRODUCERSTARTREPEATWARNING. Other threads that draw meanwhile are served from the producers that
     * have finished their start. Only the call that starts the producers is held behind their
     * start: a program must not end -- return from main() or call std::exit() -- while another of
     * its threads is still in a call to hap. Every call, the first after its wait included, then
     * waits at most DEFAULTFACTORYGETWAIT milliseconds for a filled container and returns a null
     * pointer if none came (counted in getNGetTimeouts()); callers retry.
     *
     * If only k of the n producer threads can be created, the factory runs with those k: the
     * producer count (getNProducerThreads()) and the reservation in the thread budget become k, a
     * warning names both numbers, and the start counts as done. If not one can be created, the call
     * throws, and the factory stays as if never started: a later call tries again.
     */
    std::unique_ptr<random_container> getNewRandomContainer();
    /**
     * @brief Retrieval of a new seed for external or internal random number generators.
     *
     * Thread-safe; hands out a unique seed from the pre-calculated seed collection.
     */
    seed_type getSeed();

    /**
     * @brief Allows recycling of partially used packages.
     *
     * Returns a (possibly partially consumed) container to the factory so its
     * used entries can be refilled and the container reused.
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
    [[nodiscard]] std::uint64_t getNPackagesProduced() const noexcept {
        return n_packages_produced_.load(std::memory_order_relaxed);
    }
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
    [[nodiscard]] std::uint64_t getNGetTimeouts() const noexcept {
        return n_get_timeouts_.load(std::memory_order_relaxed);
    }
    //! [hap-factory-tuning#4]

private:
    /// The state of one start of producer threads (defined in the source file)
    struct GProducerStart;

    /**
     * @brief Starts producer threads.
     */
    std::shared_ptr<GProducerStart> startProducers(std::uint16_t n_new);

    /**
     * @brief Makes the producers' reservation in the thread budget fit the producers that run.
     */
    void accountProducers(std::uint16_t n_running);

    /**
     * @brief Returns when each producer of a start has finished its start; warns while it waits.
     */
    void awaitStart(GProducerStart &start);

    /**
     * @brief The production of random number packages takes place here.
     *
     * Runs in a dedicated producer thread, continuously filling fresh containers
     * (and refilling recycled ones) until a stop is requested.
     */
    void producer(seed_type seed, std::shared_ptr<GProducerStart> start);

    std::atomic<std::uint64_t> n_packages_produced_{
        0
    }; ///< Lifetime count of packages delivered to the fresh buffer (supply-throughput signal)
    std::atomic<std::uint64_t> n_get_timeouts_{
        0
    }; ///< Lifetime count of empty getNewRandomContainer() results (aggregate starvation signal)

    std::atomic<bool> finalized_{false};
    /// Whether the producer threads have been launched (their start may still run)
    std::atomic<bool> threads_started_{false};
    std::atomic<bool> threads_stop_requested_{
        false
    }; ///< Indicates whether all threads were requested to stop
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
     *  behaviour; switchable to the preallocated ring via GEM_HAP_FACTORY_QUEUE_PREALLOCATED). */
    Gem::Common::Concurrency::GMPMCQueueT<
        std::unique_ptr<random_container>,
        DEFAULTFACTORYBUFFERSIZE,
        FACTORYQUEUEBACKEND>
        p_fresh_bfr_; // Note: Absolutely needs to be defined after the thread group !!!
    /** @brief A bounded buffer holding random number packages ready for recycling */
    Gem::Common::Concurrency::GMPMCQueueT<
        std::unique_ptr<random_container>,
        DEFAULTFACTORYBUFFERSIZE,
        FACTORYQUEUEBACKEND>
        p_ret_bfr_;

    /// Orders the start of producer threads: the first getNewRandomContainer() and
    /// setNProducerThreads(). Not held by the first draw while it waits for the start.
    mutable std::mutex thread_creation_mutex_;

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
 * @brief Allows or forbids random-number generation on the GPU for this process.
 *
 * In a build with the CUDA random-number backend, every bulk source refills its containers on the GPU
 * when a device is present. Processes that draw few random numbers, such as the clients of a networked
 * run, gain nothing from that but each pay for a CUDA context; many of them on one GPU exhaust its
 * memory. Calling this function with @c false makes every source use the CPU engine instead. Sources
 * that already draw from the GPU switch to the CPU at their next refill. The default is @c true. In a
 * build without the CUDA backend the setting has no effect.
 *
 * @param allowed Whether GPU random-number generation is allowed
 */
void setGpuRandomNumbersAllowed(bool allowed) noexcept;

/**
 * @brief Reports whether GPU random-number generation is allowed for this process.
 *
 * @return The value last set through setGpuRandomNumbersAllowed(), @c true by default
 */
[[nodiscard]] bool gpuRandomNumbersAllowed() noexcept;

/**
 * @brief Reports how many bulk random-number sources currently draw from the GPU.
 *
 * A source counts from the moment it has chosen the GPU, before its CUDA resources exist, and stops
 * counting when it switches to the CPU, either because GPU use was forbidden or because a CUDA call
 * failed. Once setGpuRandomNumbersAllowed(false) has returned and this count has been seen at 0, no
 * source uses the GPU and none begins to. Always 0 in a build without the CUDA backend.
 *
 * @return The number of sources that currently use the GPU
 */
[[nodiscard]] std::size_t nGpuRandomSources() noexcept;

/**
 * @brief Makes every GPU source behave as if its next CUDA call failed (test hook).
 *
 * Only available in builds with GEM_TESTING; otherwise it reports that the hook is unavailable.
 *
 * @param fail Whether GPU calls should be treated as failed
 */
void simulateGpuRandomFailureForTests(bool fail);

/**
 * @brief The stages a bulk random-number source passes when it starts on the GPU (test hook).
 *
 * START_FINISHED is reached by every producer thread of the factory, on the GPU or the CPU, once its
 * source has filled its first container (or the producer has ended before).
 */
enum class GpuSourceStartStage {
    GPU_CHOSEN = 0,       ///< The source has found GPU use allowed and a device present
    CONTEXT_CREATION = 1, ///< The source is about to create its CUDA resources (the slow step)
    START_FINISHED = 2    ///< A producer has finished its start
};

/**
 * @brief Holds every bulk source that starts on the GPU at the given stage, or lets the held ones go
 * (test hook).
 *
 * Starting sources wait at a held stage until it is released, or for at most 60 seconds; the first
 * @p n_pass sources that reach the stage after it was held pass without waiting. Call it from one
 * thread only, and release every stage it held before the process ends. Only available in builds with
 * GEM_TESTING; otherwise it reports that the hook is unavailable.
 */
void holdGpuRandomSourceStartsForTests(
    GpuSourceStartStage stage,
    bool hold,
    std::size_t n_pass = 0
);

/**
 * @brief Makes every bulk source that starts on the GPU throw when it reaches the given stage, as a
 * failing call there would, or stops doing so (test hook).
 *
 * The exception is a std::runtime_error; the producer whose source throws ends, as on any other
 * exception. START_FINISHED cannot be made to throw. Only available in builds with GEM_TESTING;
 * otherwise it reports that the hook is unavailable.
 */
void failGpuRandomSourceStartsForTests(GpuSourceStartStage stage, bool fail);

/**
 * @brief Reports how many bulk sources have reached the given stage of a GPU start since the process
 * began, including those held there (test hook).
 *
 * Only available in builds with GEM_TESTING; otherwise it reports that the hook is unavailable.
 */
[[nodiscard]] std::size_t nGpuRandomSourceStartsForTests(GpuSourceStartStage stage);

/**
 * @brief Makes the creation of the factory's producer threads fail, as a lack of system resources
 * would, or stops doing so (test hook).
 *
 * While it is on, the first @p n_pass creations from the call on succeed and every later one throws a
 * std::system_error. Only available in builds with GEM_TESTING; otherwise it reports that the hook is
 * unavailable.
 *
 * @param fail Whether creations fail
 * @param n_pass How many creations succeed before the others fail
 */
void failProducerThreadCreationsForTests(bool fail, std::size_t n_pass = 0);

/**
 * @brief Sets how long the call that starts producer threads waits before its first warning about a
 * start that has not finished, and how long between the warnings that follow (test hook).
 *
 * The defaults are PRODUCERSTARTFIRSTWARNING and PRODUCERSTARTREPEATWARNING. Only available in builds
 * with GEM_TESTING; otherwise it reports that the hook is unavailable.
 *
 * @param first The wait before the first warning
 * @param repeat The wait between later warnings
 */
void setProducerStartWarningPeriodsForTests(
    std::chrono::milliseconds first,
    std::chrono::milliseconds repeat
);

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
