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

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

// Boost headers go here

// Geneva headers go here
#include "common/GLogger.hpp"
#include "hap/GRandomBase.hpp"
#include "hap/GRandomDefines.hpp"
#include "hap/GQuarantineSource.hpp"
#include "hap/GStagedSource.hpp"

#include <array>
#include <cstdint>

namespace Gem::Hap {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Access to different random number distributions, whose "raw" material is
 * produced in different ways. We only define the interface here. The actual
 * implementation can be found in the (partial) specializations of this class.
 */
template <Gem::Hap::randomSource s = Gem::Hap::randomSource::QUEUE>
class GRandomT : public Gem::Hap::GRandomBase {
public:
    /***************************************************************************/
    // This class is not meant to be used.

    GRandomT() = delete;

    GRandomT(GRandomT const &) = delete;
    GRandomT(GRandomT &&) = delete;

    GRandomT &operator=(GRandomT const &) = delete;
    GRandomT &operator=(GRandomT &&) = delete;

    /***************************************************************************/

    ~GRandomT() override = default;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief QUEUE proxy: hands out raw words from container packages produced by the global factory.
 *
 * The default source. The proxy holds one @c random_container (a package of @f$B=@f$
 * @c DEFAULTARRAYSIZE 64-bit words) borrowed from the process-global Gem::Hap::GRandomFactory.
 * It serves the package one word at a time; when the package is exhausted it returns the empty
 * shell to the factory for recycling and acquires a fresh, full one. GRandomBase layers the
 * distributions on top of this raw stream. Copy/move are dedicated to keeping each instance's
 * stream unique (see the members); the source object is otherwise ignored.
 *
 * @par Data structure
 * @verbatim
   factory producer threads          MPMC "fresh" queue        this proxy
   (fill packages in bulk)                                      +-----------------+
        [pkg][pkg][pkg]  --push-->  [pkg][pkg]...[pkg]  --pop-->| p_  [B words]   |
            ^                                                    | next(): r_[i++] |
            |                                                    +-----------------+
            |   recycled empty shells                                 |
            +------------------- "return" buffer <--- returnUsedPackage(empty)
   @endverbatim
 *
 * @par Algorithm
 * Each draw advances a read cursor @f$i@f$ into the held package and refills transparently on
 * exhaustion:
 * @f[
 *   \texttt{int\_random}() =
 *   \begin{cases}
 *     p\,[\,i{+}{+}\,] & \text{if } i < B,\\[2pt]
 *     \text{return } p \text{ to the factory, acquire a fresh } p,\ i\leftarrow 0,\ \text{then } p\,[\,i{+}{+}\,] & \text{if } i = B.
 *   \end{cases}
 * @f]
 * Bulk production happens on the factory's own threads, off the consumer's path; see
 * Gem::Hap::GRandomFactory for the producer / bounded-buffer / recycle pipeline. Amortized cost
 * is one array read per draw plus one queue pop per @f$B@f$ draws.
 *
 * @par Concurrency
 * The proxy itself is single-consumer (one per thread); all cross-thread synchronization lives in
 * the factory's lock-based bounded buffers (no benign race here -- contrast STAGED / QUARANTINE).
 */
template <>
class GRandomT<Gem::Hap::randomSource::QUEUE> : public Gem::Hap::GRandomBase {
public:
    /***************************************************************************/
    /**
	 * @brief Default constructor; acquires the factory and a first random number package.
	 *
	 * Note that getNewRandomContainer() may throw.
	 */
    GRandomT() noexcept(false)
      : grf_(randomFactory()) // Make sure we have a local pointer to the factory
    {
        // Make sure we have a first random number package available
        this->getNewRandomContainer();
    }

    /***************************************************************************/
    /**
	 * @brief The standard destructor; returns the held package to the factory for recycling.
	 */
    ~GRandomT() override {
        if(p_) {
            grf_->returnUsedPackage(std::move(p_));
        }
        grf_.reset();
    }

    /***************************************************************************/
    /**
	 * @brief Copy construction is identical to default construction.
	 *
	 * Every instance should hold a unique set of random numbers, so the source
	 * is ignored and a fresh container is obtained via a delegating constructor.
	 *
	 * @param cp The object to be "copied" (unused; present only for interface compatibility)
	 */
    GRandomT([[maybe_unused]] GRandomT<Gem::Hap::randomSource::QUEUE> const & cp) noexcept(false)
      : GRandomT<Gem::Hap::randomSource::QUEUE>() { /* nothing */
    }

    /***************************************************************************/
    /**
	 * @brief Move construction. Note that getNewRandomContainer() may throw.
	 *
	 * Steals the source's random number container, then re-supplies the source
	 * with a fresh container so it remains usable.
	 *
	 * @param cp The object whose random number container is moved from (left in a pristine state)
	 */
    GRandomT(GRandomT<Gem::Hap::randomSource::QUEUE> &&cp) noexcept(false)
      : p_(std::move(cp.p_))
      , grf_(randomFactory()) // Make sure we have a local pointer to the factory
    {
        // Make sure cp is in pristine condition -- we need to give it a new random number container
        cp.getNewRandomContainer();
    }

    /***************************************************************************/
    /**
	 * @brief Copy assignment -- a no-op.
	 *
	 * Each instance keeps its own unique set of random numbers, so nothing is
	 * copied (compare the copy constructor).
	 *
	 * @param cp The object to be "assigned" (unused; present only for interface compatibility)
	 * @return A reference to this object
	 */
    GRandomT<Gem::Hap::randomSource::QUEUE> &
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp) -- intentionally trivial: each instance owns independent state
    operator=([[maybe_unused]] GRandomT<Gem::Hap::randomSource::QUEUE> const & cp) noexcept(false) {
        return *this;
    }

    /***************************************************************************/
    /**
	 * @brief Move assignment.
	 *
	 * Takes over the source's random number container (keeping this object's own
	 * factory pointer) and re-supplies the source with a fresh container.
	 *
	 * @param cp The object whose random number container is moved from (re-initialized afterwards)
	 * @return A reference to this object
	 */
    GRandomT<Gem::Hap::randomSource::QUEUE> &
    operator=(GRandomT<Gem::Hap::randomSource::QUEUE> &&cp) noexcept(false) {
        p_ = std::move(cp.p_);
        // We keep our own pointer to the random factory

        // Re-initialize the random-number container of the remote class
        cp.p_.reset();
        cp.getNewRandomContainer();

        return *this;
    }

    /***************************************************************************/
    /**
	 * @brief Retrieves the id of the currently running thread.
	 *
	 * This function exists mostly for debugging purposes.
	 *
	 * @return The std::thread::id of the calling thread
	 */
    static std::thread::id getThreadId() {
        return std::this_thread::get_id();
    }

private:
    /***************************************************************************/
    /**
	 * This function retrieves random number packages from a global
	 * factory and emits them one by one. Once a package has been fully
	 * used, it is discarded and a new package is obtained from the factory.
	 * Essentially this class thus acts as a random number proxy -- to the
	 * caller it appears as if random numbers are created locally. This function
	 * assumes that a valid container is already available.
	 *
	 * @return The next raw random value, transparently refilling from the factory when exhausted
	 */
    GRandomBase::result_type int_random() override {
        if(p_->empty()) {
            // Get rid of the old container ...
            grf_->returnUsedPackage(std::move(p_));
            // ... then get a new one
            getNewRandomContainer();
        }
        return p_->next();
    }

    /***************************************************************************/
    /**
	 * @brief (Re-)Initialization of the local random number container p_.
	 *
	 * Checks (in DEBUG builds) that a valid GRandomFactory still exists, then
	 * retries getNewRandomContainer() on the factory until a valid container is
	 * obtained (each factory call has an internal timeout).
	 */
    void getNewRandomContainer() {
        // Make sure we get rid of the old container
        // p_.reset(); No longer needed with std::unique_ptr

#ifdef DEBUG
        if(not grf_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GRandomT<QUEUE>::getNewRandomContainer(): Error!" << '\n'
                << "No connection to GRandomFactory object." << '\n'
            );
        }
#endif /* DEBUG */

#ifdef DEBUG
        std::uint32_t n_retries = 0;
#endif /* DEBUG */

        // Try until a valid container has been received. getNewRandomContainer() has a timeout of
        // DEFAULTFACTORYGETWAIT internally and signals "nothing available yet" with an empty pointer.
        // An empty return is normally TRANSIENT (the producers simply have not refilled the buffer
        // yet), so we retry. Once the factory has been FINALIZED, however, its buffers are closed for
        // good and no container will ever arrive again -- retrying would then busy-spin at 100% CPU
        // forever. Distinguish the terminal case and fail fast with a clear exception instead of
        // hanging. (Defensive: the factory is a process-global singleton finalized only by its own
        // destructor at process exit, so a live run never reaches this; it converts a would-be
        // deadlock into a diagnosable error should the factory ever be shut down out from under a
        // still-drawing consumer.)
        while(not(p_ = grf_->getNewRandomContainer())) {
            if(grf_->finalized()) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GRandomT<QUEUE>::getNewRandomContainer(): Error!" << '\n'
                    << "The random-number factory has been finalized (permanently shut down);" << '\n'
                    << "no further random-number containers will be produced." << '\n'
                );
            }
#ifdef DEBUG
            n_retries++;
#endif /* DEBUG */
        }

#ifdef DEBUG
        if(n_retries > 1) {
            std::cout << "Info: Had to try " << n_retries
                      << " times to retrieve a valid random number container." << '\n';
        }
#endif /* DEBUG */
    }

    /***************************************************************************/
    /** @brief Holds the container of uniform random numbers */
    std::unique_ptr<random_container> p_;
    /** @brief A local shared pointer to the global GRandomFactory */
    std::shared_ptr<Gem::Hap::GRandomFactory> grf_;
};

/******************************************************************************/
/**
 * @brief The default random-number proxy used throughout Geneva.
 *
 * The underlying source is selected at configure time via the HAP_RANDOM_SOURCE
 * CMake option (queue|local|staged), which defines one of the macros below on
 * the hap target's public interface. The default (no macro) is QUEUE, so
 * production behaviour is unchanged unless the build explicitly opts into another
 * source.
 */
#if defined(HAP_DEFAULT_SOURCE_LOCAL)
using GRandom = GRandomT<Gem::Hap::randomSource::LOCAL>;
#elif defined(HAP_DEFAULT_SOURCE_STAGED)
using GRandom = GRandomT<Gem::Hap::randomSource::STAGED>;
#elif defined(HAP_DEFAULT_SOURCE_QUARANTINE)
using GRandom = GRandomT<Gem::Hap::randomSource::QUARANTINE>;
#else
using GRandom = GRandomT<Gem::Hap::randomSource::QUEUE>;
#endif

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief LOCAL proxy: a private per-proxy engine, no sharing at all.
 *
 * Each proxy owns one scalar xoshiro256++ engine (@c G_CPU_BASE_GENERATOR), seeded from the
 * factory's getSeed(). Every draw is produced inline by that engine; nothing is shared
 * between proxies, so there is no queue, no pool, no background thread and no contention -- it is
 * embarrassingly parallel. The trade-off is that it cannot use the shared GPU/SIMD bulk fill the
 * other sources benefit from. GRandomBase layers the distributions on the raw stream.
 *
 * @par Data structure
 * @verbatim
   random factory --getSeed()--> [ rng_ : xoshiro256++ ]   (one per proxy, private)
                                              |
                                  int_random() = rng_()           (no shared state)
   @endverbatim
 *
 * @par Algorithm
 * @f[
 *   \texttt{int\_random}() = \texttt{rng\_}() ,
 * @f]
 * a single engine step per draw. Copy/move delegate to default construction (each instance gets
 * its own freshly seeded engine), so the source object is ignored and every proxy yields an
 * independent stream.
 */
template <>
class GRandomT<Gem::Hap::randomSource::LOCAL> : public Gem::Hap::GRandomBase {
public:
    /***************************************************************************/
    /**
	 * @brief The standard constructor; seeds the local engine from the factory's getSeed().
	 */
    GRandomT() noexcept(false)
      : rng_(randomFactory()->getSeed()) { /* nothing */
    }

    /***************************************************************************/
    /**
	 * @brief Copy construction does nothing but delegate to the default constructor.
	 *
	 * Each instance gets its own freshly seeded local engine, so the source is ignored.
	 *
	 * @param cp The object to be "copied" (unused; present only for interface compatibility)
	 */
    GRandomT([[maybe_unused]] GRandomT<Gem::Hap::randomSource::LOCAL> const & cp) noexcept(false)
      : GRandomT<Gem::Hap::randomSource::LOCAL>() { /* nothing */
    }

    /***************************************************************************/
    /**
	 * @brief Move construction does nothing but delegate to the default constructor.
	 *
	 * Each instance gets its own freshly seeded local engine, so the source is ignored.
	 *
	 * @param cp The object to be "moved" from (unused; present only for interface compatibility)
	 */
    GRandomT([[maybe_unused]] GRandomT<Gem::Hap::randomSource::LOCAL> && cp) noexcept(false)
      : GRandomT<Gem::Hap::randomSource::LOCAL>() { /* nothing */
    }

    /***************************************************************************/
    /**
	 * The standard destructor
	 */
    ~GRandomT() override = default;

    /***************************************************************************/
    /**
	 * @brief Copy-assignment does nothing.
	 *
	 * Each instance owns its independent, locally seeded engine, so nothing is copied.
	 *
	 * @param cp The object to be "assigned" (unused; present only for interface compatibility)
	 * @return A reference to this object
	 */
    GRandomT<Gem::Hap::randomSource::LOCAL> &
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp) -- intentionally trivial: each instance owns independent state
    operator=([[maybe_unused]] GRandomT<Gem::Hap::randomSource::LOCAL> const & cp) noexcept(
        false
    ) {
        return *this;
    }

    /***************************************************************************/
    /**
	 * @brief Move-assignment does nothing.
	 *
	 * Each instance owns its independent, locally seeded engine, so nothing is moved.
	 *
	 * @param cp The object to be "moved" from (unused; present only for interface compatibility)
	 * @return A reference to this object
	 */
    GRandomT<Gem::Hap::randomSource::LOCAL> &
    operator=([[maybe_unused]] GRandomT<Gem::Hap::randomSource::LOCAL> && cp) noexcept(false) {
        return *this;
    }

private:
    /***************************************************************************/
    /**
	 * @brief This function produces uniform random numbers locally.
	 *
	 * @return One raw random value drawn directly from the instance-local engine
	 */
    GRandomBase::result_type int_random() override {
        return rng_();
    }

    /***************************************************************************/
    /** @brief The actual generator for local random number creation */
    G_CPU_BASE_GENERATOR rng_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief STAGED proxy: a private double buffer copied from the shared lock-free rotating pool.
 *
 * The proxy keeps two private chunk-sized halves (@f$W=@f$ @c STAGED_CHUNK_WORDS words each). It
 * serves words from the @e active half; when that half drains it switches to the @e standby half
 * (an O(1) index flip) and refills the just-drained half by claiming a fresh chunk from the
 * shared, bulk-filled Gem::Hap::detail::GRotatingPool via @c detail::stagedClaim(), which @b copies
 * the chunk out (word-by-word, aligned 64-bit). Because the proxy serves only from its own private
 * copy, it pins no shared memory and is fully race-free once a chunk is copied -- so it is safe to
 * leave dormant indefinitely. Copy/move follow the LOCAL model (each instance claims its own
 * chunks). This is the snapshot, stable-span counterpart to QUARANTINE (which reads in place).
 *
 * @par Data structure
 * @verbatim
   shared GRotatingPool ==stagedClaim()/copy==> private double buffer (this proxy)
                                                +-----------+-----------+
                                                |  half 0   |  half 1   |  W words each
                                                +-----------+-----------+
                                                  ^active      standby (already full)
                                                  | pos_
                                    drain active -> flip active^=1 -> refill drained half
   @endverbatim
 *
 * @par Algorithm
 * With read position @f$i@f$ into the active half @f$a\in\{0,1\}@f$:
 * @f[
 *   \texttt{int\_random}() =
 *   \begin{cases}
 *     \texttt{buf}[a]\,[\,i{+}{+}\,] & \text{if } i < W,\\[2pt]
 *     a \leftarrow a \oplus 1,\ i\leftarrow 0,\ \texttt{stagedClaim}(\texttt{buf}[a\oplus 1]),\ \text{then } \texttt{buf}[a]\,[\,i{+}{+}\,] & \text{if } i = W.
 *   \end{cases}
 * @f]
 * The standby half is always already full at the swap, so the only per-chunk cost is the copy of
 * @f$W@f$ words (one queue-free claim per @f$W@f$ draws).
 *
 * @par Concurrency
 * The cross-thread hazard is entirely inside the shared pool, not here: see
 * Gem::Hap::detail::GRotatingPool for the lock-free ring, the background producer, and the full
 * benign-race analysis. STAGED takes a private snapshot, so unlike QUARANTINE it does not depend on
 * the benign race while serving -- after the copy there is no shared access at all.
 */
template <>
class GRandomT<Gem::Hap::randomSource::STAGED> : public Gem::Hap::GRandomBase {
public:
    /***************************************************************************/
    /**
	 * @brief The standard constructor; fills both halves of the double buffer from the staging pool.
	 */
    GRandomT() noexcept(false) {
        Gem::Hap::detail::stagedClaim(buf_[0].data(), Gem::Hap::STAGED_CHUNK_WORDS);
        Gem::Hap::detail::stagedClaim(buf_[1].data(), Gem::Hap::STAGED_CHUNK_WORDS);
    }

    /***************************************************************************/
    /**
	 * @brief Copy construction delegates to the default constructor (each instance gets fresh chunks).
	 *
	 * @param cp The object to be "copied" (unused; present only for interface compatibility)
	 */
    GRandomT([[maybe_unused]] GRandomT<Gem::Hap::randomSource::STAGED> const & cp) noexcept(false)
      : GRandomT<Gem::Hap::randomSource::STAGED>() { /* nothing */
    }

    /***************************************************************************/
    /**
	 * @brief Move construction delegates to the default constructor (each instance gets fresh chunks).
	 *
	 * @param cp The object to be "moved" from (unused; present only for interface compatibility)
	 */
    GRandomT([[maybe_unused]] GRandomT<Gem::Hap::randomSource::STAGED> && cp) noexcept(false)
      : GRandomT<Gem::Hap::randomSource::STAGED>() { /* nothing */
    }

    /***************************************************************************/
    /**
	 * @brief The standard destructor.
	 */
    ~GRandomT() override = default;

    /***************************************************************************/
    /**
	 * @brief Copy-assignment does nothing -- each instance owns its independent chunks.
	 *
	 * @param cp The object to be "assigned" (unused; present only for interface compatibility)
	 * @return A reference to this object
	 */
    GRandomT<Gem::Hap::randomSource::STAGED> &
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp) -- intentionally trivial: each instance owns independent state
    operator=([[maybe_unused]] GRandomT<Gem::Hap::randomSource::STAGED> const & cp) noexcept(
        false
    ) {
        return *this;
    }

    /***************************************************************************/
    /**
	 * @brief Move-assignment does nothing -- each instance owns its independent chunks.
	 *
	 * @param cp The object to be "moved" from (unused; present only for interface compatibility)
	 * @return A reference to this object
	 */
    GRandomT<Gem::Hap::randomSource::STAGED> &
    operator=([[maybe_unused]] GRandomT<Gem::Hap::randomSource::STAGED> && cp) noexcept(false) {
        return *this;
    }

private:
    /***************************************************************************/
    /**
	 * @brief Serves the next raw random number, swapping to the standby chunk and re-claiming on exhaustion.
	 *
	 * When the active half runs out, it switches to the (already-filled) standby
	 * half -- an O(1) index flip, no copy -- resets the read position, and claims
	 * a fresh chunk into the now-standby half so a full buffer is always ready.
	 *
	 * @return The next raw random value from the active double-buffer half
	 */
    GRandomBase::result_type int_random() override {
        if(pos_ >= Gem::Hap::STAGED_CHUNK_WORDS) {
            active_ ^= 1; // switch to the prefilled standby half (O(1), no copy)
            pos_ = 0;
            // Refill the half we just drained; it becomes the next standby.
            Gem::Hap::detail::stagedClaim(buf_[active_ ^ 1].data(), Gem::Hap::STAGED_CHUNK_WORDS);
        }
        return buf_[active_][pos_++];
    }

    /***************************************************************************/
    /** @brief The two private double-buffer halves, each holding one claimed chunk */
    std::array<std::array<std::uint64_t, Gem::Hap::STAGED_CHUNK_WORDS>, 2> buf_{};
    /** @brief Index (0/1) of the half currently being served */
    int active_ = 0;
    /** @brief Read position within the active half */
    std::size_t pos_ = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief QUARANTINE proxy: reads chunk spans in place from the shared rotating pool (no copy).
 *
 * The proxy claims a chunk-sized span (@f$W=@f$ @c QUARANTINE_CHUNK_WORDS words) from the shared
 * Gem::Hap::detail::GRotatingPool via @c detail::quarantineClaimSpan() and serves words @e directly
 * out of that pool span -- no private copy. Skipping the copy makes it the lowest-overhead bulk
 * source (its distinction from STAGED), at the cost of depending on the pool's benign race on every
 * read. When the span is exhausted it claims the next one. Copy/move follow the LOCAL model.
 *
 * @par Data structure
 * @verbatim
   shared GRotatingPool                          this proxy
   +----+----+----+----+                         span_ ----> points INTO a pool span
   |pool|pool|pool|pool|  <----- reads in place   pos_  ----> read index 0..W
   +----+----+----+----+                         (no private buffer; no snapshot)
        ^ claimSpan() returns a pointer; words are read where they live
   @endverbatim
 *
 * @par Algorithm
 * With read position @f$i@f$ into the current span:
 * @f[
 *   \texttt{int\_random}() =
 *   \begin{cases}
 *     \texttt{span}\,[\,i{+}{+}\,] & \text{if } i < W,\\[2pt]
 *     \texttt{span}\leftarrow\texttt{quarantineClaimSpan}(),\ i\leftarrow 0,\ \text{then } \texttt{span}\,[\,i{+}{+}\,] & \text{if } i = W.
 *   \end{cases}
 * @f]
 *
 * @par Concurrency -- relies on the benign race
 * Unlike STAGED, QUARANTINE does @e not snapshot: it reads pool memory directly, so each read may
 * race the background producer refilling a quarantined pool. That race is harmless under exactly
 * the conditions analysed in Gem::Hap::detail::GRotatingPool: aligned 64-bit reads are atomic on
 * x86-64 / AArch64 (old-or-new, never torn), the @f$N\ge 3@f$ quarantine keeps an active reader and
 * the refiller in different pools, and there are no locks so it is deadlock-free. See that class for
 * the full derivation.
 */
template <>
class GRandomT<Gem::Hap::randomSource::QUARANTINE> : public Gem::Hap::GRandomBase {
public:
    /***************************************************************************/
    /**
	 * @brief The standard constructor; claims a first span from the quarantine pool set.
	 */
    GRandomT() noexcept(false)
      : span_(Gem::Hap::detail::quarantineClaimSpan()) { /* nothing */
    }

    /***************************************************************************/
    /**
	 * @brief Copy construction delegates to the default constructor (each instance claims its own spans).
	 *
	 * @param cp The object to be "copied" (unused; present only for interface compatibility)
	 */
    GRandomT([[maybe_unused]] GRandomT<Gem::Hap::randomSource::QUARANTINE> const & cp) noexcept(false)
      : GRandomT<Gem::Hap::randomSource::QUARANTINE>() { /* nothing */
    }

    /***************************************************************************/
    /**
	 * @brief Move construction delegates to the default constructor (each instance claims its own spans).
	 *
	 * @param cp The object to be "moved" from (unused; present only for interface compatibility)
	 */
    GRandomT([[maybe_unused]] GRandomT<Gem::Hap::randomSource::QUARANTINE> && cp) noexcept(false)
      : GRandomT<Gem::Hap::randomSource::QUARANTINE>() { /* nothing */
    }

    /***************************************************************************/
    /**
	 * @brief The standard destructor.
	 */
    ~GRandomT() override = default;

    /***************************************************************************/
    /**
	 * @brief Copy-assignment does nothing -- each instance reads its own claimed spans.
	 *
	 * @param cp The object to be "assigned" (unused; present only for interface compatibility)
	 * @return A reference to this object
	 */
    GRandomT<Gem::Hap::randomSource::QUARANTINE> &
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp) -- intentionally trivial: each instance owns independent state
    operator=([[maybe_unused]] GRandomT<Gem::Hap::randomSource::QUARANTINE> const & cp) noexcept(
        false
    ) {
        return *this;
    }

    /***************************************************************************/
    /**
	 * @brief Move-assignment does nothing -- each instance reads its own claimed spans.
	 *
	 * @param cp The object to be "moved" from (unused; present only for interface compatibility)
	 * @return A reference to this object
	 */
    GRandomT<Gem::Hap::randomSource::QUARANTINE> &
    operator=([[maybe_unused]] GRandomT<Gem::Hap::randomSource::QUARANTINE> && cp) noexcept(false) {
        return *this;
    }

private:
    /***************************************************************************/
    /**
	 * @brief Serves the next raw random number, claiming a fresh span when the current one is spent.
	 *
	 * The span is read straight from the shared pool. The read may race the
	 * background producer's refill of a quarantined pool; that race is benign by
	 * design (aligned-64-bit, old-or-new) -- see GQuarantineSource.cpp.
	 *
	 * @return The next raw random value from the currently claimed span
	 */
    GRandomBase::result_type int_random() override {
        if(pos_ >= Gem::Hap::QUARANTINE_CHUNK_WORDS) {
            span_ = Gem::Hap::detail::quarantineClaimSpan();
            pos_ = 0;
        }
        return span_[pos_++];
    }

    /***************************************************************************/
    /** @brief Pointer to the currently claimed span inside a shared pool (read in place) */
    const std::uint64_t *span_;
    /** @brief Read position within the current span */
    std::size_t pos_ = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Hap */
