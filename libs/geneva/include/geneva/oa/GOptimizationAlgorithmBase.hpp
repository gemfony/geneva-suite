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
#include "geneva/GNamespaceAliases.hpp"

// Standard header files go here
#include <chrono>
#include <concepts>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// Boost header files go here

// Geneva headers go here
#include "common/GChecks.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GContainerT.hpp"
#include "common/GReflectiveInterfaceT.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "common/concurrency/GThreadPool.hpp"
#include "courtier/GCourtierEnums.hpp"     // SUBMISSION_UUID_TYPE (late-return lineage de-dup)
#include "courtier/GSubmissionStatusT.hpp" // submission_status_t (workOn's return type)
// --- Submission goes through the one process-wide consumer (GConsumerRegistry): the algorithm is
//     transport-agnostic, it just reads that consumer and calls processBatch(), see workOn ---
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/Interface/GOptimizerIT.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeFixedSizePriorityQueue.hpp"
#include "geneva/oa/GBasePluggableOM.hpp" // the pluggable-monitor CRTP root (extracted from this header)

namespace Gem::Geneva::OptimizationAlgorithms {

// Re-export the shared numeric enum stream operators (see numeric_enum_values_v in
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
        GOptimizationAlgorithmBase,
        Gem::Common::GCommonInterfaceT<GOptimizationAlgorithmBase>>
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
    // overloads are trivial forwarders. self_type is deduced as the (const) class type.
    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            // The population held by the GUniquePtrContainerT base, tied in as a member (see above).
            Gem::Common::make_cloneable_container_member("data_cnt_", self.data_cnt_),
            Gem::Common::make_member("iteration_", self.iteration_),
            Gem::Common::make_member("offset_", self.offset_),
            Gem::Common::make_member("chain_offset_", self.chain_offset_),
            Gem::Common::make_member("max_iteration_", self.max_iteration_),
            Gem::Common::make_member("min_iteration_", self.min_iteration_),
            Gem::Common::make_member("max_stall_iteration_", self.max_stall_iteration_),
            Gem::Common::make_member("report_iteration_", self.report_iteration_),
            Gem::Common::make_member(
                "n_recordbest_global_individuals_",
                self.n_recordbest_global_individuals_
            ),
            Gem::Common::make_member(
                "best_global_individuals_pq_",
                self.best_global_individuals_pq_
            ),
            Gem::Common::make_member("default_population_size_", self.default_population_size_),
            Gem::Common::make_member(
                "best_known_primary_fitness_",
                self.best_known_primary_fitness_
            ),
            Gem::Common::make_member(
                "best_current_primary_fitness_",
                self.best_current_primary_fitness_
            ),
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
            Gem::Common::make_member(
                "terminate_on_file_modification_",
                self.terminate_on_file_modification_
            ),
            Gem::Common::make_member("emit_termination_reason_", self.emit_termination_reason_),
            Gem::Common::make_member("worst_known_valids_cnt_", self.worst_known_valids_cnt_),
            Gem::Common::make_member("n_threads_", self.n_threads_),
            Gem::Common::make_atomic_member("halted_", self.halted_),
            Gem::Common::make_cloneable_container_member(
                "pluggable_monitors_cnt_",
                self.pluggable_monitors_cnt_
            ),
            // Transient per-iteration best set: not persisted, but part of comparable identity (see above).
            Gem::Common::make_transient_member(
                "best_iteration_individuals_pq_",
                self.best_iteration_individuals_pq_
            )
        );
    }

    /**
     * @brief The settings of the job that runs this algorithm, as references: loadCheckpoint() keeps
     * them across reading a checkpoint.
     *
     * A checkpoint supplies the state of a run -- the population, the iteration counter, the
     * best-so-far records, the stall counter, the worst known valid evaluations, and the search
     * state and search parameters of the derived algorithms. What the job that resumes it is to do
     * with that state comes from that job's own configuration and command line: these halt
     * criteria, checkpoint settings, threads, report interval and termination messages (maintainer
     * decision, 2026-11-20). Kept from the checkpoint instead, a resumed run could not be given a
     * higher iteration limit, nor write its checkpoints elsewhere. The pluggable optimization
     * monitors belong to the job as well (maintainer decision, 2026-11-21): those registered before
     * the load stay, those of the checkpoint are dropped -- else GOptimizer, which registers the
     * job's monitors after the load, would run each monitor twice.
     *
     * @tparam self_type The (deduced) class type
     * @param self The algorithm
     * @return A tuple of references to the settings
     */
    template <typename self_type>
    auto jobSettings_(this self_type &self) {
        return std::tie(
            self.max_iteration_,
            self.min_iteration_,
            self.max_stall_iteration_,
            self.max_duration_,
            self.min_duration_,
            self.quality_threshold_,
            self.has_quality_threshold_,
            self.termination_file_,
            self.terminate_on_file_modification_,
            self.cp_interval_,
            self.cp_base_name_,
            self.cp_directory_path_,
            self.cp_last_,
            self.cp_remove_,
            self.cp_serialization_mode_,
            self.n_threads_,
            self.report_iteration_,
            self.emit_termination_reason_,
            self.pluggable_monitors_cnt_
        );
    }

    /** @brief Disambiguating serialize(): both stateful bases in the inheritance set (the GReflectiveInterfaceBaseT
     *  mixin and the GUniquePtrContainerT base) declare a serialize(), so this one-liner resolves the
     *  ambiguity and emits the single member list -- which now ties in the container's population as
     *  data_cnt_ and skips the transient best_iteration_individuals_pq_. The stateless GCommonInterfaceT
     *  root and GOptimizerIT interface contribute nothing.
     *  @tparam archive_type The archive type used for (de-)serialization
     *  @param ar The archive to serialize to / from
     *  @param version The (unused) serialization format version */
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::serialize_members(ar, this->localMembers_());
        if constexpr(not archive_type::is_saving) {
            // The transient iteration archive is not in the archive: it takes the loaded capacity
            best_iteration_individuals_pq_.setMaxSize(n_recordbest_global_individuals_);
        }
    }

    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceBaseT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GOptimizationAlgorithmBase";

    // The private split-serialization member load(archive_type&, unsigned) below
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
     */
    GOptimizationAlgorithmBase(GOptimizationAlgorithmBase const &cp);

    /***************************************************************************/
    // Defaulted functions

    GOptimizationAlgorithmBase() = default;
    ~GOptimizationAlgorithmBase() override = default;

    /***************************************************************************/

    /**
     * @brief The algorithm's command-line mnemonic ("ea", "nm", ...): the key under which its factory
     *  is registered, and what a post-processor's allow-list names.
     * @return The mnemonic of this algorithm
     */
    [[nodiscard]] std::string getAlgorithmMnemonic() const {
        return this->getAlgorithmMnemonic_();
    }

    /**
     * @brief Performs the necessary administratory work of doing check-pointing.
     */
    void checkpoint(bool is_better) const;

    /**
     * @brief Loads the state of a run from a checkpoint file; the settings of this job
     * (jobSettings_(): halt criteria, checkpoint settings, pluggable monitors, ...) stay as they are.
     * A checkpoint of another algorithm is refused (its type is the file's first field).
     */
    void loadCheckpoint(std::filesystem::path const &cp_file);

    /**
     * @brief Writes a checkpoint of this algorithm to a file: its mnemonic first, then the
     * algorithm with its population (checkpoint() calls it with the conventional file name).
     */
    void saveCheckpoint(std::filesystem::path const &output_file) const;

    /**
     * @brief Hands this algorithm an OA-owned adaption configuration to use for the run.
     * GOptimizer installs the config it holds for this algorithm's type before the algorithm runs
     * in a chain; an adapting algorithm (EA / SA) adopts it (validating that it matches the
     * population's genome) instead of deriving a default from the genome layout. The base does
     * nothing -- non-adapting algorithms (swarm, gradient descent, scan) have no adaption config.
     *
     * The argument is the OA-owned adaption configuration to adopt for this run.
     */
    virtual void setAdaptionConfig(std::shared_ptr<GAdaptionConfigBase> /* config */) { /* no-op */
    }

    /**
     * @brief The OA-owned adaption configuration in effect for this run, or null. Adapting algorithms
     * (EA / SA) build it at init() and return it here so telemetry (e.g. the adaptor-property monitor) can
     * read each group's adaptor settings without snapshotting the structure-only genome layout. The base
     * (non-adapting algorithms) returns null.
     *
     * @return The OA-owned adaption configuration in effect for this run, or null if there is none
     */
    virtual std::shared_ptr<const GAdaptionConfigBase> getAdaptionConfig() const {
        return {};
    }

    /**
     * @brief Checks whether the optimization process has been halted.
     */
    bool halted() const;

    /**
     * @brief Allows to set the number of generations after which a checkpoint should be written.
     */
    void setCheckpointInterval(std::int32_t cp_interval);
    /**
     * @brief Allows to retrieve the number of generations after which a checkpoint should be written.
     */
    std::int32_t getCheckpointInterval() const;

    /**
     * @brief Allows to set the base name of the checkpoint file and the directory where it should be stored.
     * A missing directory is created lazily when the first checkpoint is written; configuring has no
     * filesystem side effects (an existing path must be a directory, though).
     */
    void setCheckpointBaseName(const std::string &cp_directory, const std::string &cp_base_name);
    /**
     * @brief Allows to retrieve the base name of the checkpoint file.
     */
    std::string getCheckpointBaseName() const;
    /**
     * @brief Allows to retrieve the directory where checkpoint files should be stored.
     */
    std::string getCheckpointDirectory() const;
    /**
     * @brief Allows to retrieve the directory where checkpoint files should be stored.
     */
    std::filesystem::path getCheckpointDirectoryPath() const;
    /**
     * @brief Determines whether checkpoints are written in the binary or the JSON mode.
     */
    void setCheckpointSerializationMode(Gem::Common::serializationMode cp_ser_mode);
    /**
     * @brief Retrieves the current checkpointing serialization mode.
     */
    Gem::Common::serializationMode getCheckpointSerializationMode() const;
    /**
     * @brief Allows to set the cp_remove_ flag.
     */
    void setRemoveCheckpointFiles(bool cp_remove);
    /**
     * @brief Allows to check whether checkpoint files will be removed.
     */
    bool checkpointFilesAreRemoved() const;

    /** @brief Resets the class to the state before the optimize call. */
    void resetToOptimizationStart();

    /******************************************************************************/
    /**
     * @brief Sets the number of threads used for parallel organizational work (adaption,
     *  recombination, ...). 0 means the hardware threads assigned to the process.
     */
    void setNThreads(std::uint16_t n_threads);
    /**
     * @brief Retrieves the number of threads used for parallel organizational work (0: the
     *  hardware threads assigned to the process).
     */
    [[nodiscard]] std::uint16_t getNThreads() const;

    /******************************************************************************/

    /**
     * @brief Emits information specific to this class.
     */
    void informationUpdate(const infoMode &im);

    /**
     * @brief Checks whether a better solution was found.
     */
    bool progress() const;

    /**
     * @brief Allows to register a pluggable optimization monitor.
     */
    void registerPluggableOM(const std::shared_ptr<GBasePluggableOM> &pluggable_om);
    /** @brief Allows to reset the local pluggable optimization monitors */
    void resetPluggableOM();
    /**
     * @brief Allows to check whether pluggable optimization monitors were registered.
     */
    bool hasPluggableOptimizationMonitors() const;

    /**
     * @brief Retrieves the default population size.
     */
    std::size_t getDefaultPopulationSize() const;
    /**
     * @brief Retrieve the current population size.
     */
    std::size_t getPopulationSize() const;

    /**
     * @brief Set the number of iterations after which the optimization should be stopped.
     */
    void setMaxIteration(std::uint32_t max_iteration);
    /**
     * @brief Retrieve the number of iterations after which optimization should be stopped.
     */
    std::uint32_t getMaxIteration() const;

    /**
     * @brief Sets the minimum number of iterations.
     */
    void setMinIteration(std::uint32_t min_iteration);
    /**
     * @brief Retrieves the currently set minimum number of iterations.
     */
    std::uint32_t getMinIteration() const;

    /**
     * @brief Sets the number of best individuals recorded: the capacity of the global and of the
     * iteration archive (the key n_record_best_individuals).
     */
    void setNRecordBestIndividuals(std::size_t n_record_best_individuals);
    /**
     * @brief Retrieves the number of best individuals recorded in each archive.
     */
    std::size_t getNRecordBestIndividuals() const;

    /**
     * @brief Sets the maximum number of iterations allowed without improvement of the best individual.
     */
    void setMaxStallIteration(std::uint32_t max_stall_iteration);
    /**
     * @brief Retrieves the maximum number of generations allowed in an optimization run without improvement of the best individual.
     */
    std::uint32_t getMaxStallIteration() const;

    /**
     * @brief Sets the maximum allowed processing time.
     */
    void setMaxTime(std::chrono::duration<double> max_duration);
    /**
     * @brief Retrieves the value of the max_duration_ parameter.
     */
    std::chrono::duration<double> getMaxTime() const;

    /**
     * @brief Sets the minimum required processing time.
     */
    void setMinTime(std::chrono::duration<double> min_duration);
    /**
     * @brief Retrieves the value of the min_duration_ parameter.
     */
    std::chrono::duration<double> getMinTime() const;

    /**
     * @brief Sets a quality threshold beyond which optimization is expected to stop.
     */
    void setQualityThreshold(double quality_threshold, bool has_quality_threshold);
    /**
     * @brief Retrieves the current value of the quality threshold and also indicates whether the threshold is active.
     */
    double getQualityThreshold(bool &has_quality_threshold) const;

    /**
     * @brief Sets the name of a "termination file".
     */
    void setTerminationFile(std::string termination_file, bool terminate_on_file_modification);
    /**
     * @brief Retrieves the current name of the termination file and also indicates whether the "touched halt" is active.
     */
    std::string getTerminationFile(bool &terminate_on_file_modification) const;

    /** @brief Removes the quality threshold */
    void resetQualityThreshold();
    /**
     * @brief Checks whether a quality threshold has been set.
     */
    bool hasQualityThreshold() const;

    /**
     * @brief Returns the current offset used to calculate the current iteration.
     */
    std::uint32_t getStartIteration() const;

    /** @brief Sets the number of iterations of a GOptimizer chain that ran before this algorithm */
    void setChainOffset(std::uint32_t chain_offset);
    /** @brief The number of iterations of a GOptimizer chain that ran before this algorithm */
    [[nodiscard]] std::uint32_t getChainOffset() const;

    /**
     * @brief Sets the number of iterations after which the algorithm should report about its inner state.
     */
    void setReportIteration(std::uint32_t iter);
    /**
     * @brief Returns the number of iterations after which the algorithm should report about its inner state.
     */
    std::uint32_t getReportIteration() const;

    /**
     * @brief Retrieves the current number of failed optimization attempts.
     */
    std::uint32_t getStallCounter() const;

    /**
     * @brief Allows to set the number of iterations without improvement, after which individuals are asked to update their internal data structures.
     */
    void setStallCounterThreshold(std::uint32_t stall_counter_threshold);
    /**
     * @brief Allows to retrieve the number of iterations without improvement, after which individuals are asked to update their internal data structures.
     */
    std::uint32_t getStallCounterThreshold() const;

    /**
     * @brief Sets the time-to-live (in dispatch rounds) of a networked consumer's late-return buffer
     * entry. A buffered late return that is not reaped within this many rounds is evicted (the drop is
     * counted and warned). Applied to the consumer on the next submission.
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
     */
    static void retainIntegrableLateReturns(
        std::vector<std::unique_ptr<gen::GGenome>> &items,
        std::set<Gem::Courtier::SUBMISSION_UUID_TYPE> &seen
    );

    /**
     * @brief Retrieve the best value found in the entire optimization run so far.
     */
    std::tuple<double, double> getBestKnownPrimaryFitness() const;
    /**
     * @brief Retrieves the best value found in the current iteration.
     */
    std::tuple<double, double> getBestCurrentPrimaryFitness() const;

    /**
     * @brief Specifies whether information about termination reasons should be emitted.
     */
    void setEmitTerminationReason(bool emit_termination_reason = true);
    /**
     * @brief Retrieves information on whether information about termination reasons should be emitted.
     */
    bool getEmitTerminationReason() const;

    /******************************************************************************/
    /**
     * This function converts an individual at a given position to the derived
     * type and returns it. The function checks whether the
     * requested position exists.
     *
     * @tparam target_type The concrete individual type the entry should be converted to
     * @param pos The position in our data array that shall be converted
     * @return A converted version of the GGenome object, as required by the user
     */
    template <typename target_type>
    std::shared_ptr<target_type> individual_cast(std::size_t pos) const {
        Gem::Common::checkThat(
            pos < this->size(),
            "position ",
            pos,
            " >= population size ",
            this->size()
        );

        // The population owns each individual by unique_ptr. Callers (pluggable monitors) only read the
        // individual transiently, so hand back a NON-OWNING shared_ptr view (no-op deleter) of the live
        // individual rather than co-owning or cloning it -- the population element outlives the call.
        // Does error checks on the conversion internally.
        std::shared_ptr<gen::GGenome> const view(&(*this->at(pos)), [](gen::GGenome *) {});
        return Gem::Common::g_ptr_conversion<gen::GGenome, target_type>(view);
    }

    /***************************************************************************/

    /**
     * @brief Retrieve the number of processable items in the current iteration.
     */
    std::size_t getNProcessableItems() const;

    /**
     * @brief If individuals have been stored in this population, they are added to the priority queue.
     */
    void addCleanStoredBests(gen::GGenomeFixedSizePriorityQueue &best_individuals);

    /**
     * @brief Whether this is the first iteration of the run as a whole (never in a run resumed from
     * a checkpoint).
     */
    bool inFirstIteration() const;
    /**
     * @brief Whether the run as a whole is past its first iteration (always in a run resumed from a
     * checkpoint).
     */
    bool afterFirstIteration() const;

    /**
     * @brief Checks whether a checkpoint file was written by an algorithm with our own mnemonic
     * (read from the file, not from its name).
     */
    bool cp_mnemonic_fits(const std::filesystem::path &p) const;

    /**
     * @brief Changes this algorithm for the standard tests (common/testing/GStandardTests.hpp): its
     *  maximum iteration, and every member of its population through GGenome::modifyForTest(). An
     *  algorithm that adds settings calls this function and changes its own.
     */
    bool modifyForTest();

protected:
    /***************************************************************************/
    // Some data

    /// transient: a random number generator (follows the HAP_RANDOM_SOURCE-selected backend); the
    /// numbers come from a proxy of the process-wide factory
    Gem::Hap::GRandom gr_;
    /// transient: access to uniformly distributed double random values, stateless between draws
    std::uniform_real_distribution<double> uniform_real_distribution_;

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
     */
    std::size_t requireFloatingPointGenome_(const std::string &algorithm_name) const;

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;
    // load_(), compare_() and name_() are generated by the Gem::Common::GReflectiveInterfaceBaseT base from
    // class_name and the single localMembers_() declaration (which ties in the container base'es
    // population as data_cnt_ and the transient best_iteration_individuals_pq_). clone_() stays pure
    // here -- this is the abstract category root; each concrete algorithm supplies it via GReflectiveInterfaceT.

    /** @brief Resets the class to the state before the optimize call. */
    virtual void resetToOptimizationStart_();

    /**
     * @brief Builds the run state of a new run; the setup calls it only when a run starts fresh, never
     * when it was resumed from a checkpoint.
     *
     * The rule for the author of an algorithm: what carries a run from one iteration to the next is
     * a member of the member list, so the checkpoint holds it; what a new run starts it with is built
     * here; init() does what every start needs, a resumed one as well (sizes, pools, checks). The setup
     * calls adjustPopulation_(), then this function on a fresh start, then init(). The default does
     * nothing.
     */
    virtual void initRunState_() {
        // nothing: an algorithm without a run state of its own builds none
    }

    /** @brief Initialization code to be run before the optimization cycle */
    virtual void init();
    /** @brief Finalization code to be run after the optimization cycle */
    virtual void finalize();

    /***************************************************************************/
    /**
     * transient: a thread pool shared by all optimization algorithms for parallel per-individual
     * "organizational" work (e.g. EA adaption and recombination, SA adaption). It is
     * created in init() (sized by n_threads_) and released in finalize(), so it only
     * lives for the duration of a run and is neither serialized nor cloned.
     */
    std::unique_ptr<Gem::Common::Concurrency::GThreadPool> tp_ptr_;
    /** @brief The number of threads used for parallel organizational work. */
    std::uint16_t n_threads_ = Gem::Common::DEFAULTNHARDWARETHREADS;

    /***************************************************************************/

    /**
     * @brief Submits the contiguous sub-range [start, end) of @p work_items for evaluation through
     *  courtier. The algorithm passes the range it wants evaluated explicitly (no per-item DO_PROCESS
     *  flagging needed); the consumer marks and reconciles exactly that span in place.
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
     */
    Gem::Courtier::submission_status_t workOnPopulation(std::size_t start, std::size_t end);

    /**
     * @brief Erases the individuals an evaluation outside courtier left unusable (still due for
     * processing, or error-flagged), so the population continues with evaluated individuals only. A
     * submission through workOn() needs none of this: the consumer reconciles the batch by the
     * algorithm's submission policy (getSubmissionPolicy_()) and returns only evaluated items, a
     * substitute marked as such. An algorithm that evaluates on its own (the meta-EA runs its
     * individuals on a pool of its own) reports what it got in @p status.
     */
    void discardUnusableItems_(const Gem::Courtier::submission_status_t &status);

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
    virtual bool reapsLateReturns() const {
        return false;
    }

    /**
     * @brief The maximum age (in optimization iterations since submission) of a late return this
     *  algorithm will integrate, applied by getOldWorkItems() ON TOP of the consumer-side TTL. Default:
     *  std::numeric_limits<std::uint32_t>::max(), i.e. NO age window -- integrate any clean, de-duplicated
     *  late return the consumer still holds. GParChild (EA/SA) overrides this to 1: a child evaluated in
     *  iteration N typically returns during N+1, so a one-generation window admits exactly those late
     *  returns and drops staler ones. Only consulted for reaping algorithms (reapsLateReturns() == true).
     * @return The maximum admissible late-return age in iterations (max() disables the age window)
     */
    virtual std::uint32_t lateReturnMaxAge() const {
        return std::numeric_limits<std::uint32_t>::max();
    }

    /**
     * @brief Returns a fresh personality-traits object for this algorithm. Protected, non-virtual
     *  wrapper around the private getPersonalityTraits_() factory so intermediate base classes (e.g.
     *  GParChild) can mint the correct concrete personality for slots they build themselves.
     * @return A shared pointer to a freshly created personality-traits object for this algorithm
     */
    std::shared_ptr<GPersonalityTraits> makePersonalityTraits() const {
        return getPersonalityTraits_();
    }

    /** @brief Allows to set the personality type of the individuals */
    void setIndividualPersonalities();
    /** @brief Resets the individual's personality types */
    void resetIndividualPersonalities();

    /**
     * @brief Sets the default size of the population.
     */
    void setDefaultPopulationSize(std::size_t def_pop_size);

    // NB: protected, as a derived function may fall back to this function, cmp EA in non-pareto mode
    /**
     * @brief Adds the individuals of this iteration to the global-best priority queue.
     */
    virtual void updateGlobalBestsPQ_(gen::GGenomeFixedSizePriorityQueue &best_individuals);
    /**
     * @brief Adds the individuals of this iteration to the iteration-best priority queue.
     */
    virtual void updateIterationBestsPQ_(gen::GGenomeFixedSizePriorityQueue &best_individuals);

    /** @brief Allows derived classes to reset the stall counter. */
    void resetStallCounter();

    /** @brief Lets individuals know about the current iteration of the optimization cycle. */
    void markIteration();

public:
    /**
     * @brief Whether this optimization run was just resumed from a checkpoint. Set by loadCheckpoint()
     * (after the population -- with its OA-owned scratch -- has been deserialised) and cleared once the
     * run's setup has consumed it (in optimize_(), right after init()). While true, the setup steps
     * PRESERVE the restored per-individual scratch (personality + adaption / swarm / CG POD blocks)
     * instead of re-seeding it, so a resumed algorithm keeps its evolved state. It is deliberately NOT
     * serialized (it is a transient resume marker, and loadCheckpoint sets it AFTER load_() anyway).
     *
     * A pluggable monitor reads it at INFOINIT, which comes before it is cleared: a resumed run
     * appends to the streams of its monitors (GMonitorStream::declare()). An algorithm does not ask
     * it: it builds the state of a new run in initRunState_(), which the setup of a resumed run does
     * not call.
     *
     * @return true if this run was resumed from a checkpoint and its scratch must be preserved
     */
    [[nodiscard]] bool resumedFromCheckpoint() const {
        return resumed_from_checkpoint_;
    }

private:
    bool resumed_from_checkpoint_ =
        false; ///< transient: set by the loading of a checkpoint (see resumedFromCheckpoint())
    /// transient: whether this job goes on with a run resumed from a checkpoint; set at the setup of
    /// the job from resumed_from_checkpoint_ and kept for the whole job (inFirstIteration())
    bool continues_run_ = false;

    /***************************************************************************/
    // Overloaded or virtual base functions

    /**
     * @brief This function encapsulates some common functionality of iteration-based optimization algorithms.
     */
    GOptimizationAlgorithmBase &optimize_(std::uint32_t offset) final;
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
     */
    std::uint32_t getIteration_() const override;

    /**
     * @brief Retrieves the best individual found up to now.
     */
    std::unique_ptr<gen::GGenome> getBestGlobalIndividual_() const final;
    /**
     * @brief Retrieves a list of the best individuals found.
     */
    std::vector<std::unique_ptr<gen::GGenome>> getBestGlobalIndividuals_() const final;

    /**
     * @brief Retrieves the best individual found in the iteration.
     */
    std::unique_ptr<gen::GGenome> getBestIterationIndividual_() const final;
    /**
     * @brief Retrieves a list of the best individuals found in the current iteration.
     */
    std::vector<std::unique_ptr<gen::GGenome>> getBestIterationIndividuals_() const final;

    /**
     * @brief Retrieve the number of processable items in the current iteration.
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

    /** @brief The algorithm's mnemonic; generated by GOptimizationAlgorithmT from oa_mnemonic. */
    [[nodiscard]] virtual std::string getAlgorithmMnemonic_() const = 0;

    /** @brief Resizes the population to the desired level, determines the sizes initRunState_() and
     *  init() rely on and does some error checks */
    virtual void adjustPopulation_() = 0;

    /**
     * @brief Gives derived classes an opportunity to update their internal structures
     * when the optimization has stalled for the configured number of iterations.
     *
     * The default does nothing -- most algorithms have no per-individual state to
     * refresh on a stall. An algorithm that does reacts by overriding this hook
     * (e.g. GParChild resets the adaption state of all but the best parent).
     */
    virtual void actOnStalls_() { /* nothing */
    }

    /***************************************************************************/

    /**
     * @brief Update the stall counter.
     */
    void updateStallCounter(const std::tuple<double, double> &best_eval);

    /**
     * @brief This function returns true once a given time has passed.
     */
    bool timedHalt(const std::chrono::system_clock::time_point &current_time) const;
    /**
     * @brief This function checks whether a minimum amount of time has passed.
     */
    bool minTimePassed(const std::chrono::system_clock::time_point &current_time) const;

    /**
     * @brief This function returns true once the quality has passed a given threshold.
     */
    bool qualityHalt() const;

    /**
     * @brief This function returns true once a given number of stalls has been exceeded in a row.
     */
    bool stallHalt() const;

    /**
     * @brief This function returns true once a maximum number of iterations has been exceeded.
     */
    bool iterationHalt() const;
    /**
     * @brief This function returns true when the minimum number of iterations has been passed.
     */
    bool minIterationPassed() const;

    /**
     * @brief This function returns true if a stop was requested (Gem::Common::stopRequested()).
     */
    bool stopRequestHalt() const;

    /**
     * @brief Triggers termination of the optimization run when a user-defined file is modified.
     */
    bool touchHalt() const;

    /**
     * @brief A wrapper for customHalt_ that allows to emit the termination reason.
     */
    bool customHalt() const;
    /**
     * @brief Custom setting of halt criteria.
     */
    virtual bool customHalt_() const;

    /**
     * @brief This function checks whether a halt criterion has been reached.
     */
    bool halt() const;

    /**
     * @brief Check whether the max-iteration halt is set.
     */
    bool maxIterationHaltset() const;
    /**
     * @brief Check whether a halt criterion based on the number of stalls has been set.
     */
    bool stallHaltSet() const;

    /**
     * @brief Check whether the max_duration-halt criterion has been set.
     */
    bool maxDurationHaltSet() const;

    /**
     * @brief Check whether the quality-threshold halt-criterion has been set.
     */
    bool qualityThresholdHaltSet() const;

    /**
     * @brief Indicates whether the stall_counter_threshold_ has been exceeded.
     */
    bool stallCounterThresholdExceeded() const;

    /***************************************************************************/
    // Data

    std::uint32_t iteration_ = 0; ///< The current iteration
    std::uint32_t offset_ =
        DEFAULTOFFSET; ///< An iteration offset which can be used, if the optimization starts from a checkpoint file
    std::uint32_t chain_offset_ = 0; ///< The iterations of a chain before it (getChainOffset())
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
    mutable std::string cp_last_ = "empty";  ///< The name of the last saved checkpoint
    bool cp_remove_ = true; ///< Whether checkpoint files should be overwritten or kept
    Gem::Common::serializationMode cp_serialization_mode_ =
        DEFAULTCPSERMODE; ///< Whether checkpoints are written in the binary or the JSON mode
    double quality_threshold_ =
        DEFAULTQUALITYTHRESHOLD; ///< A threshold beyond which optimization is expected to stop
    bool has_quality_threshold_ = false; ///< Specifies whether a quality_threshold has been set
    std::chrono::duration<double> max_duration_ = Gem::Common::duration_from_string(
        DEFAULTDURATION
    ); ///< Maximum time-frame for the optimization
    std::chrono::duration<double> min_duration_ = Gem::Common::duration_from_string(
        DEFAULTMINDURATION
    ); ///< Minimum time-frame for the optimization
    /// transient: the start time of this run (mutable so the halt criteria can be const)
    mutable std::chrono::system_clock::time_point start_time_;
    /// transient: the start time of this run for the touchHalt feature, as system_clock and
    /// file_time may not be comparable
    mutable std::filesystem::file_time_type file_start_time_;
    std::string termination_file_ =
        DEFAULTTERMINATIONFILE; ///< The name of a file which, when modified after the start of the optimization run, will cause termination of the run
    bool terminate_on_file_modification_ = false;
    bool emit_termination_reason_ =
        DEFAULTEMITTERMINATIONREASON; ///< Specifies whether information about reasons for termination should be emitted
    std::atomic<bool> halted_{true}; ///< Set to true when halt() has returned "true"
    std::vector<std::tuple<double, double>>
        worst_known_valids_cnt_; ///< Stores the worst known valid evaluations up to the current iteration (first entry: raw, second: tranformed)
    /// A collection of monitors. Shared, not owned: a monitor registered through GOptimizer is
    /// co-owned by GOptimizer and by every algorithm of a chain, so that it observes the whole run
    /// (see GOptimizer::pluggable_monitors_cnt_). Deep-cloned only when one algorithm is loaded
    /// from another, which is a different object and must not share the original's monitors. Not
    /// taken from a checkpoint: the monitors belong to the job (jobSettings_()).
    std::vector<std::shared_ptr<GBasePluggableOM>> pluggable_monitors_cnt_;

    /**
     * @brief Submits the contiguous sub-range [start, end) of @p work_items through the one process-wide
     * consumer (GConsumerRegistry); the default one is built from Geneva's module if the process was
     * never given one.
     */
    Gem::Courtier::submission_status_t workOnViaConsumer_(
        std::vector<std::unique_ptr<gen::GGenome>> &work_items,
        std::size_t start,
        std::size_t end
    );

    /** @brief Returns the one process-wide consumer (the default one, from Geneva's module, if the
     *  process was never given one) with this algorithm's late-return budget applied. */
    std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>> consumerForSubmission_();
};

/*******************************************************************************/
/////////////////////////////////////////////////////////////////////////////////
/*******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

/******************************************************************************/
