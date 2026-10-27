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

#include "geneva/genome/GGenome.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <ranges>
#include <string>
#include <vector>

#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "hap/GRandomLeasePool.hpp"
#include "geneva/GOptimizationEnums.hpp"

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

#include "geneva/GFaultInjector.hpp" // the pluggable, process-global evaluation fault injector
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include "common/GParserBuilder.hpp"

namespace Gem::Geneva::Genome {




/******************************************************************************/
/**
 * @brief Installs the value arrays and shared structural layout produced by a GGenomeBuilder.
 * @param g The GenomeData bundle (double/float/int/bool value arrays plus the shared layout) to install
 */
void GGenome::setGenome(GenomeData const &g) {
    // Install the new layout (re-keys the parameter-count cache).
    this->setLayout(g.layout ? g.layout : std::make_shared<const GGenomeLayout>());

    // Reject inverted bounds up front with a clear message (lower == upper is a valid frozen parameter).
    auto checkBounds = [](auto const &ch, const char *type_name) {
        for(std::size_t k = 0; k < ch.size(); ++k) {
            if(ch.fold[k] && ch.lower[k] > ch.upper[k]) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GGenome::setGenome(): Error!" << '\n'
                    << "Bounded " << type_name << " parameter " << k << " has inverted bounds (lower "
                    << ch.lower[k] << " > upper " << ch.upper[k] << ")." << '\n'
                );
            }
        }
    };
    checkBounds(layout_->d, "double");
    checkBounds(layout_->f, "float");

    // The builder's floating-point start values are EXTERNAL; convert each to the normalized internal
    // store (range-validated). The int / bool channels are not normalized and are stored as-is.
    dv_.resize(g.dv.size());
    for(std::size_t k = 0; k < g.dv.size(); ++k) {
        dv_[k] = externalToInternalChecked<double>(layout_->d, g.dv[k], k, /*allow_upper_bound=*/true);
    }
    fv_.resize(g.fv.size());
    for(std::size_t k = 0; k < g.fv.size(); ++k) {
        fv_[k] = externalToInternalChecked<float>(layout_->f, g.fv[k], k, /*allow_upper_bound=*/true);
    }
    iv_ = g.iv;
    bv_ = g.bv;

    this->mark_as_due_for_processing();
}

/******************************************************************************/
/**
 * @brief Random initialization of the active parameters across all four channels.
 * @param am The activity mode that selects which parameters are initialized
 * @return true if at least one parameter value was modified, false otherwise
 */
bool GGenome::randomInit_(activityMode const &am) {
    // The candidate holds no RNG of its own -- lease a proxy for the duration of this initialization.
    auto             lease = Gem::Hap::randomLeasePool().acquire();
    Gem::Hap::GRandomBase &gr = *lease;

    bool modified = false;
    if(randomInitFP<double>(dv_, layout_->d, am, gr)) { modified = true; }
    if(randomInitFP<float>(fv_, layout_->f, am, gr)) { modified = true; }
    if(randomInitInt(am, gr)) { modified = true; }
    if(randomInitBool(am, gr)) { modified = true; }
    return modified;
}

/******************************************************************************/
/**
 * @brief Randomly (re-)initializes the active entries of one floating-point value channel.
 * @tparam T The floating-point value type of the channel (double or float)
 * @param store The value array of this channel, modified in place
 * @param ch The channel layout providing per-element activity flags and interval bounds
 * @param am The activity mode that selects which entries are initialized
 * @return true if at least one entry was modified, false otherwise
 */
template <typename T>
bool GGenome::randomInitFP(std::vector<T> &store, ChannelLayout<T> const &ch, activityMode const &am, Gem::Hap::GRandomBase &gr) {
    bool modified = false;
    std::uniform_real_distribution<T> dist;
    for(std::size_t k = 0; k < store.size(); ++k) {
        if(not amMatch(ch.active[k], am)) {
            continue;
        }
        const T x = dist(
            gr,
            typename std::uniform_real_distribution<T>::param_type(ch.init_lower[k], ch.init_upper[k])
        );
        store[k] = externalToInternalChecked<T>(ch, x, k);
        modified = true;
    }
    return modified;
}

/******************************************************************************/
/**
 * @brief Randomly (re-)initializes the active entries of the int32 value channel.
 * @param am The activity mode that selects which entries are initialized
 * @return true if at least one entry was modified, false otherwise
 */
bool GGenome::randomInitInt(activityMode const &am, Gem::Hap::GRandomBase &gr) {
    bool modified = false;
    std::uniform_int_distribution<std::int32_t> dist;
    const ChannelLayout<std::int32_t> &ch = layout_->i;
    for(std::size_t k = 0; k < iv_.size(); ++k) {
        if(not amMatch(ch.active[k], am)) {
            continue;
        }
        std::int32_t lo = ch.fold[k] ? ch.lower[k] : ch.init_lower[k];
        std::int32_t hi = ch.fold[k] ? ch.upper[k] : ch.init_upper[k];
        if(hi < lo) {
            std::swap(lo, hi);
        }
        iv_[k] = dist(gr, std::uniform_int_distribution<std::int32_t>::param_type(lo, hi));
        modified = true;
    }
    return modified;
}

/******************************************************************************/
/**
 * @brief Randomly (re-)initializes the active entries of the boolean value channel.
 * @param am The activity mode that selects which entries are initialized
 * @return true if at least one entry was modified, false otherwise
 */
bool GGenome::randomInitBool(activityMode const &am, Gem::Hap::GRandomBase &gr) {
    bool modified = false;
    std::bernoulli_distribution dist(0.5);
    const ChannelLayout<bool> &ch = layout_->b;
    for(std::size_t k = 0; k < bv_.size(); ++k) {
        if(not amMatch(ch.active[k], am)) {
            continue;
        }
        bv_[k] = dist(gr) ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0);
        modified = true;
    }
    return modified;
}

/******************************************************************************/
/**
 * @brief Retrieval of a suitable position for cross over inside of a vector, in the range [lower, upper[.
 * @param lower The (inclusive) lower bound of the position range; must be > 0
 * @param upper The (exclusive) upper bound of the position range; must be > lower
 * @return A uniformly drawn position in the half-open range [lower, upper)
 */
std::size_t GGenome::getCrossOverPos(const std::size_t lower, const std::size_t upper) {
    if(lower == 0) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::getCrossOverPos(): Error!" << '\n'
            << "lower boundary is 0, but must be > 0" << '\n'
        );
    }
    if(upper <= lower) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::getCrossOverPos(): Error!" << '\n'
            << "Invalid range: upper (" << upper << ") must be > lower (" << lower << ")" << '\n'
        );
    }
    auto lease = Gem::Hap::randomLeasePool().acquire();
    std::uniform_int_distribution<std::size_t> dist;
    return dist(*lease, std::uniform_int_distribution<std::size_t>::param_type(lower, upper - 1));
}

/******************************************************************************/
/**
 * @brief Perform a cross-over operation between this genome and another.
 * @param cp_base A constant reference to the other parent (a GGenome)
 * @return A newly created GGenome holding the crossed-over genome, upcast to GGenome
 */
std::shared_ptr<GGenome> GGenome::crossOverWith(GGenome const &cp_base) const {
    const auto &cp = dynamic_cast<const GGenome &>(cp_base);
    auto this_cp = this->clone<GGenome>();

    std::vector<double> this_d, cp_d;
    std::vector<float> this_f, cp_f;
    std::vector<bool> this_b, cp_b;
    std::vector<std::int32_t> this_i, cp_i;

    this_cp->streamline(this_d);
    this_cp->streamline(this_f);
    this_cp->streamline(this_b);
    this_cp->streamline(this_i);

    cp.streamline(cp_d);
    cp.streamline(cp_f);
    cp.streamline(cp_b);
    cp.streamline(cp_i);

    auto splice = [&](auto &this_vec, auto const &cp_vec) {
        if(not this_vec.empty() && this_vec.size() == cp_vec.size()) {
            const auto pos = this_cp->getCrossOverPos(1, this_vec.size());
            std::copy(
                cp_vec.begin() + static_cast<std::ptrdiff_t>(pos),
                cp_vec.end(),
                this_vec.begin() + static_cast<std::ptrdiff_t>(pos)
            );
        }
    };
    splice(this_d, cp_d);
    splice(this_f, cp_f);
    splice(this_b, cp_b);
    splice(this_i, cp_i);

    this_cp->assignValueVector(this_d);
    this_cp->assignValueVector(this_f);
    this_cp->assignValueVector(this_b);
    this_cp->assignValueVector(this_i);

    this_cp->mark_as_due_for_processing();

    return this_cp;
}

/******************************************************************************/
/**
 * @brief Retrieves parameters relevant for the evaluation from another GGenome.
 * @param cp A reference to the foreign genome (a GGenome); it is moved-from and cleared
 */
void GGenome::cannibalize(GGenome &cp_base) {
    auto &cp = dynamic_cast<GGenome &>(cp_base);
    if(cp.is_due_for_processing() || cp.has_errors()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::cannibalize(): Error!" << '\n'
            << "cp isn't processed or has errors" << '\n'
        );
    }

    dv_ = std::move(cp.dv_);
    fv_ = std::move(cp.fv_);
    iv_ = std::move(cp.iv_);
    bv_ = std::move(cp.bv_);
    this->setLayout(cp.layout_);

    cp.dv_.clear();
    cp.fv_.clear();
    cp.iv_.clear();
    cp.bv_.clear();

    this->setFitness_(cp.raw_fitness_vec());
}

/******************************************************************************/
/**
 * @brief Retrieve the active double parameter at the given positional index.
 * @param idx The positional index into the streamlined double channel
 * @return The double value at the given index
 */
double GGenome::getVarVal_d_(std::size_t idx) {
    std::vector<double> v;
    this->streamline<double>(v);
    return v.at(idx);
}

/**
 * @brief Retrieve the active float parameter at the given positional index.
 * @param idx The positional index into the streamlined float channel
 * @return The float value at the given index
 */
float GGenome::getVarVal_f_(std::size_t idx) {
    std::vector<float> v;
    this->streamline<float>(v);
    return v.at(idx);
}

/**
 * @brief Retrieve the active int32 parameter at the given positional index.
 * @param idx The positional index into the streamlined int32 channel
 * @return The int32 value at the given index
 */
std::int32_t GGenome::getVarVal_i_(std::size_t idx) {
    std::vector<std::int32_t> v;
    this->streamline<std::int32_t>(v);
    return v.at(idx);
}

/**
 * @brief Retrieve the active boolean parameter at the given positional index.
 * @param idx The positional index into the streamlined boolean channel
 * @return The boolean value at the given index
 */
bool GGenome::getVarVal_b_(std::size_t idx) {
    std::vector<bool> v;
    this->streamline<bool>(v);
    return static_cast<bool>(v.at(idx));
}

/******************************************************************************/
/**
 * @brief Transformation of the individual's parameters into a JSON object.
 *
 * Returns the individual body only (no wrapping key); callers place it wherever they need it. The
 * parameters live in a "vars" JSON array (index = position) and the results in a "results" JSON array,
 * replacing the former positional var0/result0 keys. Numeric and boolean values are native JSON numbers
 * and bools; the transformation policy is a string.
 *
 * @return A boost::json::object holding this individual's parameters, metadata and results
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent JSON-serialization sweep over the four value channels + metadata + results, sharing the "emit" lambda; splitting would scatter tightly coupled body-object assembly
boost::json::object GGenome::toJSON() const {
    namespace json = boost::json;

    bool const dirty_flag = (Gem::Courtier::processingStatus::DO_PROCESS == this->getProcessingStatus());
    bool const has_errors = this->has_errors();

    json::object body;
    body["iteration"] = this->getAssignedIteration();
    body["is_dirty"] = dirty_flag;
    body["has_errors"] = has_errors;
    body["isValid"] = has_errors || dirty_flag ? false : this->isValid();
    body["type"] = "GGenome";

    std::vector<double> d_data;
    std::vector<float> f_data;
    std::vector<std::int32_t> i_data;
    std::vector<bool> b_data;
    this->streamline<double>(d_data);
    this->streamline<float>(f_data);
    this->streamline<std::int32_t>(i_data);
    this->streamline<bool>(b_data);

    const std::size_t n_vars = d_data.size() + f_data.size() + i_data.size() + b_data.size();
    body["nVars"] = n_vars;

    json::array vars;
    auto emit = [&](auto const &vec, const char *type_name) {
        for(auto const &val : vec) {
            json::object var;
            var["value"] = val;
            var["type"] = type_name;
            vars.push_back(std::move(var));
        }
    };
    emit(d_data, "double");
    emit(f_data, "float");
    emit(i_data, "int32");
    emit(b_data, "bool");
    body["vars"] = std::move(vars);

    switch(this->getEvaluationPolicy()) {
    case evaluationPolicy::USESIMPLEEVALUATION:
        body["transformationPolicy"] = "USESIMPLEEVALUATION";
        break;
    case evaluationPolicy::USESIGMOID:
        body["transformationPolicy"] = "USESIGMOID";
        break;
    case evaluationPolicy::USEWORSTCASEFORINVALID:
        body["transformationPolicy"] = "USEWORSTCASEFORINVALID";
        break;
    }

    body["n_results"] = this->getNStoredResults();
    json::array results;
    for(std::size_t i = 0; i < this->getNStoredResults(); i++) {
        const double raw_fitness =
            (dirty_flag || has_errors) ? this->getWorstCase() : this->raw_fitness(i);
        const double transformed_fitness =
            (dirty_flag || has_errors) ? this->getWorstCase() : this->transformed_fitness(i);
        json::object result;
        result["result"] = transformed_fitness;
        result["rawResult"] = raw_fitness;
        results.push_back(std::move(result));
    }
    body["results"] = std::move(results);

    return body;
}

/******************************************************************************/
/**
 * @brief Transformation of the individual's parameters into a list of comma-separated values.
 * @param with_name_and_type If true, prepend a row of variable names and a row of variable types
 * @param with_commas If true, separate fields with ",\\t"; otherwise with a plain tab
 * @param use_raw_fitness If true, emit raw fitness values; otherwise emit transformed fitness values
 * @param show_validity If true, append a trailing validity column
 * @return A string holding the CSV (tab-separated) representation, terminated with a newline
 */
// NOLINTNEXTLINE(readability-function-size) -- one coherent CSV-serialization sweep over the four value channels + fitness + validity, sharing the "emit"/"joinRow" lambdas; splitting would scatter tightly coupled row assembly
std::string GGenome::toCSV(
    bool with_name_and_type,
    bool with_commas,
    bool use_raw_fitness,
    bool show_validity
) const {
    std::vector<double> d_data;
    std::vector<float> f_data;
    std::vector<std::int32_t> i_data;
    std::vector<bool> b_data;

    this->streamline<double>(d_data);
    this->streamline<float>(f_data);
    this->streamline<std::int32_t>(i_data);
    this->streamline<bool>(b_data);

    std::vector<std::string> var_names;
    std::vector<std::string> var_types;
    std::vector<std::string> var_values;

    std::size_t var_index = 0;
    auto emit = [&](auto const &vec, const char *type_name) {
        for(std::size_t pos = 0; pos < vec.size(); ++pos) {
            if(with_name_and_type) {
                var_names.push_back(std::string("var") + Gem::Common::to_string(var_index));
                var_types.emplace_back(type_name);
            }
            var_values.push_back(Gem::Common::to_string(vec.at(pos)));
            ++var_index;
        }
    };
    emit(d_data, "double");
    emit(f_data, "float");
    emit(i_data, "int32");
    emit(b_data, "bool");

    for(std::size_t i = 0; i < this->getNStoredResults(); i++) {
        if(with_name_and_type) {
            var_names.push_back(std::string("Fitness_") + Gem::Common::to_string(i));
            var_types.emplace_back("double");
        }
        if(this->is_processed()) {
            if(use_raw_fitness) {
                var_values.push_back(Gem::Common::to_string(this->raw_fitness(i)));
            }
            else {
                var_values.push_back(Gem::Common::to_string(this->transformed_fitness(i)));
            }
        }
        else {
            if(this->has_errors()) {
                var_values.emplace_back("has_errors");
            }
            else {
                var_values.emplace_back("dirty");
            }
        }
    }

    if(show_validity) {
        if(with_name_and_type) {
            var_names.emplace_back("validity");
            var_types.emplace_back("bool");
        }
        if(this->is_processed()) {
            var_values.push_back(Gem::Common::to_string(this->isValid()));
        }
        else {
            var_values.push_back(Gem::Common::to_string(false));
        }
    }

    // Each row joins its fields with a between-fields separator only (no trailing separator) and is
    // terminated by a single newline -- byte-identical to the former "write separator unless last" loops.
    std::string const separator = with_commas ? ",\t" : "\t";
    auto const joinRow = [&separator](std::vector<std::string> const &fields) {
        return fields | std::views::join_with(separator) | std::ranges::to<std::string>();
    };

    std::string result;
    if(with_name_and_type) {
        result += joinRow(var_names);
        result += '\n';
        result += joinRow(var_types);
        result += '\n';
    }
    result += joinRow(var_values);
    result += '\n';

    return result;
}

/******************************************************************************/
/**
 * @brief Applies modifications to this object. This is needed for testing purposes.
 * @return true if modifications were made, false otherwise
 */
bool GGenome::modify_GUnitTests_() {
#ifdef GEM_TESTING
    bool result = false;

    if(this->randomInit(activityMode::ALLPARAMETERS)) {
        result = true;
    }

    // Guarantee an observable change to a compared member even for individuals with no randomisable
    // parameters (e.g. GDelayIndividual), so the clone-independence / serialize-round-trip tests remain
    // meaningful. This bumps assigned_iteration_ (a compared base member) -- the data-oriented successor
    // of the old n_stalls_ bump, which was retired when stall bookkeeping moved wholly onto the OA.
    this->setAssignedIteration(this->getAssignedIteration() + 1);
    result = true;

    return result;
#else  /* GEM_TESTING */
    Gem::Common::condnotset("GGenome::modify_GUnitTests", "GEM_TESTING");
    return false;
#endif /* GEM_TESTING */
}

/******************************************************************************/
/**
 * @brief Performs self tests that are expected to succeed. This is needed for testing purposes.
 */
// NOLINTNEXTLINE(readability-function-size) -- a self-contained unit-test function (Catch2 CHECK blocks); each {} block is an independent assertion group, standard test-function shape
void GGenome::specificTestsNoFailureExpected_GUnitTests_() {
#ifdef GEM_TESTING
    {
        auto const p_test = this->clone<GGenome>();
        CHECK_NOTHROW(p_test->setMaxMode(maxMode::MAXIMIZE));
        CHECK(p_test->getMaxMode() == maxMode::MAXIMIZE);
        CHECK_NOTHROW(p_test->setMaxMode(maxMode::MINIMIZE));
        CHECK(p_test->getMaxMode() == maxMode::MINIMIZE);
    }
    {
        auto const p_test = this->clone<GGenome>();
        for(std::uint32_t i = 1; i < 10; i++) {
            CHECK_NOTHROW(p_test->setAssignedIteration(i));
            CHECK(p_test->getAssignedIteration() == i);
        }
    }
#else  /* GEM_TESTING */
    Gem::Common::condnotset(
        "GGenome::specificTestsNoFailureExpected_GUnitTests",
        "GEM_TESTING"
    );
#endif /* GEM_TESTING */
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/******************************************************************************/
// ---- Folded in from the former GOptimizableEntity category root ----


/******************************************************************************/
/**
 * @brief The default constructor: a single fitness criterion and a fresh private policy.
 */
GGenome::GGenome() = default;

/******************************************************************************/
/**
 * @brief Initialization with the number of fitness criteria.
 * @param n_fitness_criteria The number of fitness criteria this candidate evaluates to
 */
GGenome::GGenome(const std::size_t n_fitness_criteria)
  : stored_results_cnt_(n_fitness_criteria, individual_processing_result()) {
    /* nothing */
}

/******************************************************************************/
/**
 * @brief The copy constructor. The shared policy is copied by shared pointer (1:N), the
 * pre-/post-processors are deep-cloned.
 * @param cp The other candidate whose data is copied
 */
GGenome::GGenome(GGenome const &cp) {
    // Every member -- the GProcessable base slice (copy-assigned via its descriptor), the plain members and
    // the serialized-but-uncompared processors (deep-cloned) / shared policy / result store / OA scratch
    // (deep-copied) -- is copied from the single localMembers_() declaration, the same machinery load_()
    // uses, so this constructor cannot drift from the member list. The base classes default-construct; the
    // GProcessable slice is then copy-assigned by g_load_members through its base-object descriptor.
    Gem::Common::g_load_members(this->localMembers_(), cp.localMembers_());
}

/******************************************************************************/
/**
 * @brief Installs the shared problem policy (the 1:N feasibility/ranking rules).
 * @param policy The shared policy to reference (must not be empty)
 */
void GGenome::setPolicy(std::shared_ptr<GProblemPolicy> policy) {
    if(not policy) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::setPolicy(): Error!" << '\n'
            << "Tried to install an empty problem policy" << '\n'
        );
    }
    policy_ = std::move(policy);
}

/******************************************************************************/
/**
 * @brief Resets every stored result to a default-constructed value.
 */
void GGenome::clear_stored_results_vec() {
    // Cannot use range-based for here, as the value type might be a proxy (kept for parity with the
    // historical processing container).
    for(auto it = stored_results_cnt_.begin(); it != stored_results_cnt_.end(); ++it) {
        *it = individual_processing_result();
    }
}

/******************************************************************************/
/**
 * @brief Sets the vector of stored results to a given collection and marks the candidate PROCESSED.
 * @param result_cnt The new result vector (size must match the configured number of stored results)
 * @return The first stored result after the assignment
 */
individual_processing_result
GGenome::markAsProcessedWith(std::vector<individual_processing_result> const &result_cnt) {
#ifdef DEBUG
    if(result_cnt.size() != stored_results_cnt_.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::markAsProcessedWith(): Vector dimensions" << '\n'
            << "do not fit: " << result_cnt.size() << " / " << stored_results_cnt_.size() << '\n'
        );
    }
#endif

    stored_results_cnt_ = result_cnt;
    stored_error_descriptions_.clear();
    processing_status_ = Gem::Courtier::processingStatus::PROCESSED;

    return this->stored_results_cnt_.at(0);
}

/******************************************************************************/
/**
 * @brief Read-only retrieval of a stored result. Throws if the PROCESSED flag is not set.
 * @param id The position of the stored result to return
 * @return The stored result at position id
 */
individual_processing_result GGenome::getStoredResult(const std::size_t id) const {
    if(not this->is_processed()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::getStoredResult(): Tried to" << '\n'
            << "retrieve stored result while the PROCESSED flag was not set" << '\n'
        );
    }

    return stored_results_cnt_.at(id);
}

/******************************************************************************/
/**
 * @brief Performs the evaluation of this candidate (see the header for the full sequence).
 * @param res_vec Optional pre-computed raw results; if empty, evaluate() is invoked
 * @return The first stored result after processing
 */
individual_processing_result
GGenome::process(const std::vector<individual_processing_result> &res_vec) {
    // The full lifecycle (precondition, reset, timed pre -> core -> guarded post, exception funneling,
    // error epilogue) is stated ONCE, on GProcessable::runProcessingLifecycle_. Only the evaluation
    // core is geneva-specific: it consults the process-global fault injector (no-op unless a
    // GFaultInjector is registered -- a single null-pointer check on the default path), raises a THROW
    // fault so it surfaces as EXCEPTION_CAUGHT, runs the actual evaluation, and applies a FLAG_ERROR
    // fault after it, like a user flagging an error from within evaluate() (-> ERROR_FLAGGED).
    this->runProcessingLifecycle_(
        "GGenome::process()",
        [this] { this->preProcess_(); },
        [this, &res_vec] {
            GFaultInjector::Fault injected_fault = GFaultInjector::Fault::NONE;
            if(GFaultInjector *injector = GFaultInjectorRegistry::get(); injector != nullptr) {
                // Rare (test-only) path: lease a proxy for the fault injector; the candidate holds no RNG.
                auto lease = Gem::Hap::randomLeasePool().acquire();
                injected_fault = injector->evaluate(*this, *lease);
            }

            if(injected_fault == GFaultInjector::Fault::THROW) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "Fault injected during GGenome::process() (THROW)" << '\n'
                );
            }

            this->runEvaluation_(res_vec);

            // An injected FLAG_ERROR fault mimics a user flagging an error from within evaluate()
            if(injected_fault == GFaultInjector::Fault::FLAG_ERROR && not this->has_errors()) {
                this->force_set_error(
                    "Fault injected during GGenome::process() (FLAG_ERROR)\n");
            }
        },
        [this] { this->postProcess_(); }
    );

    // This part of the code is only reached on success (the lifecycle throws on any error)
    return this->stored_results_cnt_.at(0);
}

/******************************************************************************/
/**
 * @brief The evaluation body run inside process(): feasibility check + evaluate()/res_vec adoption + the
 * evaluation-policy transform.
 * @param res_vec Optional pre-computed raw results
 */
double GGenome::adoptRawResults_(const std::vector<individual_processing_result> &res_vec) {
    double main_raw_result = 0.;

    try {
        if(not res_vec.empty()) {
            if(res_vec.size() != this->getNStoredResults()) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GGenome::adoptRawResults_(): Error!" << '\n'
                    << "res_vec has invalid size. Got " << res_vec.size() << '\n'
                    << "Expected " << this->getNStoredResults() << '\n'
                );
            }

            main_raw_result = res_vec.begin()->rawFitness();

            std::size_t pos = 0;
            for(const auto &res : res_vec) {
                if(pos == 0) {
                    ++pos;
                    continue; // skip the main raw result
                }
                this->setResult(pos, res.rawFitness());
                ++pos;
            }
        }
        else {
            // Local evaluation: the virtual evaluate() RETURNS the raw result vector (main at index 0,
            // secondary criteria after) rather than writing into the individual, mirroring the res_vec
            // path above so the evaluation-policy transform below sees a fully-populated result store.
            // (External GPU/network results took the res_vec branch; both converge into the one
            // feasibility + policy + PROCESSED pass in runEvaluation_().)
            const std::vector<double> raw = this->evaluate();
            if(raw.size() != this->getNStoredResults()) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GGenome::adoptRawResults_(): Error!" << '\n'
                    << "evaluate() returned " << raw.size() << " result(s), but " << '\n'
                    << this->getNStoredResults() << " were expected." << '\n'
                );
            }
            main_raw_result = raw.front();
            for(std::size_t pos = 1; pos < raw.size(); ++pos) {
                this->setResult(pos, raw[pos]);
            }
        }
    }
    catch(...) {
        this->setAllFitnessTo(this->getWorstCase());
        throw;
    }

    return main_raw_result;
}

/******************************************************************************/
/**
 * @brief The evaluation body run inside process(): feasibility check + evaluate()/res_vec adoption + the
 * evaluation-policy transform.
 * @param res_vec Optional pre-computed raw results
 */
void GGenome::runEvaluation_(const std::vector<individual_processing_result> &res_vec) {
    // Find out whether this is a valid solution (must be called first, to fill validity_level_).
    if(this->fulfillsConstraints(validity_level_) ||
       evaluationPolicy::USESIMPLEEVALUATION == this->getEvaluationPolicy()) {
        this->finalizeFeasibleEvaluation_(res_vec);
    }
    else {
        // Some constraints were violated. Act on the chosen policy.
        this->applyInvalidityPolicy_();
    }
}

/******************************************************************************/
/**
 * @brief runEvaluation_() feasible branch: adopt the raw results (from res_vec, or from a local
 * evaluate()) and apply the evaluation-policy transform to every stored result -- unless the user flagged
 * an error without throwing, in which case the whole quality surface is worst-cased.
 *
 * @param res_vec Optional pre-computed raw results (empty -> a local evaluate() supplies them)
 */
void GGenome::finalizeFeasibleEvaluation_(
    const std::vector<individual_processing_result> &res_vec
) {
    const double main_raw_result = this->adoptRawResults_(res_vec);

    this->setResult(0, main_raw_result);
    this->modifyStoredResult(0).setTransformedFitnessToRaw();

    if(this->error_flagged_by_user()) {
        // The user indicated a problem without throwing: worst-case the whole quality surface.
        this->setAllFitnessTo(this->getWorstCase());
        return;
    }

    for(std::size_t i = 0; i < this->getNStoredResults(); i++) {
        if(evaluationPolicy::USESIGMOID == this->getEvaluationPolicy()) {
            this->modifyStoredResult(i).setTransformedFitnessWith(
                [this](const double raw_value) { return policy_->sigmoidTransform(raw_value); }
            );
        }
        else {
            this->modifyStoredResult(i).setTransformedFitnessToRaw();
        }
    }
}

/******************************************************************************/
/**
 * @brief Sets the fitness from a vector of externally-computed raw values (the GPU consumer / external
 * evaluation), applying the feasibility check and evaluation-policy transform, then marking PROCESSED.
 * @param f_cnt A vector of raw fitness values (size must match the criteria count)
 */
void GGenome::setFitness_(std::vector<double> const &f_cnt) {
#ifdef DEBUG
    if(f_cnt.size() != this->getNStoredResults()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::setFitness_(): Error!" << '\n'
            << "Invalid size of fitness vector: " << f_cnt.size()
            << ", expected: " << this->getNStoredResults() << '\n'
        );
    }
#endif /* DEBUG */

    if(this->fulfillsConstraints(validity_level_) ||
       evaluationPolicy::USESIMPLEEVALUATION == this->getEvaluationPolicy()) {
        std::vector<individual_processing_result> processing_results(
            f_cnt.size(),
            individual_processing_result()
        );

        std::size_t pos = 0;
        for(auto &p : processing_results) {
            p.reset(f_cnt.at(pos));

            if(evaluationPolicy::USESIGMOID == this->getEvaluationPolicy()) {
                p.setTransformedFitnessWith(
                    [this](const double raw_value) { return policy_->sigmoidTransform(raw_value); }
                );
            }
            else {
                p.setTransformedFitnessToRaw();
            }
            ++pos;
        }

        this->markAsProcessedWith(processing_results);
    }
    else {
        this->applyInvalidityPolicy_();
    }
}

/******************************************************************************/
/**
 * @brief Applies the configured invalidity policy to a constraint-violating candidate: worst-case
 * the whole quality surface (USEWORSTCASEFORINVALID), or assign the sigmoid barrier value derived
 * from the validity level (USESIGMOID). Shared by runEvaluation_() and setFitness_().
 */
void GGenome::applyInvalidityPolicy_() {
    if(evaluationPolicy::USEWORSTCASEFORINVALID == this->getEvaluationPolicy()) {
        this->setAllFitnessTo(this->getWorstCase());
    }
    else if(evaluationPolicy::USESIGMOID == this->getEvaluationPolicy()) {
        double uniform_fitness_value = 0.;
        const double barrier = this->getBarrier();
        if(maxMode::MAXIMIZE == this->getMaxMode()) {
            uniform_fitness_value = (std::numeric_limits<double>::max() == validity_level_)
                                        ? this->getWorstCase()
                                        : -validity_level_ * barrier;
        }
        else {
            uniform_fitness_value = (std::numeric_limits<double>::max() == validity_level_)
                                        ? this->getWorstCase()
                                        : validity_level_ * barrier;
        }
        this->setAllFitnessTo(this->getWorstCase(), uniform_fitness_value);
    }
}

/******************************************************************************/
/**
 * @brief Registers a (raw) result value of the fitness calculation at a given criterion position.
 * @param id The position of the fitness criterion
 * @param value The raw fitness value to register
 */
void GGenome::setResult(const std::size_t id, const double value) {
#ifdef DEBUG
    if(id >= this->getNStoredResults()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::setResult(): Error!" << '\n'
            << "Invalid position in vector: " << id << " (expected min 0 and max "
            << this->getNStoredResults() - 1 << ")" << '\n'
        );
    }
#endif /* DEBUG */

    this->modifyStoredResult(id).reset(value);
}

/******************************************************************************/
/**
 * @brief Retrieve the (raw, transformed) fitness tuple at a given evaluation position.
 * @param id The evaluation position (fitness criterion index)
 * @return A (raw, transformed) fitness tuple at the requested position
 */
std::tuple<double, double> GGenome::getFitnessTuple(const std::uint32_t id) const {
    return std::make_tuple<double, double>(this->raw_fitness(id), this->transformed_fitness(id));
}

/******************************************************************************/
/**
 * @brief Checks whether this candidate is at least as good as a set of raw boundaries.
 * @param boundaries One boundary value per fitness criterion
 * @return true if every raw fitness is at least as good as its boundary
 */
bool GGenome::isGoodEnough(std::vector<double> const &boundaries) {
#ifdef DEBUG
    if(boundaries.size() != this->getNStoredResults()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::isGoodEnough(): Error!" << '\n'
            << "Number of boundaries does not match number of fitness criteria" << '\n'
        );
    }
    if(not this->is_processed()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::isGoodEnough(): Error!" << '\n'
            << "Trying to compare fitness values although the individual isn't processed" << '\n'
        );
    }
#endif /* DEBUG */

    if(maxMode::MAXIMIZE == this->getMaxMode()) {
        for(std::size_t i = 0; i < boundaries.size(); i++) {
            if(this->raw_fitness(i) < boundaries.at(i)) {
                return false;
            }
        }
    }
    else {
        for(std::size_t i = 0; i < boundaries.size(); i++) {
            if(this->raw_fitness(i) > boundaries.at(i)) {
                return false;
            }
        }
    }

    return true;
}

/******************************************************************************/
/**
 * @brief Checks whether this candidate is a valid solution (meant for processed candidates).
 * @return true if the validity level is <= 1 (all constraints fulfilled)
 */
bool GGenome::isValid() const {
#ifdef DEBUG
    if(this->is_due_for_processing() || this->has_errors()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::isValid():" << '\n'
            << "Function was called for unprocessed or erroneous individual" << '\n'
        );
    }
#endif

    return validity_level_ <= 1.;
}

/******************************************************************************/
/**
 * @brief Randomly initializes the parameters (marking the item for reprocessing on change).
 * @param am The activity mode selecting which parameters are (re-)initialized
 * @return true if at least one parameter was changed
 */
bool GGenome::randomInit(activityMode const &am) {
    bool const modifications_made = this->randomInit_(am);
    if(modifications_made) {
        this->mark_as_due_for_processing();
    }
    return modifications_made;
}

/******************************************************************************/
/**
 * @brief Returns all raw fitness results in a std::vector.
 * @return A vector of all stored raw fitness results
 */
std::vector<double> GGenome::raw_fitness_vec_() const {
    return std::views::iota(std::size_t{0}, this->getNStoredResults())
         | std::views::transform([this](std::size_t const i) { return this->raw_fitness(i); })
         | std::ranges::to<std::vector<double>>();
}

/******************************************************************************/
/**
 * @brief Returns all transformed fitness results in a std::vector.
 * @return A vector of all stored transformed fitness results
 */
std::vector<double> GGenome::transformed_fitness_vec_() const {
    return std::views::iota(std::size_t{0}, this->getNStoredResults())
         | std::views::transform([this](std::size_t const i) { return this->transformed_fitness(i); })
         | std::ranges::to<std::vector<double>>();
}

/******************************************************************************/
/**
 * @brief Runs the registered pre-processor (if allowed) on this candidate.
 */
void GGenome::preProcess_() {
    if(this->mayBePreProcessed() && pre_processor_ptr_) {
        (*pre_processor_ptr_)(*this);
    }
}

/******************************************************************************/
/**
 * @brief Runs the registered post-processor (if allowed) on this candidate.
 */
void GGenome::postProcess_() {
    if(this->mayBePostProcessed() && post_processor_ptr_) {
        (*post_processor_ptr_)(*this);
    }
}

/******************************************************************************/
/**
 * @brief Sets every raw fitness to raw_value and every transformed fitness to transformed_value.
 * @param raw_value The raw value
 * @param transformed_value The transformed value
 */
void GGenome::setAllFitnessTo(const double raw_value, const double transformed_value) {
    for(std::size_t i = 0; i < this->getNStoredResults(); i++) {
        this->modifyStoredResult(i).reset(raw_value);
        this->modifyStoredResult(i).setTransformedFitnessTo(transformed_value);
    }
}

/******************************************************************************/
/**
 * @brief @return The sum of all stored transformed fitness values.
 */
double GGenome::sumCombiner() const {
    double result = 0.;
    for(std::size_t id = 0; id < this->getNStoredResults(); id++) {
        result += this->transformed_fitness(id);
    }
    return result;
}

/******************************************************************************/
/**
 * @brief @return The sum of the absolute values of all stored transformed fitness values.
 */
double GGenome::fabsSumCombiner() const {
    double result = 0.;
    for(std::size_t id = 0; id < this->getNStoredResults(); id++) {
        result += std::abs(this->transformed_fitness(id));
    }
    return result;
}

/******************************************************************************/
/**
 * @brief @return The square root of the sum of squares of all stored transformed fitness values.
 */
double GGenome::squaredSumCombiner() const {
    double result = 0.;
    for(std::size_t id = 0; id < this->getNStoredResults(); id++) {
        result += Gem::Common::gsquared(this->transformed_fitness(id));
    }
    return sqrt(result);
}

/******************************************************************************/
/**
 * @brief @param weights The per-criterion weights. @return The square root of the weighed sum of squares.
 */
double GGenome::weighedSquaredSumCombiner(std::vector<double> const &weights) const {
    if(this->getNStoredResults() != weights.size()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GGenome::weighedSquaredSumCombiner(): Error!" << '\n'
            << "Sizes of results and the weights vector don't match: " << this->getNStoredResults()
            << " / " << weights.size() << '\n'
        );
    }

    double result = 0.;
    for(auto const &[id, weight] : weights | std::views::enumerate) {
        result += Gem::Common::gsquared(weight * this->transformed_fitness(static_cast<std::size_t>(id)));
    }
    return sqrt(result);
}

/******************************************************************************/
/**
 * @brief Adds local configuration options (eval policy, sigmoid, max mode, adaption limits).
 * @param gpb The parser builder the configuration options are registered with
 */
void GGenome::addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) {
    // Call our CRTP base class's function (the category root has no parent above it).
    Gem::Common::GCommonInterfaceT<GGenome>::addConfigurationOptions_(gpb);

    gpb.registerFileParameter<evaluationPolicy>(
        "eval_policy",
        Gem::Geneva::evaluationPolicy::USESIMPLEEVALUATION,
        [this](const evaluationPolicy ep) { this->setEvaluationPolicy(ep); }
    ) << "Specifies which strategy should be used to calculate the evaluation:" << '\n'
      << "0 (a.k.a. USESIMPLEEVALUATION): Always call the evaluation function, even for invalid solutions"
      << '\n'
      << "1 (a.k.a. USEWORSTCASEFORINVALID) : Assign the worst possible value to our fitness and evaluate only valid solutions"
      << '\n'
      << "2 (a.k.a. USESIGMOID): Assign a multiple of validity_level_ and sigmoid barrier to invalid solutions, apply a sigmoid function to valid evaluations"
      << '\n';

    gpb.registerFileParameter<double>(
        "steepness",
        Gem::Geneva::FITNESSSIGMOIDSTEEPNESS,
        [this](const double ss) { this->setSteepness(ss); }
    ) << "When using a sigmoid function to transform the individual's fitness," << '\n'
      << "this parameter influences the steepness of the function at the center of the sigmoid." << '\n'
      << "The parameter must have a value > 0.";

    gpb.registerFileParameter<double>(
        "barrier",
        Gem::Geneva::WORSTALLOWEDVALIDFITNESS,
        [this](const double barrier) { this->setBarrier(barrier); }
    ) << "When using a sigmoid function to transform the individual's fitness," << '\n'
      << "this parameter sets the upper/lower boundary of the sigmoid." << '\n'
      << "The parameter must have a value > 0.;";

    gpb.registerFileParameter<maxMode>(
        "maxmode",
        maxMode::MINIMIZE,
        [this](const maxMode mm) { this->setMaxMode(mm); }
    ) << "Specifies whether the individual should be maximized (1) or minimized (0)" << '\n'
      << "Note that minimization is the by far most common option.";
}

/******************************************************************************/
/**
 * @brief Absorbs a returned item's results + lifecycle in place, keeping this element's genome + scratch.
 *
 * The pointer-preserving counterpart of a networked return: the server keeps the originally-submitted
 * population element (its address, its genome, its evolved OA scratch) and copies in only what the worker
 * computed -- the processing lifecycle (via the base), the evaluation-derived local state (validity level,
 * …) and the result store. Deliberately NOT copied: the genome value channels (absorbed separately, and
 * only for an `evaluated_and_modified` return) and the OA scratch. This mirrors the subset of load_()
 * that a return legitimately carries, minus the genome and scratch.
 */
void GGenome::absorbResultsFrom_(const Gem::Courtier::GProcessable &src) {
    // The non-generic processing lifecycle (status / errors / timing / routing / correlation), keeping our
    // own stable lineage id (detail::LineageId's copy-assignment keeps the target's value).
    Gem::Courtier::GProcessable::absorbResultsFrom_(src);

    const auto *p_load = dynamic_cast<const GGenome *>(&src);
    if(p_load == nullptr) {
        return; // a non-individual return carries nothing more we can absorb
    }

    // An `evaluation` return carries exactly the two evaluation OUTPUTS a worker computes: the result store
    // and the feasibility (validity) level. Everything else the server already holds correctly and keeps:
    // the genome and OA scratch (kept by not re-copying them), and the config veto flags / assigned
    // iteration (unchanged by processing, so identical on both sides). Expressed through the public
    // accessors, this absorb is a fixed, semantic set independent of the member layout -- so the folded
    // localMembers_() (which now carries the whole member set) does not affect it.
    this->setStoredResults(p_load->getStoredResults());
    this->setValidityLevel(p_load->getValidityLevel());
}

/******************************************************************************/
/**
 * @brief Replaces this element's whole content with a deep copy of @p src, in place (no relocation).
 *
 * The in-place equivalent of clone(): used by the clone-on-partial-return refill to substitute a viable
 * sibling into a failed slot without changing the slot's heap address (so a concurrent snapshot of
 * population addresses stays valid). Delegates to load_() -- a full deep copy of genome + results +
 * scratch -- and returns true to signal the consumer that no clone-and-replace fallback is needed. The
 * caller mints a fresh lineage id afterwards (a refill is a new individual).
 */
bool GGenome::loadContentFrom_(const Gem::Courtier::GProcessable &src) {
    const auto *p_load = dynamic_cast<const GGenome *>(&src);
    if(p_load == nullptr) {
        return false; // not an individual -> let the consumer fall back to clone-and-replace
    }
    this->load_(p_load); // full polymorphic deep copy in place (most-derived load_ runs)
    return true;
}

/******************************************************************************/
} /* namespace Gem::Geneva::Genome */
