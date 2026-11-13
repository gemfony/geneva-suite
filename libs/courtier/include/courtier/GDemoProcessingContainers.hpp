/**
 * @file GDemoProcessingContainers.hpp
 */

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
#include <cstdint>
#include <iostream>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GArchiveNamed.hpp" // archive_named / archive_named_base (named member / base emitters)
#include "courtier/GProcessingContainerT.hpp"
#include "hap/GRandomT.hpp"

namespace Gem::Courtier {

/**********************************************************************************************/
/**
 * This class implements the simplest-possible procesiing container object, used for tests of
 * the courtier lib.
 */
class GSimpleContainer
  : public Gem::Courtier::GProcessingContainerT<GSimpleContainer, bool> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<Gem::Courtier::GProcessingContainerT<GSimpleContainer, bool>>(
            ar, "GProcessingContainerT_GSimpleContainer", *this);
        Gem::Common::archive_named(ar, "stored_number_", stored_number_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The standard constructor -- Initialization with a stored pay-load number
     *  @param snr The number to store as this object's pay-load */
    explicit GSimpleContainer(const std::size_t &snr);
    /** @brief The copy constructor */
    GSimpleContainer(const GSimpleContainer &) = default;
    /** @brief The copy assignment operator */
    GSimpleContainer &operator=(const GSimpleContainer &) = default;
    /** @brief The move constructor */
    GSimpleContainer(GSimpleContainer &&) noexcept = default;
    /** @brief The move assignment operator */
    GSimpleContainer &operator=(GSimpleContainer &&) noexcept = default;
    /** @brief The destructor */
    ~GSimpleContainer() override = default;

    /** @brief Prints out this object's stored pay-load number */
    void print() const;

private:
    /** @brief The default constructor -- only needed for de-serialization purposes */
    GSimpleContainer() = default;
    /** @brief Allows to specify the tasks to be performed for this object (a no-op for this demo container)
     *  @param res_vec An optional externally injected evaluation result vector (unused here) */
    void process_(const std::vector<bool> &res_vec = std::vector<bool>()) final;

    std::size_t stored_number_ = 0; ///< Holds the pay-load of this object
};

/**********************************************************************************************/
/**
 * This class implements a container of random objects, used for tests of the courtier lib.
 */
class GRandomNumberContainer
  : public Gem::Courtier::GProcessingContainerT<GRandomNumberContainer, bool> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<Gem::Courtier::GProcessingContainerT<GRandomNumberContainer, bool>>(
            ar, "GProcessingContainerT_GRandomNumberContainer", *this);
        Gem::Common::archive_named(ar, "random_numbers_", random_numbers_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The standard constructor -- Initialization with an amount of random numbers
     *  @param nrnr The desired count of random numbers to generate and store as the pay-load */
    explicit GRandomNumberContainer(const std::size_t &nrnr);

    /******************************************************************************************/
    // Defaulted constructors, destructor and assignment operators

    // Default constructor is in the private section (only needed for de-serialization)

    GRandomNumberContainer(const GRandomNumberContainer &) = default;
    ~GRandomNumberContainer() override = default;

    GRandomNumberContainer &operator=(GRandomNumberContainer const &) = default;

    GRandomNumberContainer(GRandomNumberContainer &&) noexcept = default;
    GRandomNumberContainer &operator=(GRandomNumberContainer &&) noexcept = default;

    /******************************************************************************************/

    /** @brief Prints out this object's random number container (index and value per line) */
    void print() const;

private:
    /** @brief The default constructor -- only needed for de-serialization purposes */
    GRandomNumberContainer() = default;
    /** @brief Performs this object's processing task: sorts the stored random numbers in place
     *  @param res_vec An optional externally injected evaluation result vector (unused here) */
    void process_(const std::vector<bool> &res_vec = std::vector<bool>()) final;

    std::vector<double> random_numbers_; ///< Holds the pay-load of this object
};

/**********************************************************************************************/
/**
 * Fault modes the GFaultyContainer can be asked to exhibit during process_(). The mode is
 * stored in the item and survives serialization, so it triggers identically whether the item
 * is processed locally (GStdThread/GSerial consumer) or on a remote client over the wire.
 */
enum class fault_mode : std::uint8_t {
    NONE = 0,             ///< Normal: register a result, no fault
    SLEEP = 1,            ///< Sleep for sleep_ms_ then register a result (slow worker)
    FLAG_ERROR = 2,       ///< Call force_set_error() (clean error flag, no throw)
    THROW_PROCESSING = 3, ///< Throw g_processing_exception (a std::exception → process()'s catch(std::exception&); item flagged)
    THROW_FATAL = 4       ///< Throw a non-std::exception (exercises process()'s catch(...) fallback; item flagged)
};

/**********************************************************************************************/
/**
 * A processing container that can be instructed to misbehave during process_(), for stress-
 * and fault-injection tests of the courtier submission path. Like the other demo containers it
 * has no dependency on the geneva optimization library. The fault mode is serialized, so a
 * remote client will exhibit it after deserialization.
 */
class GFaultyContainer
  : public Gem::Courtier::GProcessingContainerT<GFaultyContainer, bool> {
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Weft::access;

    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        using Gem::Common::archive_named;
        Gem::Common::archive_named_base<Gem::Courtier::GProcessingContainerT<GFaultyContainer, bool>>(
            ar, "GProcessingContainerT_GFaultyContainer", *this);
        archive_named(ar, "stored_number_", stored_number_);
        archive_named(ar, "fault_mode_", fault_mode_);
        archive_named(ar, "sleep_ms_", sleep_ms_);
    }
    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief Standard constructor -- an id, the fault to exhibit, and an optional sleep length
     *  @param stored_number An id used by tests to check item conservation
     *  @param fm The fault mode to exhibit during process_() (defaults to NONE = normal behaviour)
     *  @param sleep_ms The sleep length in milliseconds used by fault_mode::SLEEP (defaults to 0) */
    explicit GFaultyContainer(
        std::size_t stored_number,
        fault_mode fm = fault_mode::NONE,
        unsigned int sleep_ms = 0
    );
    GFaultyContainer(const GFaultyContainer &) = default;
    GFaultyContainer &operator=(const GFaultyContainer &) = default;
    GFaultyContainer(GFaultyContainer &&) noexcept = default;
    GFaultyContainer &operator=(GFaultyContainer &&) noexcept = default;
    ~GFaultyContainer() override = default;

    /** @brief Retrieves the configured fault mode
     *  @return The fault mode this container will exhibit during process_() */
    [[nodiscard]] fault_mode get_fault_mode() const;
    /** @brief Retrieves the stored id/number
     *  @return The id/number stored in this container */
    [[nodiscard]] std::size_t get_stored_number() const;

private:
    /** @brief The default constructor -- only needed for de-serialization purposes */
    GFaultyContainer() = default;
    /** @brief Performs the configured (mis-)behaviour
     *  @param res_vec An optional externally injected evaluation result vector (unused here) */
    void process_(const std::vector<bool> &res_vec = std::vector<bool>()) final;

    std::size_t stored_number_ = 0;              ///< Identifies the item (for conservation checks)
    fault_mode fault_mode_ = fault_mode::NONE;   ///< The fault to exhibit during process_()
    unsigned int sleep_ms_ = 0;                  ///< Sleep length for fault_mode::SLEEP
};

/**********************************************************************************************/

} /* namespace Gem::Courtier */

