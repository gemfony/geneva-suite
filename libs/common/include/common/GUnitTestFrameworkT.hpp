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

/**
 * This file holds function templates that should be specialized for each Geneva class
 * in order to facilitate unit tests.
 */

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <memory>

// Boost headers go here

// Geneva headers go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

/******************************************************************************/
/**
 * @brief This function creates a new T object. It can be specialized by the tested objects e.g. in case
 * they do not have a default constructor.
 *
 * @tparam T The type of the object to be created for testing
 * @return A std::shared_ptr to a newly created T object
 */
template <typename T>
std::shared_ptr<T> TFactory_GUnitTests() {
    std::shared_ptr<T> p;

    try {
        p = std::make_shared<T>();
    }
    catch(const geneva_exception &) {
        // Re-throw without slicing: bare `throw;` propagates the dynamic
        // type, whereas `throw g;` would copy-construct a geneva_exception
        // and discard any derived-class state.
        throw;
    }
    catch(...) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In std::shared_ptr<T> TFactory_GUnitTests(): Error!" << '\n'
            << "Caught unknown exception" << '\n'
        );
    }

    return p;
}

/******************************************************************************/
