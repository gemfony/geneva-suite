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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

// Boost headers go here

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "courtier/GProcessable.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/**
 * This class forms the basis of a hierarchy of classes designed for client-side
 * network communication. Their task is to retrieve serialized objects from the server
 * over a given protocol (implemented in derived classes), to instantiate the
 * corresponding object, to process it and to deliver the results to the server.
 * This class assumes that the template parameter implements the "process()" call.
 *
 * @tparam processable_type The work-item type the client retrieves, processes and returns (must derive GProcessable)
 *
 * TODO: Identify this client with a UUID
 */
template <typename processable_type>
class GBaseClientT {
    // The work item must be a processable (status / process() lifecycle).
    static_assert(
        std::is_base_of_v<Gem::Courtier::GProcessable, processable_type>,
        "GBaseClientT: processable_type must derive from Gem::Courtier::GProcessable"
    );

public:
    //---------------------------------------------------------------------------
    /**
	  * @brief A constructor that accepts a model of the item to be processed.
	  *
	  * This can be used to avoid having to transfer or reload data that doesn't change. Note that
	  * the model must understand the clone() command.
	  *
	  * @param additionalDataTemplate The model of the item to be processed (shared ownership; its constant data is loaded into each new item)
	  */
    GBaseClientT(std::shared_ptr<processable_type> additionalDataTemplate)
      : additional_data_template_(additionalDataTemplate) { /* nothing*/
    }

    //---------------------------------------------------------------------------
    // Defaulted or Deleted constructors and assignment operators

    GBaseClientT() = default;

    GBaseClientT(const GBaseClientT<processable_type> &) = delete;
    GBaseClientT(GBaseClientT<processable_type> &&) = delete;

    virtual ~GBaseClientT() = default;

    GBaseClientT<processable_type> &operator=(const GBaseClientT<processable_type> &) = delete;
    GBaseClientT<processable_type> &operator=(GBaseClientT<processable_type> &&) = delete;

    //---------------------------------------------------------------------------
    /**
	  * @brief Allows to set a maximum number of processing steps.
	  *
	  * If set to 0 or left unset, there is no processing-count limit (the client runs until another halt condition triggers).
	  *
	  * @param processMax Desired value for the process_max_ variable (maximum number of items to process; 0 == unlimited)
	  */
    void setProcessMax(std::uint32_t processMax) {
        process_max_ = processMax;
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Retrieves the value of the process_max_ variable.
	  *
	  * @return The maximum number of items to process (0 == unlimited)
	  */
    [[nodiscard]] std::uint32_t getProcessMax() const {
        return process_max_;
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Retrieves the number of items processed so far.
	  *
	  * @return The count of items processed since construction
	  */
    [[nodiscard]] std::uint32_t getNProcessed() const {
        return processed_;
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Sets the maximum allowed processing time.
	  *
	  * @param maxDuration The maximum allowed processing time (a duration in seconds; 0 disables the time limit)
	  */
    void setMaxTime(const std::chrono::duration<double> &maxDuration) {
        max_duration_ = maxDuration;
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Retrieves the value of the max_duration_ parameter.
	  *
	  * @return The maximum allowed processing time (a duration in seconds; 0 means no limit)
	  */
    std::chrono::duration<double> getMaxTime() {
        return max_duration_;
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Checks whether a terminal error was flagged.
	  *
	  * @return true if an unrecoverable error has been flagged
	  */
    [[nodiscard]] bool terminalErrorFlagged() const {
        return terminal_error_.load();
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Checks whether the close-flag was set.
	  *
	  * @return true if termination has been requested
	  */
    [[nodiscard]] bool closeRequested() const {
        return close_requested_.load();
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Runs the client: init -> main loop -> finally, wrapping each stage with exception handling.
	  */
    void run() {
        run_state r = run_state::INIT;

        //------------------------------------------------------------------------
        // init section
        try {
            r = run_state::INIT;

            if(not this->init()) { // Initialize the client
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GBaseClientT<T>::run(): Initialization failed. Leaving ..." << '\n'
                );
            }

            r = run_state::RUN;
            run_(); // The main loop

            r = run_state::FINALLY;
            if(not this->finally()) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GBaseClientT<T>::run(): Finalization failed." << '\n'
                );
            }
        }
        catch(geneva_exception &e) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GBaseClientT<T>::run() / " << rs_to_str(r) << ":" << '\n'
                << "Caught geneva_exception" << '\n'
                << "with message" << '\n'
                << e.what()
            );
        }
        catch(std::exception &e) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GBaseClientT<T>::run() / " << rs_to_str(r)
                << ": Caught std::exception with message" << '\n'
                << e.what() << '\n'
            );
        }
        catch(...) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GBaseClientT<T>::run() / " << rs_to_str(r) << ": Caught unknown exception."
                << '\n'
            );
        }

        //------------------------------------------------------------------------
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Sets a flag indicating that the application should terminate.
	  */
    void flagCloseRequested() {
        close_requested_.store(true);
    }

protected:
    //---------------------------------------------------------------------------
    /**
	  * @brief Increments the processing counter.
	  *
	  * We do not use atomics, as only one processing step is supposed to run at the same time.
	  */
    void incrementProcessingCounter() {
        processed_++;
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Flags an error that qualifies as a halt condition.
	  */
    void flagTerminalError() {
        terminal_error_.store(true);
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Loads the additional data template into the processable_type target.
	  *
	  * This function needs to be called for each new item by derived classes. A no-op if no template
	  * was supplied at construction.
	  *
	  * @param target The item (shared ownership) into which the template's constant data is loaded
	  */
    void loadDataTemplate(std::shared_ptr<processable_type> target) {
        // If we have a model for the item to be parallelized, load its data into the target
        if(additional_data_template_) {
            target->loadConstantData(additional_data_template_);
        }
    }

    //---------------------------------------------------------------------------
    /**
	  * Checks whether a halt condition was reached.
	  *
	  * @return A boolean indicating whether a halt condition was reached
	  */
    bool halt() {
        // The transports poll this once per second (halt timer), so the reason is logged only
        // ONCE -- otherwise every poll after the condition triggers would repeat the line.
        auto haltOnce = [this](const char *reason) {
            if(not halt_logged_) {
                halt_logged_ = true;
                glogger << "Client is terminating because " << reason << '\n' << GLOGGING;
            }
            return true;
        };

        // Has a terminal error been flagged?
        if(terminalErrorFlagged()) {
            return haltOnce("an unrecoverable error was flagged");
        }

        // Has the application been asked to shut down?
        if(closeRequested()) {
            return haltOnce("the application was asked to shut down");
        }

        // Maximum number of processing steps reached ?
        if(process_max_ > 0 && (processed_ >= process_max_)) {
            return haltOnce("the maximum number of processing steps was exceeded");
        }

        // Maximum duration reached ?
        if(max_duration_.count() > 0. &&
           ((std::chrono::high_resolution_clock::now() - start_time_) >= max_duration_)) {
            return haltOnce("the maximum time frame was exceeded");
        }

        // Custom halt condition reached ?
        if(customHalt()) {
            return haltOnce("a custom halt condition has triggered");
        }

        return false;
    }

private:
    //---------------------------------------------------------------------------
    /** @brief Performs initialization work.
     *  @return true on successful initialization; false aborts the run */
    virtual bool init() {
        return true;
    }

    //---------------------------------------------------------------------------
    /** @brief This is the main loop of the client, after initialization. */
    virtual void run_() = 0;

    //---------------------------------------------------------------------------
    /** @brief Perform necessary finalization activities.
     *  @return true on successful finalization; false signals a finalization failure */
    virtual bool finally() {
        return true;
    }

    //---------------------------------------------------------------------------
    /** @brief Custom halt condition for processing.
     *  @return true to request termination (default: never) */
    virtual bool customHalt() {
        return false;
    }

    //---------------------------------------------------------------------------
    /** @brief Transformation of run_state to a string.
     *  @param r The run state to convert
     *  @return A human-readable name for the run state */
    std::string rs_to_str(run_state r) {
        switch(r) {
        case run_state::INIT:
            return "run_state::INIT";
        case run_state::RUN:
            return "run_state::RUN";
        case run_state::FINALLY:
            return "run_state::FINALLY";
        }

        // Make the compiler happy
        return {};
    }

    //---------------------------------------------------------------------------
    // Data

    std::chrono::high_resolution_clock::time_point start_time_ = std::chrono::
        high_resolution_clock::now(); ///< Used to store the start time of the optimization
    std::chrono::duration<double> max_duration_ =
        std::chrono::microseconds(0); ///< Maximum time frame for the optimization

    std::uint32_t processed_ = 0;  ///< The number of processed items so far
    std::uint32_t process_max_ = 0; ///< The maximum number of items to process

    std::atomic<bool> terminal_error_{false}; ///< Indicates whether a terminal error was received
    std::atomic<bool> close_requested_{
        false
    }; ///< Indicates whether a the termination was requested by the server
    bool halt_logged_ = false; ///< Ensures the halt reason is logged only once (halt() is polled)

    std::shared_ptr<processable_type>
        additional_data_template_; ///< Optionally holds a template of the object to be processed

    //---------------------------------------------------------------------------
};

/******************************************************************************/

} /* namespace Gem::Courtier */
