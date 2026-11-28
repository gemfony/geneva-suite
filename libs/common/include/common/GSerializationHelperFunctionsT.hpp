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
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
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
 * The refusal of a serialization mode that is not one of the two, stated once for both directions.
 *
 * @param mode The serialization mode that names no codec
 * @throws gem_exception always
 */
[[noreturn]] inline void refuseSerializationMode(serializationMode mode) {
    throw gem_exception(
        g_error_streamer() << "unknown serialization mode " << static_cast<int>(mode) << '\n'
    );
}

/******************************************************************************/

} /* namespace detail */

/******************************************************************************/
/**
 * Writes one named value into an archive of the codec that @p mode names and returns the
 * archive's text: the flat binary codec for GEM_BINARY, the self-describing JSON codec for
 * GEM_JSON. The value may be of any type the GArchive (Weft) codecs know: one with a serialize()
 * member or a gem_archive_serialize() overload, a standard container, a std::variant, a smart
 * pointer to a type registered with GEM_REGISTER_ARCHIVABLE, and so on. archiveFromString() reads
 * the text back.
 *
 * @tparam value_type The type of the value
 * @param name The name of the value in the archive (its root name; the binary codec writes no names)
 * @param value The value to be written
 * @param mode The codec (GEM_BINARY or GEM_JSON)
 * @return The text of the archive
 * @throws gem_exception for a serialization mode that is not one of the two
 */
template <typename value_type>
std::string archiveToString(char const *name, value_type const &value, serializationMode mode) {
    switch(mode) {
        using enum serializationMode;
    case GEM_BINARY: {
        Gem::Weft::GBinaryOArchive oa;
        oa &Gem::Weft::make_nvp(name, value);
        return oa.str();
    }
    case GEM_JSON: {
        Gem::Weft::GJsonOArchive oa;
        oa &Gem::Weft::make_nvp(name, value);
        return oa.str();
    }
    }
    detail::refuseSerializationMode(mode);
}

/******************************************************************************/
/**
 * Reads one named value from the text of an archive of the codec that @p mode names, as
 * archiveToString() writes it.
 *
 * @tparam value_type The type of the value
 * @param name The name of the value in the archive (its root name; the binary codec reads no names)
 * @param value Receives the value
 * @param text The text of the archive
 * @param mode The codec (GEM_BINARY or GEM_JSON) the text was written in
 * @throws gem_exception for a serialization mode that is not one of the two
 * @throws Gem::Weft::weft_exception (or another std::exception from the value's serialize()) if
 *         the text cannot be decoded
 */
template <typename value_type>
void archiveFromString(
    char const *name,
    value_type &value,
    std::string_view text,
    serializationMode mode
) {
    switch(mode) {
        using enum serializationMode;
    case GEM_BINARY: {
        Gem::Weft::GBinaryIArchive ia(text);
        ia &Gem::Weft::make_nvp(name, value);
        return;
    }
    case GEM_JSON: {
        Gem::Weft::GJsonIArchive ia(text);
        ia &Gem::Weft::make_nvp(name, value);
        return;
    }
    }
    detail::refuseSerializationMode(mode);
}

/******************************************************************************/
/**
 * Writes one named value into a file, as archiveToString() writes it. An existing file is replaced
 * as a whole (replaceFileAtomically()): a reader, and a process that is ended while it writes, sees
 * the old file or the new one. If the value cannot be written, the file is left as it was.
 *
 * @tparam value_type The type of the value
 * @param path The file to be written (its directory must exist)
 * @param name The name of the value in the archive
 * @param value The value to be written
 * @param mode The codec (GEM_BINARY or GEM_JSON)
 * @throws gem_exception for a serialization mode that is not one of the two, or if the file cannot
 *         be written
 */
template <typename value_type>
void archiveToFile(
    std::filesystem::path const &path,
    char const *name,
    value_type const &value,
    serializationMode mode
) {
    replaceFileAtomically(
        path,
        [name, &value, mode](std::ostream &os) { os << archiveToString(name, value, mode); },
        "Gem::Common::archiveToFile()"
    );
}

/******************************************************************************/
/**
 * Reads one named value from a file that archiveToFile() (or a file holding the text of
 * archiveToString()) wrote.
 *
 * @tparam value_type The type of the value
 * @param path The file to be read
 * @param name The name of the value in the archive
 * @param value Receives the value
 * @param mode The codec (GEM_BINARY or GEM_JSON) the file was written in
 * @throws gem_exception for a serialization mode that is not one of the two, or if the file does
 *         not exist or cannot be read
 * @throws Gem::Weft::weft_exception (or another std::exception from the value's serialize()) if
 *         the content cannot be decoded
 */
template <typename value_type>
void archiveFromFile(
    std::filesystem::path const &path,
    char const *name,
    value_type &value,
    serializationMode mode
) {
    archiveFromString(name, value, loadTextDataFromFile(path), mode);
}

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
    return archiveToString(detail::OBJECT_STRING_TAG, gt_ptr, ser_mod);
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
    std::shared_ptr<T> gt_ptr;
    archiveFromString(detail::OBJECT_STRING_TAG, gt_ptr, gt_string, ser_mod);
    return gt_ptr;
}

/******************************************************************************/

} /* namespace Gem::Common */

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

/******************************************************************************/
