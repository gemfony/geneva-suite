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

#include "hap/GRandomFactory.hpp"
#include "GFillBackend.hpp" // library-private engine selection (GPU/SIMD/scalar) + fill seam
#include "common/GChecks.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GCompletionLatch.hpp"
#include "hap/GRandomDefines.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace Gem::Hap {

/******************************************************************************/
/**
 * How long the call that starts producer threads waits before it warns about a start that has not
 * finished, and between the later warnings, in milliseconds. Constant-initialized; changed only by the
 * test hook setProducerStartWarningPeriodsForTests().
 */
namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
constinit std::atomic<std::chrono::milliseconds::rep> g_producer_start_first_warning_ms{
    std::chrono::duration_cast<std::chrono::milliseconds>(PRODUCERSTARTFIRSTWARNING).count()
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
constinit std::atomic<std::chrono::milliseconds::rep> g_producer_start_repeat_warning_ms{
    std::chrono::duration_cast<std::chrono::milliseconds>(PRODUCERSTARTREPEATWARNING).count()
};

/**
 * The state of the test hook that makes the creation of producer threads fail: whether it is on, and
 * how many creations may still succeed before the others fail. Constant-initialized; changed only by
 * failProducerThreadCreationsForTests().
 */
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
constinit std::atomic<bool> g_producer_creation_fails{false};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
constinit std::atomic<std::size_t> g_producer_creations_to_pass{0};

/**
 * Throws as a failing creation of a thread would, while the test hook asks for it
 * (failProducerThreadCreationsForTests()).
 */
void producerThreadCreationHook() {
    if(g_producer_creation_fails.load()) {
        std::size_t n_pass = g_producer_creations_to_pass.load();
        while(n_pass > 0 &&
              not g_producer_creations_to_pass.compare_exchange_weak(n_pass, n_pass - 1)) {
        }
        if(0 == n_pass) {
            throw std::system_error(
                std::make_error_code(std::errc::resource_unavailable_try_again),
                "The creation of a producer thread failed (test hook)"
            );
        }
    }
}
} // namespace

/******************************************************************************/
/**
 * The state of one start of producer threads, shared by the call that starts them and by each of them:
 * how many are at which stage of their start, and the latch that the call waits on. A producer is first
 * building its random-number engine (on the GPU: its CUDA context, stream and cuRAND generator), then
 * filling its first container; when that is done, or the producer has ended before, it counts down the
 * latch.
 */
struct GRandomFactory::GProducerStart {
    /** @brief Prepares the start of n producers @param n The number of producers */
    explicit GProducerStart(std::uint16_t n)
      : n_producers(n)
      , n_building(n)
      , finished(n) { /* nothing */
    }

    std::uint16_t n_producers;             ///< How many producers this start launched
    std::atomic<std::size_t> n_building;   ///< How many are building their engine
    std::atomic<std::size_t> n_filling{0}; ///< How many are filling their first container
    Gem::Common::Concurrency::GCompletionLatch finished; ///< Counted down by each producer once
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The destructor. All work is done in the finalize() function.
 */
GRandomFactory::~GRandomFactory() {
    // Make sure the finalization code is executed
    // (if this hasn't happened already). Calling
    // finalize() multiple times is safe.
    finalize();
}

/******************************************************************************/
/**
 * @brief Finalization code for the GRandomFactory.
 *
 * All producer threads are flagged to stop and the fresh/return buffers are
 * closed to wake any producer parked in a blocking push(); the function then
 * waits for the threads to join. This performs useful work only on the first
 * call and returns immediately on subsequent calls, so it can be called as
 * often as you wish.
 */
void GRandomFactory::finalize() {
    // Only allow one finalization action to be carried out
    if(finalized_) {
        return;
    }

    // Flag all threads to stop
    threads_stop_requested_.store(true);
    // Wake any producer parked in a blocking push() so it observes the stop flag
    // and exits -- otherwise join_all() below would deadlock on a full buffer.
    p_fresh_bfr_.close();
    p_ret_bfr_.close();
    // Wait for all threads to return
    producer_threads_.join_all();

    // The producers are joined -- return their reservation to the thread budget.
    producer_budget_.release();

    // Let the audience know
    finalized_.store(true);
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the size of random number arrays.
 *
 * @return The size of a random number array (DEFAULTARRAYSIZE)
 */
std::size_t GRandomFactory::getCurrentArraySize() {
    return DEFAULTARRAYSIZE;
}

/******************************************************************************/
/**
 * @brief Retrieves the size of the random buffer.
 *
 * The random buffer is the data structure holding the random number packages.
 *
 * @return The size of the random buffer (DEFAULTFACTORYBUFFERSIZE)
 */
std::size_t GRandomFactory::getBufferSize() {
    return DEFAULTFACTORYBUFFERSIZE;
}

/******************************************************************************/
/**
 * @brief Returns a seed from a pseudo-random sequence.
 *
 * Access is serialised by seeding_mutex_; the local seed collection is generated in
 * batches, each batch from freshly drawn non-deterministic entropy (see below), on
 * first use and once the previous batch has been exhausted.
 *
 * A seed fills the whole seed type: where that holds 64 bits (Linux), it is made of two of the
 * 32-bit words std::seed_seq yields, so that an engine seeded with it starts in one of 2^64 states.
 *
 * @return A seed taken from the current seed batch
 */
seed_type GRandomFactory::getSeed() {
    // The number of non-deterministic words the batch's seed sequence is built from
    constexpr std::size_t SEED_SEQ_ENTROPY_WORDS = 16;

    std::unique_lock<std::mutex> const sm_lck(seeding_mutex_);

    // Refill at the start of seeding or when all seeds have been used.
    //
    // Every batch MUST be generated from freshly drawn entropy: std::seed_seq::generate()
    // is a pure function of the entropy the sequence was constructed with, and it does not
    // consume or advance that entropy. A single long-lived seed_seq would therefore write
    // the very same values into seed_collection_ on every refill, so the seeds handed out
    // would repeat with a period of seed_collection_.size() -- two RNGs seeded that far
    // apart would produce identical streams.
    if(not seeding_has_started_ || seed_cit_ == seed_collection_.end()) {
        std::array<std::random_device::result_type, SEED_SEQ_ENTROPY_WORDS> entropy{};
        for(auto &entropy_word : entropy) {
            entropy_word = nondet_rng_();
        }

        // std::seed_seq yields 32-bit words: two of them make one seed
        std::seed_seq seed_seq(entropy.begin(), entropy.end());
        std::vector<std::uint32_t> words(2 * seed_collection_.size());
        seed_seq.generate(words.begin(), words.end());
        for(std::size_t i = 0; i < seed_collection_.size(); ++i) {
            const std::uint64_t seed =
                (static_cast<std::uint64_t>(words[2 * i]) << 32U) | words[(2 * i) + 1];
            seed_collection_[i] = static_cast<seed_type>(seed);
        }
        seed_cit_ = seed_collection_.begin();
        seeding_has_started_.store(true);
    }

    seed_type const result = *seed_cit_;
    ++seed_cit_;

    return result;
}

/******************************************************************************/
/**
 * @brief Allows recycling of (possibly partially used) packages.
 *
 * This way we avoid the continuous allocation and deletion of new buffers.
 * Note that this function may delete its argument (via reset) if it cannot be
 * added to the return buffer.
 *
 * @param p An rvalue unique_ptr to a partially used work package; ownership is taken (moved into the return buffer or reset)
 */
void GRandomFactory::returnUsedPackage(std::unique_ptr<random_container> &&p) {
    // We try to add the item to the p_ret_bfr_ queue.
    if(not p_ret_bfr_.try_push(std::move(p))) {
        p.reset();
    }
}

/******************************************************************************/
/**
 * @brief Sets the number of producer threads for this factory.
 *
 * A request for 0 threads auto-sizes the pool to the hardware
 * (autoProducerThreadCount()). While producer threads are already running only
 * an increase is possible: the missing threads are started on the spot -- the call
 * returns when they have finished their start (see awaitStart()) -- and the
 * stored count is updated; a requested decrease is ignored with a warning.
 * Before the first start (threads are launched lazily on the first
 * getNewRandomContainer() call) the count is simply stored.
 *
 * @param n_producer_threads The requested number of threads simultaneously producing random numbers
 */
void GRandomFactory::setNProducerThreads(const std::uint16_t &n_producer_threads) {
    // thread_creation_mutex_ orders this call against the lazy thread start in
    // getNewRandomContainer() and against concurrent setNProducerThreads() calls.
    std::unique_lock<std::mutex> const tc_lk(thread_creation_mutex_);

    // A request of 0 means "auto-size to the hardware" (not an error) -- see autoProducerThreadCount().
    const std::uint16_t n_producer_threads_local =
        (0 == n_producer_threads) ? autoProducerThreadCount() : n_producer_threads;
    const std::uint16_t n_current = n_producer_threads_.load();

    if(threads_started_) {
        if(n_producer_threads_local < n_current) { // Running threads cannot be removed
            glogger << "Attempt to decrease the number of producer threads from " << n_current
                    << " to " << n_producer_threads_local << '\n'
                    << "while threads were already running. The number of threads will remain "
                       "unchanged."
                    << '\n'
                    << GWARNING;

            return;
        }

        // Start the missing producer threads (a no-op if the count is unchanged). The wait holds
        // thread_creation_mutex_, which only another setNProducerThreads() takes once the producers run:
        // a draw does not wait here.
        if(n_producer_threads_local > n_current) {
            const auto start =
                startProducers(static_cast<std::uint16_t>(n_producer_threads_local - n_current));
            // Only the producers that could be started run and are counted
            const auto n_running = static_cast<std::uint16_t>(n_current + start->n_producers);
            accountProducers(n_running);
            n_producer_threads_ = n_running;
            awaitStart(*start);
        }
        return;
    }

    // Record the count for the lazy start of the producer threads
    n_producer_threads_ = n_producer_threads_local;
}

/******************************************************************************/
/**
 * @brief Hands out a new container of random numbers.
 *
 * The contract is stated at the declaration. The producer threads are started on first access
 * (double-checked locking; see http://preshing.com/20130930/double-checked-locking-is-fixed-in-cpp11/
 * for the rationale). The call that starts them waits for their start after it has let go of
 * thread_creation_mutex_: other threads draw from the buffer meanwhile, from the producers that have
 * finished their start. That does not weaken what the wait is for -- the call that starts the producers
 * returns only when none of them is still inside the creation of its CUDA resources; a thread that
 * merely draws starts nothing.
 *
 * @return A unique_ptr to a filled container, or a null pointer if none came within
 *   DEFAULTFACTORYGETWAIT milliseconds
 * @throws gem_exception if none came and no producer thread is left (each has ended with an error),
 *   so that a caller does not wait for ever
 */
std::unique_ptr<random_container> GRandomFactory::getNewRandomContainer() {
    // Start the producer threads upon first access to this function
    if(not threads_started_) {
        std::shared_ptr<GProducerStart> start;
        {
            std::unique_lock<std::mutex> const tc_lk(thread_creation_mutex_);
            if(not threads_started_) { // double checked locking pattern
                //---------------------------------------------------------
                // Throws only if no producer at all could be started: the factory is then as if
                // never started, and a later draw tries again. Otherwise the factory runs with the
                // producers it got, and the start counts as done.
                start = startProducers(n_producer_threads_.load());
                n_producer_threads_ = start->n_producers;
                // Account the producers in the process-wide thread budget for their lifetime.
                accountProducers(start->n_producers);
                //---------------------------------------------------------

                threads_started_.store(true);
            }
        }
        if(start) {
            awaitStart(*start);
        }
    }

    std::unique_ptr<random_container> p; // empty
    if(auto popped = p_fresh_bfr_.pop_wait(std::chrono::milliseconds(DEFAULTFACTORYGETWAIT))) {
        p = std::move(*popped);
    }
    else {
        // On timeout p stays empty -- our way of signaling a time out is an empty std::unique_ptr.
        // Count it: this is the aggregate "production could not keep up with demand" signal.
        n_get_timeouts_.fetch_add(1, std::memory_order_relaxed);

        // Nothing came, and no producer is left that could still deliver: every one has ended with an
        // error. Waiting on would last for ever, so the draw fails with the reason. (At shutdown the
        // producers end as well; finalize() has then asked them to stop, and the caller sees
        // finalized().)
        if(0 == n_living_producers_.load() && not threads_stop_requested_.load()) {
            std::string last_error;
            {
                std::unique_lock<std::mutex> const tc_lk(thread_creation_mutex_);
                last_error = last_producer_error_;
            }
            throw gem_exception(
                g_error_streamer()
                << "No producer thread of the random-number factory is left: each has ended with an"
                << " error, so no further random numbers will come." << '\n'
                << (last_error.empty() ? std::string("The error of the last one is not known.")
                                       : "The error of the last one: " + last_error)
                << '\n'
            );
        }
    }

    return p;
}

/******************************************************************************/
/**
 * @brief Starts producer threads.
 *
 * Must be called with thread_creation_mutex_ held. The caller then waits for their start with
 * awaitStart().
 *
 * If the creation of the k-th of n threads fails (0 < k < n), the start goes on with the k
 * producers that run: the returned state counts k producers, and a warning names both numbers.
 * Only if not even the first thread can be created does the call throw; nothing is then started.
 *
 * @param n_new The number of producer threads to start
 * @return The state of this start, which awaitStart() waits on; its n_producers is the number of
 *   producers that were actually started
 */
std::shared_ptr<GRandomFactory::GProducerStart>
GRandomFactory::startProducers(std::uint16_t n_new) {
    // Shared with every new producer: the latch must outlive the last count_down() (see GCompletionLatch)
    auto start = std::make_shared<GProducerStart>(n_new);
    std::uint16_t n_started = 0;
    try {
        for(; n_started < n_new; n_started++) {
            producerThreadCreationHook();
            // Counted before it runs: the producer counts itself out when it ends
            n_living_producers_.fetch_add(1);
            try {
                producer_threads_.create_thread([this, start]() {
                    this->producer(this->getSeed(), start);
                });
            }
            catch(...) {
                n_living_producers_.fetch_sub(1);
                throw;
            }
        }
    }
    catch(const std::exception &e) {
        if(0 == n_started) {
            throw;
        }
        glogger << "Only " << n_started << " of " << n_new
                << " producer thread(s) could be started:" << '\n'
                << e.what() << '\n'
                << "Random-number production goes on with " << n_started << " producer thread(s)."
                << '\n'
                << GWARNING;
    }

    // The producers that were never started neither build an engine nor count down the latch
    const auto n_missing = static_cast<std::size_t>(n_new - n_started);
    start->n_producers = n_started;
    start->n_building.fetch_sub(n_missing);
    for(std::size_t i = 0; i < n_missing; i++) {
        std::ignore = start->finished.count_down();
    }
    return start;
}

/******************************************************************************/
/**
 * @brief Makes the producers' reservation in the process-wide thread budget fit the producers that run.
 *
 * Must be called with thread_creation_mutex_ held.
 *
 * @param n_running The number of producer threads that run
 */
void GRandomFactory::accountProducers(std::uint16_t n_running) {
    producer_budget_.release();
    producer_budget_ = Gem::Common::Concurrency::threadBudget().reserve(
        "hap:producers",
        n_running,
        Gem::Common::Concurrency::ThreadElasticity::Elastic
    );
}

/******************************************************************************/
/**
 * @brief Returns when each producer of a start has finished its start.
 *
 * A producer has finished its start when it has built its fill backend -- on the GPU, the CUDA
 * stream, the cuRAND generator and the device buffer, with the cuRAND kernels loaded -- and has filled
 * its first container, or when it has ended before. While a source creates CUDA resources, the CUDA
 * libraries must not be shut down: they are unloaded at process exit before this library's
 * finalize() joins the producers, and a producer caught inside their start then hangs or aborts the
 * exit. By returning only after every start has finished, the call that starts producers -- the first
 * getNewRandomContainer() or a setNProducerThreads() that adds threads -- keeps its caller behind
 * every start, and the end of main() only if that caller is the thread that ends the program
 * (getNewRandomContainer() states the contract). The first call therefore waits for the slowest
 * producer's start instead of the fastest; on the GPU they share the creation of the CUDA context
 * and finish close together.
 *
 * The wait has no time limit: a start that never ends cannot be left behind safely. So that a wedged
 * device is seen as such, a warning names how many producers are at which stage of their start after
 * PRODUCERSTARTFIRSTWARNING, and again every PRODUCERSTARTREPEATWARNING while the wait goes on.
 *
 * @param start The state of the start, as startProducers() returned it
 */
void GRandomFactory::awaitStart(GProducerStart &start) {
    const auto begin = std::chrono::steady_clock::now();
    auto period = std::chrono::milliseconds(g_producer_start_first_warning_ms.load());
    while(not start.finished.wait_for(period)) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin
        );
        glogger
            << "The start of " << start.finished.remaining() << " of " << start.n_producers
            << " producer thread(s) has not finished after "
            << static_cast<double>(waited.count()) / 1000. << " s:" << '\n'
            << start.n_building.load()
            << " still building its random-number engine (on a GPU: its CUDA"
            << " context, stream and cuRAND generator), " << start.n_filling.load()
            << " still filling its first container." << '\n'
            << "The call that starts random-number production waits for them; a GPU that does not"
            << " answer keeps it waiting." << '\n'
            << GWARNING;
        period = std::chrono::milliseconds(g_producer_start_repeat_warning_ms.load());
    }
}

/******************************************************************************/
/**
 * @brief The production of raw random words takes place here.
 *
 * Runs as the body of a producer std::thread: it (re)fills containers from the
 * active backend -- the SIMD engine when an AVX2/NEON backend is compiled in,
 * the GPU (cuRAND) when CUDA support is built and a device is present, else the
 * scalar CPU engine -- and submits them to the fresh buffer with a blocking
 * push until shutdown closes the buffer. As this function is the body of a
 * std::thread, no exception may escape it (that would call std::terminate() and
 * bring the whole process down); every exception is therefore caught, logged as
 * a warning, and this producer thread exits cleanly while the remaining
 * producer threads keep supplying numbers. Its error is kept (last_producer_error_), and the
 * producer counts itself out of n_living_producers_, so that a draw that finds no producer left
 * fails with that error instead of waiting for ever (getNewRandomContainer()).
 *
 * @param seed A seed for this producer thread's local random number generator
 * @param start The state of the start this producer belongs to; it reports its stages there
 */
void GRandomFactory::producer(seed_type seed, std::shared_ptr<GProducerStart> start) {
    // Reports the stages of this producer's start: that its engine is built, and -- exactly once --
    // that the start has finished: after its first fill, or, through the destructor, on any path that
    // leaves the producer before it (an exception, a closed buffer).
    struct GStartReport {
        explicit GStartReport(std::shared_ptr<GProducerStart> s)
          : start(std::move(s)) { /* nothing */
        }
        std::shared_ptr<GProducerStart> start;
        bool built = false;
        void engineBuilt() {
            built = true;
            start->n_filling.fetch_add(1);
            start->n_building.fetch_sub(1);
        }
        void operator()() {
            if(start) {
                (built ? start->n_filling : start->n_building).fetch_sub(1);
                detail::gpuRandomSourceStartStage(GpuSourceStartStage::START_FINISHED);
                std::ignore = start->finished.count_down();
                start.reset();
            }
        }
        GStartReport(const GStartReport &) = delete;
        GStartReport &operator=(const GStartReport &) = delete;
        ~GStartReport() {
            (*this)();
        }
    } report_start(std::move(start));

    // Keeps this producer's error for a draw that finds no producer left. The start is reported as
    // finished first: setNProducerThreads() holds thread_creation_mutex_ while it waits for a start.
    auto keep_error = [&](std::string const &what) {
        report_start();
        std::unique_lock<std::mutex> const tc_lk(thread_creation_mutex_);
        last_producer_error_ = what;
    };

    try {
        // The fill backend picks -- once -- the fastest engine available in this
        // build (GPU cuRAND when a device is present, else the SIMD xoshiro256++,
        // else the scalar engine) and logs the CUDA decision once for the whole
        // process. It duck-types as a bulk URBG, so containers (re)fill straight
        // from it. The engine-selection / fall-back policy lives entirely in
        // GFillBackend and is shared with the staged source.
        detail::GFillBackend backend(static_cast<std::uint64_t>(seed));
        report_start.engineBuilt();

        // Fills (fresh=true) or refreshes (fresh=false) a container from the backend.
        auto fill = [&](std::unique_ptr<random_container> &cont, bool fresh) {
            if(fresh) {
                // random_container's constructor is private to its friend, this factory, so
                // make_unique cannot reach it.
                // NOLINTNEXTLINE(modernize-make-unique)
                cont.reset(new random_container(backend));
            }
            else {
                cont->refresh(backend);
            }
        };

        std::unique_ptr<random_container> p;

        while(not threads_stop_requested_) {
            // First we try to retrieve a "recycled" item from the p_ret_bfr_ buffer. If this
            // fails (likely because the buffer is empty), we create a new item instead
            if(auto recycled = p_ret_bfr_.try_pop()) {
                p = std::move(*recycled);
                // If we reach this line, we have successfully retrieved a recycled container.
                // First do some error-checking
                Gem::Common::checkThat(nullptr != p, "Got empty recycling pointer");

                // Replace "used" random numbers with new ones
                fill(p, /*fresh=*/false);
            }
            else { // O.k., so we need to create a new container
                fill(p, /*fresh=*/true);
            }
            report_start(); // a no-op after the first fill

            // Blocking submit: sleep on the buffer's not-full condition until a
            // consumer frees space, or until finalize() closes the buffer at
            // shutdown (push() then returns false, and we leave the loop). No
            // timeout polling -- producers stay fully asleep while the buffer is full.
            if(not p_fresh_bfr_.push(std::move(p))) {
                break; // buffer closed at shutdown -- leave the producer loop
            }
            n_packages_produced_.fetch_add(
                1,
                std::memory_order_relaxed
            ); // supply-throughput signal
        }
    }
    // producer() is the body of a std::thread, so no exception may escape it:
    // an exception leaving a thread's top-level function calls std::terminate()
    // and crashes the whole process. We log the condition (so it is not silent)
    // and return, letting this producer thread exit cleanly; the remaining
    // producer threads keep supplying random numbers.
    catch(...) {
        const std::string what = Gem::Common::describeException(std::current_exception());
        glogger << "Caught an exception with message" << '\n'
                << what << '\n'
                << "This producer thread will now terminate cleanly. Random-number" << '\n'
                << "production continues on the remaining producer threads, if any." << '\n'
                << GWARNING;
        keep_error(what);
    }

    // Counted out after its error is kept, so that a draw that sees no producer left finds the error
    n_living_producers_.fetch_sub(1);
}

/******************************************************************************/
/**
 * The state of the test hook that holds GPU source starts, one entry per GpuSourceStartStage. It is
 * defined before the lifecycle guard below, so it is destroyed only after the guard has joined the
 * producer threads, which may wait on it.
 */
namespace {
struct GGpuSourceStartHold {
    std::atomic<bool> held{false};         ///< Whether starting sources wait at this stage
    std::atomic<std::size_t> n_reached{0}; ///< How many sources have reached this stage
    std::atomic<std::size_t> n_free{0};    ///< n_reached below which sources pass a held stage
    std::atomic<bool> fail{false};         ///< Whether starting sources throw at this stage
    Gem::Common::Concurrency::GCompletionLatch gate; ///< Opened on release
};
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::array<GGpuSourceStartHold, 3> g_gpu_source_start_holds;

/**
 * @brief The hold entry of a stage.
 *
 * @param stage The stage
 * @return The entry of that stage
 */
GGpuSourceStartHold &gpuSourceStartHold(GpuSourceStartStage stage) {
    return g_gpu_source_start_holds.at(static_cast<std::size_t>(stage));
}
} // namespace

/******************************************************************************/
/**
 * @brief Process-lifetime guard that brackets the global random-number factory around main().
 *
 * A single object with static storage duration, living in the hap library. It is constructed during
 * this shared library's dynamic initialization -- i.e. BEFORE main() -- and destroyed at library
 * unload / static teardown -- i.e. AFTER main() returns. Its constructor brings the factory online and
 * its destructor finalizes it exactly once (joining the producer threads and closing the buffers).
 *
 * Making the factory's finalize the responsibility of this ONE library-global -- rather than of
 * every initializing object of a user -- is what keeps a short-lived one from tearing the shared
 * factory down mid-run (a finalized factory can never hand out another random-number container, so
 * every later consumer would spin/throw; see GRandomT::getNewRandomContainer()). finalize() is
 * idempotent, so the factory's own destructor calling it again when the singleton storage is
 * released is a harmless no-op.
 *
 * Ordering is correct by construction: because the RNG consumers (every library that draws from
 * hap) depend on this library, their statics are destroyed BEFORE this guard's destructor runs, so
 * nothing still draws random numbers when the producers are joined. And because the guard's
 * constructor lazily builds the factory singleton, that singleton's storage completes construction
 * during this guard's construction and is therefore destroyed AFTER it -- so randomFactory() is
 * still valid inside the guard's destructor. (This relies on hap being a shared library, whose
 * object files are all loaded; if hap is ever linked statically and this TU's guard is dropped by
 * the linker, behaviour falls back to the factory being finalized by its own singleton destructor
 * -- correct, just less deterministically timed. No functional regression either way.)
 */
namespace {
struct GRandomFactoryLifecycleGuard {
    // Acquire and HOLD a strong reference to the factory. This is what makes the destructor safe against
    // static-destruction ORDER: the factory object cannot be torn down while this guard is alive, so the
    // finalize() below always runs against a live factory (and we never re-enter the GSingletonT storage
    // at teardown, where it may already be gone). Constructing factory_ is what brings the singleton
    // online before main(); no further initialization call is needed (producer threads start lazily).
    GRandomFactoryLifecycleGuard()
      : factory_(randomFactory()) { /* see above */
    }
    ~GRandomFactoryLifecycleGuard() {
        factory_->finalize();
    }

    std::shared_ptr<GRandomFactory> factory_;
};
const GRandomFactoryLifecycleGuard g_random_factory_lifecycle_guard;
} // namespace

/******************************************************************************/
/**
 * Process-wide GPU switch, GPU source count and failure hook. Constant-initialized, so they are valid
 * before any static constructor runs and survive until the last producer has been joined.
 */
namespace {
constinit std::atomic<bool> g_gpu_random_allowed{
    true
}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
constinit std::atomic<std::size_t> g_n_gpu_random_sources{
    0
}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
constinit std::atomic<bool> g_gpu_random_failure_simulated{
    false
}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
} // namespace

void setGpuRandomNumbersAllowed(bool allowed) noexcept {
    g_gpu_random_allowed.store(allowed);
}

bool gpuRandomNumbersAllowed() noexcept {
    return g_gpu_random_allowed.load();
}

std::size_t nGpuRandomSources() noexcept {
    return g_n_gpu_random_sources.load();
}

void simulateGpuRandomFailureForTests(bool fail) {
    g_gpu_random_failure_simulated.store(fail);
}

void failProducerThreadCreationsForTests(bool fail, std::size_t n_pass) {
    g_producer_creation_fails.store(false);
    g_producer_creations_to_pass.store(n_pass);
    g_producer_creation_fails.store(fail);
}

void setProducerStartWarningPeriodsForTests(
    std::chrono::milliseconds first,
    std::chrono::milliseconds repeat
) {
    g_producer_start_first_warning_ms.store(first.count());
    g_producer_start_repeat_warning_ms.store(repeat.count());
}

namespace detail {
void countGpuRandomSource(bool engaged) noexcept {
    if(engaged) {
        g_n_gpu_random_sources.fetch_add(1);
    }
    else {
        g_n_gpu_random_sources.fetch_sub(1);
    }
}

bool gpuRandomFailureSimulated() noexcept {
    return g_gpu_random_failure_simulated.load();
}

/**
 * Marks that a bulk source starting on the GPU has reached a stage of its start, and holds it there while
 * a test asks for it (holdGpuRandomSourceStartsForTests()). Only called while a source starts, never on
 * a refill.
 *
 * @param stage The stage the source has reached
 */
void gpuRandomSourceStartStage(GpuSourceStartStage stage) {
    auto &h = gpuSourceStartHold(stage);
    const std::size_t n_before = h.n_reached.fetch_add(1);
    if(h.held.load() && n_before >= h.n_free.load()) {
        // Bounded, so that a test that forgets to release the stage cannot hang the process forever
        std::ignore = h.gate.wait_for(std::chrono::seconds(60));
    }
    if(h.fail.load() && stage != GpuSourceStartStage::START_FINISHED) {
        throw std::runtime_error("A start of a GPU random-number source failed (test hook)");
    }
}
} /* namespace detail */

/**
 * Holds bulk sources that start on the GPU at the given stage, or lets the held ones go.
 *
 * @param stage The stage to hold or to release
 * @param hold Whether starting sources should wait at that stage
 * @param n_pass How many sources may pass the held stage before the others wait
 */
void holdGpuRandomSourceStartsForTests(GpuSourceStartStage stage, bool hold, std::size_t n_pass) {
    auto &h = gpuSourceStartHold(stage);
    if(hold) {
        if(not h.held.load()) {
            h.gate.arm(1);
            h.n_free.store(h.n_reached.load() + n_pass);
            h.held.store(true);
        }
    }
    else if(h.held.exchange(false)) {
        std::ignore = h.gate.count_down();
    }
}

/**
 * Makes bulk sources that start on the GPU throw at the given stage, or stops doing so.
 *
 * @param stage The stage at which starting sources throw
 * @param fail Whether they throw there
 */
void failGpuRandomSourceStartsForTests(GpuSourceStartStage stage, bool fail) {
    gpuSourceStartHold(stage).fail.store(fail);
}

/**
 * Reports how many bulk sources have reached the given stage of a GPU start since the process began.
 *
 * @param stage The stage asked about
 * @return The number of sources that have reached the stage
 */
std::size_t nGpuRandomSourceStartsForTests(GpuSourceStartStage stage) {
    return gpuSourceStartHold(stage).n_reached.load();
}

/******************************************************************************/

} /* namespace Gem::Hap */
