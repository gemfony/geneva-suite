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
#include <utility>

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
 * This class assumes that the template parameter implements the "process()" call. A processor
 * set with setItemProcessor() takes the place of that call for every item that is due.
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
	  * A client that evaluates several items at a time starts no evaluation once the limit is reached,
	  * but the evaluations that run then are finished and counted: the count may pass the limit by
	  * them.
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
                throw gem_exception(
                    g_error_streamer() << "Initialization failed. Leaving ..." << '\n'
                );
            }

            r = run_state::RUN;
            run_(); // The main loop

            r = run_state::FINALLY;
            if(not this->finally()) {
                throw gem_exception(g_error_streamer() << "Finalization failed." << '\n');
            }
        }
        catch(gem_exception &e) {
            throw gem_exception(
                g_error_streamer()
                << "In the state run_state::" << Gem::Common::enumName(r) << ":" << '\n'
                << "Caught gem_exception" << '\n'
                << "with message" << '\n'
                << e.what()
            );
        }
        catch(std::exception &e) {
            throw gem_exception(
                g_error_streamer() << "In the state run_state::" << Gem::Common::enumName(r)
                                   << ": Caught std::exception with message" << '\n'
                                   << e.what() << '\n'
            );
        }
        catch(...) {
            throw gem_exception(
                g_error_streamer() << "In the state run_state::" << Gem::Common::enumName(r)
                                   << ": Caught unknown exception." << '\n'
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

    //---------------------------------------------------------------------------
    /**
	  * @brief Replaces how this client processes a work item that is due.
	  *
	  * Without a processor (the default, and after passing an empty one) the client calls the
	  * item's process(). With one, it calls the processor for every item that is due
	  * (processingStatus::DO_PROCESS); an item that is not due and a frame without an item
	  * never reach it. The processor must leave the item processed or flagged, as process()
	  * does. If it throws, the item is returned flagged and the client keeps running.
	  *
	  * The processor must be safe to call from the thread(s) the client processes on: one for
	  * the MPI client, as many as its parallel evaluations for a socket client (1 by default).
	  * Its call operator is const (GItemProcessorT), so several threads may call the one object
	  * at the same time;
	  * a processor with unguarded mutable state does not compile. To be set before run(); it
	  * is not to be changed while the client runs.
	  *
	  * @param processor The function that processes a due item (empty: the item's process())
	  */
    void setItemProcessor(GItemProcessorT<processable_type> processor) {
        item_processor_ = std::move(processor);
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
	  * @brief The processor set with setItemProcessor(), for processWorkFrame().
	  *
	  * @return A pointer to the processor, or nullptr if none is set (the item's process())
	  */
    [[nodiscard]] GItemProcessorT<processable_type> *itemProcessor() noexcept {
        return item_processor_ ? &item_processor_ : nullptr;
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
    // Data

    std::chrono::high_resolution_clock::time_point start_time_ = std::chrono::
        high_resolution_clock::now(); ///< Used to store the start time of the client's work
    std::chrono::duration<double> max_duration_ =
        std::chrono::microseconds(0); ///< Maximum time frame for the client's work

    std::uint32_t processed_ = 0;   ///< The number of processed items so far
    std::uint32_t process_max_ = 0; ///< The maximum number of items to process

    std::atomic<bool> terminal_error_{false}; ///< Indicates whether a terminal error was received
    std::atomic<bool> close_requested_{
        false
    }; ///< Indicates whether a the termination was requested by the server
    bool halt_logged_ = false; ///< Ensures the halt reason is logged only once (halt() is polled)

    /// Processes a due item in place of its process(); empty: the item's process()
    GItemProcessorT<processable_type> item_processor_;

    //---------------------------------------------------------------------------
};

/******************************************************************************/

} /* namespace Gem::Courtier */
