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
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Boost headers go here
// nvcc (CUDA host compiler) does not suppress warnings from system/third-party
// headers the way GCC does.  The three diagnostics below are Boost-internal
// false positives that are irrelevant to Geneva code:
//   #68-D  – integer conversion sign change    (boost/mpl/print.hpp)
//   #186-D – unsigned comparison with zero     (boost/mp11, via boost/json)
//   #191-D – meaningless cast qualifier        (boost/archive/detail/iserializer.hpp)
#include <boost/json.hpp>
#include <boost/program_options.hpp>

// Geneva header files go here
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GDefaultValueT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

namespace Gem::Common {

// Forward declaration
class GParserBuilder;

/******************************************************************************/
/**
 * Internal helpers shared by the file-parameter proxies for reading and writing the JSON
 * configuration format. A parameter node is a JSON object `{ "comment": [ ... ], "default": ...,
 * "value": ... }`; scalars are stored natively (arithmetic types as JSON numbers, booleans as JSON
 * bools, strings as JSON strings, and enums / durations / other custom types as strings via
 * Gem::Common::to_string), and vectors / arrays store their elements as JSON arrays under the
 * "default"/"value" keys. The read path accepts either the native or the earlier all-strings form, so
 * a config written before the switch to native scalars still loads. Comments are write-only decoration
 * (regenerated from each proxy's registered comment on every write) and are never read back.
 */
namespace detail {

/******************************************************************************/
/** @brief Converts a scalar configuration value to its on-disk string form.
 *  @tparam T The scalar parameter type
 *  @param v The value to convert
 *  @return The string stored under a parameter's "value"/"default" key */
template <typename T>
std::string cfgScalarToString(T const &v) {
    if constexpr(std::is_same_v<T, std::string>) {
        return v; // stored verbatim so embedded spaces survive
    }
    else if constexpr(std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    }
    else if constexpr(std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t>) {
        // Render (u)int8_t numerically (via a wider integer), not as the character it aliases.
        using wider = std::conditional_t<std::is_signed_v<T>, int, unsigned int>;
        return Gem::Common::to_string(static_cast<wider>(v));
    }
    else {
        return Gem::Common::to_string(v);
    }
}

/******************************************************************************/
/** @brief Names, for an error text, what a configuration value of type T must be.
 *  @tparam T The type of the value
 *  @return "an integer", "an integer >= 0", "a number", "one of the values 0, 1 or 2" (an enum, see
 *  numeric_enum_values_v), or "a value of its type" */
template <typename T>
std::string cfgExpectedValue() {
    if constexpr(std::is_integral_v<T> && std::is_unsigned_v<T>) {
        return "an integer >= 0";
    }
    else if constexpr(std::is_integral_v<T>) {
        return "an integer";
    }
    else if constexpr(std::is_floating_point_v<T>) {
        return "a number";
    }
    else if constexpr(numeric_io_enum<T>) {
        return "one of the values " + numericEnumValueList<T>();
    }
    else {
        return "a value of its type";
    }
}

/******************************************************************************/
/** @brief Reads a value of type T from the whole of @p s: anything that does not read as a value
 *  of T is refused rather than read as far as it goes, or as an indeterminate value. Refused are
 *  text where a number belongs, a trailing remainder (the ".5" of "2.5" for an integer), a value
 *  the type's reader fails on (a number that is none of an enum's values, see
 *  numeric_enum_values_v) and a negative number for an unsigned type, which the stream would wrap
 *  around ("-1" as 4294967295).
 *  @tparam T The type to read (arithmetic, or any type with a stream extraction operator)
 *  @param s The string to parse
 *  @param key The configuration key the value belongs to (named in the error text)
 *  @return The parsed value
 *  @throws geneva_exception if @p s is not a value of type T */
template <typename T>
T cfgReadWhole(std::string const &s, std::string const &key) {
    T val{};
    std::istringstream iss(s);
    bool negative_unsigned = false;
    if constexpr(std::is_unsigned_v<T>) {
        negative_unsigned = (iss >> std::ws).peek() == '-';
    }
    iss >> val;
    if(negative_unsigned || iss.fail() || not(iss >> std::ws).eof()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "The configuration entry \"" << key << "\" has the value \"" << s
            << "\", which is not " << cfgExpectedValue<T>() << '\n'
        );
    }
    return val;
}

/******************************************************************************/
/** @brief Parses an on-disk scalar string back into its parameter type (inverse of
 *  cfgScalarToString). Booleans accept "true"/"false" and "1"/"0"; every other type is read through
 *  cfgReadWhole(), so a value that does not read as its type is refused (it used to read as false,
 *  as far as it went, or as an indeterminate value).
 *  @tparam T The scalar parameter type
 *  @param s The string to parse
 *  @param key The configuration key the value belongs to (named in the error text)
 *  @return The parsed value
 *  @throws geneva_exception if @p s is not a value of type T */
template <typename T>
T cfgScalarFromString(std::string const &s, std::string const &key) {
    if constexpr(std::is_same_v<T, std::string>) {
        return s;
    }
    else if constexpr(std::is_same_v<T, bool>) {
        if(s == "true" || s == "1") {
            return true;
        }
        if(s == "false" || s == "0") {
            return false;
        }
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "The configuration entry \"" << key << "\" has the value \"" << s
            << "\", which is not a boolean (true, false, 1 or 0)" << '\n'
        );
    }
    else if constexpr(std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t>) {
        // (u)int8_t is a character type: a stream extraction of "1" would read the character '1'
        // (value 49), not the number 1. Parse through a wider integer so small enums/counts stored in
        // an 8-bit field round-trip by value, and refuse what the 8-bit type cannot hold ("300").
        using wider = std::conditional_t<std::is_signed_v<T>, int, unsigned int>;
        auto const val = cfgReadWhole<wider>(s, key);
        if(not std::in_range<T>(val)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "The configuration entry \"" << key << "\" has the value \"" << s
                << "\", which is not an integer from "
                << static_cast<wider>(std::numeric_limits<T>::min()) << " to "
                << static_cast<wider>(std::numeric_limits<T>::max()) << '\n'
            );
        }
        return static_cast<T>(val);
    }
    else {
        return cfgReadWhole<T>(s, key);
    }
}

/******************************************************************************/
/** @brief Converts a scalar configuration value to its on-disk JSON form.
 *
 *  Plain arithmetic types and booleans are stored as native JSON numbers / bools; strings are stored
 *  as JSON strings; everything else (enums, durations, other custom types) is stored as a JSON string
 *  produced by cfgScalarToString, preserving that type's to_string/from_string round-trip. The read
 *  path (cfgValueToString + cfgScalarFromString) accepts either form, so a config written in the
 *  earlier all-strings layout still loads.
 *  @tparam T The scalar parameter type
 *  @param v The value to convert
 *  @return The JSON value stored under a parameter's "value"/"default" key */
template <typename T>
boost::json::value cfgScalarToJson(T const &v) {
    if constexpr(std::is_same_v<T, std::string>) {
        return boost::json::value(v);
    }
    else if constexpr(std::is_same_v<T, bool>) {
        return boost::json::value(v);
    }
    else if constexpr(std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t>) {
        // Widen (u)int8_t so it is emitted as a numeric value rather than a single character; the
        // symmetric read path (cfgScalarFromString) parses it back through the same wider integer.
        using wider = std::conditional_t<std::is_signed_v<T>, int, unsigned int>;
        return boost::json::value(static_cast<wider>(v));
    }
    else if constexpr(std::is_arithmetic_v<T>) {
        return boost::json::value(v);
    }
    else {
        return boost::json::value(cfgScalarToString(v));
    }
}

/******************************************************************************/
/** @brief Refuses the entry of a known configuration key that does not have the form the key needs.
 *  The rule for every parameter kind: an ABSENT key keeps its registered default, a PRESENT key of
 *  the wrong form is an error -- never skipped in silence, which would leave the default in force
 *  while the file says otherwise (e.g. a flat "scan_randomly": false).
 *  @param key The key as it stands in the file (a combined parameter's sub-key as "group/key")
 *  @param expected The form the key needs, with an example
 *  @param found The entry found under the key
 *  @throws geneva_exception always */
[[noreturn]] inline void cfgRefuseEntry(
    std::string const &key,
    std::string const &expected,
    boost::json::value const &found
) {
    throw geneva_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "The configuration entry \"" << key
        << "\" has the wrong form: " << boost::json::serialize(found) << '\n'
        << "Expected " << expected << '\n'
    );
}

/******************************************************************************/
/** @brief Returns the group object of a combined parameter stored under @p key of @p parent, or
 *  nullptr if the key is absent (the registered defaults apply).
 *  @param parent The enclosing JSON object
 *  @param key The group label to look up
 *  @return A pointer to the group object, or nullptr if @p key is absent
 *  @throws geneva_exception if the entry under @p key is not an object (see cfgRefuseEntry()) */
inline boost::json::object const *
cfgGroupEntry(boost::json::object const &parent, std::string const &key) {
    auto const *v = parent.if_contains(key);
    if(v == nullptr) {
        return nullptr;
    }
    if(not v->is_object()) {
        cfgRefuseEntry(
            key,
            "an object holding the group's parameters, e.g. \"" + key +
                "\": { \"<name>\": { \"value\": ... }, ... }",
            *v
        );
    }
    return &v->get_object();
}

/******************************************************************************/
/** @brief Returns the "value" of a parameter entry stored under @p key of @p parent, or nullptr if
 *  the key is absent (the registered default applies). A present entry must be an object whose
 *  "value" is a scalar (@p is_list false) or an array (@p is_list true).
 *  @param parent The enclosing JSON object
 *  @param key The member key to look up
 *  @param is_list Whether the parameter holds a list of values (a vector or array parameter)
 *  @param group The label of the enclosing group of a combined parameter, empty otherwise (names
 *         the key in the error text)
 *  @return A pointer to the entry's "value", or nullptr if @p key is absent
 *  @throws geneva_exception if the entry is not of that form (see cfgRefuseEntry()) */
inline boost::json::value const *cfgEntryValue(
    boost::json::object const &parent,
    std::string const &key,
    bool is_list,
    std::string const &group = std::string()
) {
    auto const *entry = parent.if_contains(key);
    if(entry == nullptr) {
        return nullptr;
    }

    auto const *v = entry->is_object() ? entry->get_object().if_contains("value") : nullptr;
    bool const well_formed =
        v != nullptr &&
        (is_list ? v->is_array() : not(v->is_object() || v->is_array() || v->is_null()));
    if(not well_formed) {
        cfgRefuseEntry(
            group.empty() ? key : group + "/" + key,
            is_list
                ? "an object with a \"value\" array, e.g. \"" + key + "\": { \"value\": [ ... ] }"
                : "an object with a single \"value\", e.g. \"" + key + "\": { \"value\": ... }",
            *entry
        );
    }
    return v;
}

/******************************************************************************/
/** @brief Renders a JSON value expected to hold a scalar as a std::string; a non-string value is
 *  rendered via its serialized token so a natively-typed value (Phase 8) still yields a parseable
 *  string.
 *  @param v The JSON value
 *  @return The string form */
inline std::string cfgValueToString(boost::json::value const &v) {
    if(v.is_string()) {
        auto const &s = v.get_string();
        return std::string(s.data(), s.size());
    }
    return boost::json::serialize(v);
}

/******************************************************************************/
/** @brief Writes @p lines as a "comment" array into @p target when the list is non-empty. Each line
 *  is a separate array element so the pretty-printer emits one comment per line.
 *  @param target The JSON object receiving the comment array
 *  @param lines The individual comment lines */
inline void cfgWriteComments(boost::json::object &target, std::vector<std::string> const &lines) {
    if(not lines.empty()) {
        boost::json::array arr;
        arr.reserve(lines.size());
        for(auto const &line : lines) {
            arr.emplace_back(line);
        }
        target["comment"] = std::move(arr);
    }
}

} /* namespace detail */

/******************************************************************************/
// Indicates whether help was requested using the -h or --help switch on the command line
constexpr bool GCL_HELP_REQUESTED = true;
constexpr bool GCL_NO_HELP_REQUESTED = false;

// Indicates whether implicit values are allowed (such as in --server vs. --server=true)
constexpr bool GCL_IMPLICIT_ALLOWED = true;
constexpr bool GCL_IMPLICIT_NOT_ALLOWED = false;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A manipulator object that allows to identify the id of the comment to be
 * added
 */
class commentLevel { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /** @brief Enforce setting of the comment level */
    explicit commentLevel(std::size_t cl);

    /*************************************************************************/
    // Defaulted or deleted functions functions

    commentLevel() = delete;

    commentLevel(commentLevel const &) = default;
    commentLevel(commentLevel &&) = delete; // enforce explicit settinf of comment level

    commentLevel &operator=(commentLevel const &) = default;
    commentLevel &operator=(commentLevel &&) = delete; // enforce explicit settinf of comment level

    /*************************************************************************/

    /** @brief Retrieves the current commentLevel */
    [[nodiscard]] std::size_t getCommentLevel() const;

private:
    std::size_t comment_level_; ///< The id of the comment inside of GParsableI
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A manipulator object that increments the comment level
 */
class nextComment {
public:
    /** @brief The default constructor */
    nextComment() = default;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class specifies the interface of parsable parameters, to
 * which a call-back function has been assigned. It also stores some
 * information common to all parameter types.
 */
class GParsableI {
public:
    /** @brief A constructor for individual items */
    GParsableI(std::string const &option_name_var, std::string const &comment_var);

    /** @brief A constructor for vectors */
    GParsableI(
        std::vector<std::string> const &option_name_vec,
        std::vector<std::string> const &comment_vec
    );

    /** @brief The destructor */
    virtual ~GParsableI() = default;

    // Prevent copying, moving and default construction
    GParsableI() = delete;
    GParsableI(GParsableI const &) = delete;
    GParsableI(GParsableI &&) = delete;
    GParsableI &operator=(GParsableI const &) = delete;
    GParsableI &operator=(GParsableI &&) = delete;

    /** @brief Retrieves the option name at a given position */
    [[nodiscard]] std::string optionName(std::size_t pos = 0) const;
    /** @brief Retrieves the comment that was assigned to this variable at a given position */
    [[nodiscard]] std::string comment(std::size_t pos = 0) const;
    /** @brief Checks whether comments have indeed been registered */
    [[nodiscard]] bool hasComments() const;
    /** @brief Retrieves the number of comments available */
    [[nodiscard]] std::size_t numberOfComments() const;
    /** @brief Retrieves the number of option names registered for this parameter */
    [[nodiscard]] std::size_t numberOfOptionNames() const;

    /***************************************************************************/
    /**
	  * Create a std::vector<T> from a single element
	  *
	  * @tparam T The element type of the resulting vector
	  * @param item The single element to place into the vector
	  * @return A vector containing the single element
	  */
    template <typename T>
    static std::vector<T> makeVector(T const &item) {
        std::vector<T> result;
        result.push_back(item);
        return result;
    }

    /***************************************************************************/
    /**
	  * Create a std::vector<T> from two elements
	  *
	  * @tparam T The element type of the resulting vector
	  * @param item1 The first element to place into the vector
	  * @param item2 The second element to place into the vector
	  * @return A vector containing both elements, in order
	  */
    template <typename T>
    static std::vector<T> makeVector(T const &item1, T const &item2) {
        std::vector<T> result;
        result.push_back(item1);
        result.push_back(item2);
        return result;
    }

    /***************************************************************************/
    /**
	  * This function will forward all arguments to a newly created ostringstream
	  * and will then be added to the current comment_ entry.
	  *
	  * @tparam T The type of the value to be streamed into the current comment
	  * @param t The value to be appended (via operator<<) to the current comment
	  * @return A reference to this object, to allow chaining
	  */
    template <typename T>
    GParsableI &operator<<(T const &t) {
        std::ostringstream oss; // NOLINT(cppcoreguidelines-init-variables)
        oss << t;
        comment_.at(cl_) += oss.str();
        return *this;
    }

    /***************************************************************************/
    /** @brief Needed for std::ostringstream */
    GParsableI &operator<<(std::ostream &(*val)(std::ostream &));
    /** @brief Needed for std::ostringstream */
    GParsableI &operator<<(std::ios &(*val)(std::ios &));
    /** @brief Needed for std::ostringstream */
    GParsableI &operator<<(std::ios_base &(*val)(std::ios_base &));
    /** @brief Allows to indicate the current comment level */
    GParsableI &operator<<(commentLevel const &cl);
    /** @brief Allows to switch to the next comment level */
    GParsableI &operator<<(nextComment const &nc);

protected:
    /***************************************************************************/
    /** @brief Splits a comment into sub-tokens */
    static std::vector<std::string> splitComment(std::string const &comment);

private:
    /***************************************************************************/
    std::vector<std::string> option_name_; ///< The name of this parameter
    std::vector<std::string> comment_;     ///< A comment assigned to this parameter

    std::size_t cl_; ///< The id of the current comment inside of the comment_ vector
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class specifies the interface of parsable file parameters, to
 * which a call-back function has been assigned. Note that this class cannot
 * be copied; copy and move are explicitly deleted.
 */
class GFileParsableI : public GParsableI {
    // We want GParserBuilder to be able to call our private load- and save functions
    friend class GParserBuilder;

public:
    /** @brief A constructor for individual items */
    GFileParsableI(
        std::string const &option_name_var,
        std::string const &comment_var,
        bool is_essential_var
    );
    /** @brief A constructor for vectors */
    GFileParsableI(
        std::vector<std::string> const &option_name_vec,
        std::vector<std::string> const &comment_vec,
        bool is_essential_var
    );

    /** @brief The destructor */
    ~GFileParsableI() override = default;

    // Prevent copying, moving and default construction
    GFileParsableI() = delete;
    GFileParsableI(GFileParsableI const &) = delete;
    GFileParsableI(GFileParsableI &&) = delete;
    GFileParsableI &operator=(GFileParsableI const &) = delete;
    GFileParsableI &operator=(GFileParsableI &&) = delete;

    /** @brief Checks whether this is an essential variable at a given position */
    [[nodiscard]] bool isEssential() const;

    /** @brief Executes a stored call-back function */
    void executeCallBackFunction();

    /** @brief Returns the top-level configuration-file (JSON) key this parameter
     *  occupies. By default this is the first option name -- single, vector and
     *  array parameters write their data directly under it. Combined parameters
     *  override this to return their JSON group label, under which their
     *  sub-options nest. Used by the unknown-key diagnostic to recognise valid
     *  top-level keys.
     *  @return The top-level configuration-file key occupied by this parameter */
    [[nodiscard]] virtual std::string topLevelConfigKey() const {
        return GParsableI::optionName(0);
    }

private:
    /***************************************************************************/
    /** @brief Loads this parameter's value from the parsed configuration document
     *  @param root The root JSON object of the parsed configuration */
    virtual void load_from(boost::json::object const &root) = 0;

    /** @brief Saves this parameter (comment, default and value) into the configuration document
     *  @param root The root JSON object being assembled */
    virtual void save_to(boost::json::object &root) const = 0;

    /** @brief Executes a stored call-back function */
    virtual void executeCallBackFunction_() = 0;

    /***************************************************************************/

    bool is_essential_; ///< Indicates whether this is an essential variable
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A base class for single parameters. This class was introduced so we can
 * reset the default values in a central location rather than having to
 * convert to different target class. This makes user-code easier.
 *
 * @tparam parameter_type The type of the wrapped single parameter
 */
template <typename parameter_type>
class GSingleParmT : public GFileParsableI {
    // We want GParserBuilder to be able to call the reset function
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * Initializes the parameter and sets values in the parent class
	  *
	  * @param option_name_var The option name of this parameter
	  * @param comment_var The comment associated with this parameter
	  * @param is_essential_var Whether this is an essential (true) or secondary (false) parameter
	  * @param def_val The default value used when the option is absent from the configuration file
	  */
    GSingleParmT(
        const std::string &option_name_var,
        const std::string &comment_var,
        const bool &is_essential_var,
        const parameter_type &def_val
    )
      : GFileParsableI(option_name_var, comment_var, is_essential_var)
      , def_val_(def_val)
      , par_(def_val) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GSingleParmT() override = default;

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GSingleParmT() = delete;
    GSingleParmT(GSingleParmT<parameter_type> const &) = delete;
    GSingleParmT(GSingleParmT<parameter_type> &&) = delete;
    GSingleParmT<parameter_type> &operator=(GSingleParmT<parameter_type> const &) = delete;
    GSingleParmT<parameter_type> &operator=(GSingleParmT<parameter_type> &&) = delete;

protected:
    /***************************************************************************/
    /**
	  * Allows derived classes to reset the default value. The function assumes
	  * that no important data is stored in par_, as its value will be overwritten
	  * as well. The reason is that configuration files will otherwise contain
	  * the "old" par_-value.
	  *
	  * @param def_val The new default value, which also overwrites the current parameter value
	  */
    void resetDefault(parameter_type const &def_val) {
        def_val_ = def_val;
        par_ = def_val;
    }

    /***************************************************************************/
    parameter_type def_val_; ///< Holds the parameter's default value
    parameter_type par_;     ///< Holds the individual parameter

private:
    /***************************************************************************/
    /** @brief Loads this parameter's value from the parsed configuration document
     *  @param root The root JSON object of the parsed configuration */
    void load_from(boost::json::object const &root) override = 0;

    /** @brief Saves this parameter into the configuration document
     *  @param root The root JSON object being assembled */
    void save_to(boost::json::object &root) const override = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class wraps individual parsable file parameters, to which a callback
 * function has been assigned.
 *
 * @tparam parameter_type The type of the wrapped single parameter
 */
template <typename parameter_type>
class GFileSingleParsableParameterT : public GSingleParmT<parameter_type> {
    // We want GParserBuilder to be able to call our load- and save functions
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * Initializes the parameter and sets values in the parent class
	  *
	  * @param option_name_var The option name of this parameter
	  * @param comment_var The comment associated with this parameter
	  * @param is_essential_var Whether this is an essential (true) or secondary (false) parameter
	  * @param def_val The default value used when the option is absent from the configuration file
	  */
    GFileSingleParsableParameterT(
        const std::string &option_name_var,
        const std::string &comment_var,
        const bool &is_essential_var,
        const parameter_type &def_val
    )
      : GSingleParmT<parameter_type>(
            option_name_var,
            comment_var,
            is_essential_var,
            def_val
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * Initializes the parameter and sets values in the parent class, except
	  * for comments.
	  *
	  * @param option_name_var The option name of this parameter
	  * @param def_val The default value used when the option is absent from the configuration file
	  */
    GFileSingleParsableParameterT(const std::string &option_name_var, const parameter_type &def_val)
      : GSingleParmT<parameter_type>(
            option_name_var,
            std::string(),
            Gem::Common::VAR_IS_ESSENTIAL,
            def_val
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GFileSingleParsableParameterT() override = default;

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GFileSingleParsableParameterT() = delete;
    GFileSingleParsableParameterT(GFileSingleParsableParameterT<parameter_type> const &) = delete;
    GFileSingleParsableParameterT(GFileSingleParsableParameterT<parameter_type> &&) = delete;
    GFileSingleParsableParameterT<parameter_type> &
    operator=(GFileSingleParsableParameterT<parameter_type> const &) = delete;
    GFileSingleParsableParameterT<parameter_type> &
    operator=(GFileSingleParsableParameterT<parameter_type> &&) = delete;

    /***************************************************************************/
    /**
	  * Allows to register a call-back function with this object
	  *
	  * @param call_back The function to be executed
	  */
    void registerCallBackFunction(std::move_only_function<void(parameter_type)> call_back) {
        if(not call_back) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GSingleParsableParameter::registerCallBackFunction(): Error" << '\n'
                << "Tried to register an empty call-back function" << '\n'
            );
        }

        call_back_func_ = std::move(call_back);
    }

private:
    /***************************************************************************/
    /**
	  * Loads this parameter's value from the parsed configuration document
	  *
	  * @param root The root JSON object of the parsed configuration
	  */
    void load_from(boost::json::object const &root) override {
        // An absent key keeps the value seeded from the default in the constructor; a present one
        // of the wrong form is refused.
        if(auto const *v = detail::cfgEntryValue(root, GParsableI::optionName(0), false)) {
            GSingleParmT<parameter_type>::par_ = detail::cfgScalarFromString<parameter_type>(
                detail::cfgValueToString(*v),
                GParsableI::optionName(0)
            );
        }
    }

    /***************************************************************************/
    /**
	  * Saves data to the configuration document, including comments.
	  *
	  * @param root The root JSON object to which this parameter is added
	  */
    void save_to(boost::json::object &root) const override {
        boost::json::object entry;

        // Check that we have the right number of comments
        if(this->hasComments()) {
            if(this->numberOfComments() != 1) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GFileSingleParsableParameterT<>::save_to(): Error!" << '\n'
                    << "Expected 0 or 1 comment but got " << this->numberOfComments() << '\n'
                );
            }
            detail::cfgWriteComments(entry, GParsableI::splitComment(this->comment(0)));
        }

        entry["default"] = detail::cfgScalarToJson(GSingleParmT<parameter_type>::def_val_);
        entry["value"] = detail::cfgScalarToJson(GSingleParmT<parameter_type>::par_);
        root[GParsableI::optionName(0)] = std::move(entry);
    }

    /***************************************************************************/
    /**
	  * Executes a stored call-back function
	  */
    void executeCallBackFunction_() override {
        if(not call_back_func_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GSingleParsableParameter::executeCallBackFunction_(): Error" << '\n'
                << "Tried to execute call-back function without a stored function" << '\n'
            );
        }

        // Execute the function
        call_back_func_(GSingleParmT<parameter_type>::par_);
    }

    /***************************************************************************/

    std::move_only_function<void(parameter_type)> call_back_func_; ///< Holds the call-back function
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A base class for combined parameters. This class was introduced so we can
 * reset the default values in a central location rather than having to
 * convert to different target class. This makes user-code easier.
 *
 * @tparam par0_type The type of the first combined parameter
 * @tparam par1_type The type of the second combined parameter
 */
template <typename par0_type, typename par1_type>
class GCombinedParT : public GFileParsableI {
    // We want GParserBuilder to be able to call the reset function
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * Initializes the parameter and sets values in the parent class
	  *
	  * @param option_name_var0 The option name of the first parameter
	  * @param comment_var0 The comment associated with the first parameter
	  * @param def_val0 The default value of the first parameter
	  * @param option_name_var1 The option name of the second parameter
	  * @param comment_var1 The comment associated with the second parameter
	  * @param def_val1 The default value of the second parameter
	  * @param is_essential_var Whether this is an essential (true) or secondary (false) parameter
	  * @param combined_label The JSON group label under which both sub-options are nested
	  */
    GCombinedParT(
        std::string const &option_name_var0,
        std::string const &comment_var0,
        par0_type const &def_val0,
        std::string const &option_name_var1,
        std::string const &comment_var1,
        par1_type const &def_val1,
        bool const &is_essential_var,
        std::string combined_label
    )
      : GFileParsableI(
            GFileParsableI::makeVector(option_name_var0, option_name_var1),
            GFileParsableI::makeVector(comment_var0, comment_var1),
            is_essential_var
        )
      , par0_(def_val0)
      , def_val0_(def_val0)
      , par1_(def_val1)
      , def_val1_(def_val1)
      , combined_label_(std::move(combined_label)) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GCombinedParT() override = default;

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GCombinedParT() = delete;
    GCombinedParT(GCombinedParT<par0_type, par1_type> const &) = delete;
    GCombinedParT(GCombinedParT<par0_type, par1_type> &&) = delete;
    GCombinedParT<par0_type, par1_type> &
    operator=(GCombinedParT<par0_type, par1_type> const &) = delete;
    GCombinedParT<par0_type, par1_type> &operator=(GCombinedParT<par0_type, par1_type> &&) = delete;

    /***************************************************************************/
    /** @brief The combined parameter nests its sub-options under a single JSON
     *  group label, so that label -- not the individual sub-option names -- is the
     *  top-level configuration-file key.
     *  @return The combined JSON group label */
    [[nodiscard]] std::string topLevelConfigKey() const override {
        return combined_label_;
    }

protected:
    /***************************************************************************/
    /**
	  * Allows derived classes to reset the default value. The function assumes
	  * that no important data is stored in par0_ and par1_, as their values will
	  * be overwritten as well. The reason is that configuration files will otherwise
	  * contain the "old" par_-value.
	  *
	  * @param def_val0 The new default value for the first parameter
	  * @param def_val1 The new default value for the second parameter
	  */
    void resetDefault(par0_type const &def_val0, par1_type const &def_val1) {
        def_val0_ = def_val0;
        def_val1_ = def_val1;
        par0_ = def_val0;
        par1_ = def_val1;
    }

    /***************************************************************************/
    par0_type par0_, def_val0_; ///< Holds the individual parameters and default values 0
    par1_type par1_, def_val1_; ///< Holds the individual parameters and default values 1

    std::string combined_label_; ///< Holds a path label for the combined JSON path

private:
    /***************************************************************************/
    /** @brief Loads this parameter's value from the parsed configuration document
     *  @param root The root JSON object of the parsed configuration */
    void load_from(boost::json::object const &root) override = 0;

    /** @brief Saves this parameter into the configuration document
     *  @param root The root JSON object being assembled */
    void save_to(boost::json::object &root) const override = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class wraps combined parsable file parameters, to which a callback
 * function has been assigned.
 *
 * @tparam par0_type The type of the first combined parameter
 * @tparam par1_type The type of the second combined parameter
 */
template <typename par0_type, typename par1_type>
class GFileCombinedParsableParameterT : public GCombinedParT<par0_type, par1_type> {
    // We want GParserBuilder to be able to call our load- and save functions
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * Initializes the parameters
	  *
	  * @param option_name_var0 The option name of the first parameter
	  * @param comment_var0 The comment associated with the first parameter
	  * @param def_val0 The default value of the first parameter
	  * @param option_name_var1 The option name of the second parameter
	  * @param comment_var1 The comment associated with the second parameter
	  * @param def_val1 The default value of the second parameter
	  * @param is_essential_var Whether this is an essential (true) or secondary (false) parameter
	  * @param combined_label The JSON group label under which both sub-options are nested
	  */
    GFileCombinedParsableParameterT(
        std::string const &option_name_var0,
        std::string const &comment_var0,
        par0_type const &def_val0,
        std::string const &option_name_var1,
        std::string const &comment_var1,
        par1_type const &def_val1,
        bool is_essential_var,
        std::string const &combined_label
    )
      : GCombinedParT<par0_type, par1_type>(
            option_name_var0,
            comment_var0,
            def_val0,
            option_name_var1,
            comment_var1,
            def_val1,
            is_essential_var,
            combined_label
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * Initializes the parameters, except for comments
	  *
	  * @param option_name_var0 The option name of the first parameter
	  * @param def_val0 The default value of the first parameter
	  * @param option_name_var1 The option name of the second parameter
	  * @param def_val1 The default value of the second parameter
	  * @param combined_label The JSON group label under which both sub-options are nested
	  */
    GFileCombinedParsableParameterT(
        std::string const &option_name_var0,
        par0_type const &def_val0,
        std::string const &option_name_var1,
        par1_type const &def_val1,
        std::string const &combined_label
    )
      : GCombinedParT<par0_type, par1_type>(
            option_name_var0,
            std::string(),
            def_val0,
            option_name_var1,
            std::string(),
            def_val1,
            Gem::Common::VAR_IS_ESSENTIAL,
            combined_label
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GFileCombinedParsableParameterT() override = default;

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GFileCombinedParsableParameterT() = delete;
    GFileCombinedParsableParameterT(GFileCombinedParsableParameterT<par0_type, par1_type> const &) =
        delete;
    GFileCombinedParsableParameterT(GFileCombinedParsableParameterT<par0_type, par1_type> &&) =
        delete;
    GFileCombinedParsableParameterT<par0_type, par1_type> &
    operator=(GFileCombinedParsableParameterT<par0_type, par1_type> const &) = delete;
    GFileCombinedParsableParameterT<par0_type, par1_type> &
    operator=(GFileCombinedParsableParameterT<par0_type, par1_type> &&) = delete;

    /***************************************************************************/
    /**
	  * Allows to register a call-back function with this object
	  *
	  * @param call_back The function to be executed
	  */
    void registerCallBackFunction(std::move_only_function<void(par0_type, par1_type)> call_back) {
        if(not call_back) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileCombinedParsableParameterT::registerCallBackFunction(): Error" << '\n'
                << "Tried to register an empty call-back function" << '\n'
            );
        }

        call_back_func_ = std::move(call_back);
    }

private:
    /***************************************************************************/
    /**
	  * Loads this parameter's value from the parsed configuration document
	  *
	  * @param root The root JSON object of the parsed configuration
	  */
    void load_from(boost::json::object const &root) override {
        // An absent group or sub-key keeps the defaults seeded in the constructor; a present one of
        // the wrong form is refused.
        std::string const &label = GCombinedParT<par0_type, par1_type>::combined_label_;
        auto const *group = detail::cfgGroupEntry(root, label);
        if(group == nullptr) {
            return;
        }
        if(auto const *v = detail::cfgEntryValue(*group, GParsableI::optionName(0), false, label)) {
            GCombinedParT<par0_type, par1_type>::par0_ = detail::cfgScalarFromString<par0_type>(
                detail::cfgValueToString(*v),
                label + "/" + GParsableI::optionName(0)
            );
        }
        if(auto const *v = detail::cfgEntryValue(*group, GParsableI::optionName(1), false, label)) {
            GCombinedParT<par0_type, par1_type>::par1_ = detail::cfgScalarFromString<par1_type>(
                detail::cfgValueToString(*v),
                label + "/" + GParsableI::optionName(1)
            );
        }
    }

    /***************************************************************************/
    /**
	  * Saves data to the configuration document, including comments. Both sub-options are nested
	  * under the combined group label.
	  *
	  * @param root The root JSON object to which this parameter is added
	  */
    void save_to(boost::json::object &root) const override {
        // Check that we have the right number of comments
        if(this->hasComments() && this->numberOfComments() != 2) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileCombinedParsableParameterT<>::save_to(): Error!" << '\n'
                << "Expected 0 or 2 comments but got " << this->numberOfComments() << '\n'
            );
        }

        boost::json::object entry0;
        if(this->hasComments()) {
            detail::cfgWriteComments(entry0, GParsableI::splitComment(this->comment(0)));
        }
        entry0["default"] = detail::cfgScalarToJson(GCombinedParT<par0_type, par1_type>::def_val0_);
        entry0["value"] = detail::cfgScalarToJson(GCombinedParT<par0_type, par1_type>::par0_);

        boost::json::object entry1;
        if(this->hasComments()) {
            detail::cfgWriteComments(entry1, GParsableI::splitComment(this->comment(1)));
        }
        entry1["default"] = detail::cfgScalarToJson(GCombinedParT<par0_type, par1_type>::def_val1_);
        entry1["value"] = detail::cfgScalarToJson(GCombinedParT<par0_type, par1_type>::par1_);

        boost::json::object group;
        group[GParsableI::optionName(0)] = std::move(entry0);
        group[GParsableI::optionName(1)] = std::move(entry1);
        root[GCombinedParT<par0_type, par1_type>::combined_label_] = std::move(group);
    }

    /***************************************************************************/
    /**
	  * Executes a stored call-back function
	  */
    void executeCallBackFunction_() override {
        if(not call_back_func_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileCombinedParsableParameterT::executeCallBackFunction_(): Error" << '\n'
                << "Tried to execute call-back function without a stored function" << '\n'
            );
        }

        // Execute the function
        call_back_func_(
            GCombinedParT<par0_type, par1_type>::par0_,
            GCombinedParT<par0_type, par1_type>::par1_
        );
    }

    /***************************************************************************/

    std::move_only_function<void(par0_type, par1_type)>
        call_back_func_; ///< Holds the call-back function
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A base class for vector parameters. This class was introduced so we can
 * reset the default values in a central location rather than having to
 * convert to different target class. This makes user-code easier.
 *
 * @tparam parameter_type The element type of the wrapped parameter vector
 */
template <typename parameter_type>
class GVectorParT : public GFileParsableI {
    // We want GParserBuilder to be able to call the reset function
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * Initializes the parameter and sets values in the parent class
	  *
	  * @param option_name_var The option name of this parameter
	  * @param comment_var The comment associated with this parameter
	  * @param def_val The default values; par_cnt_ is seeded with these so a write before parsing emits one value per default
	  * @param is_essential_var Whether this is an essential (true) or secondary (false) parameter
	  */
    GVectorParT(
        std::string const &option_name_var,
        std::string const &comment_var,
        std::vector<parameter_type> const &def_val,
        bool is_essential_var
    )
      : GFileParsableI(option_name_var, comment_var, is_essential_var)
      , def_val_cnt_(def_val)
      // Seed par_cnt_ with the defaults so writeConfigFile() can emit a "value"
      // for every "default" entry before any parsing has populated par_cnt_.
      // The previous version left par_cnt_ empty, and save_to() then iterated
      // def_val_cnt_ while dereferencing par_cnt_.cbegin() — UB whenever no
      // load_from() had run yet, observable as silent garbage values in the
      // generated config or a segfault depending on allocator layout.
      , par_cnt_(def_val) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GVectorParT() override = default;

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GVectorParT() = delete;
    GVectorParT(GVectorParT<parameter_type> const &) = delete;
    GVectorParT(GVectorParT<parameter_type> &&) = delete;
    GVectorParT<parameter_type> &operator=(GVectorParT<parameter_type> const &) = delete;
    GVectorParT<parameter_type> &operator=(GVectorParT<parameter_type> &&) = delete;

protected:
    /***************************************************************************/
    /**
	  * Allows derived classes to reset the default value. Keeps par_cnt_ in
	  * lock-step with the new defaults so a subsequent writeConfigFile()
	  * before parsing still emits one "value" per "default" entry.
	  *
	  * @param def_val The new default values, which also reseed the current parameter values
	  */
    void resetDefault(std::vector<parameter_type> const &def_val) {
        def_val_cnt_ = def_val;
        par_cnt_ = def_val;
    }

    /***************************************************************************/
    std::vector<parameter_type> def_val_cnt_; ///< Holds default values
    std::vector<parameter_type> par_cnt_;     ///< Holds the parsed parameters

private:
    /***************************************************************************/
    /** @brief Loads this parameter's value from the parsed configuration document
     *  @param root The root JSON object of the parsed configuration */
    void load_from(boost::json::object const &root) override = 0;

    /** @brief Saves this parameter into the configuration document
     *  @param root The root JSON object being assembled */
    void save_to(boost::json::object &root) const override = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class wraps a std::vector of values (obviously of identical type).
 * The default vector may be empty -- that denotes a list-valued parameter which
 * is empty unless the user fills it in; it round-trips through a config file as "[]".
 *
 * @tparam parameter_type The element type of the wrapped parameter vector
 */
template <typename parameter_type>
class GFileVectorParsableParameterT : public GVectorParT<parameter_type> {
    // We want GParserBuilder to be able to call our load- and save functions
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * Initializes the parameters
	  *
	  * @param option_name_var The option name of this parameter
	  * @param comment_var The comment associated with this parameter
	  * @param def_val The default values used when the option is absent from the configuration file
	  * @param is_essential_var Whether this is an essential (true) or secondary (false) parameter
	  */
    GFileVectorParsableParameterT(
        std::string const &option_name_var,
        std::string const &comment_var,
        std::vector<parameter_type> const &def_val,
        bool is_essential_var
    )
      : GVectorParT<parameter_type>(
            option_name_var,
            comment_var,
            def_val,
            is_essential_var
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * Initializes the parameters, except for comments
	  *
	  * @param option_name_var The option name of this parameter
	  * @param def_val The default values used when the option is absent from the configuration file
	  */
    GFileVectorParsableParameterT(
        std::string const &option_name_var,
        std::vector<parameter_type> const &def_val
    )
      : GVectorParT<parameter_type>(
            option_name_var,
            std::string(),
            def_val,
            Gem::Common::VAR_IS_ESSENTIAL
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GFileVectorParsableParameterT() override = default;

    // Prevent copying, moving and default construction
    GFileVectorParsableParameterT() = delete;
    GFileVectorParsableParameterT(GFileVectorParsableParameterT<parameter_type> const &) = delete;
    GFileVectorParsableParameterT(GFileVectorParsableParameterT<parameter_type> &&) = delete;
    GFileVectorParsableParameterT<parameter_type> &
    operator=(GFileVectorParsableParameterT<parameter_type> const &) = delete;
    GFileVectorParsableParameterT<parameter_type> &
    operator=(GFileVectorParsableParameterT<parameter_type> &&) = delete;

    /***************************************************************************/
    /**
	  * Allows to register a call-back function with this object
	  *
	  * @param call_back The function to be executed
	  */
    void
    registerCallBackFunction(std::move_only_function<void(std::vector<parameter_type>)> call_back) {
        if(not call_back) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileVectorParsableParameterT::registerCallBackFunction(): Error" << '\n'
                << "Tried to register an empty call-back function" << '\n'
            );
        }

        call_back_func_ = std::move(call_back);
    }

private:
    /***************************************************************************/
    /**
	  * Loads this parameter's value from the parsed configuration document
	  *
	  * @param root The root JSON object of the parsed configuration
	  */
    void load_from(boost::json::object const &root) override {
        auto const *values = detail::cfgEntryValue(root, GParsableI::optionName(0), true);
        if(values == nullptr) {
            // The key is absent from the file -- e.g. a newly-registered vector parameter, or an
            // update-in-place pass over a config written before this parameter existed. Keep the
            // defaults par_cnt_ was seeded with in the constructor. (A present key of the wrong
            // form was refused.)
            return;
        }

        // The values are present: replace the seeded defaults with the on-disk values.
        GVectorParT<parameter_type>::par_cnt_.clear();
        for(auto const &v : values->get_array()) {
            GVectorParT<parameter_type>::par_cnt_.push_back(
                detail::cfgScalarFromString<parameter_type>(
                    detail::cfgValueToString(v),
                    GParsableI::optionName(0)
                )
            );
        }
    }

    /***************************************************************************/
    /**
	  * Saves data to a property tree object, including comments. Default
	  * values are taken from the def_val_ vector, which may be empty (it is
	  * then written out as an empty "default"/"value" array).
	  *
	  * @param root The JSON object to which data should be saved
	  */
    void save_to(boost::json::object &root) const override {
        // Check that we have the right number of comments
        if(this->hasComments() && this->numberOfComments() != 1) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileVectorParsableParameterT<>::save_to(): Error!" << '\n'
                << "Expected 0 or 1 comment but got " << this->numberOfComments() << '\n'
            );
        }

        // An empty default is valid: it denotes a list-valued parameter that is empty unless the user fills
        // it in (e.g. a set of optional paths). save_to() and load_from() both round-trip it as "[]"; the
        // two loops below iterate def_val_cnt_ / par_cnt_ independently, so an empty vector is harmless.
        boost::json::object entry;
        if(this->hasComments()) {
            detail::cfgWriteComments(entry, GParsableI::splitComment(this->comment(0)));
        }

        boost::json::array default_arr;
        default_arr.reserve(GVectorParT<parameter_type>::def_val_cnt_.size());
        for(auto const &def_val : GVectorParT<parameter_type>::def_val_cnt_) {
            default_arr.emplace_back(detail::cfgScalarToJson(def_val));
        }

        boost::json::array value_arr;
        value_arr.reserve(GVectorParT<parameter_type>::par_cnt_.size());
        for(auto const &value : GVectorParT<parameter_type>::par_cnt_) {
            value_arr.emplace_back(detail::cfgScalarToJson(value));
        }

        entry["default"] = std::move(default_arr);
        entry["value"] = std::move(value_arr);
        root[GParsableI::optionName(0)] = std::move(entry);
    }

    /***************************************************************************/
    /**
	  * Executes a stored call-back function
	  */
    void executeCallBackFunction_() override {
        if(not call_back_func_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileVectorParsableParameterT::executeCallBackFunction_(): Error" << '\n'
                << "Tried to execute call-back function without a stored function" << '\n'
            );
        }

        // Execute the function
        call_back_func_(GVectorParT<parameter_type>::par_cnt_);
    }

    /***************************************************************************/

    std::move_only_function<void(std::vector<parameter_type>)>
        call_back_func_; ///< Holds the call-back function
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A base class for array parameters. This class was introduced so we can
 * reset the default values in a central location rather than having to
 * convert to different target class. This makes user-code easier.
 *
 * @tparam parameter_type The element type of the wrapped parameter array
 * @tparam N The fixed number of elements in the array
 */
template <typename parameter_type, std::size_t N>
class GArrayParT : public GFileParsableI {
    // We want GParserBuilder to be able to call the reset function
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * Initializes the parameter and sets values in the parent class
	  *
	  * @param option_name_var The option name of this parameter
	  * @param comment_var The comment associated with this parameter
	  * @param def_val The default values used when the option is absent from the configuration file
	  * @param is_essential_var Whether this is an essential (true) or secondary (false) parameter
	  */
    GArrayParT(
        std::string const &option_name_var,
        std::string const &comment_var,
        std::array<parameter_type, N> const &def_val,
        bool is_essential_var
    )
      : GFileParsableI(option_name_var, comment_var, is_essential_var)
      , def_val_arr_(def_val)
      , par_arr_(def_val) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GArrayParT() override = default;

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GArrayParT() = delete;
    GArrayParT(GArrayParT<parameter_type, N> const &) = delete;
    GArrayParT(GArrayParT<parameter_type, N> &&) = delete;
    GArrayParT<parameter_type, N> &operator=(GArrayParT<parameter_type, N> const &) = delete;
    GArrayParT<parameter_type, N> &operator=(GArrayParT<parameter_type, N> &&) = delete;

protected:
    /***************************************************************************/
    /**
	  * Allows derived classes to reset the default value. The function assumes
	  * that no important data is stored in par_, as its value will be overwritten
	  * as well. The reason is that configuration files will otherwise contain
	  * the "old" par_-value.
	  *
	  * @param def_val_arr The new default values, which also overwrite the current parameter values
	  */
    void resetDefault(std::array<parameter_type, N> const &def_val_arr) {
        def_val_arr_ = def_val_arr;
        par_arr_ = def_val_arr;
    }

    /***************************************************************************/
    std::array<parameter_type, N> def_val_arr_; ///< Holds default values
    std::array<parameter_type, N> par_arr_;     ///< Holds the parsed parameters

private:
    /***************************************************************************/
    /** @brief Loads this parameter's value from the parsed configuration document
     *  @param root The root JSON object of the parsed configuration */
    void load_from(boost::json::object const &root) override = 0;

    /** @brief Saves this parameter into the configuration document
     *  @param root The root JSON object being assembled */
    void save_to(boost::json::object &root) const override = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class wraps a std::array of values (obviously of identical type).
 * This class enforces a fixed number of items in the array.
 *
 * @tparam parameter_type The element type of the wrapped parameter array
 * @tparam N The fixed number of elements in the array
 */
template <typename parameter_type, std::size_t N>
class GFileArrayParsableParameterT : public GArrayParT<parameter_type, N> {
    // We want GParserBuilder to be able to call our load- and save functions
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * Initializes the parameters
	  *
	  * @param option_name_var The option name of this parameter
	  * @param comment_var The comment associated with this parameter
	  * @param def_val The default values used when the option is absent from the configuration file
	  * @param is_essential_var Whether this is an essential (true) or secondary (false) parameter
	  */
    GFileArrayParsableParameterT(
        std::string const &option_name_var,
        std::string const &comment_var,
        std::array<parameter_type, N> const &def_val,
        bool is_essential_var
    )
      : GArrayParT<parameter_type, N>(
            option_name_var,
            comment_var,
            def_val,
            is_essential_var
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * Initializes the parameters, except for comments
	  *
	  * @param option_name_var The option name of this parameter
	  * @param def_val The default values used when the option is absent from the configuration file
	  */
    GFileArrayParsableParameterT(
        std::string const &option_name_var,
        std::array<parameter_type, N> const &def_val
    )
      : GArrayParT<parameter_type, N>(
            option_name_var,
            std::string(),
            def_val,
            Gem::Common::VAR_IS_ESSENTIAL
        ) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GFileArrayParsableParameterT() override = default;

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GFileArrayParsableParameterT() = delete;
    GFileArrayParsableParameterT(GFileArrayParsableParameterT<parameter_type, N> const &) = delete;
    GFileArrayParsableParameterT(GFileArrayParsableParameterT<parameter_type, N> &&) = delete;
    GFileArrayParsableParameterT<parameter_type, N> &
    operator=(GFileArrayParsableParameterT<parameter_type, N> const &) = delete;
    GFileArrayParsableParameterT<parameter_type, N> &
    operator=(GFileArrayParsableParameterT<parameter_type, N> &&) = delete;

    /***************************************************************************/
    /**
	  * Allows to register a call-back function with this object
	  *
	  * @param call_back The function to be executed
	  */
    void registerCallBackFunction(
        std::move_only_function<void(std::array<parameter_type, N>)> call_back
    ) {
        if(not call_back) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileArrayParsableParameterT::registerCallBackFunction(): Error" << '\n'
                << "Tried to register an empty call-back function" << '\n'
            );
        }

        call_back_func_ = std::move(call_back);
    }

private:
    /***************************************************************************/
    /**
	  * Loads this parameter's value from the parsed configuration document
	  *
	  * @param root The root JSON object of the parsed configuration
	  */
    void load_from(boost::json::object const &root) override {
        auto const *value = detail::cfgEntryValue(root, GParsableI::optionName(0), true);
        if(value == nullptr) {
            return; // an absent key keeps the constructor-seeded defaults (a wrong form was refused)
        }
        auto const &values = value->get_array();
        for(std::size_t i = 0;
            i < GArrayParT<parameter_type, N>::par_arr_.size() && i < values.size();
            ++i) {
            GArrayParT<parameter_type, N>::par_arr_.at(i) =
                detail::cfgScalarFromString<parameter_type>(
                    detail::cfgValueToString(values[i]),
                    GParsableI::optionName(0)
                );
        }
        // Elements beyond the on-disk array length keep their defaults.
    }

    /***************************************************************************/
    /**
	  * Saves data to a property tree object, including comments. Default
	  * values are taken from the def_val_ vector.
	  *
	  * @param root The JSON object to which data should be saved
	  */
    void save_to(boost::json::object &root) const override {
        // Check that we have the right number of comments
        if(this->hasComments() && this->numberOfComments() != 1) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileArrayParsableParameterT<>::save_to(): Error!" << '\n'
                << "Expected 0 or 1 comment but got " << this->numberOfComments() << '\n'
            );
        }

        // Do some error checking
        if(GArrayParT<parameter_type, N>::def_val_arr_.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileArrayParsableParameterT::save_to(): Error!" << '\n'
                << "You need to provide at least one default value" << '\n'
            );
        }

        boost::json::object entry;
        if(this->hasComments()) {
            detail::cfgWriteComments(entry, GParsableI::splitComment(this->comment(0)));
        }

        boost::json::array default_arr;
        default_arr.reserve(GArrayParT<parameter_type, N>::def_val_arr_.size());
        for(auto const &def_val : GArrayParT<parameter_type, N>::def_val_arr_) {
            default_arr.emplace_back(detail::cfgScalarToJson(def_val));
        }

        boost::json::array value_arr;
        value_arr.reserve(GArrayParT<parameter_type, N>::par_arr_.size());
        for(auto const &value : GArrayParT<parameter_type, N>::par_arr_) {
            value_arr.emplace_back(detail::cfgScalarToJson(value));
        }

        entry["default"] = std::move(default_arr);
        entry["value"] = std::move(value_arr);
        root[GParsableI::optionName(0)] = std::move(entry);
    }

    /***************************************************************************/
    /**
	  * Executes a stored call-back function
	  */
    void executeCallBackFunction_() override {
        if(not call_back_func_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileArrayParsableParameterT::executeCallBackFunction_(): Error" << '\n'
                << "Tried to execute call-back function without a stored function" << '\n'
            );
        }

        // Execute the function
        call_back_func_(GArrayParT<parameter_type, N>::par_arr_);
    }

    /***************************************************************************/

    std::move_only_function<void(std::array<parameter_type, N>)>
        call_back_func_; ///< Holds the call-back function
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class specifies the interface of parsable command line parameters. Note
 * that this class cannot be copied; copy and move are explicitly deleted.
 */
class GCLParsableI : public GParsableI {
    // We want GParserBuilder to be able to call our private load- and save functions
    friend class GParserBuilder;

public:
    /** @brief A constructor for individual items */
    GCLParsableI(std::string const &option_name_var, std::string const &comment_var);
    /** @brief A constructor for vectors */
    GCLParsableI(
        std::vector<std::string> const &option_name_vec,
        std::vector<std::string> const &comment_vec
    );

    /** @brief The destructor */
    ~GCLParsableI() override = default;

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GCLParsableI() = delete;
    GCLParsableI(GCLParsableI const &) = delete;
    GCLParsableI(GCLParsableI &&) = delete;
    GCLParsableI &operator=(GCLParsableI const &) = delete;
    GCLParsableI &operator=(GCLParsableI &&) = delete;

protected:
    /** @brief Registers this option with a Boost.ProgramOptions options description
     *  @param desc The options description to which this option is added */
    virtual void save_to(boost::program_options::options_description &desc) const = 0;

    /** @brief Returns the content of this object as a std::string
     *  @return A human-readable representation of this option's name and value */
    [[nodiscard]] virtual std::string content() const = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class wraps a reference to individual command line parameters.
 *
 * @tparam parameter_type The type of the referenced command-line parameter
 */
template <typename parameter_type>
class GCLReferenceParsableParameterT // NOLINT(cppcoreguidelines-special-member-functions)
  : public GCLParsableI {
    // We want GParserBuilder to be able to call our private functions
    friend class GParserBuilder;

public:
    /***************************************************************************/
    /**
	  * A constructor that initializes the internal reference
	  *
	  * @param stored_reference The variable to which the parsed value will be assigned
	  * @param option_name_var The option name of this command-line parameter
	  * @param comment_var The comment associated with this command-line parameter
	  * @param def_val The default value used when the option is absent from the command line
	  * @param implicit_allowed Whether the option may be given without a value (e.g. --server vs --server=true)
	  * @param impl_val The implicit value used when only the option name is given
	  */
    GCLReferenceParsableParameterT(
        parameter_type &stored_reference,
        std::string const &option_name_var,
        std::string const &comment_var,
        parameter_type def_val,
        bool implicit_allowed,
        parameter_type impl_val
    )
      : GCLParsableI(
            GCLParsableI::makeVector(option_name_var),
            GCLParsableI::makeVector(comment_var)
        )
      , stored_reference_(stored_reference)
      , def_val_(std::move(def_val))
      , implicit_allowed_(implicit_allowed)
      , impl_val_(std::move(impl_val)) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * A constructor that initializes the internal variables, except for comments
	  *
	  * @param stored_reference The variable to which the parsed value will be assigned
	  * @param option_name_var The option name of this command-line parameter
	  * @param def_val The default value used when the option is absent from the command line
	  * @param implicit_allowed Whether the option may be given without a value (e.g. --server vs --server=true)
	  * @param impl_val The implicit value used when only the option name is given
	  */
    GCLReferenceParsableParameterT(
        parameter_type &stored_reference,
        std::string const &option_name_var,
        parameter_type def_val,
        bool implicit_allowed,
        parameter_type impl_val
    )
      : GCLParsableI(
            GCLParsableI::makeVector(option_name_var),
            GCLParsableI::makeVector(std::string())
        )
      , stored_reference_(stored_reference)
      , def_val_(std::move(def_val))
      , implicit_allowed_(implicit_allowed)
      , impl_val_(std::move(impl_val)) { /* nothing */
    }

    /***************************************************************************/
    // Prevent copying, moving and default construction
    GCLReferenceParsableParameterT() = delete;
    GCLReferenceParsableParameterT(GCLReferenceParsableParameterT<parameter_type> const &) = delete;
    GCLReferenceParsableParameterT(GCLReferenceParsableParameterT<parameter_type> &&) = delete;
    GCLReferenceParsableParameterT<parameter_type> &
    operator=(GCLReferenceParsableParameterT<parameter_type> const &) = delete;
    GCLReferenceParsableParameterT<parameter_type> &
    operator=(GCLReferenceParsableParameterT<parameter_type> &&) = delete;

private:
    /***************************************************************************/
    /**
	  * Registers this option with a Boost.ProgramOptions options description
	  *
	  * @param desc The options description to which this option is added
	  */
    void save_to(boost::program_options::options_description &desc) const override {
        namespace po = boost::program_options;
        if(GCL_IMPLICIT_ALLOWED == implicit_allowed_) {
            desc.add_options()(
                (this->optionName()).c_str(),
                po::value<parameter_type>(&stored_reference_)
                    ->implicit_value(impl_val_)
                    ->default_value(def_val_),
                (this->comment()).c_str()
            );
        }
        else { // GCL_IMPLICIT_NOT_ALLOWED
            desc.add_options()(
                (this->optionName()).c_str(),
                po::value<parameter_type>(&stored_reference_)->default_value(def_val_),
                (this->comment()).c_str()
            );
        }
    }

    /***************************************************************************/
    /**
	  * Returns the content of this object as a std::string
	  *
	  * @return A human-readable representation of this option's name and value
	  */
    [[nodiscard]] std::string content() const override {
        std::ostringstream result; // NOLINT(cppcoreguidelines-init-variables)
        result << this->optionName() << " :\t" << stored_reference_ << "\t"
               << ((stored_reference_ != def_val_) ? "default: " + Gem::Common::to_string(def_val_)
                                                   : std::string());
        return result.str();
    }

    /***************************************************************************/

    parameter_type
        &stored_reference_;  ///< Holds the reference to which the parsed value will be assigned
    parameter_type def_val_; ///< Holds the default value
    bool
        implicit_allowed_; ///< Indicates, whether implicit values (e.g. --server=true vs. --server) are allowed
    parameter_type impl_val_; ///< Holds an implicit value used if only the option name is given
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class implements a "parser builder", that allows to easily specify
 * the options that the parser should search for in a configuration file.
 * Results of the parsing process will be written directly into the supplied
 * variables. If not found, a default value will be used. Note that this
 * class assumes that the parameter_type can be streamed using operator<< or
 * operator>>
 */
class GParserBuilder {
public:
    /** @brief The default constructor */
    GParserBuilder();

    /** @brief The destructor */
    virtual ~GParserBuilder() = default;

    // Prevent copying and moving
    GParserBuilder(GParserBuilder const &) = delete;
    GParserBuilder(GParserBuilder &&) = delete;
    GParserBuilder &operator=(GParserBuilder const &) = delete;
    GParserBuilder &operator=(GParserBuilder &&) = delete;

    /** @brief Reads and parses a configuration file, applying the values to the registered options. A
     *  missing file is created from defaults. Optionally hands the parsed JSON document back via the
     *  second argument so callers can cache it. Throws if the file cannot be read or applied. */
    void parseConfigFile(
        std::filesystem::path const &config_file,
        boost::json::value *captured = nullptr
    );
    /** @brief Applies an already-parsed configuration document to the registered options (no file
     *  access); runs the optional unknown-key diagnostic. Throws if an entry has the wrong form or
     *  a value does not read as its type. */
    void loadFromDocument(
        boost::json::value const &root,
        std::filesystem::path const &config_path = {},
        bool run_unknown_key_check = true
    );
    /** @brief Writes out a configuration file */
    void writeConfigFile(
        std::filesystem::path const &config_file,
        std::string const &header = "",
        bool write_all = true
    ) const;
    /** @brief Update-in-place: parse @p config_file (if it exists) into the registered options, then rewrite
     *  it in canonical form -- stale keys (that no registered parameter consumes) dropped, existing values
     *  preserved, newly-registered parameters emitted with their defaults, comments/order refreshed. A
     *  missing file is created from defaults (same as parseConfigFile). The rewrite is atomic (a temp sibling
     *  is written and then renamed over the target), and never runs during a build into the source tree
     *  because it targets exactly the file it was given. Throws if the file cannot be read, applied or
     *  rewritten. */
    void updateConfigFile(std::filesystem::path const &config_file, std::string const &header = "");
    /** @brief Globally enables/disables update-in-place: when enabled, every successful parseConfigFile() that
     *  read an existing file also rewrites it in canonical form (mirrors setCheckUnknownKeys). Call once at startup. */
    static void setUpdateInPlace(bool enabled);
    /** @brief Retrieves whether update-in-place is globally enabled */
    static bool updateInPlace();
    /** @brief Globally enables/disables the creation-timestamp line in a generated config's header (default:
     *  enabled). Disable it to produce byte-stable, reproducible output -- e.g. for a config set that is
     *  checked into version control (the config-reference tree), where a changing timestamp would show as a
     *  spurious diff on every regeneration. Call once at startup. */
    static void setEmitTimestamp(bool enabled);
    /** @brief Retrieves whether the header creation-timestamp line is emitted */
    static bool emitTimestamp();
    /** @brief Globally enables/disables the unknown-configuration-key diagnostic (default: enabled; warns on config keys no registered parameter consumes). */
    static void setCheckUnknownKeys(bool enabled);
    /** @brief Retrieves whether the unknown-configuration-key diagnostic is enabled */
    static bool checkUnknownKeys();
    /** @brief Globally selects whether an unknown configuration-file key is an error (true) or a warning (false, the default). Only takes effect when the check is enabled. Call once at startup. */
    static void setUnknownKeyIsError(bool is_error);
    /** @brief Retrieves whether unknown configuration-file keys are treated as an error */
    static bool unknownKeyIsError();
    /** @brief Provides information on the number of file configuration options stored in this class */
    [[nodiscard]] std::size_t numberOfFileOptions() const;

    /** @brief Parses the commandline for options */
    bool parseCommandLine(int argc, char **argv, bool verbose = false);
    /** @brief Provides information on the number of command line configuration options stored in this class */
    [[nodiscard]] std::size_t numberOfCLOptions() const;

    /***************************************************************************/
    /**
	  * Allows to retrieve a GFileParsableI-derivative by name and to convert it to
	  * the derived type. This allows us to selectively change properties of these
	  * objects.
	  *
	  * @tparam file_parsable_derivative_type The concrete GFileParsableI-derived type to cast to
	  * @param option_name The first option name identifying the desired file parameter
	  * @return A shared pointer to the matching parameter cast to the requested type, or an empty pointer if none matches
	  */
    template <typename file_parsable_derivative_type>
    std::shared_ptr<file_parsable_derivative_type>
    file_at(std::string const &option_name) { // NOLINT(misc-unused-parameters)
        auto it = findProxyByName_(file_parameter_proxies_, option_name);
        if(it != file_parameter_proxies_.end()) {
            return std::dynamic_pointer_cast<file_parsable_derivative_type>(*it);
        }

        return {};
    }

    /////////////////////////////////////////////////////////////////////////////
    /***************************************************************************/
    /**
	  * Adds a single parameter of configurable type to the collection. When
	  * this parameter has been read using parseConfigFile, a call-back
	  * function is executed.
	  *
	  * @tparam parameter_type The type of the parameter being registered
	  * @param option_name The name of the option
	  * @param def_val A default value used if the parameter is absent from the configuration file
	  * @param call_back The function to be executed with the parsed value
	  * @param is_essential Whether this is an essential or a secondary parameter
	  * @param comment A comment to be associated with the parameter in configuration files
	  * @return A reference to the registered parameter proxy, to allow further configuration
	  */
    template <typename parameter_type>
    GParsableI &registerFileParameter(
        std::string const &option_name,
        const parameter_type &def_val,
        std::move_only_function<void(parameter_type)> call_back,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment = std::string()
    ) {
#ifdef DEBUG
        assertNotRegistered_(
            option_name,
            file_parameter_proxies_,
            "registerFileParameter(single_parm_ptr)"
        );
#endif /* DEBUG */

        // Always route through the full constructor: the comment may legitimately be empty, but the
        // caller's is_essential choice must never be dropped (the comment-less constructor hardcodes
        // VAR_IS_ESSENTIAL, which used to silently mark comment-less SECONDARY parameters essential).
        auto single_parm_ptr = std::make_shared<GFileSingleParsableParameterT<parameter_type>>(
            option_name,
            comment,
            is_essential,
            def_val
        );

        single_parm_ptr->registerCallBackFunction(std::move(call_back));

        // Add to the proxy store
        file_parameter_proxies_.push_back(single_parm_ptr);
        return *single_parm_ptr;
    }

    /***************************************************************************/
    /**
	  * Adds a parameter with a configurable type to the collection.
	  *
	  * @tparam parameter_type The type of the parameter being registered
	  * @param option_name The name of the option
	  * @param parameter The parameter into which the value will be written
	  * @param def_val A default value to be used if the corresponding parameter was not found in the configuration file
	  * @param is_essential A boolean which indicates whether this is an essential or a secondary parameter
	  * @param comment A comment to be associated with the parameter in configuration files
	  * @return A reference to the registered parameter proxy, to allow further configuration
	  */
    template <typename parameter_type>
    GParsableI &registerFileParameter(
        std::string const &option_name,
        parameter_type &parameter,
        parameter_type def_val,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment = std::string()
    ) {
        // Assigning to the caller's reference is just a special case of the callback proxy (the former
        // dedicated reference proxy byte-duplicated it apart from this one line). The reference must
        // outlive this parser builder, as it always had to.
        return registerFileParameter<parameter_type>(
            option_name,
            std::move(def_val),
            [&parameter](parameter_type v) { parameter = std::move(v); },
            is_essential,
            comment
        );
    }

    /***************************************************************************/
    /**
	  * Allows to reset default values. This is useful, if a derived class needs
	  * a different default value in configuration files. This function is meant
	  * to be called before any parsing takes place, as the par_-value will be
	  * overwritten as well.
	  *
	  * @tparam parameter_type The type of the parameter whose default is reset
	  * @param option_name The name of the option whose default value should be reset
	  * @param def_val The new default value
	  */
    template <typename parameter_type>
    void resetFileParameterDefaults(std::string const &option_name, parameter_type def_val) {
        // Retrieve the parameter object with this name
        std::shared_ptr<GSingleParmT<parameter_type>> const parm_object =
            file_at<GSingleParmT<parameter_type>>(option_name);

        // Check that we have indeed received an item
        if(not parm_object) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParameterObject::resetFileParameterDefaults(GSingleParmT): Error!" << '\n'
                << "Parameter object couldn't be found" << '\n'
            );
        }

        // Reset the default value
        parm_object->resetDefault(def_val);
    }

    /////////////////////////////////////////////////////////////////////////////
    /***************************************************************************/
    /**
	  * Adds two parameters of configurable types to the collection. When
	  * these parameters have been read using parseConfigFile, a call-back
	  * function will be executed.
	  *
	  * @tparam par1_type The type of the first parameter
	  * @tparam par2_type The type of the second parameter
	  * @param option_name1 The name of the first option
	  * @param option_name2 The name of the second option
	  * @param def_val1 The default value of the first parameter
	  * @param def_val2 The default value of the second parameter
	  * @param call_back The function to be executed with both parsed values
	  * @param combined_label The JSON group label under which both sub-options are nested
	  * @param is_essential Whether this is an essential or a secondary parameter
	  * @param comment1 A comment to be associated with the first parameter
	  * @param comment2 A comment to be associated with the second parameter
	  * @return A reference to the registered parameter proxy, to allow further configuration
	  */
    template <typename par1_type, typename par2_type>
    // NOLINTNEXTLINE(readability-function-size) -- combined two-parameter file registration needs both parameters' name/default/comment plus the shared call-back and label; the parameter count is the API, not splittable
    GParsableI &registerFileParameter(
        std::string const &option_name1,
        std::string const &option_name2,
        const par1_type &def_val1,
        const par2_type &def_val2,
        std::move_only_function<void(par1_type, par2_type)> call_back,
        std::string const &combined_label,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment1 = std::string(),
        std::string const &comment2 = std::string()
    ) {
#ifdef DEBUG
        assertNotRegistered_(
            option_name1,
            file_parameter_proxies_,
            "registerFileParameter(comb_parm_ptr)"
        );
#endif /* DEBUG */

        // Always route through the full constructor -- see registerFileParameter(single_parm_ptr):
        // empty comments must not drop the caller's is_essential choice.
        auto comb_parm_ptr =
            std::make_shared<GFileCombinedParsableParameterT<par1_type, par2_type>>(
                option_name1,
                comment1,
                def_val1,
                option_name2,
                comment2,
                def_val2,
                is_essential,
                combined_label
            );

        comb_parm_ptr->registerCallBackFunction(std::move(call_back));

        // Add to the proxy store
        file_parameter_proxies_.push_back(comb_parm_ptr);
        return *comb_parm_ptr;
    }

    /***************************************************************************/
    /**
	  * Allows to reset default values. This is useful, if a derived class needs
	  * a different default value in configuration files. Note that we only need
	  * the first option name here, but two default values. This function is meant
	  * to be called before any parsing takes place, as the par_-value will be
	  * overwritten as well.
	  *
	  * @tparam par1_type The type of the first parameter
	  * @tparam par2_type The type of the second parameter
	  * @param option_name1 The name of the first option, which identifies the combined parameter
	  * @param def_val1 The new default value for the first parameter
	  * @param def_val2 The new default value for the second parameter
	  */
    template <typename par1_type, typename par2_type>
    void resetFileParameterDefaults(
        std::string const &option_name1,
        par1_type def_val1,
        par2_type def_val2
    ) {
        // Retrieve the parameter object with this name
        std::shared_ptr<GCombinedParT<par1_type, par2_type>> const parm_object =
            file_at<GCombinedParT<par1_type, par2_type>>(option_name1);

        // Check that we have indeed received an item
        if(not parm_object) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParameterObject::resetFileParameterDefaults(GCombinedParT): Error!" << '\n'
                << "Parameter object couldn't be found" << '\n'
            );
        }

        // Reset the default value
        parm_object->resetDefault(def_val1, def_val2);
    }

    /////////////////////////////////////////////////////////////////////////////
    /***************************************************************************/
    /**
	  * Adds a vector of configurable type to the collection, using a
	  * call-back function
	  *
	  * @tparam parameter_type The element type of the parameter vector
	  * @param option_name The name of the option
	  * @param def_val The default values used if the parameter is absent from the configuration file
	  * @param call_back The function to be executed with the parsed vector
	  * @param is_essential Whether this is an essential or a secondary parameter
	  * @param comment A comment to be associated with the parameter in configuration files
	  * @return A reference to the registered parameter proxy, to allow further configuration
	  */
    template <typename parameter_type>
    GParsableI &registerFileParameter(
        std::string const &option_name,
        std::vector<parameter_type> const &def_val,
        std::move_only_function<void(std::vector<parameter_type>)> call_back,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment = std::string()
    ) {
#ifdef DEBUG
        assertNotRegistered_(
            option_name,
            file_parameter_proxies_,
            "registerFileParameter(vec_parm_ptr)"
        );
#endif /* DEBUG */

        // Always route through the full constructor -- see registerFileParameter(single_parm_ptr):
        // an empty comment must not drop the caller's is_essential choice.
        auto vec_parm_ptr = std::make_shared<GFileVectorParsableParameterT<parameter_type>>(
            option_name,
            comment,
            def_val,
            is_essential
        );

        vec_parm_ptr->registerCallBackFunction(std::move(call_back));

        // Add to the proxy store
        file_parameter_proxies_.push_back(vec_parm_ptr);
        return *vec_parm_ptr;
    }

    /***************************************************************************/
    /**
	  * Adds a reference to a vector of configurable type to the collection
	  *
	  * @tparam parameter_type The element type of the parameter vector
	  * @param option_name The name of the option
	  * @param stored_reference The vector to which the parsed values will be assigned
	  * @param def_val The default values used if the parameter is absent from the configuration file
	  * @param is_essential Whether this is an essential or a secondary parameter
	  * @param comment A comment to be associated with the parameter in configuration files
	  * @return A reference to the registered parameter proxy, to allow further configuration
	  */
    template <typename parameter_type>
    GParsableI &registerFileParameter(
        std::string const &option_name,
        std::vector<parameter_type> &stored_reference,
        std::vector<parameter_type> const &def_val,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment = std::string()
    ) {
        // Assigning to the caller's reference is just a special case of the callback proxy (the former
        // dedicated reference proxy byte-duplicated it apart from this one line).
        return registerFileParameter<parameter_type>(
            option_name,
            def_val,
            [&stored_reference](std::vector<parameter_type> v) { stored_reference = std::move(v); },
            is_essential,
            comment
        );
    }

    /***************************************************************************/
    /**
	  * Allows to reset default values. This is useful, if a derived class needs
	  * a different default value in configuration files. This function is meant
	  * to be called before any parsing takes place, as the par_-value will be
	  * overwritten as well.
	  *
	  * @tparam parameter_type The element type of the parameter vector
	  * @param option_name The name of the option whose default values should be reset
	  * @param def_val The new default values
	  */
    template <typename parameter_type>
    void resetFileParameterDefaults(
        std::string const &option_name,
        std::vector<parameter_type> const &def_val
    ) {
        // Retrieve the parameter object with this name
        std::shared_ptr<GVectorParT<parameter_type>> const parm_object =
            file_at<GVectorParT<parameter_type>>(option_name);

        // Check that we have indeed received an item
        if(not parm_object) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParameterObject::resetFileParameterDefaults(GVectorParT): Error!" << '\n'
                << "Parameter object couldn't be found" << '\n'
            );
        }

        // Reset the default value
        parm_object->resetDefault(def_val);
    }

    /////////////////////////////////////////////////////////////////////////////
    /***************************************************************************/
    /**
	  * Adds an array of configurable type but fixed size to the collection.
	  * This allows to make sure that a given amount of configuration options
	  * must be available.
	  *
	  * @tparam parameter_type The element type of the parameter array
	  * @tparam N The fixed number of elements in the array
	  * @param option_name The name of the option
	  * @param def_val The default values used if the parameter is absent from the configuration file
	  * @param call_back The function to be executed with the parsed array
	  * @param is_essential Whether this is an essential or a secondary parameter
	  * @param comment A comment to be associated with the parameter in configuration files
	  * @return A reference to the registered parameter proxy, to allow further configuration
	  */
    template <typename parameter_type, std::size_t N>
    GParsableI &registerFileParameter(
        std::string const &option_name,
        std::array<parameter_type, N> const &def_val,
        std::move_only_function<void(std::array<parameter_type, N>)> call_back,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment = std::string()
    ) {
#ifdef DEBUG
        assertNotRegistered_(
            option_name,
            file_parameter_proxies_,
            "registerFileParameter(array_parm_ptr)"
        );
#endif /* DEBUG */

        std::shared_ptr<GFileArrayParsableParameterT<parameter_type, N>> array_parm_ptr;

        // Always route through the full constructor -- see registerFileParameter(single_parm_ptr):
        // an empty comment must not drop the caller's is_essential choice.
        array_parm_ptr = std::make_shared<GFileArrayParsableParameterT<parameter_type, N>>(
            option_name,
            comment,
            def_val,
            is_essential
        );

        // Register the call back function
        array_parm_ptr->registerCallBackFunction(std::move(call_back));

        // Add to the proxy store
        file_parameter_proxies_.push_back(array_parm_ptr);
        return *array_parm_ptr;
    }

    /***************************************************************************/
    /**
	  * Adds a reference to an array of configurable type but fixed size
	  * to the file parameter collection
	  *
	  * @tparam parameter_type The element type of the parameter array
	  * @tparam N The fixed number of elements in the array
	  * @param option_name The name of the option
	  * @param stored_reference The array to which the parsed values will be assigned
	  * @param def_val The default values used if the parameter is absent from the configuration file
	  * @param is_essential Whether this is an essential or a secondary parameter
	  * @param comment A comment to be associated with the parameter in configuration files
	  * @return A reference to the registered parameter proxy, to allow further configuration
	  */
    template <typename parameter_type, std::size_t N>
    GParsableI &registerFileParameter(
        std::string const &option_name,
        std::array<parameter_type, N> &stored_reference,
        std::array<parameter_type, N> const &def_val,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment = std::string()
    ) {
        // Assigning to the caller's reference is just a special case of the callback proxy (the former
        // dedicated reference proxy byte-duplicated it apart from this one line).
        return registerFileParameter<parameter_type, N>(
            option_name,
            def_val,
            [&stored_reference](std::array<parameter_type, N> v) {
                stored_reference = std::move(v);
            },
            is_essential,
            comment
        );
    }

    /***************************************************************************/
    /**
	  * Allows to reset default values. This is useful, if a derived class needs
	  * a different default value in configuration files. This function is meant
	  * to be called before any parsing takes place, as the par_-value will be
	  * overwritten as well.
	  *
	  * @tparam parameter_type The element type of the parameter array
	  * @tparam N The fixed number of elements in the array
	  * @param option_name The name of the option whose default values should be reset
	  * @param def_val The new default values
	  */

    template <typename parameter_type, std::size_t N>
    void resetFileParameterDefaults(
        std::string const &option_name,
        std::array<parameter_type, N> const &def_val
    ) {
        // Retrieve the parameter object with this name
        std::shared_ptr<GArrayParT<parameter_type, N>> const parm_object =
            file_at<GArrayParT<parameter_type, N>>(option_name);

        // Check that we have indeed received an item
        if(not parm_object) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParameterObject::resetFileParameterDefaults(GArrayParT): Error!" << '\n'
                << "Parameter object couldn't be found" << '\n'
            );
        }

        // Reset the default value
        parm_object->resetDefault(def_val);
    }

    /////////////////////////////////////////////////////////////////////////////
    /***************************************************************************/
    /**
	  * Adds a reference to a configurable type to the command line parameters.
	  *
	  * @tparam parameter_type The type of the command-line parameter
	  * @param option_name The name of the option
	  * @param parameter The variable to which the parsed value will be assigned
	  * @param def_val The default value used if the option is absent from the command line
	  * @param comment A comment to be shown in the command-line help
	  * @param implicit_allowed Whether the option may be given without a value (e.g. --server vs --server=true)
	  * @param impl_val The implicit value used when only the option name is given
	  * @return A reference to the registered parameter proxy, to allow further configuration
	  */
    template <typename parameter_type>
    GParsableI &registerCLParameter(
        std::string const &option_name,
        parameter_type &parameter,
        parameter_type const &def_val,
        std::string const &comment = std::string(),
        bool implicit_allowed = GCL_IMPLICIT_NOT_ALLOWED,
        const parameter_type &impl_val = GDefaultValueT<parameter_type>::value()
    ) {
#ifdef DEBUG
        assertNotRegistered_(
            option_name,
            cl_parameter_proxies_,
            "registerCLParameter(ref_parm_ptr)"
        );
#endif /* DEBUG */

        std::shared_ptr<GCLReferenceParsableParameterT<parameter_type>> ref_parm_ptr;

        if(comment.empty()) {
            ref_parm_ptr = std::make_shared<GCLReferenceParsableParameterT<parameter_type>>(
                parameter,
                option_name,
                def_val,
                implicit_allowed,
                impl_val
            );
        }
        else {
            ref_parm_ptr = std::make_shared<GCLReferenceParsableParameterT<parameter_type>>(
                parameter,
                option_name,
                comment,
                def_val,
                implicit_allowed,
                impl_val
            );
        }

        // Add to the proxy store
        cl_parameter_proxies_.push_back(ref_parm_ptr);
        return *ref_parm_ptr;
    }

private:
    /***************************************************************************/
    /** @brief Locates the proxy registered under @p option_name, or the end iterator.
     *
     *  The one place that states how a proxy is identified: by its FIRST option name. Both users --
     *  the duplicate-registration guard below and file_at()'s retrieval -- ask exactly that
     *  question, and spelling the predicate twice is how the two would eventually come to disagree
     *  about it.
     *  @tparam proxy_vector_type The proxy-vector type (file or command-line proxies; const or not)
     *  @param proxies The proxy store to search
     *  @param option_name The (first) option name to look for
     *  @return An iterator to the matching proxy, or proxies.end() if there is none */
    template <typename proxy_vector_type>
    static auto findProxyByName_(proxy_vector_type &proxies, std::string const &option_name) {
        return std::ranges::find_if(proxies, [&](auto const &candidate_ptr) {
            return (candidate_ptr->GParsableI::optionName(0) == option_name);
        });
    }

    /***************************************************************************/
    /** @brief DEBUG-build helper shared by every registration overload: throws if an option of
     *  the given name was already registered in @p proxies (formerly an identical 15-line
     *  \#ifdef DEBUG block per overload).
     *  @tparam proxy_vector_type The proxy-vector type (file or command-line proxies)
     *  @param option_name The (first) option name about to be registered
     *  @param proxies The proxy store to check for a duplicate
     *  @param caller The registering overload, used in the error message */
    template <typename proxy_vector_type>
    static void assertNotRegistered_(
        std::string const &option_name,
        proxy_vector_type const &proxies,
        char const *caller
    ) {
        auto it = findProxyByName_(proxies, option_name);
        if(it != proxies.end()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParserBuilder::" << caller << ": Error!" << '\n'
                << "Parameter " << option_name << " has already been registered" << '\n'
            );
        }
    }

    /***************************************************************************/

    // Plain ordered vectors on purpose, not a keyed store: REGISTRATION ORDER IS THE CONTRACT --
    // it is the order options are emitted in a written configuration file, which a user reads and
    // diffs. A hash map would destroy it, and lookup is not the hot path (a handful of options,
    // consulted at registration and at retrieval, never in an iteration loop), so the linear
    // findProxyByName_() above is the right shape. Single-threaded by construction: a parser
    // builder belongs to the one object being configured (Inv 2 governs thread-safe primitives).
    std::vector<std::shared_ptr<GFileParsableI>>
        file_parameter_proxies_; ///< Holds file parameter proxies, in registration order
    std::vector<std::shared_ptr<GCLParsableI>>
        cl_parameter_proxies_; ///< Holds command line parameter proxies, in registration order

    std::filesystem::path config_base_dir_;

    /** @brief Shared implementation of parseConfigFile()/updateConfigFile(): reads @p config_file (creating
     *  it from defaults if absent), applies it to the registered options and, when @p do_rewrite is set and
     *  the file already existed, rewrites it in canonical form; throws on any failure. */
    void doParseConfigFile_(
        std::filesystem::path const &config_file,
        boost::json::value *captured,
        bool do_rewrite,
        std::string const &rewrite_header = ""
    );
    /** @brief Resolves @p config_file against the configured base directory (creating the directory if
     *  absent), creates the file from defaults if it does not yet exist, and validates an existing file
     *  (regular file, ".json" extension). Sets @p file_existed and returns the resolved path. */
    std::filesystem::path
    resolveConfigPathAndEnsureFile(std::filesystem::path const &config_file, bool &file_existed);
    /** @brief Update-in-place rewrite: reports on-disk keys no registered parameter consumes, then rewrites
     *  @p config_path in canonical form (using @p root's current values / @p rewrite_header). */
    void reportAndRewriteConfigFile(
        boost::json::value const &root,
        std::filesystem::path const &config_path,
        std::string const &rewrite_header
    );
    /** @brief Builds the canonical configuration document (header + one entry per registered file
     *  option: value = the current parsed/default value, default = the registered default). Shared by
     *  writeConfigFile() and the update-in-place rewrite. */
    [[nodiscard]] boost::json::value
    buildConfigDocument_(std::string const &header, bool write_all) const;
    /** @brief Atomically replaces @p config_file with the pretty-printed serialization of @p document
     *  (writes a temp sibling, then renames it over the target), bypassing the deliberate no-overwrite
     *  guard of writeConfigFile() -- the update path is the one caller that legitimately overwrites a file. */
    void atomicReplaceConfigFile_(
        std::filesystem::path const &config_file,
        boost::json::value const &document
    ) const;

    static std::mutex
        configfile_parser_mutex_; ///< Synchronization of access to configuration files (may only happen serially)
    static bool
        unknown_key_is_error_; ///< If true, an unknown configuration-file key throws instead of warning (default: false)
    static bool
        check_unknown_keys_; ///< If true, a genuine config-file parse warns about keys no registered parameter consumes (default: true; group-aware, runs once per parse)
    static bool
        update_in_place_; ///< If true, a successful parseConfigFile() that read an existing file also rewrites it in canonical form (default: false)
    static bool
        emit_timestamp_; ///< If true, a generated config's header carries a creation timestamp (default: true; disable for byte-stable, version-controlled output)
};

/******************************************************************************/
} /* namespace Gem::Common */
