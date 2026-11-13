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
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Boost headers go here


// Geneva headers go here
#include "weft/GArchivePolymorphic.hpp" // GArchive codecs (smart-ptr arm) for sharedPtr[To|From]String
#include "weft/GBinaryArchive.hpp"       // GArchive flat binary codec
#include "weft/GJsonArchive.hpp"         // GArchive JSON codec
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * Converts a shared_ptr<T> into its string representation using one of the GArchive (Weft) codecs.
 * T must be GArchive-serializable and (as a polymorphic root) registered via GEM_REGISTER_ARCHIVABLE.
 *
 * @param gt_ptr A shared_ptr to the object to be serialized
 * @param ser_mod The corresponding serialization mode (GEM_BINARY or GEM_JSON)
 * @return A string representation of gt_ptr
 */
template <typename T>
std::string
sharedPtrToString(std::shared_ptr<T> gt_ptr, const Gem::Common::serializationMode &ser_mod) {
    switch(ser_mod) {
        using enum Gem::Common::serializationMode;
    case GEM_BINARY: {
        Gem::Weft::GBinaryOArchive oa;
        oa &Gem::Weft::make_nvp("classHierarchyFromT_ptr", gt_ptr);
        return oa.str();
    }

    case GEM_JSON: {
        Gem::Weft::GJsonOArchive oa;
        oa &Gem::Weft::make_nvp("classHierarchyFromT_ptr", gt_ptr);
        return oa.str();
    }
    }

    throw geneva_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In sharedPtrToString(): unknown serialization mode " << static_cast<int>(ser_mod) << '\n');
}

/******************************************************************************/
/**
 * Loads a shared_ptr<T> from its string representation using one of the GArchive (Weft) codecs.
 * T must be GArchive-serializable and (as a polymorphic root) registered via GEM_REGISTER_ARCHIVABLE.
 *
 * @param gt_string A string representation of the object to be restored
 * @param ser_mod The corresponding serialization mode (GEM_BINARY or GEM_JSON)
 * @return A shared_ptr to the restored object
 */
template <typename T>
std::shared_ptr<T>
sharedPtrFromString(const std::string &gt_string, const Gem::Common::serializationMode &ser_mod) {
    std::shared_ptr<T> gt_ptr;

    try {
        switch(ser_mod) {
            using enum Gem::Common::serializationMode;
        case GEM_BINARY: {
            // GBinaryIArchive holds a string_view over gt_string, which outlives it here.
            Gem::Weft::GBinaryIArchive ia(gt_string);
            ia &Gem::Weft::make_nvp("classHierarchyFromT_ptr", gt_ptr);
        } break;

        case GEM_JSON: {
            Gem::Weft::GJsonIArchive ia(gt_string);
            ia &Gem::Weft::make_nvp("classHierarchyFromT_ptr", gt_ptr);
        } break;
        }
    }
    catch(std::exception &e) {
        glogger << "In sharedPtrFromString(): Error!" << '\n'
                << "Caught std::exception with message" << '\n'
                << e.what() << '\n'
                << "We will return an empty pointer." << '\n'
                << GWARNING;

        return std::shared_ptr<T>();
    }
    catch(...) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In sharedPtrFromString(): Error!" << '\n'
            << "Caught unknown exception" << '\n'
        );
    }

    return gt_ptr;
}

/******************************************************************************/

} /* namespace Gem::Common */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/



/******************************************************************************/
