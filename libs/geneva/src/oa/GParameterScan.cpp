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

#include "geneva/oa/GParameterScan.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonInterfaceT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GPersonalityTraits.hpp"
#include "geneva/GenevaHelperFunctions.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GParameterScan_PersonalityTraits.hpp"
#include "geneva/genome/GParameterPropertyParser.hpp"
#include "geneva/genome/GGenome.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <ostream>
#include <tuple>
#include <utility>
#include <vector>

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GBScanPar) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GInt32ScanPar) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GDScanPar) // NOLINT
GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GFScanPar) // NOLINT

GEM_REGISTER_ARCHIVABLE(Gem::Geneva::OptimizationAlgorithms::GParameterScan) // NOLINT

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Returns a set of boolean data items (always both false and true).
 *
 * @param n_steps The requested number of grid steps (unused for booleans, which always yield two values)
 * @param lower The lower scan boundary (unused for booleans)
 * @param upper The upper scan boundary (unused for booleans)
 * @return A vector containing the two boolean values {false, true}
 */
template <>
std::vector<bool> fillWithData<bool>(
    [[maybe_unused]] std::size_t n_steps
    ,
    [[maybe_unused]] bool lower
    ,
    [[maybe_unused]] bool upper
) {
    std::vector<bool> result;
    result.push_back(false);
    result.push_back(true);
    return result;
}

/******************************************************************************/
/**
 * @brief Returns a set of std::int32_t data items spread evenly over the inclusive scan interval.
 *
 * Like the float/double specializations, this honours n_steps: it generates exactly n_steps values
 * evenly spaced over [lower, upper] (endpoints included), each rounded to the nearest integer. Honouring
 * n_steps keeps the grid vector size equal to n_steps_, which the grid traversal logic (goToNextItem /
 * isAtTerminalPosition) relies on. Note that for a small integer range with many steps, neighbouring
 * grid points may round to the same value (harmless, redundant evaluations).
 *
 * @param n_steps The number of grid points to generate (must be at least 2)
 * @param lower The (inclusive) lower scan boundary (first generated value)
 * @param upper The (inclusive) upper scan boundary (last generated value)
 * @return A vector of n_steps integer values evenly spaced over [lower, upper]
 */
template <>
std::vector<std::int32_t> fillWithData<std::int32_t>(
    std::size_t n_steps
    ,
    std::int32_t lower,
    std::int32_t upper // inclusive
) {
    std::vector<std::int32_t> result;

    // We require at least 2 steps, unless we are in random mode
    if(n_steps < 2) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In std::vector<std::int32_t> fillWithData<std::int32_t>(): Error!" << '\n'
            << "Number of requested steps is too low: " << n_steps << '\n'
        );
    }

    for(std::size_t i = 0; i < n_steps; i++) {
        const double v = static_cast<double>(lower) +
            (static_cast<double>(upper - lower) * static_cast<double>(i) /
                static_cast<double>(n_steps - 1));
        result.push_back(static_cast<std::int32_t>(std::llround(v)));
    }

    return result;
}

/******************************************************************************/
/**
 * @brief Returns a set of float data items spread evenly over the scan interval.
 *
 * @param n_steps The number of grid points to generate (must be at least 2)
 * @param lower The lower scan boundary (first generated value)
 * @param upper The upper scan boundary (last generated value)
 * @return A vector of n_steps float values evenly spaced over [lower, upper]
 */
template <>
std::vector<float> fillWithData<float>(std::size_t n_steps, float lower, float upper) {
    std::vector<float> result;

    // We require at least 2 steps, unless we are in random mode
    if(n_steps < 2) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In std::vector<float> fillWithData<float>(): Error!" << '\n'
            << "Number of requested steps is too low: " << n_steps << '\n'
        );
    }

    for(std::size_t i = 0; i < n_steps; i++) {
        result.push_back(lower + ((upper - lower) * static_cast<float>(i) / static_cast<float>(n_steps - 1)));
    }

    return result;
}

/******************************************************************************/
/**
 * @brief Returns a set of double data items spread evenly over the scan interval.
 *
 * @param n_steps The number of grid points to generate (must be at least 2)
 * @param lower The lower scan boundary (first generated value)
 * @param upper The upper scan boundary (last generated value)
 * @return A vector of n_steps double values evenly spaced over [lower, upper]
 */
template <>
std::vector<double> fillWithData<double>(std::size_t n_steps, double lower, double upper) {
    std::vector<double> result;

    // We require at least 2 steps, unless we are in random mode
    if(n_steps < 2) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In std::vector<float> fillWithData<double>(): Error!" << '\n'
            << "Number of requested steps is too low: " << n_steps << '\n'
        );
    }

    for(std::size_t i = 0; i < n_steps; i++) {
        result.push_back(lower + ((upper - lower) * static_cast<double>(i) / static_cast<double>(n_steps - 1)));
    }

    return result;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// The concrete scan-parameter classes (GBScanPar, GInt32ScanPar, GDScanPar, GFScanPar) are now fully
// defined header-side via the GScanParT CRTP helper (construction, cloning and the default constructor
// are all inherited / generated). Only their Boost export-implement macros live in this translation
// unit (see the top of the file).

/******************************************************************************/
/**
 * @brief A simple output operator for parSet objects, mostly meant for debugging.
 *
 * @param os The output stream to write the human-readable representation to
 * @param p_s The parSet object whose boolean, integer, float and double parameters are printed
 * @return A reference to the output stream os, to allow chaining
 */
std::ostream &operator<<(std::ostream &os, const parSet &p_s) {
    os << "###########################################################" << '\n'
       << "# New parSet object:" << '\n';

    // Each entry is printed as "position:value".

    // Boolean data
    if(not p_s.bParVec.empty()) {
        os << "# Boolean data" << '\n';
        for(auto cit = p_s.bParVec.begin(); cit != p_s.bParVec.end(); ++cit) {
            os << cit->pos << ":" << (cit->value ? "true" : "false");
            if(cit + 1 != p_s.bParVec.end()) {
                os << ", ";
            }
        }
        os << '\n';
    }

    // std::int32_t data
    if(not p_s.iParVec.empty()) {
        os << "# std::int32_t data" << '\n';
        for(auto cit = p_s.iParVec.begin(); cit != p_s.iParVec.end(); ++cit) {
            os << cit->pos << ":" << cit->value;
            if(cit + 1 != p_s.iParVec.end()) {
                os << ", ";
            }
        }
        os << '\n';
    }

    // float data
    if(not p_s.fParVec.empty()) {
        os << "# float data" << '\n';
        for(auto cit = p_s.fParVec.begin(); cit != p_s.fParVec.end(); ++cit) {
            os << cit->pos << ":" << cit->value;
            if(cit + 1 != p_s.fParVec.end()) {
                os << ", ";
            }
        }
        os << '\n';
    }

    // double data
    if(not p_s.dParVec.empty()) {
        os << "# double data" << '\n';
        for(auto cit = p_s.dParVec.begin(); cit != p_s.dParVec.end(); ++cit) {
            os << cit->pos << ":" << cit->value;
            if(cit + 1 != p_s.dParVec.end()) {
                os << ", ";
            }
        }
        os << '\n';
    }

    return os;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A standard copy constructor.
 *
 * @param cp A constant reference to another GParameterScan object to be copied
 */
GParameterScan::GParameterScan(const GParameterScan &cp)
  : GOptimizationAlgorithmT<GParameterScan>(cp)
  , cycle_logic_halt_(cp.cycle_logic_halt_)
  , scan_randomly_(cp.scan_randomly_)
  , simple_scan_items_(cp.simple_scan_items_)
  , scans_performed_(cp.scans_performed_) {
    // Copying / setting of the optimization algorithm id is done by the parent class. The same
    // applies to the copying of the optimization monitor.

    // Deep-copy the scan-parameter objects through the Gemfony common interface.
    Gem::Common::copyCloneableSmartPointerContainer(cp.b_cnt_, b_cnt_);
    Gem::Common::copyCloneableSmartPointerContainer(cp.int32_cnt_, int32_cnt_);
    Gem::Common::copyCloneableSmartPointerContainer(cp.d_cnt_, d_cnt_);
    Gem::Common::copyCloneableSmartPointerContainer(cp.f_cnt_, f_cnt_);
}

/******************************************************************************/
/**
 * @brief Resets the settings of this population to what was configured when
 * the optimize()-call was issued.
 */
void GParameterScan::resetToOptimizationStart_() {
    // Reset b_cnt_, int32_cnt_, d_cnt_ and f_cnt_
    this->resetParameterObjects();

    // Reset the custom halt criterion
    cycle_logic_halt_ = false;

    // No scans have been peformed so far
    scans_performed_ = 0;

    // Make sure we start with a fresh central vector of parameter objects
    this->clearAllParVec();

    // There is no more work to be done here, so we simply call the
    // function of the parent class
    GOptimizationAlgorithmBase::resetToOptimizationStart_();
}

/******************************************************************************/
/**
 * @brief The actual business logic to be performed during each iteration. Returns the best achieved fitness.
 *
 * @return A tuple holding the raw and transformed fitness of the best individual found this iteration
 */
std::tuple<double, double> GParameterScan::cycleLogic_() {
    std::tuple<double, double> best_fitness =
        std::make_tuple(this->at(0)->getWorstCase(), this->at(0)->getWorstCase());

    // Apply all necessary modifications to individuals
    if(0 == simple_scan_items_) { // We have been asked to deal with specific parameters
        updateSelectedParameters();
    }
    else { // We have been asked to randomly initialize the individuals a given number of times
        randomInitPopulation();
    }

    // Trigger value calculation for all individuals
    // This function is purely virtual and needs to be
    // re-implemented in derived classes
    evaluatePopulation_();

    // Retrieve information about the best fitness found and disallow re-evaluation
    GParameterScan::iterator it;
    std::tuple<double, double> new_eval = std::make_tuple(0., 0.);
    auto m =
        this->at(0)->getMaxMode(); // We assume that the maxMode is the same for all individuals
    for(it = this->begin(); it != this->end(); ++it) {
#ifdef DEBUG
        if(not(*it)->is_processed()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParameterScan::cycleLogic(): Error!" << '\n'
                << "Individual in position " << (it - this->begin()) << " is not processed"
                << '\n'
            );
        }
#endif

        new_eval = (*it)->getFitnessTuple();
        if(isBetter(
               std::get<G_TRANSFORMED_FITNESS>(new_eval),
               std::get<G_TRANSFORMED_FITNESS>(best_fitness),
               m
           )) {
            best_fitness = new_eval;
        }
    }

    // Let the audience know
    return best_fitness;
}

/******************************************************************************/
/**
 * @brief Adds new values to the population's individuals. Note that this function
 * may resize the population and set the default population size, if there
 * is no sufficient number of data sets to be evaluated left.
 */
// NOLINTNEXTLINE(readability-function-size) -- single coherent per-item scan-population procedure: for each individual, fill the four typed parameter channels (bool/int32/float/double) from the parSet, snap floating-point grid endpoints into the genome's half-open bounds, write back, then advance/terminate the scan loop; the channels and the loop-continuation logic are tightly coupled around the same work item
void GParameterScan::updateSelectedParameters() {
    std::size_t ind_pos = 0;

    while(true) {
        //------------------------------------------------------------------------
        // Retrieve a work item (all parameters are addressed positionally)
        std::shared_ptr<parSet> const p_s = getParameterSet();

        {
            std::vector<bool> b_data;
            std::vector<std::int32_t> i_data;
            std::vector<float> f_data;
            std::vector<double> d_data;

            // Fill the parameter set data into the current individual.
            // Read/write the parameter values through the genome-agnostic value channels.
            auto &ind = (*this->at(ind_pos));

            // Retrieve the parameter vectors
            ind.streamline<bool>(b_data);
            ind.streamline<std::int32_t>(i_data);
            ind.streamline<float>(f_data);
            ind.streamline<double>(d_data);

            // Add the data items from the parSet object to the vectors

            // 1) For boolean data
            for(const auto &b_par : p_s->bParVec) {
                this->addDataPoint<bool>(b_par, b_data);
            }

            // 2) For std::int32_t data
            for(const auto &i_par : p_s->iParVec) {
                this->addDataPoint<std::int32_t>(i_par, i_data);
            }

            // 3) For float values
            for(const auto &f_par : p_s->fParVec) {
                this->addDataPoint<float>(f_par, f_data);
            }

            // 4) For double values
            for(const auto &d_par : p_s->dParVec) {
                this->addDataPoint<double>(d_par, d_data);
            }

            // A floating-point scan grid is generated over the CLOSED interval [lower, upper] the user
            // requested, but a bounded genome parameter is half-open [lower, upper) -- so a grid endpoint
            // sitting exactly on a parameter's upper bound is not a representable value and an external
            // write would (correctly) reject it. Snap such an endpoint to the largest representable value
            // just inside the bound, so a "scan up to the boundary" samples the boundary instead of
            // throwing. Unbounded parameters report ±max() bounds, so they are left untouched.
            auto snapHalfOpen = [](auto &data, const auto &l, const auto &u) {
                for(std::size_t k = 0; k < data.size(); ++k) {
                    if(data[k] >= u[k]) {
                        data[k] = std::nextafter(u[k], l[k]);
                    }
                    else if(data[k] < l[k]) {
                        data[k] = l[k];
                    }
                }
            };
            std::vector<double> d_lo;
            std::vector<double> d_hi;
            ind.boundaries<double>(d_lo, d_hi);
            snapHalfOpen(d_data, d_lo, d_hi);
            std::vector<float> f_lo;
            std::vector<float> f_hi;
            ind.boundaries<float>(f_lo, f_hi);
            snapHalfOpen(f_data, f_lo, f_hi);

            // Copy the data back into the individual
            ind.assignValueVector<bool>(b_data);
            ind.assignValueVector<std::int32_t>(i_data);
            ind.assignValueVector<float>(f_data);
            ind.assignValueVector<double>(d_data);
        }

        //------------------------------------------------------------------------
        // Mark the individual as "dirty", so it gets re-evaluated the
        // next time the fitness() function is called
        this->at(ind_pos)->mark_as_due_for_processing();

        // We were successful
        cycle_logic_halt_ = false;

        //------------------------------------------------------------------------
        // Make sure we continue with the next parameter set in the next iteration
        if(not this->switchToNextParameterSet()) {
            // Let the audience know that the optimization may be stopped
            this->cycle_logic_halt_ = true;

            // Reset all parameter objects for the next run (if desired)
            this->resetParameterObjects();

            // Resize the population, so we only have modified individuals
            this->resize(ind_pos + 1);

            // Terminate the loop
            break;
        }

        //------------------------------------------------------------------------
        // We do not want to exceed the boundaries of the population
        if(++ind_pos >= this->getDefaultPopulationSize()) {
            break;
        }
    }
}

/******************************************************************************/
/**
 * @brief Randomly (re-)initializes the population's individuals (simple-scan mode).
 *
 * Fills as many individuals as the population holds, counting each one in scans_performed_, until the
 * requested overall number of random scans (simple_scan_items_) has been reached. When the target is
 * hit mid-population the population is trimmed to exactly the individuals initialized this iteration, so
 * the count is exact and no stale (un-initialized) clone is left behind -- mirroring the grid path's
 * resize semantics in updateSelectedParameters().
 */
void GParameterScan::randomInitPopulation() {
    std::size_t ind_pos = 0;

    while(true) {
        // Randomly (re-)initialize the current individual and mark it for re-evaluation.
        this->at(ind_pos)->randomInit(activityMode::ACTIVEONLY);
        this->at(ind_pos)->mark_as_due_for_processing();

        // Count this initialized work item.
        ++scans_performed_;

        // We were successful
        cycle_logic_halt_ = false;

        //------------------------------------------------------------------------
        // Stop once the requested overall number of random scans has been performed. ind_pos still
        // points at the individual just initialized, so keep exactly [0 .. ind_pos].
        if(scans_performed_ >= simple_scan_items_) {
            // Let the audience know that the optimization may be stopped
            this->cycle_logic_halt_ = true;

            // Reset all parameter objects for the next run (if desired)
            this->resetParameterObjects();

            // Resize the population, so we only have modified individuals
            this->resize(ind_pos + 1);

            // Terminate the loop
            break;
        }

        //------------------------------------------------------------------------
        // We do not want to exceed the boundaries of the population -- stop if we have reached the end
        // of the population (the next iteration fills the rest).
        if(++ind_pos >= this->getDefaultPopulationSize()) {
            break;
        }
    }
}

/******************************************************************************/
/**
 * @brief Resets all parameter objects (booleans, integers, floats and doubles) to their start position.
 */
void GParameterScan::resetParameterObjects() {
    for(auto &p : b_cnt_)     p->resetPosition();
    for(auto &p : int32_cnt_) p->resetPosition();
    for(auto &p : f_cnt_)     p->resetPosition();
    for(auto &p : d_cnt_)     p->resetPosition();
    // simple_scan_items_ is NOT reset here: it is set by the user via setNSimpleScans()
    // and must survive across resetParameterObjects() calls within a single optimize() run.
}

/******************************************************************************/
/**
 * @brief Retrieves a parameter set by filling the current parameter combinations
 * into a parSet object.
 *
 * @return A shared pointer to a freshly filled parSet object holding the current parameter values
 */
std::shared_ptr<parSet> GParameterScan::getParameterSet() {
    // Create a new parSet object
    std::shared_ptr<parSet> result(new parSet());

    // Every scan parameter is positional (the former by-name addressing modes have been removed);
    // guard once per parameter against a corrupted specification.
    auto append = [this](const auto &scan_par_cnt, auto &target_vec) {
        using single_t = typename std::remove_reference_t<decltype(target_vec)>::value_type;
        for(const auto &scan_par : scan_par_cnt) {
            gen::NAMEANDIDTYPE var = scan_par->getVarAddress();
            if(std::get<0>(var) != 0) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GParameterScan::getParameterSet(): Error!" << '\n'
                    << "Encountered non-positional addressing mode " << std::get<0>(var)
                    << " (by-name addressing has been removed)" << '\n'
                );
            }
            target_vec.push_back(single_t{
                scan_par->getCurrentItem(gr_), // value
                std::get<2>(var)               // position
            });
        }
    };
    append(b_cnt_, result->bParVec);
    append(int32_cnt_, result->iParVec);
    append(f_cnt_, result->fParVec);
    append(d_cnt_, result->dParVec);

    return result;
}

/******************************************************************************/
/**
 * @brief Switches to the next parameter set.
 *
 * @return A boolean indicating whether there indeed is a following
 * parameter set (true) or whether we have reached the end of the
 * collection (false)
 */
bool GParameterScan::switchToNextParameterSet() {
    // Nothing to advance through (e.g. a spec that yielded no scanned parameters) -- treat it as
    // "all combinations exhausted" rather than dereferencing an empty vector.
    if(all_par_cnt_.empty()) {
        return false;
    }

    auto it = all_par_cnt_.begin();

    // Switch to the next parameter set
    while(true) {
        if((*it)->goToNextItem()) { // Will trigger if a warp has occurred
            if(it + 1 == all_par_cnt_.end()) {
                return false; // All possible combinations were found
            }
            ++it; // Try the next parameter object
        }
        else {
            return true; // We have successfully switched to the next parameter set
        }
    }
}

/******************************************************************************/
/**
 * @brief Fills all parameter objects (booleans, integers, floats, doubles) into the central
 * all_par_cnt_ vector for unified handling.
 */
void GParameterScan::fillAllParVec() {
    // 1) For boolean objects
    for(const auto &item_ptr : b_cnt_) {
        all_par_cnt_.push_back(item_ptr);
    }
    // 2) For std::int32_t objects
    for(const auto &item_ptr : int32_cnt_) {
        all_par_cnt_.push_back(item_ptr);
    }
    // 3) For float objects
    for(const auto &item_ptr : f_cnt_) {
        all_par_cnt_.push_back(item_ptr);
    }
    // 4) For double objects
    for(const auto &item_ptr : d_cnt_) {
        all_par_cnt_.push_back(item_ptr);
    }
}

/******************************************************************************/
/**
 * @brief Clears the central all_par_cnt_ vector.
 */
void GParameterScan::clearAllParVec() {
    all_par_cnt_.clear();
}

/******************************************************************************/
/**
 * @brief A custom halt criterion for the optimization, allowing to stop the loop
 * when no items are left to be scanned.
 *
 * @return true if the scan has exhausted all parameter sets and should terminate, false otherwise
 */
bool GParameterScan::customHalt_() const {
    if(this->cycle_logic_halt_) {
        glogger << "Terminating the loop as no items are left to be" << '\n'
                << "processed in parameter scan." << '\n'
                << GLOGGING;
        return true;
    }
    return false;
}

/******************************************************************************/
/**
 * @brief Adds local configuration options to a GParserBuilder object.
 *
 * @param gpb The GParserBuilder object to which configuration options should be added
 */
void GParameterScan::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our parent class'es function
    GOptimizationAlgorithmBase::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<std::size_t>(
        "size" // The name of the first variable
        ,
        DEFAULTPOPULATIONSIZE,
        [this](std::size_t dps) { this->setDefaultPopulationSize(dps); }
    ) << "The total size of the population";

    gpb.registerFileParameter<std::string>(
        "parameter_options",
        std::string("d(0, -10., 10., 100), d(1, -10., 10., 100)"),
        [this](std::string par_specs) { this->setParameterSpecs(std::move(par_specs)); }
    ) << "Specification of the parameters to be used in the parameter scan"
      << '\n';

    gpb.registerFileParameter<bool>(
        "scan_randomly" // The name of the variable
        ,
        true // The default value
        ,
        [this](bool sr) { this->setScanRandomly(sr); }
    ) << "Indicates whether scans of individual variables should be done randomly"
      << '\n'
      << "(1) or on a grid (0)";

    // Override the default value of max_stall_iteration, as the parent
    // default does not make sense for us (we do not need stall iterations)
    gpb.resetFileParameterDefaults("max_stall_iteration", DEFAULTMAXPARSCANSTALLIT);
}

/******************************************************************************/
/**
 * @brief Triggers fitness calculation of a number of individuals by delegating work to the process
 * consumer.
 *
 * Items are evaluated up to a maximum position in the vector. Note that we always start the
 * evaluation with the first item in the vector. Throws if not every work item returned or if
 * any returned item carried errors.
 */
void GParameterScan::evaluatePopulation_() {
    using namespace Gem::Courtier;

#ifdef DEBUG
    GParameterScan::iterator it;
    for(it = this->begin(); it != this->end(); ++it) {
        // Make sure the evaluated individuals have the dirty flag set
        if(not(*it)->is_due_for_processing()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParameterScan::evaluatePopulation_():" << '\n'
                << "Found individual in position " << std::distance(this->begin(), it)
                << ", which has not been marked as due for processing" << '\n'
            );
        }
    }
#endif /* DEBUG */

    //--------------------------------------------------------------------------------
    // Submit all work items and wait for their return

    auto status = this->workOnPopulation(0, this->data_cnt_.size());

    // An incomplete or errored return cannot be accepted in a parameter scan.
    this->requireCompleteEvaluation_(status, "GParameterScan::evaluatePopulation_()");
}

/******************************************************************************/
/**
 * @brief Analyzes the parameters to be scanned. Note that this function will clear any
 * existing parameter definitions, as par_str represents a new set of parameters
 * to be scanned.
 *
 * @param par_str A specification string describing the parameters to scan (e.g. "d(0, -10., 10., 100)"),
 *                or a simple-scan request; must not be empty
 */
void GParameterScan::setParameterSpecs(const std::string& par_str) {
    // Check that the parameter string isn't empty
    if(par_str.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParameterScan::addParameterSpecs(): Error!" << '\n'
            << "Parameter string " << par_str << " is empty" << '\n'
        );
    }

    //---------------------------------------------------------------------------
    // Clear the parameter vectors
    d_cnt_.clear();
    f_cnt_.clear();
    int32_cnt_.clear();
    b_cnt_.clear();

    // Parse the parameter string
    gen::GParameterPropertyParser const ppp(par_str);

    //---------------------------------------------------------------------------
    // Assign the parameter definitions to our internal parameter vectors.
    // We distinguish between a simple scan, where only a number of work items
    // will be initialized randomly repeatedly, and scans of individual variables.
    simple_scan_items_ = ppp.getNSimpleScanItems();
    if(0 == simple_scan_items_) { // Only act if no "simple scan" was requested
        // Retrieve double parameters
        std::tuple<
            std::vector<gen::parPropSpec<double>>::const_iterator,
            std::vector<gen::parPropSpec<double>>::const_iterator>
            t_d = ppp.getIterators<double>();

        auto d_cit = std::get<0>(t_d);
        auto d_end = std::get<1>(t_d);
        for(; d_cit != d_end;
            ++d_cit) { // Note: d_cit is already set to the begin of the double parameter arrays
            d_cnt_.push_back(std::make_shared<GDScanPar>(*d_cit, scan_randomly_));
        }

        // Retrieve float parameters
        std::tuple<
            std::vector<gen::parPropSpec<float>>::const_iterator,
            std::vector<gen::parPropSpec<float>>::const_iterator>
            t_f = ppp.getIterators<float>();

        auto f_cit = std::get<0>(t_f);
        auto f_end = std::get<1>(t_f);
        for(; f_cit != f_end;
            ++f_cit) { // Note: f_cit is already set to the begin of the float parameter arrays
            f_cnt_.push_back(std::make_shared<GFScanPar>(*f_cit, scan_randomly_));
        }

        // Retrieve integer parameters
        std::tuple<
            std::vector<gen::parPropSpec<std::int32_t>>::const_iterator,
            std::vector<gen::parPropSpec<std::int32_t>>::const_iterator>
            t_i = ppp.getIterators<std::int32_t>();

        auto i_cit = std::get<0>(t_i);
        auto i_end = std::get<1>(t_i);
        for(; i_cit != i_end;
            ++i_cit) { // Note: i_cit is already set to the begin of the double parameter arrays
            int32_cnt_.push_back(
                std::make_shared<GInt32ScanPar>(*i_cit, scan_randomly_)
            );
        }

        // Retrieve boolean parameters
        std::tuple<
            std::vector<gen::parPropSpec<bool>>::const_iterator,
            std::vector<gen::parPropSpec<bool>>::const_iterator>
            t_b = ppp.getIterators<bool>();

        auto b_cit = std::get<0>(t_b);
        auto b_end = std::get<1>(t_b);
        for(; b_cit != b_end;
            ++b_cit) { // Note: b_cit is already set to the begin of the double parameter arrays
            b_cnt_.push_back(std::make_shared<GBScanPar>(*b_cit, scan_randomly_));
        }
    }

    //---------------------------------------------------------------------------
}

/******************************************************************************/
/**
 * @brief Specifies the number of simple scans and puts the class in "simple scan" mode.
 *
 * @param simple_scan_items The number of randomly-initialized work items to evaluate (0 disables simple-scan mode)
 */
void GParameterScan::setNSimpleScans(std::size_t simple_scan_items) {
    simple_scan_items_ = simple_scan_items;
}

/******************************************************************************/
/**
 * @brief Retrieves the number of simple scans (or 0, if disabled).
 *
 * @return The configured number of simple scans, or 0 if simple-scan mode is disabled
 */
std::size_t GParameterScan::getNSimpleScans() const {
    return simple_scan_items_;
}

/******************************************************************************/
/**
 * @brief Retrieves the number of simple scans performed so far.
 *
 * @return The count of simple scans completed in the current optimization run
 */
std::size_t GParameterScan::getNScansPerformed() const {
    return scans_performed_;
}

/******************************************************************************/
/**
 * @brief Allows to specify whether the parameter space should be scanned randomly
 * or on a grid.
 *
 * @param scan_randomly true to scan individual variables randomly, false to scan on a grid
 */
void GParameterScan::setScanRandomly(bool scan_randomly) {
    scan_randomly_ = scan_randomly;
}

/******************************************************************************/
/**
 * @brief Allows to check whether the parameter space should be scanned randomly
 * or on a grid.
 *
 * @return true if variables are scanned randomly, false if scanned on a grid
 */
bool GParameterScan::getScanRandomly() const {
    return scan_randomly_;
}

/******************************************************************************/
/**
 * @brief Does some preparatory work before the optimization starts.
 */
void GParameterScan::init() {
    // To be performed before any other action
    GOptimizationAlgorithmBase::init();

    // Reset the custom halt criterion
    cycle_logic_halt_ = false;

    // No scans have been peformed so far
    scans_performed_ = 0;

    // Make sure we start with a fresh central vector of parameter objects
    this->clearAllParVec();

    // Copy all parameter objects to the central vector for easier handling
    this->fillAllParVec();
}

/******************************************************************************/
/**
 * @brief Does any necessary finalization work.
 */
void GParameterScan::finalize() {
    // Last action
    GOptimizationAlgorithmBase::finalize();
}

/******************************************************************************/
/**
 * @brief Retrieve a GPersonalityTraits object belonging to this algorithm.
 *
 * @return A shared pointer to a new GParameterScan_PersonalityTraits object
 */
std::shared_ptr<GPersonalityTraits> GParameterScan::getPersonalityTraits_() const {
    return std::make_shared<personality_traits_type>();
}

/******************************************************************************/
/**
 * @brief Resizes the population to the desired level and does some error checks.
 */
void GParameterScan::adjustPopulation_() {
    // Check how many individuals we already have
    std::size_t n_start = this->size();

    // Do some error checking ...

    // An empty population is an error
    if(n_start == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParameterScan::adjustPopulation(): Error!" << '\n'
            << "You didn't add any individuals to the collection. We need at least one."
            << '\n'
        );
    }

    // We want exactly one individual in the beginning. All other registered
    // individuals will be discarded.
    if(n_start > 1) {
        this->resize(1);
        n_start = 1;
    }

    // Check that we have a valid default population size
    if(0 == this->getDefaultPopulationSize()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParameterScan::adjustPopulation(): Error!" << '\n'
            << "Default-size of the population is 0" << '\n'
        );
    }

    // Create the desired number of (identical) individuals in the population.
    for(std::size_t ind = 1; ind < this->getDefaultPopulationSize(); ind++) {
        this->push_back(this->at(0)->clone());
    }
}

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */
