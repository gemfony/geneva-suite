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
#include <chrono>
#include <concepts>
#include <exception>
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

// Boost headers go here
// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named / archive_named_base (named member / base emitters)
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GMemberReflectionT.hpp" // make_member / g_load_members / serialize_members
#include "common/GSerializableFunctionObjectT.hpp"
#include "common/GSerializationHelperFunctionsT.hpp" // serialization of std::chrono time_point members
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GCourtierHelperFunctions.hpp"
#include "courtier/GProcessable.hpp" // the non-generic processing-lifecycle base

namespace Gem::Courtier {

/******************************************************************************/
// The processing exception (g_processing_exception) lives on the GProcessable lifecycle base, included
// above, so transports and processables share one type.

/******************************************************************************/
/**
	 * @brief The generic, self-contained base for a consumer-submittable work item: GProcessable plus a
	 * typed result store and the process() orchestration.
	 *
	 * GProcessable (the base) carries only the non-generic processing LIFECYCLE (status / routing / timing)
	 * and has no process() and no result store, so it is not by itself a submittable work item. This class
	 * adds the missing half: a typed result store (one or more processing_result_type values), the
	 * process() orchestration (timing, the configured pre-/post-processors, error/exception handling around
	 * the user's process_() hook) and the return reconciliation -- i.e. the full contract the courtier
	 * consumers require of an item they evaluate. Derive this (CRTP-style, passing yourself as
	 * processable_type) to make your OWN type runnable on any courtier consumer; re-implement the pure
	 * virtual process_() and make the derived class serializable so it travels on the networked transports.
	 *
	 * This base is deliberately free of any dependency on the geneva optimization library, which keeps the
	 * courtier consumer/transport machinery testable on its own (courtier sits BELOW geneva in the library
	 * order): the work items of courtier's test support (libs/courtier/tests/support:
	 * GDemoProcessingContainers) derive this and are what courtier's tests submit. Geneva's own work
	 * item, Gem::Geneva::Genome::GGenome, is a SPECIALIZED, standalone second implementation of the
	 * same contract -- it derives GProcessable directly and provides its own result store + process()
	 * (folding in optimization-specific orchestration: feasibility, the evaluation policy,
	 * multi-criterion fitness), so it does NOT derive this class.
	 *
	 * @tparam processable_type The concrete type deriving GProcessingContainerT (CRTP)
	 * @tparam processing_result_type The result type of the process_ call; should be copyable
	 */
template <typename processable_type, typename processing_result_type>
    requires (!std::is_void_v<processing_result_type>)
class GProcessingContainerT : public GProcessable {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    /**
     * @brief The single declaration of this class's members, from which serialize() and the copy
     * operations are derived (the pre-/post-processors are deep-cloned on copy).
     * @return A tuple of named member references
     */
    template <typename self_type>
    auto localMembers_(this self_type &self) {
        return std::make_tuple(
            Gem::Common::make_member("pre_processing_disabled_", self.pre_processing_disabled_),
            Gem::Common::make_member("post_processing_disabled_", self.post_processing_disabled_),
            Gem::Common::make_cloneable_member("pre_processor_ptr_", self.pre_processor_ptr_),
            Gem::Common::make_cloneable_member("post_processor_ptr_", self.post_processor_ptr_),
            Gem::Common::make_member("stored_results_cnt_", self.stored_results_cnt_)
        );
    }

    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        // The non-generic lifecycle state (status, errors, routing ids, timing) is serialised by
        // the GProcessable base; this class adds only the result store and the (typed) pre-/post-
        // processors plus their veto flags.
        Gem::Common::archive_named_base<GProcessable>(ar, "GProcessable", *this);
        Gem::Common::serialize_members(ar, this->localMembers_());
    }

    ///////////////////////////////////////////////////////////////////////

public:
    using payload_type = processable_type;
    using result_type = processing_result_type;

    /***************************************************************************/
    /**
	  * @brief Initialization with the number of stored results
	  *
	  * @param n_stored_results The number of result slots to allocate (each default-initialized)
	  */
    explicit GProcessingContainerT(std::size_t n_stored_results)
      : stored_results_cnt_(n_stored_results, processing_result_type()) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * @brief The copy constructor
	  *
	  * @param cp Another GProcessingContainerT object to be copied
	  */
    explicit GProcessingContainerT(
        GProcessingContainerT<processable_type, processing_result_type> const &cp
    )
      : GProcessable(
            cp
        ) { // copies the non-generic lifecycle state (status, errors, counters, timing)
        Gem::Common::g_load_members(this->localMembers_(), cp.localMembers_());
    }

    /***************************************************************************/
    /**
	  * @brief Copy assignment operator. The class is abstract (process_() is pure), so it cannot
	  * copy-construct a temporary; it assigns the base and copies the member list, as the copy
	  * constructor does.
	  *
	  * @param cp Another GProcessingContainerT object whose data is copied into this one
	  * @return A reference to this object
	  */
    GProcessingContainerT<processable_type, processing_result_type> &
    operator=(GProcessingContainerT<processable_type, processing_result_type> const &cp) {
        if(this != &cp) {
            GProcessable::operator=(cp); // the non-generic lifecycle state
            Gem::Common::g_load_members(this->localMembers_(), cp.localMembers_());
        }
        return *this;
    }

    /***************************************************************************/
    // Defaulted or deleted constructors, destructor and move assignment operator

    // Default constructor may be found in private section

    // Move operations: a defaulted member-wise move is correct and noexcept
    // here. The heavy members (stored_results_cnt_, stored_error_descriptions_,
    // pre_/post_processor_ptr_) have their ownership transferred rather than
    // being deep-copied as the copy operations do -- this is the point of being
    // movable on the work-transport (thread-pool / MPI / websocket) path. The
    // remaining scalar/enum/time-point members are moved trivially.
    GProcessingContainerT(
        GProcessingContainerT<processable_type, processing_result_type> &&
    ) noexcept = default;
    GProcessingContainerT<processable_type, processing_result_type> &operator=(
        GProcessingContainerT<processable_type, processing_result_type> &&
    ) noexcept = default;

    virtual ~GProcessingContainerT() = default;

    /***************************************************************************/
    /**
	  * @brief Sets the vector of stored results to a given collection and marks
	  * the object as processed
	  *
	  * @param result_cnt The new result vector (must match the configured number of stored results)
	  * @return The first stored result after the assignment
	  */
    processing_result_type
    markAsProcessedWith(std::vector<processing_result_type> const &result_cnt) {
#ifdef DEBUG
        // Check that we have been given a suitable new results vector
        if(result_cnt.size() != stored_results_cnt_.size()) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GProcessingContainerT::markAsProcessedWith(): Vector dimensions" << '\n'
                << "do not fit: " << result_cnt.size() << " / " << stored_results_cnt_.size()
                << '\n'
            );
        }
#endif

        // Transfer the new values
        stored_results_cnt_ = result_cnt;

        // Clear the error descriptions
        stored_error_descriptions_.clear();

        // Mark as processed; a begun processing is over
        this->assignStatus_(processingStatus::PROCESSED);

        // This part of the code should never be reached if an exception was thrown
        return this->stored_results_cnt_.at(0);
    }

    /***************************************************************************/
    /**
	  * Perform the actual processing steps. Post-processing may refine the
	  * processed item further (Geneva runs a short sub-optimization there). The amount of time
	  * needed for processing is measured for logging purposes. Where one of the
	  * processing functions throws an exception, the function will store the
	  * necessary exception information locally and rethrow the exception.
	  * Note that user-defined processing- and post-processing functions need
	  * to make sure to set the results (be it main- or secondary results) of the
	  * process()-call. This function has no way to ensure that this is the case.
	  *
	  * @param res_vec Allows to inject an external evaluation, handed to process_(). Results
	  *        computed after a beginProcessing() are refused here: they belong to
	  *        completeProcessing().
	  * @return The first result of the processing calls
	  */
    processing_result_type process(
        const std::vector<processing_result_type> &res_vec = std::vector<processing_result_type>()
    ) {
        // The full lifecycle (precondition, reset, timed pre -> core -> guarded post, exception
        // funneling, error epilogue) is stated ONCE, on GProcessable::runProcessingLifecycle_.
        this->runProcessingLifecycle_(
            "GProcessingContainerT<processable_type>::process()",
            not res_vec.empty(),
            [this, &res_vec] { this->process_(res_vec); },
            [this] { this->postProcess_(); }
        );

        // This part of the code is only reached on success (the lifecycle throws on any error)
        return this->stored_results_cnt_.at(0);
    }

    /***************************************************************************/
    /**
	  * @brief Completes the processing begun with GProcessable::beginProcessing(), with the
	  * results computed for this item since (e.g. by a device, for a whole batch at once): the
	  * part of process(res_vec) after the pre-processor -- process_(res_vec), PROCESSED, the
	  * post-processor. The processing time is @p evaluation_time plus the time of process_().
	  *
	  * @param res_vec The results computed for this item, handed to process_(); must not be empty
	  * @param evaluation_time The time in seconds it took to compute @p res_vec (on a batch path
	  *        the evaluation of the whole batch)
	  * @return The first result after processing
	  * @throws gem_exception if @p res_vec is empty or the processing was not begun (the item is
	  *         left untouched)
	  * @throws g_processing_exception as process() does
	  */
    processing_result_type completeProcessing(
        const std::vector<processing_result_type> &res_vec,
        double evaluation_time = 0.
    ) {
        static constexpr std::string_view context =
            "GProcessingContainerT<processable_type>::completeProcessing()";
        GProcessable::requireResults_(context, not res_vec.empty());
        this->completeProcessingLifecycle_(
            context,
            evaluation_time,
            [this, &res_vec] { this->process_(res_vec); },
            [this] { this->postProcess_(); }
        );

        // This part of the code is only reached on success (the lifecycle throws on any error)
        return this->stored_results_cnt_.at(0);
    }

    /***************************************************************************/
    /**
	  * Retrieval of the stored result. This function does not allow modifications
	  * of its return value. It will throw, if value retrieval is initiated for a work
	  * item which does not have the PROCESSED flag set.
	  *
	  * @param id The id of the stored result to be returned
	  * @return The stored result at position id in stored_results_vec_
	  */
    [[nodiscard]] processing_result_type getStoredResult(std::size_t id = 0) const {
        if(not this->is_processed()) {
            throw gem_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GProcessingContainerT::getStoredResult(): Tried to" << '\n'
                << "retrieve stored result while the PROCESSED flag was not set" << '\n'
            );
        }

        return stored_results_cnt_.at(id);
    }

    /******************************************************************************/
    /**
	  * Retrieve the id assigned to the current evaluation. Note that there is no
	  * guaranty that the item has indeed been processed. This is id simply represents
	  * the processing id assigned at the beginning of the last process()-call.
	  */
    /*
		std::string getCurrentEvaluationID() const {
			return evaluation_id_;
		}*/

    /***************************************************************************/
    /**
	  * @brief Allows to check whether any user-defined pre-processing before the process()-
	  * step may occur. This may alter the item's data.
	  *
	  * @return true if pre-processing is currently allowed, false if it has been vetoed
	  */
    [[nodiscard]] bool mayBePreProcessed() const noexcept {
        return not pre_processing_disabled_;
    }

    /***************************************************************************/
    /**
	  * @brief Allow or prevent pre-processing (used by a pre-processor that processes the
	  * item again itself, to prevent recursive pre-processing). Once a veto
	  * exists, no pre-processing will occur until the veto is lifted.
	  *
	  * @param veto true to disable (veto) pre-processing, false to allow it
	  */
    void vetoPreProcessing(bool veto) noexcept {
        pre_processing_disabled_ = veto;
    }

    /***************************************************************************/
    /**
	  * @brief Allows to register a pre-processor object
	  *
	  * @param pre_processor_ptr The pre-processor function object to register (ignored if empty)
	  */
    void registerPreProcessor(
        std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<processable_type>>
            pre_processor_ptr
    ) {
        if(pre_processor_ptr) {
            pre_processor_ptr_ = pre_processor_ptr;
        }
    }

    /***************************************************************************/
    /**
	  * @brief Allows to check whether any user-defined post-processing after the process()-
	  * step may occur. This matters, e.g., when a post-processor submits work of its own to the
	  * process consumer (Geneva's post-optimizers run a sub-optimization on the item). This may
	  * alter the item's data.
	  *
	  * @return true if post-processing is currently allowed, false if it has been vetoed
	  */
    [[nodiscard]] bool mayBePostProcessed() const {
        return not post_processing_disabled_;
    }

    /***************************************************************************/
    /**
	  * @brief Allow or prevent post-processing (used by a post-processor that processes the
	  * item again itself, such as Geneva's post-optimizers, to prevent recursive
	  * post-processing). Once a veto
	  * exists, no post-processing will occur until the veto is lifted.
	  *
	  * @param veto true to disable (veto) post-processing, false to allow it
	  */
    void vetoPostProcessing(bool veto) {
        post_processing_disabled_ = veto;
    }

    /***************************************************************************/
    /**
	  * @brief Allows to register a post-processor object
	  *
	  * @param post_processor_ptr The post-processor function object to register (ignored if empty)
	  */
    void registerPostProcessor(
        const std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<processable_type>>
            &post_processor_ptr
    ) {
        if(post_processor_ptr) {
            post_processor_ptr_ = post_processor_ptr;
        }
    }

    /***************************************************************************/
    /**
	  * @brief Retrieves the registered post-processor (or an empty pointer). A submitter uses this to
	  * decide whether to veto post-processing on this work item (Geneva's algorithms compare the
	  * post-processor's allowed mnemonics with their own), so the work item needs no knowledge of the
	  * submitter.
	  *
	  * @return The registered post-processor, or an empty pointer if none is registered
	  */
    [[nodiscard]] std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<processable_type>>
    postProcessor() const {
        return post_processor_ptr_;
    }

    /***************************************************************************/
    /**
	  * @brief Removes any registered post-processor. Used by a post-processor on the copy it processes
	  * further, so that the items it submits for that carry no post-processor and cannot recurse into
	  * further post-processing.
	  */
    void clearPostProcessor() {
        post_processor_ptr_.reset();
    }

    /***************************************************************************/
    /**
	  * @brief Allows to retrieve the number of stored results
	  *
	  * @return The number of result slots held by this object
	  */
    [[nodiscard]] std::size_t getNStoredResults() const {
        return stored_results_cnt_.size();
    }

protected:
    /***************************************************************************/
    /**
	  * @brief Retrieval of the stored result. This function allows modifications of its
	  * return value and is hence protected and only accessible by derived classes.
	  *
	  * @param id The id (position) of the stored result to be returned
	  * @return A modifiable reference to the stored result at position id
	  */
    processing_result_type &modifyStoredResult(std::size_t id = 0) {
        return stored_results_cnt_.at(id);
    }

    /***************************************************************************/
    /**
	  * @brief Allows derived classes to set the number of stored results. Note that this
	  * should happen prior to any operation with this object. Also note that this
	  * operation may invalidate other results already stored in this object.
	  *
	  * @param n_stored_results The number of stored results in this class
	  * @param new_val A value to be copied into new positions when the vector is increased
	  */
    void setNStoredResults(std::size_t n_stored_results, processing_result_type new_val) {
        stored_results_cnt_.resize(n_stored_results, new_val);
    }

    /***************************************************************************/
    /**
	  * @brief Allows derived classes to set the number of stored results, default-initializing
	  * new positions. Note that this should happen prior to any operation with this object. Also
	  * note that this operation may invalidate other results already stored in this object.
	  *
	  * @param n_stored_results The number of stored results in this class
	  */
    void setNStoredResults(std::size_t n_stored_results) {
        processing_result_type p;
        this->setNStoredResults(n_stored_results, p);
    }

    /***************************************************************************/
    /**
	  * @brief Allows to register a result, using its id (i.e. position in the internal
	  * result storage). This function should be called from inside of the process_ call.
	  *
	  * @param id The id (position) at which to store the result
	  * @param r The result value to store at position id
	  */
    void registerResult(std::size_t id, const processing_result_type &r) {
        stored_results_cnt_.at(id) = r;
    }

    /***************************************************************************/
    /**
     * @brief The default constructor. It is only needed for (de-)serialization purposes.
     * We want to enforce the specification of the number of evaluation criteria
     * in derived classes. Protected, so that a derived class can have a defaulted
     * default constructor.
     */
    GProcessingContainerT() = default;

private:
    /***************************************************************************/
    /**
	  * @brief Little helper function to (re-)initialize the result storage vector
	  */
    void clear_stored_results_vec() {
        std::ranges::fill(stored_results_cnt_, processing_result_type());
    }

    /***************************************************************************/
    /** @brief Bridges the GProcessable status machine to this class's result store: when a status reset
     *  clears stored results, GProcessable::set_processing_status() calls this hook. */
    void clearStoredResults_() override {
        this->clear_stored_results_vec();
    }

    /***************************************************************************/
    /** @brief In-place return reconciliation (see GProcessable::absorbResultsFrom): copies the processing
     *  lifecycle (via the base) plus this container's result store from @p src, keeping this item's own
     *  lineage id. The container keeps no state that stays off the wire, so nothing else is retained.
     *  @param src The returned, evaluated item whose results + lifecycle are absorbed into this one */
    void absorbResultsFrom_(const GProcessable &src) override {
        GProcessable::absorbResultsFrom_(
            src
        ); // status / errors / timing / routing, keeping our lineage id
        if(const auto *p = dynamic_cast<const GProcessingContainerT *>(&src); p != nullptr) {
            stored_results_cnt_ = p->stored_results_cnt_;
        }
    }

    /***************************************************************************/

    /** @brief Allows derived classes to specify the tasks to be performed for this object
     *  @param res_vec An optional externally injected evaluation result vector available to the implementation */
    virtual void process_(
        const std::vector<processing_result_type> &res_vec = std::vector<processing_result_type>()
    ) = 0;

    /***************************************************************************/
    /**
		  * @brief Runs the registered pre-processor, if pre-processing is both permitted for this
		  * object and configured. It resets nothing -- the permission flags are the caller's.
		  * (The GProcessable hook of the lifecycle's pre-processing step. It is final: a member of
		  * that name in a derived class would otherwise replace the registered pre-processor
		  * without a word.)
  		  */
    void preProcess_() final {
        if(this->mayBePreProcessed() && pre_processor_ptr_) {
            auto &p = dynamic_cast<processable_type &>(*this);
            (*pre_processor_ptr_)(p);
        }
    }

    /***************************************************************************/
    /**
	  * @brief Runs the registered post-processor, if post-processing is both permitted for this
	  * object and configured. It resets nothing -- the permission flags are the caller's.
  	  */
    void postProcess_() {
        if(this->mayBePostProcessed() && post_processor_ptr_) {
            auto &p = dynamic_cast<processable_type &>(*this);
            (*post_processor_ptr_)(p);
        }
    }

    /***************************************************************************/
    // Data -- the result store and the (typed) pre-/post-processors. The non-generic lifecycle state
    // (status, errors, routing ids, dispatch-scheduling, timing) lives on the GProcessable base.

    bool pre_processing_disabled_ =
        false; ///< Indicates whether pre-processing was disabled entirely
    bool post_processing_disabled_ =
        false; ///< Indicates whether post-processing was disabled entirely

    std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<processable_type>>
        pre_processor_ptr_; ///< Actions to be performed before processing
    std::shared_ptr<Gem::Common::GSerializableFunctionObjectT<processable_type>>
        post_processor_ptr_; ///< Actions to be performed after processing

    std::vector<processing_result_type> stored_results_cnt_ = std::vector<processing_result_type>(
        1,
        processing_result_type()
    ); ///< The results stored by this object

    // std::string evaluation_id_ = "empty"; ///< A unique id that is assigned to an evaluation
};

/******************************************************************************/

} /* namespace Gem::Courtier */

/******************************************************************************/
/** @brief Mark this class as abstract */

/******************************************************************************/
