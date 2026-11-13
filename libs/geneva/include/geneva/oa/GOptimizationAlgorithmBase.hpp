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
#include <chrono>
#include <concepts>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// Boost header files go here

// Geneva headers go here
#include "common/GReflectiveInterfaceT.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/concurrency/GThreadPool.hpp"
#include "common/GContainerT.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "courtier/GCourtierEnums.hpp"     // SUBMISSION_UUID_TYPE (late-return lineage de-dup)
#include "courtier/GSubmissionStatusT.hpp" // submission_status_t (workOn's return type)
// --- Submission goes through the one process-wide consumer (GConsumerRegistry): the algorithm is
//     transport-agnostic, it just reads that consumer and calls processBatch(), see workOn ---
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeFixedSizePriorityQueue.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/Interface/GOptimizerIT.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/oa/GBasePluggableOM.hpp" // the pluggable-monitor CRTP root (extracted from this header)

namespace Gem::Geneva::OptimizationAlgorithms {

// Re-export the shared numeric enum stream operators (see numeric_enum_io_v in
// GCommonEnums.hpp) into this namespace, so that argument-dependent lookup
// finds them for the algorithm-specific enums (e.g. the CGD gradientMethod).
using Gem::Common::operator<<;
using Gem::Common::operator>>;

/******************************************************************************/
// GBasePluggableOM -- the CRTP category root of all pluggable optimization monitors -- now lives in
// its own header (geneva/oa/GBasePluggableOM.hpp, included at the top of this file) so that
// monitor-only consumers do not drag in the large GOptimizationAlgorithmBase definition, and changes
// to the algorithm base do not force the monitor hierarchy to recompile.

// Forward declaration
class GAdaptionConfigBase;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class implements basic operations found in iteration-based optimization algorithms.
 * E.g., one might want to stop the optimization after a given number of cycles, or after
 * a given amount of time. The class also defines the interface functions common to these
 * algorithms, such as a general call to "optimize()".
 */
class GOptimizationAlgorithmBase // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GReflectiveInterfaceBaseT<
        GOptimizationAlgorithmBase, Gem::Common::GCommonInterfaceT<GOptimizationAlgorithmBase>
    >
  , public Gem::Common::GUniquePtrContainerT<gen::GGenome>
  , public Interface::GOptimizerIT<GOptimizationAlgorithmBase> {
private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /***************************************************************************/
    /**
     * Single declaration of this class'es local data members. This drives the
     * GReflectiveInterfaceBaseT-generated serialize(), load_(), compare_() and name_() from one
     * place. Plain members use make_member(); the cloneable pointer container
     * pluggable_monitors_cnt_ uses make_cloneable_container_member() (deep-cloned on load);
     * the atomic halted_ uses make_atomic_member() (loaded via .store(.load()), compared via
     * its loaded value, serialised through the std::atomic<bool> free serialization).
     *
     * The two members that are NOT plain-and-serialized are expressed through their own
     * descriptor kinds, so the whole class still derives from this one list:
     *  - the population held by the GUniquePtrContainerT<GGenome> base is tied in
     *    as data_cnt_ via make_cloneable_container_member() (deep-cloned on load, element-wise
     *    compared, serialized as a member) -- so that container stays a behaviour-only base and
     *    needs no base_object handling, mirroring GBaseScanParT;
     *  - best_iteration_individuals_pq_ is transient (per iteration) and must never be
     *    persisted, yet two live algorithms are only equal if they agree on it, so it uses
     *    make_transient_member() (skipped by serialize, copied on load, still compared).
     *
     * cp_directory_path_ (std::filesystem::path) is a plain member: it serialises via the
     * free serialization in GStdFilesystemPathSerialization.hpp and is plain-assignable in
     * memory, so it needs no special handling.
     */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            // The population held by the GUniquePtrContainerT base, tied in as a member (see above).
            Gem::Common::make_cloneable_container_member("data_cnt_", self.data_cnt_),
            Gem::Common::make_member("iteration_", self.iteration_),
            Gem::Common::make_member("offset_", self.offset_),
            Gem::Common::make_member("max_iteration_", self.max_iteration_),
            Gem::Common::make_member("min_iteration_", self.min_iteration_),
            Gem::Common::make_member("max_stall_iteration_", self.max_stall_iteration_),
            Gem::Common::make_member("report_iteration_", self.report_iteration_),
            Gem::Common::make_member("n_recordbest_global_individuals_", self.n_recordbest_global_individuals_),
            Gem::Common::make_member("best_global_individuals_pq_", self.best_global_individuals_pq_),
            Gem::Common::make_member("default_population_size_", self.default_population_size_),
            Gem::Common::make_member("best_known_primary_fitness_", self.best_known_primary_fitness_),
            Gem::Common::make_member("best_current_primary_fitness_", self.best_current_primary_fitness_),
            Gem::Common::make_member("stall_counter_", self.stall_counter_),
            Gem::Common::make_member("stall_counter_threshold_", self.stall_counter_threshold_),
            Gem::Common::make_member("late_return_ttl_", self.late_return_ttl_),
            Gem::Common::make_member("late_return_cap_factor_", self.late_return_cap_factor_),
            Gem::Common::make_member("cp_interval_", self.cp_interval_),
            Gem::Common::make_member("cp_base_name_", self.cp_base_name_),
            Gem::Common::make_member("cp_directory_path_", self.cp_directory_path_),
            Gem::Common::make_member("cp_last_", self.cp_last_),
            Gem::Common::make_member("cp_remove_", self.cp_remove_),
            Gem::Common::make_member("cp_serialization_mode_", self.cp_serialization_mode_),
            Gem::Common::make_member("quality_threshold_", self.quality_threshold_),
            Gem::Common::make_member("has_quality_threshold_", self.has_quality_threshold_),
            Gem::Common::make_member("max_duration_", self.max_duration_),
            Gem::Common::make_member("min_duration_", self.min_duration_),
            Gem::Common::make_member("termination_file_", self.termination_file_),
            Gem::Common::make_member("terminate_on_file_modification_", self.terminate_on_file_modification_),
            Gem::Common::make_member("emit_termination_reason_", self.emit_termination_reason_),
            Gem::Common::make_member("worst_known_valids_cnt_", self.worst_known_valids_cnt_),
            Gem::Common::make_member("n_threads_", self.n_threads_),
            Gem::Common::make_atomic_member("halted_", self.halted_),
            Gem::Common::make_cloneable_container_member("pluggable_monitors_cnt_", self.pluggable_monitors_cnt_),
            // Transient per-iteration best set: not persisted, but part of comparable identity (see above).
            Gem::Common::make_transient_member("best_iteration_individuals_pq_", self.best_iteration_individuals_pq_)
        );
    }

    /** @brief Disambiguating serialize(): both stateful bases in the inheritance set (the GReflectiveInterfaceBaseT
     *  mixin and the GUniquePtrContainerT base) declare a serialize(), so this one-liner resolves the
     *  ambiguity and emits the single member list -- which now ties in the container's population as
     *  data_cnt_ and skips the transient best_iteration_individuals_pq_. The stateless GCommonInterfaceT
     *  root and GOptimizerIT interface contribute nothing.
     *  @tparam Archive The archive type used for (de-)serialization
     *  @param ar The archive to serialize to / from
     *  @param version The (unused) serialization format version */
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::serialize_members(ar, this->localMembers_());
    }

    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GOptimizationAlgorithmBase";

    // The private split-serialization member load(Archive&, unsigned) below
    // name-hides the public load(const&) / load(shared_ptr<>) inherited from
    // Gem::Common::GCommonInterfaceT<GOptimizationAlgorithmBase>. Re-expose them so callers (and
    // the standard unit tests) can load one GOptimizationAlgorithmBase from another.
    using Gem::Common::GCommonInterfaceT<GOptimizationAlgorithmBase>::load;

    /***************************************************************************/
    // The population element IS the work item (gen::GGenome), which carries its own OA
    // scratch. A user adds bare individuals straight into the population; the inherited push_back
    // overloads are re-exposed so callers can grow the population directly.
    using Gem::Common::GUniquePtrContainerT<gen::GGenome>::push_back;

    /**
     * @brief The copy constructor.
     * @param cp Another GOptimizationAlgorithmBase object whose state is copied
     */
    GOptimizationAlgorithmBase(GOptimizationAlgorithmBase const &cp);

    /***************************************************************************/
    // Defaulted functions

    GOptimizationAlgorithmBase() = default;
    ~GOptimizationAlgorithmBase() override = default;

    /***************************************************************************/

    /**
     * @brief Performs the necessary administratory work of doing check-pointing.
     * @param is_better Whether the current iteration produced an improvement over the previous best
     */
    void checkpoint(bool is_better) const;

    /**
     * @brief Loads the state of the class from disc.
     * @param cp_file The path to the checkpoint file to restore the algorithm state from
     */
    void loadCheckpoint(std::filesystem::path const &cp_file);

    /**
     * @brief Hands this algorithm an OA-owned adaption configuration to use for the run.
     * Go2 installs the config it holds for this algorithm's type before the algorithm runs in a chain; an
     * adapting algorithm (EA / SA) adopts it (validating that it matches the population's genome) instead
     * of deriving a default from the genome layout. The base does nothing -- non-adapting algorithms
     * (swarm, gradient descent, scan) have no adaption config.
     *
     * @param config The OA-owned adaption configuration to adopt for this run
     */
    virtual void setAdaptionConfig(std::shared_ptr<GAdaptionConfigBase> /* config */) { /* no-op */ }

    /**
     * @brief The OA-owned adaption configuration in effect for this run, or null. Adapting algorithms
     * (EA / SA) build it at init() and return it here so telemetry (e.g. the adaptor-property monitor) can
     * read each group's adaptor settings without snapshotting the structure-only genome layout. The base
     * (non-adapting algorithms) returns null.
     *
     * @return The OA-owned adaption configuration in effect for this run, or null if there is none
     */
    virtual std::shared_ptr<const GAdaptionConfigBase> getAdaptionConfig() const { return {}; }

    /**
     * @brief Checks whether the optimization process has been halted.
     * @return true if the optimization has halted, false otherwise
     */
    bool halted() const;

    /**
     * @brief Allows to set the number of generations after which a checkpoint should be written.
     * @param cp_interval The checkpoint interval in iterations (-1 means: write whenever an improvement occurs)
     */
    void setCheckpointInterval(std::int32_t cp_interval);
    /**
     * @brief Allows to retrieve the number of generations after which a checkpoint should be written.
     * @return The checkpoint interval in iterations
     */
    std::int32_t getCheckpointInterval() const;

    /**
     * @brief Allows to set the base name of the checkpoint file and the directory where it should be stored.
     * A missing directory is created lazily when the first checkpoint is written; configuring has no
     * filesystem side effects (an existing path must be a directory, though).
     * @param cp_directory The directory in which checkpoint files are stored
     * @param cp_base_name The base name of the checkpoint files
     */
    void setCheckpointBaseName(const std::string& cp_directory, const std::string& cp_base_name);
    /**
     * @brief Allows to retrieve the base name of the checkpoint file.
     * @return The base name of the checkpoint files
     */
    std::string getCheckpointBaseName() const;
    /**
     * @brief Allows to retrieve the directory where checkpoint files should be stored.
     * @return The checkpoint directory as a string
     */
    std::string getCheckpointDirectory() const;
    /**
     * @brief Allows to retrieve the directory where checkpoint files should be stored.
     * @return The checkpoint directory as a filesystem path
     */
    std::filesystem::path getCheckpointDirectoryPath() const;
    /**
     * @brief Determines whether checkpointing should be done in Text-, XML- or Binary-mode.
     * @param cp_ser_mode The serialization mode used for checkpoint files
     */
    void setCheckpointSerializationMode(Gem::Common::serializationMode cp_ser_mode);
    /**
     * @brief Retrieves the current checkpointing serialization mode.
     * @return The serialization mode used for checkpoint files
     */
    Gem::Common::serializationMode getCheckpointSerializationMode() const;
    /**
     * @brief Allows to set the cp_remove_ flag.
     * @param cp_remove Whether checkpoint files should be removed (true) or kept (false)
     */
    void setRemoveCheckpointFiles(bool cp_remove);
    /**
     * @brief Allows to check whether checkpoint files will be removed.
     * @return true if checkpoint files are removed, false if they are kept
     */
    bool checkpointFilesAreRemoved() const;

    /** @brief Resets the class to the state before the optimize call. */
    void resetToOptimizationStart();

    /******************************************************************************/
    /**
     * @brief Sets the number of threads used for parallel organizational work (adaption,
     *  recombination, ...). 0 means "automatic" (hardware concurrency).
     * @param n_threads The number of organizational-work threads (0 == hardware concurrency)
     */
    void setNThreads(std::uint16_t n_threads);
    /**
     * @brief Retrieves the number of threads used for parallel organizational work.
     * @return The number of threads used for organizational work
     */
    [[nodiscard]] std::uint16_t getNThreads() const;

    /******************************************************************************/

    /**
     * @brief Emits information specific to this class.
     * @param im The information mode, e.g. start / processing / end of an optimization run
     */
    void informationUpdate(const infoMode &im);

    /**
     * @brief Checks whether a better solution was found.
     * @return true if the current iteration improved on the best known fitness, false otherwise
     */
    bool progress() const;

    /**
     * @brief Allows to register a pluggable optimization monitor.
     * @param pluggable_om The pluggable optimization monitor to register
     */
    void registerPluggableOM(const std::shared_ptr<GBasePluggableOM>& pluggable_om);
    /** @brief Allows to reset the local pluggable optimization monitors */
    void resetPluggableOM();
    /**
     * @brief Allows to check whether pluggable optimization monitors were registered.
     * @return true if at least one pluggable optimization monitor is registered, false otherwise
     */
    bool hasPluggableOptimizationMonitors() const;

    /**
     * @brief Retrieves the default population size.
     * @return The nominal (default) population size
     */
    std::size_t getDefaultPopulationSize() const;
    /**
     * @brief Retrieve the current population size.
     * @return The current number of individuals in the population
     */
    std::size_t getPopulationSize() const;

    /**
     * @brief Set the number of iterations after which the optimization should be stopped.
     * @param max_iteration The maximum number of iterations
     */
    void setMaxIteration(std::uint32_t max_iteration);
    /**
     * @brief Retrieve the number of iterations after which optimization should be stopped.
     * @return The maximum number of iterations
     */
    std::uint32_t getMaxIteration() const;

    /**
     * @brief Sets the minimum number of iterations.
     * @param min_iteration The minimum number of iterations to run before a halt criterion may take effect
     */
    void setMinIteration(std::uint32_t min_iteration);
    /**
     * @brief Retrieves the currently set minimum number of iterations.
     * @return The minimum number of iterations
     */
    std::uint32_t getMinIteration() const;

    /**
     * @brief Sets the maximum number of iterations allowed without improvement of the best individual.
     * @param max_stall_iteration The maximum number of stalled (improvement-free) iterations
     */
    void setMaxStallIteration(std::uint32_t max_stall_iteration);
    /**
     * @brief Retrieves the maximum number of generations allowed in an optimization run without improvement of the best individual.
     * @return The maximum number of stalled iterations
     */
    std::uint32_t getMaxStallIteration() const;

    /**
     * @brief Sets the maximum allowed processing time.
     * @param max_duration The maximum allowed run duration
     */
    void setMaxTime(std::chrono::duration<double> max_duration);
    /**
     * @brief Retrieves the value of the max_duration_ parameter.
     * @return The maximum allowed run duration
     */
    std::chrono::duration<double> getMaxTime() const;

    /**
     * @brief Sets the minimum required processing time.
     * @param min_duration The minimum run duration before a halt criterion may take effect
     */
    void setMinTime(std::chrono::duration<double> min_duration);
    /**
     * @brief Retrieves the value of the min_duration_ parameter.
     * @return The minimum required run duration
     */
    std::chrono::duration<double> getMinTime() const;

    /**
     * @brief Sets a quality threshold beyond which optimization is expected to stop.
     * @param quality_threshold The quality threshold value
     * @param has_quality_threshold Whether the quality threshold is active
     */
    void setQualityThreshold(double quality_threshold, bool has_quality_threshold);
    /**
     * @brief Retrieves the current value of the quality threshold and also indicates whether the threshold is active.
     * @param has_quality_threshold Output parameter set to true if the threshold is active, false otherwise
     * @return The current quality threshold value
     */
    double getQualityThreshold(bool &has_quality_threshold) const;

    /**
     * @brief Sets the name of a "termination file".
     * @param termination_file The path to a file whose modification triggers termination
     * @param terminate_on_file_modification Whether modification of the file should trigger termination
     */
    void setTerminationFile(std::string termination_file, bool terminate_on_file_modification);
    /**
     * @brief Retrieves the current name of the termination file and also indicates whether the "touched halt" is active.
     * @param terminate_on_file_modification Output parameter set to true if the touched-halt feature is active
     * @return The current termination file name
     */
    std::string getTerminationFile(bool &terminate_on_file_modification) const;

    /** @brief Removes the quality threshold */
    void resetQualityThreshold();
    /**
     * @brief Checks whether a quality threshold has been set.
     * @return true if a quality threshold is active, false otherwise
     */
    bool hasQualityThreshold() const;

    /**
     * @brief Returns the current offset used to calculate the current iteration.
     * @return The starting iteration offset (non-zero when chaining algorithms, e.g. in a Go2 chain)
     */
    std::uint32_t getStartIteration() const;

    /**
     * @brief Sets the number of iterations after which the algorithm should report about its inner state.
     * @param iter The reporting interval in iterations
     */
    void setReportIteration(std::uint32_t iter);
    /**
     * @brief Returns the number of iterations after which the algorithm should report about its inner state.
     * @return The reporting interval in iterations
     */
    std::uint32_t getReportIteration() const;

    /**
     * @brief Retrieves the current number of failed optimization attempts.
     * @return The current stall counter (number of consecutive improvement-free iterations)
     */
    std::uint32_t getStallCounter() const;

    /**
     * @brief Allows to set the number of iterations without improvement, after which individuals are asked to update their internal data structures.
     * @param stall_counter_threshold The stall count after which actOnStalls is triggered
     */
    void setStallCounterThreshold(std::uint32_t stall_counter_threshold);
    /**
     * @brief Allows to retrieve the number of iterations without improvement, after which individuals are asked to update their internal data structures.
     * @return The stall-counter threshold
     */
    std::uint32_t getStallCounterThreshold() const;

    /**
     * @brief Sets the time-to-live (in dispatch rounds) of a networked consumer's late-return buffer
     * entry. A buffered late return that is not reaped within this many rounds is evicted (the drop is
     * counted and warned). Applied to the consumer on the next submission.
     * @param ttl_rounds The late-return buffer TTL, in dispatch rounds
     */
    void setLateReturnTTL(std::uint64_t ttl_rounds);
    /**
     * @brief @return The late-return buffer TTL (in dispatch rounds)
     */
    std::uint64_t getLateReturnTTL() const;

    /**
     * @brief Sets the capacity of a networked consumer's late-return buffer as a MULTIPLE of the
     * population size (the absolute cap is recomputed from the live population on each submission, so it
     * is independent of how a generation is split into submission batches). 0.0 disables late-return
     * buffering entirely.
     * @param cap_factor The late-return buffer capacity as a multiple of the population size (>= 0)
     */
    void setLateReturnCapFactor(double cap_factor);
    /**
     * @brief @return The late-return buffer capacity factor (multiple of the population size)
     */
    double getLateReturnCapFactor() const;

    /**
     * @brief The pure validity + lineage-dedup filter behind getOldWorkItems(), exposed (and static) so
     * it can be unit-tested in isolation without a consumer. Mutates @p items in place, keeping only
     * clean successes whose submission UUID is not already in @p seen; each surviving item's UUID is
     * inserted into @p seen (so within-batch duplicates are also dropped). Pre-load @p seen with the
     * live population's UUIDs to also reject lineages that are still present.
     * @param items The drained late returns to filter (mutated in place)
     * @param seen The set of already-represented submission UUIDs (updated with survivors)
     */
    static void retainIntegrableLateReturns(
        std::vector<std::unique_ptr<gen::GGenome>> &items,
        std::set<Gem::Courtier::SUBMISSION_UUID_TYPE> &seen
    );

    /**
     * @brief Retrieve the best value found in the entire optimization run so far.
     * @return A tuple holding the best known primary fitness (raw, transformed)
     */
    std::tuple<double, double> getBestKnownPrimaryFitness() const;
    /**
     * @brief Retrieves the best value found in the current iteration.
     * @return A tuple holding the best current primary fitness (raw, transformed)
     */
    std::tuple<double, double> getBestCurrentPrimaryFitness() const;

    /**
     * @brief Specifies whether information about termination reasons should be emitted.
     * @param emit_terminatio_reason Whether termination reasons should be emitted (default true)
     */
    void setEmitTerminationReason(bool emit_termination_reason = true);
    /**
     * @brief Retrieves information on whether information about termination reasons should be emitted.
     * @return true if termination reasons are emitted, false otherwise
     */
    bool getEmitTerminationReason() const;

    /******************************************************************************/
    /**
     * This function converts an individual at a given position to the derived
     * type and returns it. In DEBUG mode, the function will check whether the
     * requested position exists.
     *
     * @tparam target_type The concrete individual type the entry should be converted to
     * @param pos The position in our data array that shall be converted
     * @return A converted version of the GGenome object, as required by the user
     */
    template <typename target_type>
    std::shared_ptr<target_type> individual_cast(std::size_t pos) const {
#ifdef DEBUG
        if(pos >= this->size()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GOptimizationAlgorithmBase::individual_cast<>() : Error" << '\n'
                << "Tried to access position " << pos << " which is >= array size " << this->size()
                << '\n'
            );
        }
#endif /* DEBUG */

        // The population owns each individual by unique_ptr. Callers (pluggable monitors) only read the
        // individual transiently, so hand back a NON-OWNING shared_ptr view (no-op deleter) of the live
        // individual rather than co-owning or cloning it -- the population element outlives the call.
        // Does error checks on the conversion internally.
        std::shared_ptr<gen::GGenome> const view(&(*this->at(pos)), [](gen::GGenome *) {});
        return Gem::Common::convertSmartPointer<gen::GGenome, target_type>(view);
    }

    /***************************************************************************/

    /**
     * @brief Retrieve the number of processable items in the current iteration.
     * @return The number of individuals to be evaluated in the current iteration
     */
    std::size_t getNProcessableItems() const;

    /**
     * @brief If individuals have been stored in this population, they are added to the priority queue.
     * @param best_individuals The priority queue that the stored best individuals are added to
     */
    void addCleanStoredBests(gen::GGenomeFixedSizePriorityQueue &best_individuals);

    /**
     * @brief Helper function that determines whether we are currently inside of the first iteration.
     * @return true if the current iteration is the first one, false otherwise
     */
    bool inFirstIteration() const;
    /**
     * @brief Helper function that determines whether we are after the first iteration.
     * @return true if the optimization is past the first iteration, false otherwise
     */
    bool afterFirstIteration() const;

    /**
     * @brief Checks whether a checkpoint-file has the same "personality" as our own algorithm.
     * @param p The path to the checkpoint file to inspect
     * @return true if the checkpoint's personality matches this algorithm, false otherwise
     */
    bool cp_personality_fits(const std::filesystem::path &p) const;

protected:
    /***************************************************************************/
    // Some data

    Gem::Hap::GRandom
        gr_; ///< A random number generator (follows the HAP_RANDOM_SOURCE-selected backend). The actual calculation is done in a random number proxy / factory
    std::uniform_real_distribution<double>
        uniform_real_distribution_; ///< Access to uniformly distributed double random values

    /***************************************************************************/
    // Overridden or virtual protected functions

    /**
     * @brief Lets all individuals know about their position in the population.
     *
     * The position is stamped into the shared GPositionPersonalityTraits base of the
     * algorithm's personality traits, so one implementation serves every algorithm
     * (the per-algorithm copies this replaces differed only in the concrete traits type).
     */
    void markIndividualPositions();

    /**
     * @brief The shared population precondition of the floating-point-only algorithms (CGD, Nelder-
     * Mead): requires a non-empty population whose first individual carries at least one active
     * floating-point parameter (throws otherwise), and logs a note when integer/boolean parameters
     * ride along (they are left unchanged by such an algorithm).
     * @param algorithm_name The calling algorithm's class name, used in the error/log texts
     * @return The number of active floating-point parameters of the first individual
     */
    std::size_t requireFloatingPointGenome_(const std::string &algorithm_name) const;

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb A reference to the parser-builder that collects this algorithm's configuration options
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;
    // load_(), compare_() and name_() are generated by the Gem::Common::GReflectiveInterfaceBaseT base from
    // class_name and the single localMembers_() declaration (which ties in the container base'es
    // population as data_cnt_ and the transient best_iteration_individuals_pq_). clone_() stays pure
    // here -- this is the abstract category root; each concrete algorithm supplies it via GReflectiveInterfaceT.

    /** @brief Resets the class to the state before the optimize call. */
    virtual void resetToOptimizationStart_();

    /** @brief Initialization code to be run before the optimization cycle */
    virtual void init();
    /** @brief Finalization code to be run after the optimization cycle */
    virtual void finalize();

    /***************************************************************************/
    /**
     * A thread pool shared by all optimization algorithms for parallel per-individual
     * "organizational" work (e.g. EA adaption and recombination, SA adaption). It is
     * created in init() (sized by n_threads_) and released in finalize(), so it only
     * lives for the duration of a run and is neither serialized nor cloned.
     */
    std::unique_ptr<Gem::Common::Concurrency::GThreadPool> tp_ptr_;
    /** @brief The number of threads used for parallel organizational work. */
    std::uint16_t n_threads_ = Gem::Common::DEFAULTNHARDWARETHREADS;

    /** @brief Applies modifications to this object */
    bool modify_GUnitTests_();
    /***************************************************************************/

    /**
     * @brief Submits the contiguous sub-range [start, end) of @p work_items for evaluation through
     *  courtier. The algorithm passes the range it wants evaluated explicitly (no per-item DO_PROCESS
     *  flagging needed); the consumer marks and reconciles exactly that span in place.
     * @param work_items The work-item vector whose sub-range is submitted (reconciled in place)
     * @param start The (inclusive) start index of the range to evaluate
     * @param end The (exclusive) end index of the range to evaluate
     * @return The executor status describing the outcome of the submission
     */
    Gem::Courtier::submission_status_t workOn(
        std::vector<std::unique_ptr<gen::GGenome>> &work_items,
        std::size_t start,
        std::size_t end
    );

    /**
     * @brief Submits the population's [start, end) range for evaluation. The population element IS the
     * work item (carrying its own OA scratch), so the population vector is the submission vector:
     * workOn() submits a span over the live sub-range and reconciles it in place -- no move-out /
     * move-back, the scratch rides along on each individual untouched.
     * @param start The (inclusive) start index of the population range to evaluate
     * @param end The (exclusive) end index of the population range to evaluate
     * @return The executor status describing the outcome of the submission
     */
    Gem::Courtier::submission_status_t workOnPopulation(std::size_t start, std::size_t end);

    /**
     * @brief Enforces the "need-all" evaluation policy after a submission: throws if the returned
     * status is incomplete or carries errors. The strict counterpart of discardUnusableItems_() --
     * together the two helpers state the strict/tolerant split exactly once, mirroring the
     * per-algorithm submission policy (getSubmissionPolicy_()).
     * @param status The executor status returned by the submission
     * @param caller The calling function's name, used in the error message
     */
    void requireCompleteEvaluation_(
        const Gem::Courtier::submission_status_t &status,
        const std::string &caller
    ) const;

    /**
     * @brief Applies the "tolerant" evaluation policy after a submission: erases the individuals a
     * partial or errored return left unusable (still due for processing, or error-flagged), so the
     * population continues with evaluated individuals only. In DEBUG builds the number of erased
     * individuals is logged. See requireCompleteEvaluation_().
     * @param status The executor status returned by the submission
     * @param caller The calling function's name, used in the DEBUG log lines
     */
    void discardUnusableItems_(
        const Gem::Courtier::submission_status_t &status,
        const std::string &caller
    );

    /**
     * @brief Drains the consumer's late-return buffer and returns only the late returns that are SAFE
     * TO INTEGRATE -- this is the single, algorithm-agnostic gate every optimization algorithm reaps
     * late returns through. Two universal correctness filters are applied here (NOT per algorithm), so
     * no algorithm can integrate a meaningless or duplicated late return:
     *  - VALIDITY: only clean successes survive (is_processed() and not has_errors()); an errored,
     *    exception-flagged or still-unprocessed return is dropped (its results are meaningless and
     *    selection would otherwise treat them as a real solution).
     *  - LINEAGE de-duplication: a return whose stable per-individual submission UUID is already
     *    represented in the live population (a re-dispatched individual whose fresh copy already
     *    returned) or duplicated within this drained batch (a reclaimed lease re-dispatched one
     *    individual to two clients, both returning late) is dropped, so a lineage is never counted twice.
     * On top of those two universal filters, an OPTIONAL per-algorithm age window is applied here via
     * the virtual lateReturnMaxAge() (default: no window). The remaining reaping policy (personality
     * re-stamping, neighborhood handling) stays in the calling algorithm.
     * @return The integrable (clean, de-duplicated, in-age-window) late returns the consumer buffered
     */
    std::vector<std::unique_ptr<gen::GGenome>> getOldWorkItems() const;

    /**
     * @brief Whether this algorithm reuses late returns -- results that arrived after their submission
     *  batch had already been reconciled in place. Default: FALSE, in which case the networked consumer's
     *  late-return buffer is left DISABLED (nothing is retained on this algorithm's behalf, see
     *  consumerForSubmission_). Population-based algorithms whose selection can absorb an extra candidate
     *  (EA/SA via GParChild, and the swarm algorithm) override this to true. Algorithms whose population
     *  is bound to the current iteration -- gradient descent's finite-difference stencil -- or walked as
     *  an ordered grid (parameter scan) leave it false, so they neither buffer nor reap.
     * @return true if the algorithm reaps and integrates late returns; false to disable late-return buffering
     */
    virtual bool reapsLateReturns() const { return false; }

    /**
     * @brief The maximum age (in optimization iterations since submission) of a late return this
     *  algorithm will integrate, applied by getOldWorkItems() ON TOP of the consumer-side TTL. Default:
     *  std::numeric_limits<std::uint32_t>::max(), i.e. NO age window -- integrate any clean, de-duplicated
     *  late return the consumer still holds. GParChild (EA/SA) overrides this to 1: a child evaluated in
     *  iteration N typically returns during N+1, so a one-generation window admits exactly those late
     *  returns and drops staler ones. Only consulted for reaping algorithms (reapsLateReturns() == true).
     * @return The maximum admissible late-return age in iterations (max() disables the age window)
     */
    virtual std::uint32_t lateReturnMaxAge() const { return std::numeric_limits<std::uint32_t>::max(); }

    /**
     * @brief Returns a fresh personality-traits object for this algorithm. Protected, non-virtual
     *  wrapper around the private getPersonalityTraits_() factory so intermediate base classes (e.g.
     *  GParChild) can mint the correct concrete personality for slots they build themselves.
     * @return A shared pointer to a freshly created personality-traits object for this algorithm
     */
    std::shared_ptr<GPersonalityTraits> makePersonalityTraits() const { return getPersonalityTraits_(); }

    /**
     * @brief Saves the state of the class to disc.
     * @param output_file The path of the checkpoint file to write the algorithm state to
     */
    void saveCheckpoint(std::filesystem::path const &output_file) const;

    /**
     * @brief Extracts the short name of the optimization algorithm.
     * @param p The path from which the algorithm's short name is extracted
     * @return The short name of the optimization algorithm encoded in the path
     */
    static std::string extractOptAlgFromPath(const std::filesystem::path &p);

    /** @brief Allows to set the personality type of the individuals */
    void setIndividualPersonalities();
    /** @brief Resets the individual's personality types */
    void resetIndividualPersonalities();

    /**
     * @brief Sets the default size of the population.
     * @param def_pop_size The nominal (default) population size
     */
    void setDefaultPopulationSize(std::size_t def_pop_size);

    // NB: protected, as a derived function may fall back to this function, cmp EA in non-pareto mode
    /**
     * @brief Adds the individuals of this iteration to the global-best priority queue.
     * @param best_individuals The priority queue the current iteration's individuals are added to
     */
    virtual void updateGlobalBestsPQ_(gen::GGenomeFixedSizePriorityQueue &best_individuals);
    /**
     * @brief Adds the individuals of this iteration to the iteration-best priority queue.
     * @param best_individuals The priority queue the current iteration's individuals are added to
     */
    virtual void updateIterationBestsPQ_(gen::GGenomeFixedSizePriorityQueue &best_individuals);

    /**
     * @brief Set the number of "best" individuals to be recorded in each iteration.
     * @param n_record_best_individuals The number of best individuals to record per iteration
     */
    void setNRecordBestIndividuals(std::size_t n_record_best_individuals);
    /**
     * @brief Retrieve the number of best individuals to be recorded in each iteration.
     * @return The number of best individuals recorded per iteration
     */
    std::size_t getNRecordBestIndividuals() const;

    /** @brief Allows derived classes to reset the stall counter. */
    void resetStallCounter();

    /** @brief Lets individuals know about the current iteration of the optimization cycle. */
    void markIteration();

    /**
     * @brief Whether this optimization run was just resumed from a checkpoint. Set by loadCheckpoint()
     * (after the population -- with its OA-owned scratch -- has been deserialised) and cleared once the
     * run's setup has consumed it (in optimize_(), right after init()). While true, the setup steps
     * PRESERVE the restored per-individual scratch (personality + adaption / swarm / CG POD blocks)
     * instead of re-seeding it, so a resumed algorithm keeps its evolved state. It is deliberately NOT
     * serialized (it is a transient resume marker, and loadCheckpoint sets it AFTER fromFile anyway).
     *
     * @return true if this run was resumed from a checkpoint and its scratch must be preserved
     */
    bool resumedFromCheckpoint() const { return resumed_from_checkpoint_; }
    bool resumed_from_checkpoint_ = false;

private:
    /***************************************************************************/
    // Overloaded or virtual base functions

    /**
     * @brief This function encapsulates some common functionality of iteration-based optimization algorithms.
     * @param offset An iteration offset to start from (non-zero when chaining algorithms, e.g. in a Go2 chain)
     * @return A pointer to this algorithm after the optimization run has completed
     */
    GOptimizationAlgorithmBase const *optimize_(std::uint32_t offset) final;
    /**
     * @brief Creates a deep clone of this object.
     * @return A newly allocated deep copy of this object
     */
    GOptimizationAlgorithmBase *clone_() const override = 0;

    /** @brief Calculates the fitness of all required individuals; to be re-implemented in derived classes */
    void evaluatePopulation_() override = 0;
    /**
     * @brief The actual business logic to be performed during each iteration.
     * @return A tuple holding the best achieved fitness (raw, transformed) of this iteration
     */
    virtual std::tuple<double, double> cycleLogic_() = 0;

    /**
     * @brief Retrieve the current iteration of the optimization run.
     * @return The current iteration number
     */
    std::uint32_t getIteration_() const override;

    /**
     * @brief Retrieves the best individual found up to now.
     * @return A sole-ownership pointer to a copy of the globally best individual
     */
    std::unique_ptr<gen::GGenome> getBestGlobalIndividual_() const final;
    /**
     * @brief Retrieves a list of the best individuals found.
     * @return A vector of sole-ownership pointers to copies of the globally best individuals
     */
    std::vector<std::unique_ptr<gen::GGenome>>
    getBestGlobalIndividuals_() const final;

    /**
     * @brief Retrieves the best individual found in the iteration.
     * @return A sole-ownership pointer to a copy of the best individual of the current iteration
     */
    std::unique_ptr<gen::GGenome> getBestIterationIndividual_() const final;
    /**
     * @brief Retrieves a list of the best individuals found in the current iteration.
     * @return A vector of sole-ownership pointers to copies of the current iteration's best individuals
     */
    std::vector<std::unique_ptr<gen::GGenome>>
    getBestIterationIndividuals_() const final;

    /**
     * @brief Retrieve the number of processable items in the current iteration.
     * @return The number of individuals to be evaluated in the current iteration
     */
    virtual std::size_t getNProcessableItems_() const;

    /**
     * @brief The submission policy this algorithm uses when routed through courtier.
     *  Default: clone-on-partial-return (population-based, tolerant -- EA/SA/Swarm). The "need-all"
     *  algorithms (GD/CGD/Nelder-Mead/ParameterScan), which cannot proceed with a missing or failed
     *  evaluation, override this to full-success-or-fatal.
     * @return The submission policy used for courtier submissions (clone-on-partial-return by default)
     */
    virtual Gem::Courtier::GSubmissionPolicy getSubmissionPolicy_() const {
        return Gem::Courtier::GSubmissionPolicy::clone_on_partial_return();
    }

    /**
     * @brief Retrieve a personality trait object belonging to this algorithm.
     * @return A shared pointer to a freshly created personality-traits object for this algorithm
     */
    virtual std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const = 0;

    /** @brief Resizes the population to the desired level and does some error checks */
    virtual void adjustPopulation_() = 0;

    /**
     * @brief Gives derived classes an opportunity to update their internal structures
     * when the optimization has stalled for the configured number of iterations.
     *
     * The default does nothing -- most algorithms have no per-individual state to
     * refresh on a stall. An algorithm that does reacts by overriding this hook
     * (e.g. GParChild resets the adaption state of all but the best parent).
     */
    virtual void actOnStalls_() { /* nothing */ }

    /***************************************************************************/

    /**
     * @brief Update the stall counter.
     * @param best_eval The best evaluation (raw, transformed) of the current iteration used to decide on improvement
     */
    void updateStallCounter(const std::tuple<double, double> &best_eval);

    /**
     * @brief This function returns true once a given time has passed.
     * @param current_time The current time against which the maximum duration is checked
     * @return true if the maximum duration has been exceeded, false otherwise
     */
    bool timedHalt(const std::chrono::system_clock::time_point &current_time) const;
    /**
     * @brief This function checks whether a minimum amount of time has passed.
     * @param current_time The current time against which the minimum duration is checked
     * @return true if the minimum duration has passed, false otherwise
     */
    bool minTimePassed(const std::chrono::system_clock::time_point &current_time) const;

    /**
     * @brief This function returns true once the quality has passed a given threshold.
     * @return true if the quality threshold has been reached, false otherwise
     */
    bool qualityHalt() const;

    /**
     * @brief This function returns true once a given number of stalls has been exceeded in a row.
     * @return true if the maximum number of stalls has been exceeded, false otherwise
     */
    bool stallHalt() const;

    /**
     * @brief This function returns true once a maximum number of iterations has been exceeded.
     * @return true if the maximum number of iterations has been exceeded, false otherwise
     */
    bool iterationHalt() const;
    /**
     * @brief This function returns true when the minimum number of iterations has been passed.
     * @return true if the minimum number of iterations has been passed, false otherwise
     */
    bool minIterationPassed() const;

    /**
     * @brief This function returns true if a SIGHUP / CTRL_CLOSE_EVENT signal was sent.
     * @return true if a termination signal was received, false otherwise
     */
    static bool sigHupHalt();

    /**
     * @brief Triggers termination of the optimization run when a user-defined file is modified.
     * @return true if the termination file was modified after the run started, false otherwise
     */
    bool touchHalt() const;

    /**
     * @brief A wrapper for customHalt_ that allows to emit the termination reason.
     * @return true if the custom halt criterion has been reached, false otherwise
     */
    bool customHalt() const;
    /**
     * @brief Custom setting of halt criteria.
     * @return true if the custom halt criterion has been reached, false otherwise
     */
    virtual bool customHalt_() const;

    /**
     * @brief This function checks whether a halt criterion has been reached.
     * @return true if any halt criterion has been reached, false otherwise
     */
    bool halt() const;

    /**
     * @brief Check whether the max-iteration halt is set.
     * @return true if a maximum-iteration halt criterion is active, false otherwise
     */
    bool maxIterationHaltset() const;
    /**
     * @brief Check whether a halt criterion based on the number of stalls has been set.
     * @return true if a stall-based halt criterion is active, false otherwise
     */
    bool stallHaltSet() const;

    /**
     * @brief Check whether the max_duration-halt criterion has been set.
     * @return true if a maximum-duration halt criterion is active, false otherwise
     */
    bool maxDurationHaltSet() const;

    /**
     * @brief Check whether the quality-threshold halt-criterion has been set.
     * @return true if a quality-threshold halt criterion is active, false otherwise
     */
    bool qualityThresholdHaltSet() const;

    /**
     * @brief Indicates whether the stall_counter_threshold_ has been exceeded.
     * @return true if the stall-counter threshold has been exceeded, false otherwise
     */
    bool stallCounterThresholdExceeded() const;

    /***************************************************************************/
    // Data

    std::uint32_t iteration_ = 0; ///< The current iteration
    std::uint32_t offset_ =
        DEFAULTOFFSET; ///< An iteration offset which can be used, if the optimization starts from a checkpoint file
    std::uint32_t min_iteration_ = DEFAULTMINIT; ///< The minimum number of iterations
    std::uint32_t max_iteration_ = DEFAULTMAXIT; ///< The maximum number of iterations
    std::uint32_t max_stall_iteration_ =
        DEFAULTMAXSTALLIT; ///< The maximum number of generations without improvement, after which optimization is stopped
    std::uint32_t report_iteration_ =
        DEFAULTREPORTITER; ///< The number of generations after which a report should be issued

    std::size_t n_recordbest_global_individuals_ =
        DEFNRECORDBESTINDIVIDUALS; ///< Indicates the number of best individuals to be recorded/updated in each iteration
    gen::GGenomeFixedSizePriorityQueue best_global_individuals_pq_{
        n_recordbest_global_individuals_
    }; ///< A priority queue with the best individuals found so far
    gen::GGenomeFixedSizePriorityQueue best_iteration_individuals_pq_{
        n_recordbest_global_individuals_
    }; ///< A priority queue with the best individuals of a given iteration; capped at n_recordbest_global_individuals_ (same capacity as the global-best queue)

    std::size_t default_population_size_ =
        DEFAULTPOPULATIONSIZE; ///< The nominal size of the population
    std::tuple<double, double> best_known_primary_fitness_ =
        std::tuple<double, double>(0., 0.); ///< Records the best primary fitness found so far
    std::tuple<double, double> best_current_primary_fitness_ = std::tuple<double, double>(
        0.,
        0.
    ); ///< Records the best fitness found in the current iteration

    std::uint32_t stall_counter_ = 0; ///< Counts the number of iterations without improvement
    std::uint32_t stall_counter_threshold_ =
        DEFAULTSTALLCOUNTERTHRESHOLD; ///< The number of stalls after which individuals are asked to update their internal data structures

    std::uint64_t late_return_ttl_ =
        DEFAULTLATERETURNTTL; ///< TTL (in dispatch rounds) of a networked consumer's late-return buffer entry
    double late_return_cap_factor_ =
        DEFAULTLATERETURNCAPFACTOR; ///< Late-return buffer capacity as a multiple of the population size (0 disables buffering)

    std::int32_t cp_interval_ =
        DEFAULTCHECKPOINTIT; ///< Number of iterations after which a checkpoint should be written. -1 means: Write whenever an improvement was encountered
    std::string cp_base_name_ = DEFAULTCPBASENAME; ///< The base name of the checkpoint file
    std::filesystem::path cp_directory_path_ =
        std::filesystem::path(DEFAULTCPDIR); ///< Path object to store the directory
    mutable std::string cp_last_ = "empty"; ///< The name of the last saved checkpoint
    bool cp_remove_ = true; ///< Whether checkpoint files should be overwritten or kept
    Gem::Common::serializationMode cp_serialization_mode_ =
        DEFAULTCPSERMODE; ///< Determines whether check-pointing should be done in text-, XML, or binary mode
    double quality_threshold_ =
        DEFAULTQUALITYTHRESHOLD; ///< A threshold beyond which optimization is expected to stop
    bool has_quality_threshold_ = false; ///< Specifies whether a quality_threshold has been set
    std::chrono::duration<double> max_duration_ = Gem::Common::duration_from_string(
        DEFAULTDURATION
    ); ///< Maximum time-frame for the optimization
    std::chrono::duration<double> min_duration_ = Gem::Common::duration_from_string(
        DEFAULTMINDURATION
    ); ///< Minimum time-frame for the optimization
    mutable std::chrono::system_clock::time_point
        start_time_; ///< Used to store the start time of the optimization. Declared mutable so the halt criteria can be const
    mutable std::filesystem::file_time_type
        file_start_time_; ///< Used for the touchHalt-feature, as system_clock file_time may not be comparable
    std::string termination_file_ =
        DEFAULTTERMINATIONFILE; ///< The name of a file which, when modified after the start of the optimization run, will cause termination of the run
    bool terminate_on_file_modification_ = false;
    bool emit_termination_reason_ =
        DEFAULTEMITTERMINATIONREASON; ///< Specifies whether information about reasons for termination should be emitted
    std::atomic<bool> halted_{true}; ///< Set to true when halt() has returned "true"
    std::vector<std::tuple<double, double>>
        worst_known_valids_cnt_; ///< Stores the worst known valid evaluations up to the current iteration (first entry: raw, second: tranformed)
    /// A collection of monitors. Shared, not owned: a monitor registered through Go2 is
    /// co-owned by Go2 and by every algorithm of a chain, so that it observes the whole run
    /// (see Go2::pluggable_monitors_cnt_). Deep-cloned only when one algorithm is loaded from
    /// another, which is a different object and must not share the original's monitors.
    std::vector<std::shared_ptr<GBasePluggableOM>> pluggable_monitors_cnt_;

    /**
     * @brief Submits the contiguous sub-range [start, end) of @p work_items through the one process-wide
     * consumer (GConsumerRegistry), which the setup layer establishes if the process was never given one.
     * @param work_items The work-item vector whose sub-range is submitted (reconciled in place)
     * @param start The (inclusive) start index of the range to evaluate
     * @param end The (exclusive) end index of the range to evaluate
     * @return The executor status describing the outcome of the submission
     */
    Gem::Courtier::submission_status_t workOnViaConsumer_(
        std::vector<std::unique_ptr<gen::GGenome>> &work_items,
        std::size_t start,
        std::size_t end
    );

    /** @brief Returns the one process-wide consumer (established by the setup layer if the process was
     *  never given one) with this algorithm's late-return budget applied.
     *  @return The shared consumer this algorithm submits through */
    std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> consumerForSubmission_();
};

/*******************************************************************************/
/////////////////////////////////////////////////////////////////////////////////
/*******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

/******************************************************************************/
