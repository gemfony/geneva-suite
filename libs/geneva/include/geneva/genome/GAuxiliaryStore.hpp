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
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named / archive_named_binary (named member / base emitters)
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "geneva/GPersonalityTraits.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
/** @brief Stable key identifying an auxiliary block (e.g. an FNV hash of "ea.gauss.state") */
using AuxKey = std::uint32_t;

/** @brief Whether an auxiliary POD block holds one record per genome group or one per individual */
enum class AuxScope : std::uint8_t { PerIndividual, PerGroup };

/******************************************************************************/
/**
 * An opaque, memcpy-clonable per-individual block of optimization-algorithm metadata
 * (e.g. EA Gauss state: sigma / adProb / counter, one record per genome group). The store
 * treats it as raw bytes; the OA that installed it knows the POD type and reads it back through
 * a typed span. Standard-layout, trivially-copyable POD only — no pointers / vectors inside the
 * record — so the block is memcpy-clonable and (later) GPU-uploadable.
 */
struct AuxBlock {
    AuxScope               scope = AuxScope::PerGroup; ///< per-group or per-individual
    std::uint32_t          stride = 0;                 ///< bytes per record (== sizeof(POD))
    std::uint32_t          tag = 0;                    ///< hash of the POD type, for a debug sanity check
    std::vector<std::byte> bytes;                      ///< stride * record_count bytes

    /**
     * @brief Serialization of one opaque POD block (used for full-state checkpointing of the
     * per-individual OA scratch). The raw bytes are written as a binary blob (a hex string in the
     * JSON codec). The type tag is DELIBERATELY NOT serialized: it is a typeid().hash_code() that is only
     * stable within a single process, so a checkpoint written by one run would never match the reader's
     * tag. It therefore stays at its default 0 on load, which metaRecords()'s debug check treats as
     * "type-unchecked" (the stride / size check, which IS stable, still applies).
     *
     * @tparam Archive The GArchive codec type
     * @param ar The archive to serialize to / from
     * @param version The serialization version (unused)
     */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        auto scope_u = std::to_underlying(scope);
        archive_named(ar, "scope", scope_u);
        scope = static_cast<AuxScope>(scope_u);

        archive_named(ar, "stride", stride);

        std::size_t n_bytes = bytes.size();
        archive_named(ar, "n_bytes", n_bytes);
        if(n_bytes != bytes.size()) {
            bytes.resize(n_bytes); // on load
        }
        if(n_bytes > 0) {
            Gem::Common::archive_named_binary(ar, "bytes", bytes.data(), n_bytes);
        }
    }
};

/******************************************************************************/
/**
 * @brief A cheap, stable-per-process type tag used to sanity-check typed POD access (debug only)
 *
 * @tparam POD The POD type whose type tag is requested
 * @return A per-process-stable hash of the POD type's typeid
 */
template <typename POD>
inline std::uint32_t auxTypeTag() {
    return static_cast<std::uint32_t>(typeid(POD).hash_code());
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The unified, per-individual home for optimization-algorithm-owned auxiliary data.
 *
 * It is genome-layout-agnostic: the flat individual (GGenome) holds exactly one, so the place
 * where an optimization algorithm stashes its per-individual data is the same regardless of how the
 * genome is stored.
 *
 * It has two channels:
 *  - an OBJECT slot: the personality traits (the one per-individual OA *object*; deep-cloned).
 *  - a POD-block map: opaque, keyed, memcpy-clonable per-group/per-individual metadata (adaptor
 *    state, swarm velocity, gradient, …), typed by the *caller* (the OA).
 *
 * Both channels are OA-scoped: they are owned by whichever optimization algorithm currently holds
 * the individual, persist and evolve across that OA's iterations, and are dropped at the algorithm
 * boundary when the individual is handed to the next (possibly different) OA — an EA's sigma is
 * meaningless to a chained CGD and must not ride along. clearScratch() implements that boundary
 * reset for *both* channels; it is therefore called on hand-over, not per iteration.
 *
 * Both channels are OA-installed scratch and are kept OUT of the individual's COMPARED identity (they
 * are not in localMembers()): two individuals differing only in which OA last touched them compare
 * equal. They are deep-copied on CLONE (a clone mid-optimization keeps the live scratch) and dropped
 * by clearScratch(). Both are serialized (so a checkpoint/resume restores the evolved per-individual
 * state in place): the personality is serialized explicitly by the individual's serialize() (NOT via
 * localMembers, so it is serialized but not compared), and the POD blocks are serialized here (see
 * serialize() below). The aux type tag is deliberately left out of the wire form (AuxBlock::serialize);
 * the stride is checked on access instead.
 */
class GAuxiliaryStore {
public:
    /** @brief The default constructor */
    GAuxiliaryStore() = default;

    /**
     * @brief The copy constructor deep-clones the personality and copies the POD blocks
     *
     * @param cp A constant reference to another GAuxiliaryStore object to be copied
     */
    GAuxiliaryStore(const GAuxiliaryStore &cp)
      : pods_(cp.pods_)
      , n_adaptions_(cp.n_adaptions_) {
        Gem::Common::copyCloneableSmartPointer(cp.personality_, personality_);
    }

    /** @brief The move constructor */
    GAuxiliaryStore(GAuxiliaryStore &&) = default;

    /** @brief The destructor */
    ~GAuxiliaryStore() = default;

    /**
     * @brief Copy assignment deep-clones the personality and copies the POD blocks
     *
     * @param cp A constant reference to another GAuxiliaryStore object to be copied
     * @return A reference to this object
     */
    GAuxiliaryStore &operator=(const GAuxiliaryStore &cp) {
        if(this != &cp) {
            Gem::Common::copyCloneableSmartPointer(cp.personality_, personality_);
            pods_ = cp.pods_;
            n_adaptions_ = cp.n_adaptions_;
        }
        return *this;
    }

    /**
     * @brief Move assignment
     *
     * @return A reference to this object
     */
    GAuxiliaryStore &operator=(GAuxiliaryStore &&) = default;

    /***************************************************************************/
    // Personality (the per-individual OA object). Like the rest of the scratch it is serialized
    // (checkpoints resume the evolved per-individual state) but deliberately NOT part of the
    // compared identity -- see the class comment above.

    /**
     * @brief Direct access to the personality-traits slot. Returned by reference so the individual's
     * serialization / clone machinery can drive it directly; the
     * explicit object parameter lets its constness flow (a const store yields a const reference).
     *
     * @return A reference to the personality-traits shared pointer slot (const iff *this is const)
     */
    template <typename Self>
    auto &personalityRef(this Self &&self) {
        return self.personality_;
    }

    /***************************************************************************/
    // POD blocks (per-parameter / per-group OA metadata; transient run-scoped scratch).

    /**
     * @brief Whether a POD block is installed under the given key
     *
     * @param key The key identifying the auxiliary POD block
     * @return true if a block is installed under the key, false otherwise
     */
    [[nodiscard]] bool hasAux(AuxKey key) const {
        return pods_.contains(key);
    }

    /**
     * @brief Installs (or replaces) a zero-initialised POD block of record_count records of type POD
     * under the given key. The OA calls this at setup to size its per-group metadata.
     *
     * @tparam POD The standard-layout, trivially-copyable record type held by the block
     * @param key The key under which the block is installed (overwriting any existing block)
     * @param record_count The number of records to allocate (each sizeof(POD) bytes)
     * @param scope Whether the block holds one record per genome group or one per individual
     */
    template <typename POD>
    void installAuxBlock(AuxKey key, std::size_t record_count, AuxScope scope = AuxScope::PerGroup) {
        static_assert(
            std::is_trivially_copyable_v<POD> && std::is_standard_layout_v<POD>,
            "GAuxiliaryStore: auxiliary POD blocks must be standard-layout, trivially-copyable"
        );
        AuxBlock b;
        b.scope = scope;
        b.stride = static_cast<std::uint32_t>(sizeof(POD));
        b.tag = auxTypeTag<POD>();
        b.bytes.assign(record_count * sizeof(POD), std::byte{0});
        pods_[key] = std::move(b);
    }

    /**
     * @brief A typed view over the records of the POD block under key. The explicit object parameter
     * lets the store's constness flow through to the span: a const store yields std::span<const POD>.
     *
     * @tparam POD The record type the block holds
     * @param key The key identifying the auxiliary POD block
     * @return A span over the block's records (const iff *this is const)
     */
    template <typename POD, typename Self>
    auto metaRecords(this Self &&self, AuxKey key) {
        auto &b = self.fetch(key, sizeof(POD), auxTypeTag<POD>());
        using QPOD = std::conditional_t<std::is_const_v<std::remove_reference_t<decltype(b)>>, const POD, POD>;
        return std::span<QPOD>(reinterpret_cast<QPOD *>(b.bytes.data()), b.bytes.size() / sizeof(POD));
    }

    /**
     * @brief Typed access to a per-individual (single-record) POD block. The explicit object parameter
     * lets the store's constness flow: a const store yields a const reference to the record.
     *
     * @tparam POD The record type the block holds
     * @param key The key identifying the single-record auxiliary POD block
     * @return A reference to the block's single record (const iff *this is const)
     */
    template <typename POD, typename Self>
    auto &metaScalar(this Self &&self, AuxKey key) {
        return self.template metaRecords<POD>(key)[0];
    }

    /**
     * @brief Copies just the POD blocks from another store (the personality is loaded separately, via localMembers)
     *
     * @param src The store whose POD blocks are copied into this one
     */
    void copyPodsFrom(const GAuxiliaryStore &src) {
        pods_ = src.pods_;
    }

    /***************************************************************************/
    /**
     * Clears all algorithm-scoped scratch held by this store: the personality traits and the POD
     * blocks. Called at optimization-algorithm boundaries (the incoming algorithm re-installs its own).
     */
    void clearScratch() {
        personality_.reset();
        pods_.clear();
    }

    /***************************************************************************/
    // Diagnostic adaption counter (OA-side scratch; a transient per-individual count of how many values
    // the last adaption changed -- read by the adaption monitor). Lives here rather than on the genome so
    // the individual stays pure data; nulled on the wire with the rest of the scratch.

    /** @brief @return The number of adaptions performed during the individual's last adaption */
    [[nodiscard]] std::size_t getNAdaptions() const { return n_adaptions_; }
    /** @brief Records the number of adaptions performed during the last adaption. @param n The count */
    void setNAdaptions(std::size_t n) { n_adaptions_ = n; }

private:
    /***************************************************************************/
    // Full-state serialization (personality OBJECT + the opaque POD blocks). Used ONLY for
    // check-pointing -- the slot that owns this store is never sent over the wire (transport submits
    // bare individuals), so this always runs in the "checkpoint" form. The genome carries the
    // optimization forward; this lets a resumed algorithm keep its evolved scratch (sigma, swarm
    // velocity / personal-best, conjugate-gradient memory, personality) instead of restarting it.
    friend struct Gem::Weft::access;

    /**
     * @brief Full-state (de)serialization of the personality object and the opaque POD blocks
     *
     * @tparam Archive The GArchive codec type
     * @param ar The archive to serialize to / from
     * @param version The serialization version (unused)
     */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        archive_named(ar, "personality_", personality_);
        archive_named(ar, "pods_", pods_);
        archive_named(ar, "n_adaptions_", n_adaptions_);
    }

    /***************************************************************************/
    /**
     * @brief Looks up a POD block, sanity-checking its stride and type tag in DEBUG mode. The explicit
     * object parameter lets the store's constness flow to the returned block reference.
     *
     * @param key The key identifying the auxiliary POD block
     * @param pod_size The expected record stride (sizeof of the caller's POD type)
     * @param pod_tag The expected type tag of the caller's POD type
     * @return A reference to the matching auxiliary block (const iff *this is const)
     */
    template <typename Self>
    auto &fetch(this Self &&self, AuxKey key, [[maybe_unused]] std::size_t pod_size, [[maybe_unused]] std::uint32_t pod_tag) {
        auto it = self.pods_.find(key);
#ifdef DEBUG
        self.verify(it != self.pods_.end(), key, pod_size, pod_tag, it);
#endif
        return it->second;
    }
#ifdef DEBUG
    /**
     * @brief Throws if a fetched POD block is absent or its stride / type tag disagrees with the caller's POD type
     *
     * @tparam It The map iterator type pointing at the looked-up block
     * @param found Whether the lookup found a block under the key
     * @param key The key that was looked up (for the error message)
     * @param pod_size The expected record stride (sizeof of the caller's POD type)
     * @param pod_tag The expected type tag of the caller's POD type (0 on a deserialised block is treated as type-unchecked)
     * @param it An iterator to the looked-up block (valid only when found is true)
     */
    template <typename It>
    void verify(bool found, AuxKey key, std::size_t pod_size, std::uint32_t pod_tag, It it) const {
        if(not found) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GAuxiliaryStore::metaRecords(): no aux block under key " << key << '\n'
            );
        }
        // The stride (== sizeof(POD)) is always checked; the type tag is only checked when present (a
        // deserialised block has tag 0, see AuxBlock::serialize -- the per-process typeid hash cannot be
        // compared across a checkpoint, so it is treated as "type-unchecked").
        if(it->second.stride != pod_size || (it->second.tag != 0 && it->second.tag != pod_tag)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GAuxiliaryStore::metaRecords(): POD type mismatch for key " << key << '\n'
                << "stride " << it->second.stride << " vs sizeof " << pod_size << '\n'
            );
        }
    }
#endif

    /***************************************************************************/
    // Data

    /** @brief The personality traits -- the per-individual, algorithm-scoped OA object */
    std::shared_ptr<Gem::Geneva::GPersonalityTraits> personality_;

    /** @brief Opaque per-group/per-individual OA metadata blocks (adaptor state, …), keyed; transient scratch */
    std::map<AuxKey, AuxBlock> pods_;

    /** @brief Number of adaptions performed during the individual's last adaption (diagnostic) */
    std::size_t n_adaptions_ = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
