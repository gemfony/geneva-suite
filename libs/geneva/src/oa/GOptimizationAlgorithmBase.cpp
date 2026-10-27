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

#include "geneva/oa/GOptimizationAlgorithmBase.hpp"

// Needed for the G_SIGHUP_SENT() signal-state query in sigHupHalt().
#include "common/GSigHupHandler.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GContainerT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "common/GSelfTestable.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GSubmissionStatusT.hpp"
#include "geneva/GConsumerSetup.hpp" // ensureProcessConsumer() -- the setup layer owns the consumer choice
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/oa/GPositionPersonalityTraits.hpp"
#include "geneva/oa/GPostProcessorT.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <set>
#include <vector>

/******************************************************************************/

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The copy constructor. There is no per-algorithm executor or consumer to copy: every
 * algorithm submits to the process-wide consumer held in GConsumerRegistry.
 *
 * @param cp A constant reference to another GOptimizationAlgorithmBase object
 */
GOptimizationAlgorithmBase::GOptimizationAlgorithmBase(const GOptimizationAlgorithmBase &cp)
  : Gem::Common::GReflectiveInterfaceBaseT<
        GOptimizationAlgorithmBase, Gem::Common::GCommonInterfaceT<GOptimizationAlgorithmBase>
    >(cp) {
    // All local data -- including the container base'es population (tied in as data_cnt_) and the
    // transient best_iteration_individuals_pq_ -- is copied from the single localMembers_() declaration,
    // the same machinery load_() uses, so this constructor cannot drift from the member list: plain
    // members are assigned, the cloneable containers are deep-cloned, halted_ is transferred via
    // .store(.load()) and the transient pq is copied. The container base is left default-constructed
    // above; g_load_members deep-clones the population into it.
    Gem::Common::g_load_members(this->localMembers_(), cp.localMembers_());

    // A copied algorithm starts a NEW run: its iteration offset reverts to the default
    // instead of inheriting a chaining offset from the source.
    offset_ = DEFAULTOFFSET;
}

/******************************************************************************/
/**
 * Performs the necessary administratory work of doing check-pointing. Special
 * work necessary for a given optimization algorithm may be performed in the
 * virtual function saveCheckpoint(), which is called by this function.
 *
 * @param is_better A boolean which indicates whether a better result was found
 */
void GOptimizationAlgorithmBase::checkpoint(bool is_better) const {
    bool do_save = false;

    // Determine a suitable name for the checkpoint file
    std::filesystem::path output_file;
    output_file =
        getCheckpointDirectoryPath() /
        std::filesystem::path(
            "checkpoint-" + this->getAlgorithmPersonalityType() + "-" +
            (this->halted() ? "final" : Gem::Common::to_string(getIteration())) + "-" +
            Gem::Common::to_string(std::get<G_TRANSFORMED_FITNESS>(getBestKnownPrimaryFitness())) +
            "-" + getCheckpointBaseName()
        );

    // Save checkpoints if required by the user
    if(cp_interval_ < 0 && is_better) {
        do_save = true;
    } // Only save when a better solution was found
    else if(cp_interval_ > 0 && iteration_ % cp_interval_ == 0) {
        do_save = true;
    } // Save in regular intervals
    else if(cp_interval_ != 0 && this->halted()) {
        do_save = true;
    } // Save the final result -- only when the user enabled checkpointing at all

    if(do_save) {
        // Create the checkpoint directory lazily, at the first actual write (configuring an
        // algorithm must not touch the filesystem -- see setCheckpointBaseName()).
        const auto &cp_dir = getCheckpointDirectoryPath();
        if(not std::filesystem::exists(cp_dir)) {
            std::error_code ec;
            if(not std::filesystem::create_directories(cp_dir, ec) || ec) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GOptimizationAlgorithmBase::checkpoint(): Error!" << '\n'
                    << "Could not create checkpoint directory " << cp_dir.string()
                    << (ec ? (": " + ec.message()) : std::string()) << '\n'
                );
            }
        }

        saveCheckpoint(output_file);

        // Remove the last checkoint file if requested by the user
        if(cp_remove_ && cp_last_ != "empty") {
            if(std::filesystem::exists(std::filesystem::path(cp_last_))) {
                std::filesystem::remove(std::filesystem::path(cp_last_));
            }
        }

        // Record the name of the last known checkpoint file
        cp_last_ = output_file.string();
    }
}

/******************************************************************************/
/**
 * @brief Loads the state of the class from disc
 *
 * @param cp_file The path to the checkpoint file to be loaded
 */
void GOptimizationAlgorithmBase::loadCheckpoint(std::filesystem::path const &cp_file) {
    // Extract the name of the optimization algorithm used for this file
    std::string const opt_desc = this->extractOptAlgFromPath(cp_file);

    // Make sure it fits our own algorithm
    if(opt_desc != this->getAlgorithmPersonalityType()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase<>::loadCheckpoint(): Error!" << '\n'
            << "Checkpoint file " << cp_file << '\n'
            << "seems to belong to another algorithm. Expected "
            << this->getAlgorithmPersonalityType() << '\n'
            << "but got " << opt_desc << '\n'
        );
    }

    // Deserialize the checkpoint. A checkpoint is a GArchive of this algorithm AND its polymorphic
    // population, so the concrete individual type must be registered (compiled in, or loaded from an
    // --individual plugin) BEFORE this point -- exactly like the networked wire. If it is not, the codec
    // throws deep inside deserialization (an unknown archive tag); translate that into actionable
    // guidance rather than an opaque archive error, while preserving the original message.
    try {
        this->fromFile(cp_file, this->getCheckpointSerializationMode());
    }
    catch(const std::exception &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase<>::loadCheckpoint(): Error!" << '\n'
            << "Failed to deserialize the checkpoint file " << cp_file << '\n'
            << "Underlying error: " << e.what() << '\n'
            << "This usually means the individual (optimization-problem) type stored in the checkpoint is"
            << '\n'
            << "not available. Resume with the SAME individual that wrote the checkpoint -- compiled in, or"
            << '\n'
            << "loaded via --individual <path>.so -- built against the same Geneva version." << '\n'
        );
    }

    // The population (with its OA-owned scratch: personality + adaption / swarm / CG POD blocks) has
    // just been restored. Mark the run as resumed so the upcoming setup PRESERVES that scratch instead
    // of re-seeding it (the flag is set AFTER fromFile, so it is unaffected by deserialization, and is
    // cleared once setup has consumed it). The genome carries the optimization forward; this keeps the
    // evolved tuning state in place across the resume.
    resumed_from_checkpoint_ = true;
}

/******************************************************************************/
/**
 * Checks whether the optimization process has been halted, because the halt() function
 * has returned "true"
 *
 * @return A boolean indicating whether the optimization process has been halted
 */
bool GOptimizationAlgorithmBase::halted() const {
    return halted_;
}

/******************************************************************************/
/**
 * Allows to set the number of generations after which a checkpoint should be written.
 * A negative value will result in automatic checkpointing, whenever a better solution
 * was found.
 *
 * @param cp_interval The number of generations after which a checkpoint should be written
 */
void GOptimizationAlgorithmBase::setCheckpointInterval(std::int32_t cp_interval) {
    cp_interval_ = cp_interval;
}

/******************************************************************************/
/**
 * Allows to retrieve the number of generations after which a checkpoint should be written
 *
 * @return The number of generations after which a checkpoint should be written
 */
std::int32_t GOptimizationAlgorithmBase::getCheckpointInterval() const {
    return cp_interval_;
}

/******************************************************************************/
/**
 * @brief The shared population precondition of the floating-point-only algorithms (CGD, Nelder-Mead):
 * requires a non-empty population whose first individual carries at least one active floating-point
 * parameter, and logs a note when integer/boolean parameters ride along (such an algorithm leaves
 * them unchanged). Formerly duplicated verbatim in both algorithms' adjustPopulation_().
 *
 * @param algorithm_name The calling algorithm's class name, used in the error/log texts
 * @return The number of active floating-point parameters of the first individual
 */
std::size_t GOptimizationAlgorithmBase::requireFloatingPointGenome_(
    const std::string &algorithm_name
) const {
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In " << algorithm_name << "::adjustPopulation():" << '\n'
            << "You didn't add any individuals to the collection. We need at least one."
            << '\n'
        );
    }

    const std::size_t n_fp_parms = this->at(0)->countFPParameters(activityMode::ACTIVEONLY);

    if(n_fp_parms == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In " << algorithm_name << "::adjustPopulation():" << '\n'
            << "No floating point parameters in individual." << '\n'
        );
    }

    // A floating-point-only algorithm leaves any integer / boolean parameters unchanged -- this is
    // normal, user-expected behaviour, so it is merely logged (not warned about).
    auto const &ind0 = (*this->at(0));
    const std::size_t n_int_parms = ind0.countParameters<std::int32_t>(activityMode::ACTIVEONLY);
    const std::size_t n_bool_parms = ind0.countParameters<bool>(activityMode::ACTIVEONLY);
    if(n_int_parms + n_bool_parms > 0) {
        glogger << "In " << algorithm_name << "::adjustPopulation_(): Note:" << '\n'
                << "The individual carries " << n_int_parms << " integer and " << n_bool_parms
                << " boolean parameter(s) alongside " << n_fp_parms
                << " floating point parameter(s)." << '\n'
                << "The algorithm only operates on the floating point parameters;" << '\n'
                << "the non-floating-point parameters are left unchanged." << '\n'
                << GLOGGING;
    }

    return n_fp_parms;
}

/******************************************************************************/
/**
 * Allows to set the base name of the checkpoint file and the directory where it
 * should be stored. The directory is only validated here (an existing path must
 * be a directory); a missing directory is created lazily when the first
 * checkpoint is actually written, so merely configuring an algorithm has no
 * filesystem side effects.
 *
 * @param cp_directory The directory where checkpoint files should be stored
 * @param cp_base_name The base name used for the checkpoint files
 */
void GOptimizationAlgorithmBase::setCheckpointBaseName(
    const std::string& cp_directory,
    const std::string& cp_base_name
) {
    // Do some basic checks
    if(cp_base_name == "empty" || cp_base_name.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase::setCheckpointBaseName(const std::string&, const "
               "std::string&):"
            << '\n'
            << "Error: Invalid cp_base_name: " << cp_base_name << '\n'
        );
    }

    if(cp_directory == "empty" || cp_directory.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase::setCheckpointBaseName(const std::string&, const "
               "std::string&):"
            << '\n'
            << "Error: Invalid cp_directory: " << cp_directory << '\n'
        );
    }

    cp_base_name_ = cp_base_name;

    // Transform the directory into a path
    cp_directory_path_ = std::filesystem::path(cp_directory);

    // Validate only -- a missing directory is created lazily by checkpoint() when the first
    // checkpoint is written, so that a default-constructed / merely-configured algorithm has no
    // filesystem side effects (previously an eager create_directory here fired for every
    // default-constructed algorithm, checkpointing enabled or not).
    if(std::filesystem::exists(cp_directory_path_) &&
       not std::filesystem::is_directory(cp_directory_path_)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase::setCheckpointBaseName(): Error!" << '\n'
            << cp_directory_path_.string() << " exists but is no directory." << '\n'
        );
    }
}

/******************************************************************************/
/**
 * Allows to retrieve the base name of the checkpoint file.
 *
 * @return The base name used for checkpoint files
 */
std::string GOptimizationAlgorithmBase::getCheckpointBaseName() const {
    return cp_base_name_;
}

/******************************************************************************/
/**
 * Allows to retrieve the directory where checkpoint files should be stored
 *
 * @return The path (as a string) to the directory where checkpoint files are stored
 */
std::string GOptimizationAlgorithmBase::getCheckpointDirectory() const {
    return cp_directory_path_.string();
}

/******************************************************************************/
/**
 * Allows to retrieve the directory where checkpoint files should be stored
 *
 * @return The path to the directory where checkpoint files are stored
 */
std::filesystem::path GOptimizationAlgorithmBase::getCheckpointDirectoryPath() const {
    return cp_directory_path_;
}

/******************************************************************************/
/**
 * Determines whether checkpointing should be done in Text-, XML- or Binary-mode
 *
 * @param cp_ser_mode The desired new checkpointing serialization mode
 */
void GOptimizationAlgorithmBase::setCheckpointSerializationMode(
    Gem::Common::serializationMode cp_ser_mode
) {
    cp_serialization_mode_ = cp_ser_mode;
}

/******************************************************************************/
/**
 * Retrieves the current checkpointing serialization mode
 *
 * @return The current checkpointing serialization mode
 */
Gem::Common::serializationMode
GOptimizationAlgorithmBase::getCheckpointSerializationMode() const {
    return cp_serialization_mode_;
}

/******************************************************************************/
/**
 * @brief Allows to set the cp_overwrite_ flag (determines whether checkpoint files
 * should be removed or kept)
 *
 * @param cp_remove If true, old checkpoint files are removed; if false, they are kept
 */
void GOptimizationAlgorithmBase::setRemoveCheckpointFiles(bool cp_remove) {
    cp_remove_ = cp_remove;
}

/******************************************************************************/
/**
 * @brief Allows to check whether checkpoint files will be removed
 *
 * @return A boolean indicating whether old checkpoint files will be removed
 */
bool GOptimizationAlgorithmBase::checkpointFilesAreRemoved() const {
    return cp_remove_;
}

/******************************************************************************/
/**
 * @brief Resets the class to the state before the optimize call.
 */
void GOptimizationAlgorithmBase::resetToOptimizationStart() {
    resetToOptimizationStart_();
}

/******************************************************************************/
/**
 * @brief Resets the class to the state before the optimize call. This will in
 * particular erase all individuals stored in this class and clear the list
 * of best individuals. Please note that a subsequent call to optimize will
 * result in an error, unless you add new individuals. The purpose of this
 * function is allow repeated optimization with the same settings, but different
 * starting points. Actual implementations of optimization algorithms derived
 * from this class may have to perform additional work by overloading (and
 * calling) this function. Since this function will also reset the executor,
 * unless you register a new executor, calling this function will result in
 * the default executor being used.
 */
void GOptimizationAlgorithmBase::resetToOptimizationStart_() {
    this->clear(); // Remove all individuals found in this population

    iteration_ = 0;                    // The current iteration
    best_global_individuals_pq_.clear(); // A priority queue with the best individuals found so far
    best_iteration_individuals_pq_
        .clear(); // A priority queue with the best individuals of a given iteration

    best_known_primary_fitness_ =
        std::tuple<double, double>(0., 0.); // Records the best primary fitness found so far
    best_current_primary_fitness_ = std::tuple<double, double>(
        0.,
        0.
    ); // Records the best fitness found in the current iteration

    stall_counter_ = 0; // Counts the number of iterations without improvement

    halted_ = true; // Also means: No optimization is currently running

    worst_known_valids_cnt_
        .clear(); // Stores the worst known valid evaluations up to the current iteration (first entry: raw, second: tranformed)

}

/******************************************************************************/
/**
 * This function encapsulates some common functionality of iteration-based
 * optimization algorithms. E.g., they all need a loop that stops if some
 * predefined criterion is reached. This function is also the main entry
 * point for all optimization algorithms.
 *
 * @param offset Specifies the iteration number to start with (e.g. useful when starting from a checkpoint file)
 * @return A constant pointer to this object
 */
// NOLINTNEXTLINE(readability-function-size) -- the single top-level optimization lifecycle driver (setup, the init/cycle/checkpoint/halt loop, teardown) shared by every optimization algorithm; splitting the sequenced setup->loop->teardown would scatter one coherent control-flow entry point
GOptimizationAlgorithmBase const *GOptimizationAlgorithmBase::optimize_(std::uint32_t offset) {
    // Reset the generation counter
    iteration_ = offset;

    // Set the iteration offset
    offset_ = offset;

    // Store any *clean* individuals that have been added to this algorithm
    // in the priority queue. This happens so that best individuals from a
    // previous "chained" optimization run aren't lost.
    addCleanStoredBests(best_global_individuals_pq_);

    // Resize the population to the desired size and do some error checks.
    // This function will also check that individuals have indeed been registered
    adjustPopulation_();

    // Set the individual's personalities (some algorithm-specific information needs to be stored
    // in individuals. Optimization algorithms need to re-implement this function to add
    // the required functionality.)
    setIndividualPersonalities();

    // Emit the info header, unless we do not want any info (parameter 0).
    // Note that this call needs to come after the initialization, so we have the
    // complete set of individuals available.
    //! [monitors:info-cycle#1]
    if(report_iteration_) {
        informationUpdate(infoMode::INFOINIT);
    }
    //! [monitors:info-cycle#1]

    // We want to know if no better values were found for a longer period of time
    double const worst_case = this->at(0)->getWorstCase();
    best_known_primary_fitness_ = std::make_tuple(worst_case, worst_case);
    best_current_primary_fitness_ = std::make_tuple(worst_case, worst_case);

    stall_counter_ = 0;

    // Give derived classes the opportunity to perform any other necessary preparatory work.
    init();

    // The resume marker has now been consumed by setIndividualPersonalities() + init() (which preserved
    // the restored scratch). Clear it so the remainder of the run -- and any subsequent algorithm in a
    // Go2 chain -- treats the scratch as ordinary OA-owned state (seeded / reset normally).
    resumed_from_checkpoint_ = false;

    // Let the algorithm know that the optimization process hasn't been halted yet.
    halted_ = false; // general halt criterion

    // Initialize the start time with the current time.
    start_time_ = std::chrono::system_clock::now();

    // Initialize a file-clock start time for the touchHalt feature, which compares it against a
    // termination file's last_write_time() (both are std::filesystem::file_time_type). C++20's
    // std::chrono::file_clock::now() reads the filesystem clock directly -- no need to create, stat and
    // remove a marker file. The former touch-a-file approach used a single fixed name in the working
    // directory, which raced fatally when several algorithms started concurrently (one removed the marker
    // between another's create and its last_write_time() read, throwing std::filesystem_error and
    // aborting the process).
    file_start_time_ = std::chrono::file_clock::now();

    //! [geneva.oa.cycle]
    do {
        // Let all individuals know the current iteration
        markIteration();

        // Update fitness values and the stall counter
        updateStallCounter((best_current_primary_fitness_ = cycleLogic_()));

        // Add the best individuals to the best_global_individuals_pq_
        // and best_iteration_individuals_pq_ vectors
        updateGlobalBestsPQ_(best_global_individuals_pq_);
        updateIterationBestsPQ_(best_iteration_individuals_pq_);

        // Check whether a better value was found, and do the check-pointing, if necessary and requested.
        checkpoint(progress());

        // Give derived classes an opportunity to act on stalls. NOTE that no action
        // may be taken that affects the "dirty" state of individuals
        if(stall_counter_threshold_ && stallCounterThresholdExceeded()) {
            actOnStalls_();
        }

        // We want to provide feedback to the user in regular intervals.
        // Set the reportGeneration_ variable to 0 in order not to emit
        // any information at all.
        //! [monitors:info-cycle#2]
        if(report_iteration_ && (iteration_ % report_iteration_ == 0)) {
            informationUpdate(infoMode::INFOPROCESSING);
        }
        //! [monitors:info-cycle#2]

        // update the iteration_ counter
        iteration_++;
    }
    while(not(halted_ = halt()));

    // Write the final checkpoint. halted_ is only set in the loop condition ABOVE the in-loop
    // checkpoint() call, so the in-loop calls never see halted() == true -- without this post-loop
    // call the "final" checkpoint (see checkpoint()'s halted() branch and the "final" file-name tag)
    // would never be written at all. checkpoint() itself gates on the user having enabled
    // checkpointing (cp_interval_ != 0).
    checkpoint(progress());
    //! [geneva.oa.cycle]

    // Give derived classes the opportunity to perform any remaining clean-up work
    finalize();

    // Finalize the info output
    //! [monitors:info-cycle#3]
    if(report_iteration_) {
        informationUpdate(infoMode::INFOEND);
    }
    //! [monitors:info-cycle#3]

    // Remove information particular to the optimization algorithms from the individuals
    resetIndividualPersonalities();

    return this;
}

/******************************************************************************/
/**
 * Emits information specific to this class (basic information in each iteration
 * plus some user-defined information via pluggable optimization monitors)
 *
 * @param im The information mode (INFOINIT, INFOPROCESSING or INFOEND)
 */
void GOptimizationAlgorithmBase::informationUpdate(infoMode const &im) {
    // Act on the information mode provided
    switch(im) {
    case Gem::Geneva::infoMode::INFOINIT:
        std::cout << "Starting an optimization run with algorithm \"" << this->getAlgorithmName()
                  << "\"" << '\n';
        break;

    case Gem::Geneva::infoMode::INFOPROCESSING: {
        // We output raw values here, as this is likely what the user is interested in
        std::cout << std::setprecision(5) << this->getIteration() << ": "
                  << Gem::Common::g_to_string(this->getBestCurrentPrimaryFitness())
                  << " // best past: "
                  << Gem::Common::g_to_string(this->getBestKnownPrimaryFitness()) << '\n';
    } break;

    case Gem::Geneva::infoMode::INFOEND:
        std::cout << "End of optimization reached in algorithm \"" << this->getAlgorithmName()
                  << "\"" << '\n';
        break;
    };

    // Perform any action defined by the user through pluggable monitor objects
    for(auto const &pm_ptr : pluggable_monitors_cnt_) {
        pm_ptr->informationFunction(im, this);
    }
}

/******************************************************************************/
/**
 * Checks whether a better solution was found. If so, the stall_counter_
 * variable will have been set to 0
 *
 * @return A boolean indicating whether a better solution was found
 */
bool GOptimizationAlgorithmBase::progress() const {
    return (0 == stall_counter_);
}

/******************************************************************************/
/**
 * @brief Allows to register a pluggable optimization monitor. Note that this
 * function shares ownership of the monitor -- it is held by shared_ptr and kept alive while registered.
 *
 * @param pluggable_om A shared pointer to the pluggable optimization monitor to be registered (must not be empty)
 */
void GOptimizationAlgorithmBase::registerPluggableOM(
    const std::shared_ptr<GBasePluggableOM>& pluggable_om
) {
    if(pluggable_om) {
        pluggable_monitors_cnt_.push_back(pluggable_om);
    }
    else {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GoptimizationMonitorT<>::registerPluggableOM(): Tried to register empty "
               "pluggable optimization monitor"
            << '\n'
        );
    }
}

/************************************************************************/
/**
 * @brief Allows to reset the local pluggable optimization monitors
 */
void GOptimizationAlgorithmBase::resetPluggableOM() {
    pluggable_monitors_cnt_.clear();
}

/******************************************************************************/
/**
 * @brief Allows to check whether pluggable optimization monitors were registered
 *
 * @return A boolean indicating whether at least one pluggable optimization monitor is registered
 */
bool GOptimizationAlgorithmBase::hasPluggableOptimizationMonitors() const {
    return not pluggable_monitors_cnt_.empty();
}

/******************************************************************************/
/**
 * Retrieves the default population size
 *
 * @return The default population size
 */
std::size_t GOptimizationAlgorithmBase::getDefaultPopulationSize() const {
    return default_population_size_;
}

/******************************************************************************/
/**
 * Retrieve the current population size
 *
 * @return The current population size
 */
std::size_t GOptimizationAlgorithmBase::getPopulationSize() const {
    return this->size();
}

/******************************************************************************/
/**
 * Set the number of iterations after which the optimization should
 * be stopped
 *
 * @param max_iteration The number of iterations after which the optimization should terminate
 */
void GOptimizationAlgorithmBase::setMaxIteration(std::uint32_t max_iteration) {
    // Check that the new maximum is > the current minimum (guard only applies when max != 0)
    if(max_iteration > 0 && max_iteration <= min_iteration_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase<>::setMaxIteration(): Error!" << '\n'
            << "Maximum number of iterations " << max_iteration << " is <= the minimum number "
            << min_iteration_ << '\n'
        );
    }

    max_iteration_ = max_iteration;
}

/******************************************************************************/
/**
 * Retrieve the number of iterations after which optimization should
 * be stopped
 *
 * @return The number of iterations after which the optimization should terminate
 */
std::uint32_t GOptimizationAlgorithmBase::getMaxIteration() const {
    return max_iteration_;
}

/******************************************************************************/
/**
 * @brief Sets the minimal number of iterations to be performed before a halt may occur.
  * No halt will be performed if this is not the case (with the exception of halts
  * that are triggered by user-actions, such as Ctrl-C (Sighup-Halt) and touched halt
  * (Geneva checks whether a file was modified after Geneva has started). Set the number
  * of iterations to 0 in order to disable a check for the minimal number of iterations.
  *
  * @param min_iteration The minimum number of iterations to perform before a halt criterion may take effect
*/
void GOptimizationAlgorithmBase::setMinIteration(std::uint32_t min_iteration) {
    // Check that the current maximum will remain > the new minimum (guard only applies when max != 0)
    if(max_iteration_ > 0 && max_iteration_ <= min_iteration) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase<>::setMinIteration(): Error!" << '\n'
            << "Maximum number of iterations " << max_iteration_ << " is <= the minimum number "
            << min_iteration << '\n'
        );
    }

    min_iteration_ = min_iteration;
}

/******************************************************************************/
/**
 * @brief This function retrieves the value of the min_iteration_ variable
 *
 * @return The minimum number of iterations to perform before a halt criterion may take effect
 */
std::uint32_t GOptimizationAlgorithmBase::getMinIteration() const {
    return min_iteration_;
}

/******************************************************************************/
/**
 * Sets the maximum number of generations allowed without improvement of the best
 * individual. Set to 0 in order for this stop criterion to be disabled.
 *
 * @param max_stall_iteration The maximum number of allowed generations
 */
void GOptimizationAlgorithmBase::setMaxStallIteration(std::uint32_t max_stall_iteration) {
    max_stall_iteration_ = max_stall_iteration;
}

/******************************************************************************/
/**
 * Retrieves the maximum number of generations allowed in an optimization run without
 * improvement of the best individual.
 *
 * @return The maximum number of generations
 */
std::uint32_t GOptimizationAlgorithmBase::getMaxStallIteration() const {
    return max_stall_iteration_;
}

/******************************************************************************/
/**
 * Sets the maximum allowed processing time
 *
 * @param max_duration The maximum allowed processing time
 */
void GOptimizationAlgorithmBase::setMaxTime(std::chrono::duration<double> max_duration) {
    if(not Gem::Common::isClose<double>(max_duration.count(), 0.) && max_duration < min_duration_) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase<>::setMaxTime(): Error!" << '\n'
            << "Desired max_duration (" << max_duration.count() << " is smaller than min_duration_("
            << min_duration_.count() << ")" << '\n'
        );
    }

    max_duration_ = max_duration;
}

/******************************************************************************/
/**
 * Retrieves the value of the max_duration_ parameter.
 *
 * @return The maximum allowed processing time
 */
std::chrono::duration<double> GOptimizationAlgorithmBase::getMaxTime() const {
    return max_duration_;
}

/******************************************************************************/
/**
* Sets the minimum required processing time. NOTE: Always set the maximum duration
* before the minumum duration.
*
* @param min_duration The minimum allowed processing time
*/
void GOptimizationAlgorithmBase::setMinTime(std::chrono::duration<double> min_duration) {
    if(not Gem::Common::isClose<double>(max_duration_.count(), 0.) && max_duration_ < min_duration) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase<>::setMinTime(): Error!" << '\n'
            << "Desired max_duration (" << max_duration_.count() << " is smaller than min_duration_("
            << min_duration.count() << ")" << '\n'
        );
    }

    min_duration_ = min_duration;
}

/******************************************************************************/
/**
* Retrieves the value of the min_duration_ parameter.
*
* @return The minimum required processing time
*/
std::chrono::duration<double> GOptimizationAlgorithmBase::getMinTime() const {
    return min_duration_;
}

/******************************************************************************/
/**
 *  Sets a quality threshold beyond which optimization is expected to stop
 *
 *  @param quality_threshold A threshold beyond which optimization should stop
 *  @param has_quality_threshold Allows to (de-)activate the quality threshold
 */
void GOptimizationAlgorithmBase::setQualityThreshold(
    double quality_threshold,
    bool has_quality_threshold
) {
    quality_threshold_ = quality_threshold;
    has_quality_threshold_ = has_quality_threshold;
}

/******************************************************************************/
/**
 * Retrieves the current value of the quality threshold and also indicates whether
 * the threshold is active
 *
 * @param has_quality_threshold A boolean indicating whether a quality threshold has been set
 * @return The current value of the quality threshold
 */
double GOptimizationAlgorithmBase::getQualityThreshold(bool &has_quality_threshold) const {
    has_quality_threshold = has_quality_threshold_;
    return quality_threshold_;
}

/******************************************************************************/
/**
 *  Sets the name of a "termination file" (optimization is supposed to
 *  stop when the modification time of this file is more recent than the
 *  start of the optimizatoon rn.
 *
 *  @param termination_file The name of a file used to initiate termination
 *  @param terminate_on_file_modification Allows to (de-)activate "touched termination"
 */
void GOptimizationAlgorithmBase::setTerminationFile(
    std::string termination_file,
    bool terminate_on_file_modification
) {
    termination_file_ = std::move(termination_file);
    terminate_on_file_modification_ = terminate_on_file_modification;
}

/******************************************************************************/
/**
 * Retrieves the current name of the termination file and also indicates whether
 * the "touched halt" is active
 *
 * @param terminate_on_file_modification A boolean indicating whether "touched termination" is active
 * @return The current value of the termination_file_ variable
 */
std::string
GOptimizationAlgorithmBase::getTerminationFile(bool &terminate_on_file_modification) const {
    terminate_on_file_modification = terminate_on_file_modification_;
    return termination_file_;
}

/******************************************************************************/
/**
 * Removes the quality threshold
 */
void GOptimizationAlgorithmBase::resetQualityThreshold() {
    has_quality_threshold_ = false;
}

/******************************************************************************/
/**
 * Checks whether a quality threshold has been set
 *
 * @return A boolean indicating whether a quality threshold has been set
 */
bool GOptimizationAlgorithmBase::hasQualityThreshold() const {
    return has_quality_threshold_;
}

/******************************************************************************/
/**
 * Retrieve the current iteration of the optimization run
 *
 * @return The current iteration of the optimization run
 */
std::uint32_t GOptimizationAlgorithmBase::getIteration_() const {
    return iteration_;
}

/******************************************************************************/
/**
 * Returns the current offset used to calculate the current iteration. This
 * is identical to the iteration the optimization starts with.
 *
 * @return The current iteration offset
 */
std::uint32_t GOptimizationAlgorithmBase::getStartIteration() const {
    return offset_;
}

/******************************************************************************/
/**
 * Sets the number of iterations after which the algorithm should
 * report about its inner state.
 *
 * @param iter The number of iterations after which information should be emitted
 */
void GOptimizationAlgorithmBase::setReportIteration(std::uint32_t iter) {
    report_iteration_ = iter;
}

/******************************************************************************/
/**
 * Returns the number of iterations after which the algorithm should
 * report about its inner state.
 *
 * @return The number of iterations after which information is emitted
 */
std::uint32_t GOptimizationAlgorithmBase::getReportIteration() const {
    return report_iteration_;
}

/******************************************************************************/
/**
 * Retrieves the current number of failed optimization attempts
 *
 * @return The current number of failed optimization attempts
 */
std::uint32_t GOptimizationAlgorithmBase::getStallCounter() const {
    return stall_counter_;
}

/******************************************************************************/
/**
 * @brief Allows to set the number of iterations without improvement, after which
 * individuals are asked to update their internal data structures
 *
 * @param stall_counter_threshold The number of stall iterations after which individuals update their internal data structures
 */
void GOptimizationAlgorithmBase::setStallCounterThreshold(std::uint32_t stall_counter_threshold) {
    stall_counter_threshold_ = stall_counter_threshold;
}

/******************************************************************************/
/**
 * @brief Allows to retrieve the number of iterations without improvement, after which
 * individuals are asked to update their internal data structures
 *
 * @return The number of stall iterations after which individuals update their internal data structures
 */
std::uint32_t GOptimizationAlgorithmBase::getStallCounterThreshold() const {
    return stall_counter_threshold_;
}

/******************************************************************************/
/**
 * @brief Sets the time-to-live (in dispatch rounds) of a networked consumer's late-return buffer entry.
 *
 * @param ttl_rounds The late-return buffer TTL, in dispatch rounds
 */
void GOptimizationAlgorithmBase::setLateReturnTTL(std::uint64_t ttl_rounds) {
    late_return_ttl_ = ttl_rounds;
}

/******************************************************************************/
/**
 * @brief Retrieves the time-to-live (in dispatch rounds) of a networked consumer's late-return entry.
 *
 * @return The late-return buffer TTL, in dispatch rounds
 */
std::uint64_t GOptimizationAlgorithmBase::getLateReturnTTL() const {
    return late_return_ttl_;
}

/******************************************************************************/
/**
 * @brief Sets the late-return buffer capacity as a multiple of the population size (0 disables it).
 *
 * @param cap_factor The late-return buffer capacity as a multiple of the population size (>= 0)
 */
void GOptimizationAlgorithmBase::setLateReturnCapFactor(double cap_factor) {
    if(cap_factor < 0.) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase::setLateReturnCapFactor(): Error!" << '\n'
            << "Received a negative capacity factor " << cap_factor << '\n'
        );
    }
    late_return_cap_factor_ = cap_factor;
}

/******************************************************************************/
/**
 * @brief Retrieves the late-return buffer capacity factor (multiple of the population size).
 *
 * @return The late-return buffer capacity factor
 */
double GOptimizationAlgorithmBase::getLateReturnCapFactor() const {
    return late_return_cap_factor_;
}

/******************************************************************************/
/**
 * Retrieve the best value found in the entire optimization run so far
 *
 * @return The best raw and transformed fitness found so far
 */
std::tuple<double, double> GOptimizationAlgorithmBase::getBestKnownPrimaryFitness() const {
    // The single source of the best-known fitness: the same member the stall counter and the
    // quality-threshold halt use (serialized, initialized to the worst case at optimization
    // start). Reading the best-individuals priority queue here instead would create a second
    // source of truth -- and throw on an empty queue (e.g. for a checkpoint file name written
    // before the first iteration completes).
    return best_known_primary_fitness_;
}

/******************************************************************************/
/**
 * Retrieves the best value found in the current iteration
 *
 * @return The best raw and transformed fitness found in the current iteration
 */
std::tuple<double, double> GOptimizationAlgorithmBase::getBestCurrentPrimaryFitness() const {
    return best_current_primary_fitness_;
}

/******************************************************************************/
/**
 * Specifies whether information about termination reasons should be emitted
 *
 * @param emit_termination_reason A boolean which specifies whether reasons for the termination of the optimization run should be emitted
 */
void GOptimizationAlgorithmBase::setEmitTerminationReason(bool emit_termination_reason) {
    emit_termination_reason_ = emit_termination_reason;
}

/******************************************************************************/
/**
 * Retrieves information on whether information about termination reasons should be emitted
 *
 * @return A boolean which specifies whether reasons for the termination of the optimization run will be emitted
 */
bool GOptimizationAlgorithmBase::getEmitTerminationReason() const {
    return emit_termination_reason_;
}

/******************************************************************************/
/**
 * @brief Retrieve the number of processable items in the current iteration.
 *
 * @return The number of processable items in the current iteration
 */
std::size_t GOptimizationAlgorithmBase::getNProcessableItems() const {
    return getNProcessableItems_();
}

/******************************************************************************/
/**
 * Retrieve the number of processable items in the current iteration. This function should
 * be overloaded for derived classes. It is used to determine a suitable wait factor for
 * networked execution.
 *
 * @return The number of processable items in the current iteration
 */
std::size_t GOptimizationAlgorithmBase::getNProcessableItems_() const {
    return this->size();
}

/******************************************************************************/
/**
 * Adds local configuration options to a GParserBuilder object
 *
 * @param gpb The GParserBuilder object to which configuration options should be added
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent config-registration sweep binding every base-class optimization option (iteration limits, checkpointing, termination file, quality threshold, ...); splitting would scatter the option list
void GOptimizationAlgorithmBase::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our CRTP base class'es function (the category root has no parent above it)
    Gem::Common::GCommonInterfaceT<GOptimizationAlgorithmBase>::addConfigurationOptions_(gpb);

    // The number of threads used for parallel organizational work (adaption, recombination, ...).
    // The option keeps its historical name for config-file compatibility.
    gpb.registerFileParameter<std::uint16_t>(
        "n_adaption_threads" // The name of the variable
        ,
        Gem::Courtier::DEFAULTNSTDTHREADS // The default value
        ,
        [this](std::uint16_t nt) { this->setNThreads(nt); }
    ) << "The number of threads used to simultaneously adapt and recombine individuals"
      << '\n'
      << "0 means \"automatic\"";

    // Add local data
    gpb.registerFileParameter<std::uint32_t>(
        "max_iteration" // The name of the variable
        ,
        DEFAULTMAXIT // The default value
        ,
        [this](std::uint32_t max_it) { this->setMaxIteration(max_it); }
    ) << "The maximum allowed number of iterations";

    gpb.registerFileParameter<std::uint32_t>(
        "min_iteration" // The name of the variable
        ,
        DEFAULTMINIT // The default value
        ,
        [this](std::uint32_t min_it) { this->setMinIteration(min_it); }
    ) << "The minimum allowed number of iterations";

    gpb.registerFileParameter<std::uint32_t>(
        "max_stall_iteration" // The name of the variable
        ,
        DEFAULTMAXSTALLIT // The default value
        ,
        [this](std::uint32_t max_stall_it) { this->setMaxStallIteration(max_stall_it); }
    ) << "The maximum allowed number of iterations without improvement"
      << '\n'
      << "0 means: no constraint.";

    gpb.registerFileParameter<std::string, bool>(
        "termination_file" // The name of the variable
        ,
        "touched_termination_active",
        DEFAULTTERMINATIONFILE // The default value
        ,
        false,
        [this](std::string tf, bool tfa) { this->setTerminationFile(std::move(tf), tfa); },
        "touched_termination"
    ) << "The name of a file which, when modified after the start of an"
      << '\n'
      << "optimization run, instructs Geneva to terminate optimitation." << '\n'
      << "This can be used to \"touch a file\" after the start of an optimization" << '\n'
      << "run, which will lead to the termination of the run after the current iteration."
      << Gem::Common::nextComment()
      << "Activates (1) or de-activates (0) the \"touched termination\"";

    gpb.registerFileParameter<std::uint32_t>(
        "individual_update_stall_counter_threshold" // The name of the variable
        ,
        DEFAULTSTALLCOUNTERTHRESHOLD // The default value
        ,
        [this](std::uint32_t stall_counter_threshold) {
            this->setStallCounterThreshold(stall_counter_threshold);
        }
    ) << "The number of iterations without improvement after which"
      << '\n'
      << "individuals are asked to update their internal data structures" << '\n'
      << "through the actOnStalls() function. A value of 0 disables this check";

    gpb.registerFileParameter<std::uint64_t>(
        "late_return_ttl" // The name of the variable
        ,
        DEFAULTLATERETURNTTL // The default value
        ,
        [this](std::uint64_t ttl) { this->setLateReturnTTL(ttl); }
    ) << "Time-to-live (in dispatch rounds) of a networked consumer's late-return" << '\n'
      << "buffer entry. A late return not reaped within this many rounds is evicted.";

    gpb.registerFileParameter<double>(
        "late_return_cap_factor" // The name of the variable
        ,
        DEFAULTLATERETURNCAPFACTOR // The default value
        ,
        [this](double cap_factor) { this->setLateReturnCapFactor(cap_factor); }
    ) << "Capacity of a networked consumer's late-return buffer, as a multiple of" << '\n'
      << "the population size. 0 disables late-return buffering entirely.";

    gpb.registerFileParameter<std::uint32_t>(
        "report_iteration" // The name of the variable
        ,
        DEFAULTREPORTITER // The default value
        ,
        [this](std::uint32_t r_i) { this->setReportIteration(r_i); }
    ) << "The number of iterations after which a report should be issued";

    gpb.registerFileParameter<std::size_t>(
        "n_record_best_individuals" // The name of the variable
        ,
        DEFNRECORDBESTINDIVIDUALS // The default value
        ,
        [this](std::size_t n_rec_bi) { this->setNRecordBestIndividuals(n_rec_bi); }
    ) << "Indicates how many \"best\" individuals should be recorded in each iteration";

    gpb.registerFileParameter<std::int32_t>(
        "cp_interval" // The name of the variable
        ,
        DEFAULTCHECKPOINTIT // The default value
        ,
        [this](std::int32_t cp_i) { this->setCheckpointInterval(cp_i); }
    ) << "The number of iterations after which a checkpoint should be written."
      << '\n'
      << "-1 means: Write a checkpoint file whenever an improvement was encountered" << '\n'
      << " 0 means: Never emit checkpoint files.";

    gpb.registerFileParameter<std::string, std::string>(
        "cp_directory" // The name of the first variable
        ,
        "cp_base_name" // The name of the second variable
        ,
        DEFAULTCPDIR // Default value for the first variable
        ,
        DEFAULTCPBASENAME // Default value for the second variable
        ,
        [this](std::string cp_dir, std::string cp_bn) {
            this->setCheckpointBaseName(std::move(cp_dir), std::move(cp_bn));
        },
        "checkpoint_location"
    ) << "The directory where checkpoint files should be stored."
      << Gem::Common::nextComment() // comments for the second option follow
      << "The significant part of the checkpoint file name.";

    gpb.registerFileParameter<bool>(
        "cp_overwrite" // The name of the variable
        ,
        true // The default value -- we always remove old checkpoints
        ,
        [this](bool cp_overwrite) { this->setRemoveCheckpointFiles(cp_overwrite); }
    ) << "When set to \"true\", old checkpoint files will not be kept";

    gpb.registerFileParameter<Gem::Common::serializationMode>(
        "cp_ser_mode" // The name of the variable
        ,
        DEFAULTCPSERMODE // The default value
        ,
        [this](Gem::Common::serializationMode s_m) { this->setCheckpointSerializationMode(s_m); }
    ) << "Determines whether check-pointing should be done in"
      << '\n'
      << "text- (0), XML- (1), binary- (2), GArchive-binary- (3) or GArchive-JSON-mode (4)";

    gpb.registerFileParameter<double, bool>(
        "threshold" // The name of the variable
        ,
        "threshold_active",
        DEFAULTQUALITYTHRESHOLD // The default value
        ,
        false,
        [this](double qt, bool ta) { this->setQualityThreshold(qt, ta); },
        "quality_termination"
    ) << "A threshold beyond which optimization is expected to stop"
      << '\n'
      << "Note that in order to activate this threshold, you also need to" << '\n'
      << "set \"hasQualityThreshold\" to 1." << Gem::Common::nextComment()
      << "Activates (1) or de-activates (0) the quality threshold";

    gpb.registerFileParameter<std::string>(
        "max_duration" // The name of the variable
        ,
        DEFAULTDURATION // The default value
        ,
        [this](const std::string& mt_str) { this->setMaxTime(Gem::Common::duration_from_string(mt_str)); }
    ) << "The maximum allowed time-frame for the optimization"
      << '\n'
      << "in the format hours:minutes:seconds";

    gpb.registerFileParameter<std::string>(
        "min_duration" // The name of the variable
        ,
        DEFAULTMINDURATION // The default value
        ,
        [this](const std::string& mt_str) { this->setMinTime(Gem::Common::duration_from_string(mt_str)); }
    ) << "The minimum required time-frame for the optimization"
      << '\n'
      << "in the format hours:minutes:seconds";

    gpb.registerFileParameter<bool>(
        "emit_termination_reason" // The name of the variable
        ,
        DEFAULTEMITTERMINATIONREASON // The default value
        ,
        [this](bool etr) { this->setEmitTerminationReason(etr); }
    ) << "Triggers emission (1) or omission (0) of information about reasons for termination";
}

/******************************************************************************/
/**
	 * @brief Adds the individuals of this iteration to a (global) priority queue. The
	 * queue will be sorted by the first evaluation criterion of the individuals
	 * and may either have a limited or unlimited size, depending on user-settings
	 *
	 * @param best_individuals The priority queue to which the current population's individuals are added (best ones are cloned in)
	 */
void GOptimizationAlgorithmBase::updateGlobalBestsPQ_(
    gen::GGenomeFixedSizePriorityQueue &best_individuals
) {
    constexpr bool donotreplace = false;

#ifdef DEBUG
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase::updateGlobalBestsPQ() :" << '\n'
            << "Tried to retrieve the best individuals even though the population is empty."
            << '\n'
        );
    }
#endif /* DEBUG */

    // We simply offer all individuals to the archive -- it admits only the processed ones, keeps only
    // the best, and holds independent copies, so a later generation cannot mutate what was recorded.
    // Unless the queue has an unlimited size, it will be trimmed to its maximum allowed size.
    best_individuals.addClone(this->data_cnt_, donotreplace);
}

/******************************************************************************/
/**
	 * @brief Adds the individuals of this iteration to a (per-iteration) priority queue. The
	 * queue will be sorted by the first evaluation criterion of the individuals
	 * and may either have a limited or unlimited size, depending on user-
	 * settings
	 *
	 * @param best_individuals The priority queue to which the current population's individuals are added (cloned in, replacing prior content)
	 */
void GOptimizationAlgorithmBase::updateIterationBestsPQ_(
    gen::GGenomeFixedSizePriorityQueue &best_individuals
) {
    constexpr bool replace = true;

#ifdef DEBUG
    if(this->empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase::updateIterationBestsPQ() :" << '\n'
            << "Tried to retrieve the best individuals even though the population is empty."
            << '\n'
        );
    }
#endif /* DEBUG */

    // We simply offer all individuals to the archive. They will automatically be sorted.
    best_individuals.addClone(this->data_cnt_, replace);
}

/******************************************************************************/
/**
	 * @brief If individuals have been stored in this population, they are added to the
	 * priority queue. This happens before the optimization cycle starts, so that
	 * best individuals from a previous "chained" optimization run aren't lost.
	 * Only those individuals are stored in the priority queue that do not have the
	 * "dirty flag" set.
	 *
	 * @param best_individuals The priority queue to which the already-processed (clean) individuals of this population are added
	 */
void GOptimizationAlgorithmBase::addCleanStoredBests(
    gen::GGenomeFixedSizePriorityQueue &best_individuals
) {
    // We simply offer every individual of this population to the archive: it admits only the
    // processed ones (its own isValid() rule), keeps only the best, and stores independent copies --
    // this population goes on evolving and must not be aliased by the archive. Unless the queue has
    // an unlimited size, it will be trimmed to its maximum allowed size.
    for(auto const &ind_ptr : *this) {
        best_individuals.addClone(ind_ptr);
    }
}

/******************************************************************************/
/**
 * A little helper function that determines whether we are currently inside of the first
 * iteration
 *
 * @return A boolean indicating whether we are inside of the first iteration
 */
bool GOptimizationAlgorithmBase::inFirstIteration() const {
    return iteration_ == offset_;
}

/******************************************************************************/
/**
 * A little helper function that determines whether we are after the first iteration
 *
 * @return A boolean indicating whether we are after the first iteration
 */
bool GOptimizationAlgorithmBase::afterFirstIteration() const {
    return iteration_ > offset_;
}

/******************************************************************************/
/**
 * @brief Checks whether a checkpoint-file has the same "personality" as our
 * own algorithm
 *
 * @param p The path to the checkpoint file whose algorithm personality is to be checked
 * @return A boolean indicating whether the file's algorithm personality matches this algorithm's
 */
bool GOptimizationAlgorithmBase::cp_personality_fits(const std::filesystem::path &p) const {
    // Extract the name of the optimization algorithm used for this file
    std::string const opt_desc = this->extractOptAlgFromPath(p);

    // Make sure it fits our own algorithm
    return opt_desc == this->getAlgorithmPersonalityType();
}

/******************************************************************************/
/**
	 * Submits the sub-range [start, end) of @p work_items to the one process-wide consumer for
	 * evaluation. Note that the returned "is_complete" and "has_errors" may both be true, i.e. all
	 * items may have returned, but there were errors in some or all of them.
	 *
	 * @param work_items The work-item vector whose sub-range is submitted (reconciled in place)
	 * @param start The (inclusive) start index of the range to evaluate
	 * @param end The (exclusive) end index of the range to evaluate
	 * @return A struct which indicates whether all items have returned ("is_complete") and whether there were errors ("has_errors")
	 */
Gem::Courtier::submission_status_t GOptimizationAlgorithmBase::workOn(
    std::vector<std::unique_ptr<gen::GGenome>> &work_items,
    std::size_t start,
    std::size_t end
) {
    // All submission goes through courtier's span+policy path. init() guarantees the process
    // consumer is established (registered explicitly, or the lazily-built local default).
    return this->workOnViaConsumer_(work_items, start, end);
}

/******************************************************************************/
/**
 * Enforces the "need-all" evaluation policy after a submission (see the header).
 *
 * @param status The executor status returned by the submission
 * @param caller The calling function's name, used in the error message
 */
void GOptimizationAlgorithmBase::requireCompleteEvaluation_(
    const Gem::Courtier::submission_status_t &status,
    const std::string &caller
) const {
    if(not status.is_complete || status.has_errors) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In " << caller << ": Error!" << '\n'
            << "No complete set of items received or errors found in some individuals"
            << '\n'
        );
    }
}

/******************************************************************************/
/**
 * Applies the "tolerant" evaluation policy after a submission (see the header): individuals that a
 * partial or errored return left unusable -- still flagged DO_PROCESS, or error-flagged -- are
 * erased from the population.
 *
 * @param status The executor status returned by the submission
 */
void GOptimizationAlgorithmBase::discardUnusableItems_(
    const Gem::Courtier::submission_status_t &status,
    [[maybe_unused]] const std::string &caller
) {
    if(not status.is_complete) {
        [[maybe_unused]] const std::size_t n_erased =
            std::erase_if(data_cnt_, [](const std::unique_ptr<gen::GGenome> &p) -> bool {
                return (p->getProcessingStatus() == Gem::Courtier::processingStatus::DO_PROCESS);
            });

#ifdef DEBUG
        glogger << "In " << caller << ": " << '\n'
                << "Removed " << n_erased << " unprocessed work items in iteration "
                << this->getIteration() << '\n'
                << GLOGGING;
#endif
    }

    if(status.has_errors) {
        [[maybe_unused]] const std::size_t n_erased =
            std::erase_if(data_cnt_, [](const auto &p) -> bool { return p->has_errors(); });

#ifdef DEBUG
        glogger << "In " << caller << ": " << '\n'
                << "Removed " << n_erased << " erroneous work items in iteration "
                << this->getIteration() << '\n'
                << GLOGGING;
#endif
    }
}

/******************************************************************************/
/**
 * Submits the population's [start, end) range for evaluation. The population element IS the work item
 * (a gen::GGenome carrying its own OA scratch), so the population vector is the submission
 * vector: workOn() submits a span over the live sub-range and reconciles it in place. No move-out /
 * move-back is needed -- the scratch rides along on each individual untouched.
 *
 * @param start The index of the first individual in the population to be evaluated
 * @param end One past the index of the last individual to be evaluated (range is [start, end))
 * @return A struct indicating whether all items returned ("is_complete") and whether there were errors ("has_errors")
 */
Gem::Courtier::submission_status_t
GOptimizationAlgorithmBase::workOnPopulation(std::size_t start, std::size_t end) {
    return this->workOn(this->data_cnt_, start, end);
}

/******************************************************************************/
/**
 * Submit through courtier's span+policy executor. The algorithm passes the contiguous sub-range
 * [start, end) it wants evaluated; we submit a std::span over exactly that range. The span aliases the
 * live population sub-range, so results -- and any cloned refills, written in place over the slot --
 * land directly in the population: no subset copy, no write-back, and no per-item DO_PROCESS/UNPROCESSED
 * flagging (the consumer marks the span DO_PROCESS internally). The policy is chosen per algorithm via
 * getSubmissionPolicy_(): clone-on-partial-return for the tolerant population-based OAs, full-success-
 * or-fatal for the need-all OAs.
 *
 * @param work_items The vector of work items (bare individuals) to be evaluated
 * @param start The index of the first work item in the contiguous sub-range to submit
 * @param end One past the index of the last work item to submit (range is [start, end), clamped to the vector size)
 * @return A struct indicating whether all items returned ("is_complete") and whether there were errors ("has_errors")
 */
Gem::Courtier::submission_status_t GOptimizationAlgorithmBase::workOnViaConsumer_(
    std::vector<std::unique_ptr<gen::GGenome>> &work_items,
    std::size_t start,
    std::size_t end
) {
    // Clamp the requested range to the population and bail out if it is empty.
    end = std::min(end, work_items.size());
    if(end <= start) {
        return Gem::Courtier::submission_status_t{.is_complete=true, .has_errors=false};
    }

    // Submit a span over exactly [start, end); it aliases the population sub-range, so results + any
    // cloned refills are written straight into work_items[start..end). The algorithm is
    // transport-agnostic: it submits through the one process-wide consumer and gets back a fully
    // reconciled span. The submission policy is the algorithm's choice (clone-on-partial-return vs
    // full-success-or-fatal).
    std::span<std::unique_ptr<gen::GGenome>> const sp(work_items.data() + start, end - start);
    auto consumer = this->consumerForSubmission_();
    consumer->processBatch(sp, this->getSubmissionPolicy_());

    const bool has_errors =
        std::ranges::any_of(sp, [](const auto &it) { return it && it->has_errors(); });
    return Gem::Courtier::submission_status_t{.is_complete=true, .has_errors=has_errors};
}

/******************************************************************************/
/**
 * @brief Returns the one process-wide consumer and enables its late-return buffer (sized to roughly one
 * generation).
 *
 * The algorithm is transport-agnostic: it neither builds nor selects a consumer. It asks the setup layer
 * for the process consumer -- which establishes the default one if the process was never given one -- and
 * then applies the only consumer setting that is genuinely the algorithm's, its late-return budget.
 *
 * @return The shared consumer this algorithm submits through
 */
std::shared_ptr<Gem::Courtier::GBaseConsumerT<gen::GGenome>>
GOptimizationAlgorithmBase::consumerForSubmission_() {
    auto consumer = Gem::Geneva::ensureProcessConsumer();
    // Enable the late-return buffer -- but ONLY for algorithms that actually reap late returns
    // (reapsLateReturns()); a non-reaping algorithm (gradient descent, parameter scan, ...) passes cap 0
    // so nothing is retained on its behalf. For a reaper the cap scales with the live population
    // (cap_factor x size) so it is independent of how a generation is chunked into submission batches;
    // cap_factor 0 also disables buffering. TTL and cap_factor are configurable (see
    // set/getLateReturnTTL / set/getLateReturnCapFactor). Idempotent: re-setting the knobs each
    // submission is harmless.
    const std::size_t cap =
        this->reapsLateReturns()
            ? static_cast<std::size_t>(late_return_cap_factor_ * static_cast<double>(this->size()))
            : 0;
    consumer->enableLateReturns(cap, late_return_ttl_);
    return consumer;
}

/******************************************************************************/
/**
 * @brief Drains the consumer's late-return buffer and returns only the late returns that are SAFE TO
 * INTEGRATE. See the header for the full contract; in short this is the single, algorithm-agnostic gate
 * every OA reaps late returns through, applying a VALIDITY filter (clean successes only) and a LINEAGE
 * de-duplication (drop a return whose submission UUID is already live or duplicated within the batch).
 *
 * @return A vector of integrable (clean, de-duplicated) late-returned individuals the consumer buffered
 */
std::vector<std::unique_ptr<gen::GGenome>> GOptimizationAlgorithmBase::getOldWorkItems() const {
    // Reap any LATE returns the consumer buffered -- results that came back after their batch had
    // already been reconciled in place (empty for a local consumer; a networked consumer hands back its
    // bounded late-return buffer). The OA folds the integrable individuals into the next selection.
    auto consumer = Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().consumer();
    if(not consumer) {
        return {};
    }
    auto items = consumer->getLateReturns();

    // Pre-load the de-dup set with the UUIDs already represented in the LIVE population, so a late
    // return whose lineage is still present (a re-dispatched individual whose fresh copy already
    // returned) is rejected. retainIntegrableLateReturns() then drops invalid and duplicate returns.
    auto seen = *this
        | std::views::transform([](const auto &p) { return p->getSubmissionUuid(); })
        | std::ranges::to<std::set<Gem::Courtier::SUBMISSION_UUID_TYPE>>();
    retainIntegrableLateReturns(items, seen);

    // OPTIONAL per-algorithm age window (on top of the consumer-side TTL): drop late returns older than
    // lateReturnMaxAge() iterations. The default (max()) means "no window" -- the common case, skipped
    // entirely. GParChild (EA/SA) returns 1 here: a child evaluated in iteration N typically returns
    // during N+1, so a one-generation window admits exactly those and discards staler returns. iteration
    // >= getAssignedIteration() always holds (no item from the future), so the subtraction cannot underflow.
    const std::uint32_t max_age = this->lateReturnMaxAge();
    if(max_age != std::numeric_limits<std::uint32_t>::max()) {
        const std::uint32_t iteration = this->getIteration();
        std::erase_if(items, [iteration, max_age](const auto &x) -> bool {
            return (iteration - x->getAssignedIteration()) > max_age;
        });
    }
    return items;
}

/******************************************************************************/
/**
 * @brief The pure validity + lineage-dedup filter behind getOldWorkItems(). See the header for the
 * contract. Factored out (and static) so it can be unit-tested with synthetic items, no consumer needed.
 *
 * @param items The drained late returns to filter (mutated in place)
 * @param seen The set of already-represented submission UUIDs (updated with survivors)
 */
void GOptimizationAlgorithmBase::retainIntegrableLateReturns(
    std::vector<std::unique_ptr<gen::GGenome>> &items,
    std::set<Gem::Courtier::SUBMISSION_UUID_TYPE> &seen
) {
    // VALIDITY: keep only clean successes. is_processed() and has_errors() are mutually exclusive states,
    // but we test both so the filter stays watertight against any status added between them in future.
    std::erase_if(items, [](const auto &x) -> bool {
        return (not x->is_processed()) || x->has_errors();
    });

    // LINEAGE de-dup: drop a return whose UUID is already represented (live population, pre-loaded above)
    // or that recurs within this batch. set::insert reports false on a duplicate, so the first occurrence
    // of each UUID is kept and every later one removed.
    std::erase_if(items, [&seen](const auto &x) -> bool {
        return not seen.insert(x->getSubmissionUuid()).second;
    });
}

/******************************************************************************/
/**
 * @brief Saves the state of the class to disc
 *
 * @param output_file The path of the file the checkpoint is written to
 */
void GOptimizationAlgorithmBase::saveCheckpoint(std::filesystem::path const &output_file) const {
    this->toFile(output_file, this->getCheckpointSerializationMode());
}

/******************************************************************************/
/**
 * @brief Extracts the short name of the optimization algorithm (example:
 * "PERSONALITY_EA") from a path which complies to the following
 * scheme: /some/path/word1-PERSONALITY_EA-some-other-information .
 * This is mainly used for checkpointing and associated cross-checks.
 *
 * @param p The checkpoint-file path from whose filename the algorithm personality token is extracted
 * @return The extracted algorithm personality token (the second hyphen-separated token of the filename)
 */
std::string
GOptimizationAlgorithmBase::extractOptAlgFromPath(const std::filesystem::path &p) {
    // Extract the filename
    std::string const filename = p.filename().string();

    // Divide the name into tokens
    std::vector<std::string> tokens = Gem::Common::splitString(filename, "-");

    // Check that the size is at least 2 (i.e. the PERSONALITY_X-part may exist)
    if(tokens.size() < 2) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase<>::extractOptAlgFromPath(): Error!" << '\n'
            << "Found file name " << filename << " that does not comply to rules." << '\n'
            << "Expected \"/some/path/word1-PERSONALITY_EA-some-other-information \"" << '\n'
        );
    }

    // Let the audience know
    return tokens[1];
}

/******************************************************************************/
/**
 * @brief Retrieves the best individual found up to now (which is usually the best individual
 * in the priority queue).
 *
 * @return A sole-ownership pointer to a clone of the globally best individual found so far
 */
std::unique_ptr<gen::GGenome> GOptimizationAlgorithmBase::getBestGlobalIndividual_() const {
    // best() throws on an empty archive and the archive never admits an empty handle, so the entry
    // is known to be there. Always clone: the archive solely owns its entries and callers get a copy.
    return best_global_individuals_pq_.best()->clone<gen::GGenome>();
}

/******************************************************************************/
/**
 * @brief Retrieves a list of the best individuals found (equal to the content of
 * the priority queue)
 *
 * @return A vector of sole-ownership pointers to clones of the globally best individuals found so far
 */
std::vector<std::unique_ptr<gen::GGenome>>
GOptimizationAlgorithmBase::getBestGlobalIndividuals_() const {
    std::vector<std::unique_ptr<gen::GGenome>> best_individuals_vec;
    best_individuals_vec.reserve(best_global_individuals_pq_.size());

    for(const auto &ind_ptr : best_global_individuals_pq_) {
        best_individuals_vec.push_back(ind_ptr->clone<gen::GGenome>());
    }

    return best_individuals_vec;
}

/******************************************************************************/
/**
 * @brief Retrieves the best individual found in the iteration (which is the best individual
 * in the priority queue).
 *
 * @return A sole-ownership pointer to a clone of the best individual of the current iteration
 */
std::unique_ptr<gen::GGenome> GOptimizationAlgorithmBase::getBestIterationIndividual_() const {
    // See getBestGlobalIndividual_(): best() throws on an empty archive, and the caller gets a copy.
    return best_iteration_individuals_pq_.best()->clone<gen::GGenome>();
}

/******************************************************************************/
/**
 * @brief Retrieves a list of the best individuals found in the iteration (equal to the content of
 * the priority queue)
 *
 * @return A vector of sole-ownership pointers to clones of the current iteration's best individuals
 */
std::vector<std::unique_ptr<gen::GGenome>>
GOptimizationAlgorithmBase::getBestIterationIndividuals_() const {
    std::vector<std::unique_ptr<gen::GGenome>> best_individuals_vec;
    best_individuals_vec.reserve(best_iteration_individuals_pq_.size());

    // Clone, exactly like the global-best accessor above: this used to hand out the archive's own
    // entries, which contradicted both the documented contract ("copies") and its own sibling. The
    // archive owns its individuals outright now, so the copy is no longer optional.
    for(const auto &ind_ptr : best_iteration_individuals_pq_) {
        best_individuals_vec.push_back(ind_ptr->clone<gen::GGenome>());
    }

    return best_individuals_vec;
}

/******************************************************************************/
/**
 * @brief Allows to set the personality type of the individuals
 */
void GOptimizationAlgorithmBase::setIndividualPersonalities() {
    const std::string oa_mnemonic = this->getPersonalityTraits_()->getMnemonic();
    for(auto const &ind : *this) {
        // The rich personality OBJECT is OA scratch -- it rides on the individual itself. Each individual
        // gets its own (getPersonalityTraits_() returns a fresh instance per call). On a checkpoint resume,
        // however, the individual already carries its restored personality (e.g. a swarm's personal-best
        // lives there); preserve it rather than overwriting it with a fresh, empty one.
        if(not(resumed_from_checkpoint_ && ind->scratch().personalityRef())) {
            ind->setPersonality(this->getPersonalityTraits_());
        }

        // Decide post-processing eligibility HERE (the algorithm knows its own mnemonic) and veto it on
        // the work items this algorithm is not allowed to post-process -- so the individual carries no
        // knowledge of which algorithm owns it. The veto rides on the work item's processing metadata
        // (where the post-processor already lives) and is consulted by GProcessingContainerT::postProcess_().
        auto pp = ind->postProcessor();
        if(pp) {
            auto post_processor =
                std::dynamic_pointer_cast<GPostProcessorBaseT<gen::GGenome>>(pp);
            const bool eligible =
                post_processor and post_processor->postProcessingAllowedFor(oa_mnemonic);
            ind->vetoPostProcessing(not eligible);
        }
    }
}

/******************************************************************************/
/**
 * @brief Resets the individual's personality types
 */
void GOptimizationAlgorithmBase::resetIndividualPersonalities() {
    for(auto const &ind : *this) {
        // Drop the individual's OA-owned scratch -- BOTH the personality OBJECT and the per-group adaption
        // POD state ride on the individual, so resetPersonality() (which clears the whole GAuxiliaryStore)
        // performs the algorithm-boundary teardown (an EA's sigma / personality is meaningless to a
        // chained CGD).
        ind->resetPersonality();
    }
}

/******************************************************************************/
/**
 * Sets the default size of the population
 *
 * @param def_pop_size The desired size of the population
 */
void GOptimizationAlgorithmBase::setDefaultPopulationSize(std::size_t def_pop_size) {
    default_population_size_ = def_pop_size;
}

/******************************************************************************/
/**
 * Set the number of "best" individuals to be recorded in each iteration
 *
 * @param n_record_best_individuals The number of "best" individuals to be recorded in each iteration
 */
void GOptimizationAlgorithmBase::setNRecordBestIndividuals(
    std::size_t n_record_best_individuals
) {
    if(0 == n_record_best_individuals) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizationAlgorithmBase<>::setNRecordBestIndividuals(): Error!" << '\n'
            << "Invalid number of individuals to be recorded: " << n_record_best_individuals << '\n'
        );
    }

    n_recordbest_global_individuals_ = n_record_best_individuals;
    best_global_individuals_pq_.setMaxSize(n_recordbest_global_individuals_);
}

/******************************************************************************/
/**
 * Retrieve the number of best individuals to be recorded in each iteration
 *
 * @return The number of best individuals to be recorded in each iteration
 */
std::size_t GOptimizationAlgorithmBase::getNRecordBestIndividuals() const {
    return n_recordbest_global_individuals_;
}

/******************************************************************************/
/**
 * @brief Allows derived classes to reset the stall counter.
 */
void GOptimizationAlgorithmBase::resetStallCounter() {
    stall_counter_ = 0;
}

/******************************************************************************/
/**
 * @brief Allows to perform initialization work before the optimization cycle starts. This
 * function will usually be overloaded by derived functions, which should however,
 * as their first action, call this function.
 */
void GOptimizationAlgorithmBase::init() {
    // Submission goes through the one process-wide consumer (GConsumerRegistry). No routing is
    // configured here: the first submission asks the setup layer for that consumer, which establishes the
    // default one if the process has none (see consumerForSubmission_), so a bare alg->optimize() works
    // standalone.

    // Create the shared thread pool used for parallel organizational work (adaption,
    // recombination, ...). Derived algorithms that call GOptimizationAlgorithmBase::init() first get it for free.
    tp_ptr_ = std::make_unique<Gem::Common::Concurrency::GThreadPool>(
        "oa:tp",
        n_threads_,
        Gem::Common::Concurrency::ThreadElasticity::Elastic
    );
}

/******************************************************************************/
/**
 * @brief Allows to perform any remaining work after the optimization cycle has finished.
 * This function will usually be overloaded by derived functions, which should however
 * call this function as their last action.
 */
void GOptimizationAlgorithmBase::finalize() {
    // Release the shared thread pool created in init().
    tp_ptr_.reset();
    // Otherwise nothing to do: courtier needs no executor teardown (the process consumer is released by RAII).
}

/******************************************************************************/
/**
 * @brief Sets the number of threads used for parallel organizational work (adaption,
 * recombination, ...). If n_threads is 0, the count falls back to the default.
 *
 * @param n_threads The number of threads to use; 0 selects the default thread count
 */
void GOptimizationAlgorithmBase::setNThreads(std::uint16_t n_threads) {
    if(n_threads == 0) {
        glogger << "In GOptimizationAlgorithmBase::setNThreads(n_threads):" << '\n'
                << "n_threads == 0 was requested. n_threads_ was reset to the default "
                << Gem::Courtier::DEFAULTNSTDTHREADS << '\n'
                << GWARNING;

        n_threads_ = Gem::Courtier::DEFAULTNSTDTHREADS;
    }
    else {
        n_threads_ = n_threads;
    }
}

/******************************************************************************/
/**
 * @brief Retrieves the number of threads used for parallel organizational work.
 *
 * @return The number of threads used for parallel organizational work
 */
std::uint16_t GOptimizationAlgorithmBase::getNThreads() const {
    return n_threads_;
}

/******************************************************************************/
/**
 * @brief Lets individuals know about the current iteration of the optimization
 * cycle.
 */
void GOptimizationAlgorithmBase::markIteration() {
    for(auto const &ind_ptr : *this) {
        ind_ptr->setAssignedIteration(iteration_);
    }
}

/******************************************************************************/
/**
 * @brief Lets all individuals know about their position in the population.
 *
 * The position is stamped into the shared GPositionPersonalityTraits base of the algorithm's
 * personality traits, so this one implementation serves every algorithm.
 */
void GOptimizationAlgorithmBase::markIndividualPositions() {
    for(auto const &[pos, individual] : data_cnt_ | std::views::enumerate) {
        individual->getPersonalityTraits<GPositionPersonalityTraits>()->setPopulationPosition(
            static_cast<std::size_t>(pos)
        );
    }
}

/******************************************************************************/
/**
 * @brief Update the stall counter. We use the transformed fitness for comparison
 * here, so we can usually deal with finite values (due to the transformation
 * in the case of a constraint violation).
 *
 * @param best_eval The best raw and transformed fitness tuple found in the current iteration; the transformed value is compared against the best known so far
 */
void GOptimizationAlgorithmBase::updateStallCounter(const std::tuple<double, double> &best_eval) {
    auto m = this->at(0)->getMaxMode(); // We assume the same maxMode for all individuals
    if(isBetter(
           std::get<G_TRANSFORMED_FITNESS>(best_eval),
           std::get<G_TRANSFORMED_FITNESS>(best_known_primary_fitness_),
           m
       )) {
        best_known_primary_fitness_ = best_eval;
        stall_counter_ = 0;
    }
    else {
        stall_counter_++;
    }
}

/******************************************************************************/
/**
 * This function returns true once a given time (set with
 * GOptimizationAlgorithm<GGenome>::setMaxTime()) has passed.
 * It is used in the GOptimizationAlgorithmBase::halt() function.
 *
 * @param current_time The reference time point against which the elapsed time since the start of the run is measured
 * @return A boolean indicating whether a given amount of time has passed
 */
bool GOptimizationAlgorithmBase::timedHalt(
    const std::chrono::system_clock::time_point &current_time
) const {
    if((current_time - start_time_) >= max_duration_) {
        if(emit_termination_reason_) {
            glogger << "Terminating optimization run because maximum time frame has been exceeded."
                    << '\n'
                    << GLOGGING;
        }

        return true;
    }
            return false;
   
}

/******************************************************************************/
/**
 * @brief This function checks whether a minimum amount of time has passed
 *
 * @param current_time The reference time point against which the elapsed time since the start of the run is measured
 * @return A boolean indicating whether the minimum required processing time has elapsed
 */
bool GOptimizationAlgorithmBase::minTimePassed(
    const std::chrono::system_clock::time_point &current_time
) const {
    return (current_time - start_time_) > min_duration_;
}

/******************************************************************************/
/**
 * This function returns true once the quality is below or above a given threshold
 * (depending on whether we maximize or minimize). This function uses user-visible
 * (i.e. untransformed) fitness values, as a quality threshold will usually be
 * set using a true "physical" value.
 *
 * @return A boolean indicating whether the quality is above or below a given threshold
 */
bool GOptimizationAlgorithmBase::qualityHalt() const {
    auto m = this->at(0)->getMaxMode(); // We assume the same maxMode for all individuals
    if(isBetter(
           std::get<G_RAW_FITNESS>(
               best_known_primary_fitness_
           ) // note: we use the raw fitness so users do not have to specify "transformed" thresholds
           ,
           quality_threshold_,
           m
       )) {
        if(emit_termination_reason_) {
            glogger << "Terminating optimization run because" << '\n'
                    << "quality threshold " << quality_threshold_ << " has been exceeded."
                    << '\n'
                    << "Best untransformed quality found was "
                    << std::get<G_RAW_FITNESS>(best_known_primary_fitness_) << '\n'
                    << "with termination in iteration " << iteration_ << '\n'
                    << GLOGGING;
        }

        return true;
    }
            return false;
   
}

/******************************************************************************/
/**
 * This function returns true once a given number of stalls has been exceeded in a row
 *
 * @return A boolean indicating whether the optimization has stalled too often in a row
 */
bool GOptimizationAlgorithmBase::stallHalt() const {
    if(stall_counter_ >= max_stall_iteration_) {
        if(emit_termination_reason_) {
            glogger << "Terminating optimization run because" << '\n'
                    << "maximum number of stalls " << max_stall_iteration_ << " has been exceeded."
                    << '\n'
                    << "This is considered to be a criterion for convergence." << '\n'
                    << GLOGGING;
        }

        return true;
    }
            return false;
   
}

/******************************************************************************/
/**
 * This function returns true once a maximum number of iterations has been exceeded
 *
 * @return A boolean indicating whether the maximum number of iterations has been exceeded
 */
bool GOptimizationAlgorithmBase::iterationHalt() const {
    if(iteration_ >= max_iteration_) {
        if(emit_termination_reason_) {
            glogger << "Terminating optimization run because" << '\n'
                    << "iteration threshold " << max_iteration_ << " has been exceeded."
                    << '\n'
                    << GLOGGING;
        }

        return true;
    }
            return false;
   
}

/******************************************************************************/
/**
 * @brief This function returns true when the minimum number of iterations has
 * been passed.
 *
 * @return A boolean indicating whether the minimum number of iterations has been reached
 */
bool GOptimizationAlgorithmBase::minIterationPassed() const {
    // iteration_ is incremented before halt()/this check is evaluated, so after
    // the N-th cycle iteration_ == N. ">=" makes the minimum pass at exactly
    // min_iteration_ cycles; ">" would run one extra iteration (and is
    // inconsistent with iterationHalt(), which uses ">=" for the maximum).
    return iteration_ >= min_iteration_;
}

/******************************************************************************/
/**
 * This function returns true if a SIGHUP / CTRL_CLOSE_EVENT signal was sent (provided the user
 * has registered the Gem::Common::sigHupHandler signal handler
 *
 * @return A boolean indicating whether the program was interrupted with a SIGHUP or CTRL_CLOSE_EVENT signal
 */
bool GOptimizationAlgorithmBase::sigHupHalt() {
    if(Gem::Common::G_SIGHUP_SENT()) {
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
        std::cout
            << "Terminating optimization run because a CTRL_CLOSE_EVENT signal has been received"
            << '\n';
#else
        std::cout << "Terminating optimization run because a SIGHUP signal has been received"
                  << '\n';
#endif
        return true;
    }
            return false;
   
}

/******************************************************************************/
/**
 * Triggers termination of the optimization run, when a file with a user-defined
 * name is modified (e.g. "touch'ed") after the optimization run was started. Note
 * that the function will silently return false if the file does not exist, as it
 * is assumed that users may "touch" the file for termination only, so that the
 * possibility exists that the file isn't there until that time.
 */
bool GOptimizationAlgorithmBase::touchHalt() const {
    // Create a suitable path object
    std::filesystem::path const p(termination_file_);

    // Return if the file doesn't exist
    if(not std::filesystem::exists(p)) {
        return false;
    }

    // Determine the modification time of the file
    const auto mod_time = std::filesystem::last_write_time(p);

    // Check if the file was modified after the start of the optimization run
    if(mod_time > file_start_time_) {
        if(emit_termination_reason_) {
            glogger << "Terminating optimization run because" << '\n'
                    << p << " was modified after the start of the optimization" << '\n'
                    << GLOGGING;
        }

        return true;
    }
            return false;
   
}

/******************************************************************************/
/**
 * A wrapper for the customHalt() function that allows us to emit the termination reason
 *
 * @return A boolean indicating whether a custom halt criterion has been reached
 */
bool GOptimizationAlgorithmBase::customHalt() const {
    if(customHalt_()) {
        if(emit_termination_reason_) {
            glogger << "Terminating optimization run because custom halt criterion has triggered."
                    << '\n'
                    << GLOGGING;
        }

        return true;
    }
            return false;
   
}

/******************************************************************************/
/**
 * Custom halt condition
 *
 * @return A boolean indicating whether a custom halt criterion has been reached
 */
bool GOptimizationAlgorithmBase::customHalt_() const {
    return false;
}

/******************************************************************************/
/**
 * This function checks whether a halt criterion has been reached. The most
 * common criterion is the maximum number of iterations. Set the max_iteration_
 * counter to 0 if you want to disable this criterion.
 *
 * @return A boolean indicating whether a halt criterion has been reached
 */
bool GOptimizationAlgorithmBase::halt() const {
    // Retrieve the current time, so all time-based functions act on the same basis
    std::chrono::system_clock::time_point const current_time = std::chrono::system_clock::now();

    //------------------------------------------------------------------------
    // The following halt criteria are triggered by the user. They override
    // all other (automatic) criteria

    // Have we received a SIGHUP signal ?
    if(sigHupHalt()) {
        return true;
    }

    // Are we supposed to stop when a file was modified after the start of the optimization run ?
    if(terminate_on_file_modification_ && touchHalt()) {
        return true;
    }

    //------------------------------------------------------------------------
    // With the exception of the above criteria, no other halt criterion will
    // have an effect unless some minimum criteria have been met. E.g., if the
    // minimum number of iterations, as defined by the user, hasn't been passwd,
    // the optimization will continue (no matter whether e.g. the optimization
    // has stalled for a given number of times).

    // Has the minimum number of iterations, as defined by the user, been passed?
    if(not minIterationPassed()) {
        return false;
    }

    // Has the minimum required optimization time been passed?
    if(not minTimePassed(current_time)) {
        return false;
    }

    //------------------------------------------------------------------------
    // The following halt criteria are evaluated by Geneva at run-time,
    // without any user-interaction.

    // Have we exceeded the maximum number of iterations and
    // do we indeed intend to stop in this case ?
    if(maxIterationHaltset() && iterationHalt()) {
        return true;
    }

    // Has the optimization stalled too often ?
    if(stallHaltSet() && stallHalt()) {
        return true;
    }

    // Do we have a scheduled halt time ? The comparatively expensive
    // timedHalt() calculation is only called if max_duration_
    // is at least one microsecond.
    if(maxDurationHaltSet() && timedHalt(current_time)) {
        return true;
    }

    // Are we supposed to stop when the quality has exceeded a threshold ?
    if(qualityThresholdHaltSet() && qualityHalt()) {
        return true;
    }

    // Has the user specified an additional stop criterion ?
    return customHalt();
}

/******************************************************************************/
/**
 * Check whether the max-iteration halt is set
 *
 * @return A boolean indicating whether the "max-iteration halt" has been set
 */
bool GOptimizationAlgorithmBase::maxIterationHaltset() const {
    return 0 != max_iteration_;
}

/******************************************************************************/
/**
 * Check whether a halt criterion based on the number of stalls has been set
 *
 * @return A boolean indicating whether a halt criterion based on the number of stalls has been set
 */
bool GOptimizationAlgorithmBase::stallHaltSet() const {
    return 0 != max_stall_iteration_;
}

/******************************************************************************/
/**
 * Check whether the max_duration-halt criterion has been set
 *
 * @return A boolean indication whether the max-duration halt criterion has been set
 */
bool GOptimizationAlgorithmBase::maxDurationHaltSet() const {
    return 0. != max_duration_.count();
}

/******************************************************************************/
/**
 * Check whether the quality-threshold halt-criterion has been set
 *
 * @return A boolean indicating whether the quality-threshold halt-criterion has been set
 */
bool GOptimizationAlgorithmBase::qualityThresholdHaltSet() const {
    return has_quality_threshold_;
}

/******************************************************************************/
/**
 * @brief Indicates whether the stall_counter_threshold_ has been exceeded
 *
 * @return A boolean indicating whether the current stall counter exceeds the configured threshold
 */
bool GOptimizationAlgorithmBase::stallCounterThresholdExceeded() const {
    return (stall_counter_ > stall_counter_threshold_);
}

/******************************************************************************/
/**
 * Applies modifications to this object. This is needed for testing purposes
 *
 * @return A boolean which indicates whether modifications were made
 */
bool GOptimizationAlgorithmBase::modify_GUnitTests_() {
#ifdef GEM_TESTING
    bool result = false;


    // Try to change the objects contained in the collection. The population is heterogeneous and
    // its element type -- the gen::GGenome category root -- deliberately does not carry the
    // self-test facet, so an element contributes only if it opted into GSelfTestable. Skipping the
    // others is the correct semantics: it is exactly what the facet's no-op default produced when
    // the hooks still sat on every object.
    for(auto const &o_ptr : *this) {
        if(auto *self_testable = dynamic_cast<Gem::Common::GSelfTestable *>(o_ptr.get());
           self_testable != nullptr && self_testable->modify_GUnitTests()) {
            result = true;
        }
    }

    this->setMaxIteration(this->getMaxIteration() + 1);
    result = true;

    return result;

#else /* GEM_TESTING */ // If this function is called when GEM_TESTING isn't set, throw
    Gem::Common::condnotset("GOptimizationAlgorithmBase<>::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif                  /* GEM_TESTING */
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
