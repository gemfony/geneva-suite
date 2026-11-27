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
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "weft/GArchivePolymorphic.hpp" // GArchive codecs (smart-ptr arm) for sharedPtr[To|From]String
#include "weft/GBinaryArchive.hpp"      // GArchive flat binary codec
#include "weft/GJsonArchive.hpp"        // GArchive JSON codec

namespace Gem::Common {

namespace detail {

/******************************************************************************/
/**
 * The one name under which sharedPtrToString() and the object streams of GCommonInterfaceT
 * (toString(), toStream(), toFile()) store an object, so that both write the same text.
 */
inline constexpr char const *OBJECT_STRING_TAG = "object";

/******************************************************************************/
/**
 * Writes the object a shared_ptr points to into an archive of the given codec and returns the text.
 *
 * @tparam oarchive_type The output archive of the codec (Gem::Weft::GBinaryOArchive or GJsonOArchive)
 * @tparam T The pointee type
 * @param gt_ptr The pointer to the object to be serialized (an empty pointer is stored as empty)
 * @return The text of the archive
 */
template <typename oarchive_type, typename T>
std::string toArchiveString(std::shared_ptr<T> &gt_ptr) {
    oarchive_type oa;
    oa &Gem::Weft::make_nvp(OBJECT_STRING_TAG, gt_ptr);
    return oa.str();
}

/******************************************************************************/
/**
 * Reads an object from the text of an archive of the given codec.
 *
 * @tparam iarchive_type The input archive of the codec (Gem::Weft::GBinaryIArchive or GJsonIArchive)
 * @tparam T The pointee type
 * @param gt_string The text of the archive; it outlives the archive, which may refer to it
 * @return A shared_ptr to the restored object (empty if the archive holds an empty pointer)
 */
template <typename iarchive_type, typename T>
std::shared_ptr<T> fromArchiveString(const std::string &gt_string) {
    std::shared_ptr<T> gt_ptr;
    iarchive_type ia(gt_string);
    ia &Gem::Weft::make_nvp(OBJECT_STRING_TAG, gt_ptr);
    return gt_ptr;
}

/******************************************************************************/

} /* namespace detail */

/******************************************************************************/
/**
 * Converts a shared_ptr<T> into its string representation using one of the GArchive (Weft) codecs.
 * T must be GArchive-serializable and (as a polymorphic root) registered via GEM_REGISTER_ARCHIVABLE.
 * The string is the one GCommonInterfaceT::toString() writes for the same object, and
 * GCommonInterfaceT::fromString() reads it.
 *
 * @param gt_ptr A shared_ptr to the object to be serialized
 * @param ser_mod The corresponding serialization mode (GEM_BINARY or GEM_JSON)
 * @return A string representation of gt_ptr
 * @throws gem_exception for a serialization mode that is not one of the two
 */
template <typename T>
std::string
sharedPtrToString(std::shared_ptr<T> gt_ptr, const Gem::Common::serializationMode &ser_mod) {
    switch(ser_mod) {
        using enum Gem::Common::serializationMode;
    case GEM_BINARY:
        return detail::toArchiveString<Gem::Weft::GBinaryOArchive>(gt_ptr);
    case GEM_JSON:
        return detail::toArchiveString<Gem::Weft::GJsonOArchive>(gt_ptr);
    }

    throw gem_exception(
        g_error_streamer() << "unknown serialization mode " << static_cast<int>(ser_mod) << '\n'
    );
}

/******************************************************************************/
/**
 * Loads a shared_ptr<T> from its string representation using one of the GArchive (Weft) codecs.
 * T must be GArchive-serializable and (as a polymorphic root) registered via GEM_REGISTER_ARCHIVABLE.
 * It reads the strings of sharedPtrToString() and of GCommonInterfaceT::toString() alike.
 *
 * @param gt_string A string representation of the object to be restored
 * @param ser_mod The corresponding serialization mode (GEM_BINARY or GEM_JSON)
 * @return A shared_ptr to the restored object (empty if the string holds an empty pointer)
 * @throws Gem::Weft::weft_exception (or another std::exception from the object's serialize()) if
 *         the string cannot be decoded; the error reaches the caller unchanged, as with sharedPtrToString()
 * @throws gem_exception for a serialization mode that is not one of the two
 */
template <typename T>
std::shared_ptr<T>
sharedPtrFromString(const std::string &gt_string, const Gem::Common::serializationMode &ser_mod) {
    switch(ser_mod) {
        using enum Gem::Common::serializationMode;
    case GEM_BINARY:
        return detail::fromArchiveString<Gem::Weft::GBinaryIArchive, T>(gt_string);
    case GEM_JSON:
        return detail::fromArchiveString<Gem::Weft::GJsonIArchive, T>(gt_string);
    }

    throw gem_exception(
        g_error_streamer() << "unknown serialization mode " << static_cast<int>(ser_mod) << '\n'
    );
}

/******************************************************************************/

} /* namespace Gem::Common */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/******************************************************************************/
