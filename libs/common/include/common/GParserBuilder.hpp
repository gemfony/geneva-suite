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
/** @brief Parses an on-disk scalar string back into its parameter type (inverse of
 *  cfgScalarToString). Booleans accept both "true"/"false" and "1"/"0".
 *  @tparam T The scalar parameter type
 *  @param s The string to parse
 *  @return The parsed value */
template <typename T>
T cfgScalarFromString(std::string const &s) {
    if constexpr(std::is_same_v<T, std::string>) {
        return s;
    }
    else if constexpr(std::is_same_v<T, bool>) {
        return (s == "true" || s == "1");
    }
    else if constexpr(std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t>) {
        // (u)int8_t is a character type: a stream extraction of "1" would read the character '1'
        // (value 49), not the number 1. Parse through a wider integer so small enums/counts stored in
        // an 8-bit field round-trip by value.
        using wider = std::conditional_t<std::is_signed_v<T>, int, unsigned int>;
        return static_cast<T>(Gem::Common::from_string<wider>(s));
    }
    else {
        return Gem::Common::from_string<T>(s);
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
/** @brief Returns the object stored under @p key of @p parent, or nullptr if the key is absent or
 *  does not hold an object.
 *  @param parent The enclosing JSON object
 *  @param key The member key to look up
 *  @return A pointer to the child object, or nullptr */
inline boost::json::object const *
cfgChildObject(boost::json::object const &parent, std::string const &key) {
    auto const *v = parent.if_contains(key);
    if(v == nullptr || not v->is_object()) { return nullptr; }
    return &v->get_object();
}

/******************************************************************************/
/** @brief Returns the array stored under @p key of @p parent, or nullptr if the key is absent or
 *  does not hold an array. A value written in the historical dup-key object form therefore reads as
 *  "absent" here, so the caller falls back to the registered defaults.
 *  @param parent The enclosing JSON object
 *  @param key The member key to look up
 *  @return A pointer to the child array, or nullptr */
inline boost::json::array const *
cfgChildArray(boost::json::object const &parent, std::string const &key) {
    auto const *v = parent.if_contains(key);
    if(v == nullptr || not v->is_array()) { return nullptr; }
    return &v->get_array();
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
        for(auto const &line : lines) { arr.emplace_back(line); }
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
    /** @brief Enforce setting of the comment level
     *  @param comment_level The id of the comment to be selected inside GParsableI */
    explicit commentLevel(std::size_t cl);

    /*************************************************************************/
    // Defaulted or deleted functions functions

    commentLevel() = delete;

    commentLevel(commentLevel const &) = default;
    commentLevel(commentLevel &&) = delete; // enforce explicit settinf of comment level

    commentLevel &operator=(commentLevel const &) = default;
    commentLevel &operator=(commentLevel &&) = delete; // enforce explicit settinf of comment level

    /*************************************************************************/

    /** @brief Retrieves the current commentLevel
     *  @return The stored comment level id */
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
    /** @brief A constructor for individual items
     *  @param option_name The single option name of this parameter
     *  @param comment The single comment associated with this parameter */
    GParsableI(std::string const &option_name_var, std::string const &comment_var);

    /** @brief A constructor for vectors
     *  @param option_names The list of option names of this parameter
     *  @param comments The list of comments associated with this parameter */
    GParsableI(std::vector<std::string> const &option_name_vec, std::vector<std::string> const &comment_vec);

    /** @brief The destructor */
    virtual ~GParsableI() = default;

    // Prevent copying, moving and default construction
    GParsableI() = delete;
    GParsableI(GParsableI const &) = delete;
    GParsableI(GParsableI &&) = delete;
    GParsableI &operator=(GParsableI const &) = delete;
    GParsableI &operator=(GParsableI &&) = delete;

    /** @brief Retrieves the option name at a given position
     *  @param pos The index of the option name to retrieve (defaults to the first)
     *  @return The option name stored at the given position */
    [[nodiscard]] std::string optionName(std::size_t pos = 0) const;
    /** @brief Retrieves the comment that was assigned to this variable at a given position
     *  @param pos The index of the comment to retrieve (defaults to the first)
     *  @return The comment stored at the given position */
    [[nodiscard]] std::string comment(std::size_t pos = 0) const;
    /** @brief Checks whether comments have indeed been registered
     *  @return true if at least one non-empty comment is stored, false otherwise */
    [[nodiscard]] bool hasComments() const;
    /** @brief Retrieves the number of comments available
     *  @return The number of comment entries stored */
    [[nodiscard]] std::size_t numberOfComments() const;
    /** @brief Retrieves the number of option names registered for this parameter
     *  @return The number of option-name entries stored */
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
    /** @brief Needed for std::ostringstream
     *  @param val A stream manipulator (such as std::endl) to apply to the current comment
     *  @return A reference to this object, to allow chaining */
    GParsableI &operator<<(std::ostream &(*val)(std::ostream &));
    /** @brief Needed for std::ostringstream
     *  @param val An std::ios manipulator to apply to the current comment
     *  @return A reference to this object, to allow chaining */
    GParsableI &operator<<(std::ios &(*val)(std::ios &));
    /** @brief Needed for std::ostringstream
     *  @param val An std::ios_base manipulator to apply to the current comment
     *  @return A reference to this object, to allow chaining */
    GParsableI &operator<<(std::ios_base &(*val)(std::ios_base &));
    /** @brief Allows to indicate the current comment level
     *  @param cl A commentLevel manipulator selecting the comment entry to append to
     *  @return A reference to this object, to allow chaining */
    GParsableI &operator<<(commentLevel const &cl);
    /** @brief Allows to switch to the next comment level
     *  @param nc A nextComment manipulator that advances to the next comment entry
     *  @return A reference to this object, to allow chaining */
    GParsableI &operator<<(nextComment const &nc);

protected:
    /***************************************************************************/
    /** @brief Splits a comment into sub-tokens
     *  @param comment The comment string to split into sub-tokens
     *  @return The list of sub-tokens extracted from the comment */
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
    /** @brief A constructor for individual items
     *  @param option_name The single option name of this parameter
     *  @param comment The single comment associated with this parameter
     *  @param is_essential Whether this is an essential (true) or secondary (false) parameter */
    GFileParsableI(std::string const &option_name_var, std::string const &comment_var, bool is_essential_var);
    /** @brief A constructor for vectors
     *  @param option_names The list of option names of this parameter
     *  @param comments The list of comments associated with this parameter
     *  @param is_essential Whether this is an essential (true) or secondary (false) parameter */
    GFileParsableI(std::vector<std::string> const &option_name_vec, std::vector<std::string> const &comment_vec, bool is_essential_var);

    /** @brief The destructor */
    ~GFileParsableI() override = default;

    // Prevent copying, moving and default construction
    GFileParsableI() = delete;
    GFileParsableI(GFileParsableI const &) = delete;
    GFileParsableI(GFileParsableI &&) = delete;
    GFileParsableI &operator=(GFileParsableI const &) = delete;
    GFileParsableI &operator=(GFileParsableI &&) = delete;

    /** @brief Checks whether this is an essential variable at a given position
     *  @return true if this parameter is essential, false if it is secondary */
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
        if(auto const *entry = detail::cfgChildObject(root, GParsableI::optionName(0))) {
            if(auto const *v = entry->if_contains("value")) {
                GSingleParmT<parameter_type>::par_ =
                    detail::cfgScalarFromString<parameter_type>(detail::cfgValueToString(*v));
            }
        }
        // An absent key keeps the value seeded from the default in the constructor.
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
 * @tparam par_type0 The type of the first combined parameter
 * @tparam par_type1 The type of the second combined parameter
 */
template <typename par_type0, typename par_type1>
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
        par_type0 const &def_val0,
        std::string const &option_name_var1,
        std::string const &comment_var1,
        par_type1 const &def_val1,
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
    GCombinedParT(GCombinedParT<par_type0, par_type1> const &) = delete;
    GCombinedParT(GCombinedParT<par_type0, par_type1> &&) = delete;
    GCombinedParT<par_type0, par_type1> &
    operator=(GCombinedParT<par_type0, par_type1> const &) = delete;
    GCombinedParT<par_type0, par_type1> &operator=(GCombinedParT<par_type0, par_type1> &&) = delete;

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
    void resetDefault(par_type0 const &def_val0, par_type1 const &def_val1) {
        def_val0_ = def_val0;
        def_val1_ = def_val1;
        par0_ = def_val0;
        par1_ = def_val1;
    }

    /***************************************************************************/
    par_type0 par0_, def_val0_; ///< Holds the individual parameters and default values 0
    par_type1 par1_, def_val1_; ///< Holds the individual parameters and default values 1

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
 * @tparam par_type0 The type of the first combined parameter
 * @tparam par_type1 The type of the second combined parameter
 */
template <typename par_type0, typename par_type1>
class GFileCombinedParsableParameterT : public GCombinedParT<par_type0, par_type1> {
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
        par_type0 const &def_val0,
        std::string const &option_name_var1,
        std::string const &comment_var1,
        par_type1 const &def_val1,
        bool is_essential_var,
        std::string const &combined_label
    )
      : GCombinedParT<par_type0, par_type1>(
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
        par_type0 const &def_val0,
        std::string const &option_name_var1,
        par_type1 const &def_val1,
        std::string const &combined_label
    )
      : GCombinedParT<par_type0, par_type1>(
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
    GFileCombinedParsableParameterT(GFileCombinedParsableParameterT<par_type0, par_type1> const &) =
        delete;
    GFileCombinedParsableParameterT(GFileCombinedParsableParameterT<par_type0, par_type1> &&) =
        delete;
    GFileCombinedParsableParameterT<par_type0, par_type1> &
    operator=(GFileCombinedParsableParameterT<par_type0, par_type1> const &) = delete;
    GFileCombinedParsableParameterT<par_type0, par_type1> &
    operator=(GFileCombinedParsableParameterT<par_type0, par_type1> &&) = delete;

    /***************************************************************************/
    /**
	  * Allows to register a call-back function with this object
	  *
	  * @param call_back The function to be executed
	  */
    void registerCallBackFunction(std::move_only_function<void(par_type0, par_type1)> call_back) {
        if(not call_back) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileCombinedParsableParameterT::registerCallBackFunction(): Error"
                << '\n'
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
        auto const *group = detail::cfgChildObject(
            root, GCombinedParT<par_type0, par_type1>::combined_label_
        );
        if(group == nullptr) {
            return; // absent group keeps both defaults seeded in the constructor
        }
        if(auto const *e0 = detail::cfgChildObject(*group, GParsableI::optionName(0))) {
            if(auto const *v = e0->if_contains("value")) {
                GCombinedParT<par_type0, par_type1>::par0_ =
                    detail::cfgScalarFromString<par_type0>(detail::cfgValueToString(*v));
            }
        }
        if(auto const *e1 = detail::cfgChildObject(*group, GParsableI::optionName(1))) {
            if(auto const *v = e1->if_contains("value")) {
                GCombinedParT<par_type0, par_type1>::par1_ =
                    detail::cfgScalarFromString<par_type1>(detail::cfgValueToString(*v));
            }
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
        entry0["default"] =
            detail::cfgScalarToJson(GCombinedParT<par_type0, par_type1>::def_val0_);
        entry0["value"] = detail::cfgScalarToJson(GCombinedParT<par_type0, par_type1>::par0_);

        boost::json::object entry1;
        if(this->hasComments()) {
            detail::cfgWriteComments(entry1, GParsableI::splitComment(this->comment(1)));
        }
        entry1["default"] =
            detail::cfgScalarToJson(GCombinedParT<par_type0, par_type1>::def_val1_);
        entry1["value"] = detail::cfgScalarToJson(GCombinedParT<par_type0, par_type1>::par1_);

        boost::json::object group;
        group[GParsableI::optionName(0)] = std::move(entry0);
        group[GParsableI::optionName(1)] = std::move(entry1);
        root[GCombinedParT<par_type0, par_type1>::combined_label_] = std::move(group);
    }

    /***************************************************************************/
    /**
	  * Executes a stored call-back function
	  */
    void executeCallBackFunction_() override {
        if(not call_back_func_) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileCombinedParsableParameterT::executeCallBackFunction_(): Error"
                << '\n'
                << "Tried to execute call-back function without a stored function" << '\n'
            );
        }

        // Execute the function
        call_back_func_(
            GCombinedParT<par_type0, par_type1>::par0_,
            GCombinedParT<par_type0, par_type1>::par1_
        );
    }

    /***************************************************************************/

    std::move_only_function<void(par_type0, par_type1)> call_back_func_; ///< Holds the call-back function
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
        par_cnt_     = def_val;
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
    void registerCallBackFunction(std::move_only_function<void(std::vector<parameter_type>)> call_back) {
        if(not call_back) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GFileVectorParsableParameterT::registerCallBackFunction(): Error"
                << '\n'
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
        auto const *entry = detail::cfgChildObject(root, GParsableI::optionName(0));
        if(entry == nullptr) {
            // The key is absent from the file -- e.g. a newly-registered vector parameter, or an
            // update-in-place pass over a config written before this parameter existed. Keep the
            // defaults par_cnt_ was seeded with in the constructor.
            return;
        }
        auto const *values = detail::cfgChildArray(*entry, "value");
        if(values == nullptr) {
            // The "value" key is absent (or, for a config in the historical dup-key object form,
            // does not read back as an array). Keep the seeded defaults.
            return;
        }

        // The values are present: replace the seeded defaults with the on-disk values.
        GVectorParT<parameter_type>::par_cnt_.clear();
        for(auto const &v : *values) {
            GVectorParT<parameter_type>::par_cnt_.push_back(
                detail::cfgScalarFromString<parameter_type>(detail::cfgValueToString(v))
            );
        }
    }

    /***************************************************************************/
    /**
	  * Saves data to a property tree object, including comments. Default
	  * values are taken from the def_val_ vector, which may be empty (it is
	  * then written out as an empty "default"/"value" array).
	  *
	  * @param pt The object to which data should be saved
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
                << "In GFileVectorParsableParameterT::executeCallBackFunction_(): Error"
                << '\n'
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
    void registerCallBackFunction(std::move_only_function<void(std::array<parameter_type, N>)> call_back) {
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
        auto const *entry = detail::cfgChildObject(root, GParsableI::optionName(0));
        if(entry == nullptr) {
            return; // absent key keeps the constructor-seeded defaults
        }
        auto const *values = detail::cfgChildArray(*entry, "value");
        if(values == nullptr) {
            return;
        }
        for(std::size_t i = 0;
            i < GArrayParT<parameter_type, N>::par_arr_.size() && i < values->size();
            ++i) {
            GArrayParT<parameter_type, N>::par_arr_.at(i) =
                detail::cfgScalarFromString<parameter_type>(detail::cfgValueToString((*values)[i]));
        }
        // Elements beyond the on-disk array length keep their defaults.
    }

    /***************************************************************************/
    /**
	  * Saves data to a property tree object, including comments. Default
	  * values are taken from the def_val_ vector.
	  *
	  * @param pt The object to which data should be saved
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
    /** @brief A constructor for individual items
     *  @param option_name The single option name of this command-line parameter
     *  @param comment The single comment associated with this command-line parameter */
    GCLParsableI(std::string const &option_name_var, std::string const &comment_var);
    /** @brief A constructor for vectors
     *  @param option_names The list of option names of this command-line parameter
     *  @param comments The list of comments associated with this command-line parameter */
    GCLParsableI(std::vector<std::string> const &option_name_vec, std::vector<std::string> const &comment_vec);

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
               << ((stored_reference_ != def_val_)
                       ? "default: " + Gem::Common::to_string(def_val_)
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
     *  missing file is created from defaults (also a success). Optionally hands the parsed JSON document
     *  back via the second argument so callers can cache it.
     *  @param config_file The path of the configuration file to read and parse
     *  @param captured Optional output pointer; if non-null, receives a copy of the parsed JSON document for caching
     *  @return true on success (the file was parsed, or a missing one was created from its defaults);
     *          false if an error occurred (the exception is logged and swallowed) */
    bool parseConfigFile(std::filesystem::path const &config_file, boost::json::value *captured = nullptr);
    /** @brief Applies an already-parsed configuration document to the registered options (no file access); runs the optional unknown-key diagnostic.
     *  @param root The already-parsed JSON document to apply to the registered options
     *  @param config_path The originating file path, used only for diagnostic messages (may be empty)
     *  @param run_unknown_key_check Whether to run the unknown-key diagnostic for this load */
    void loadFromDocument(
        boost::json::value const &root,
        std::filesystem::path const &config_path = {},
        bool run_unknown_key_check = true
    );
    /** @brief Writes out a configuration file
     *  @param config_file The path of the configuration file to write
     *  @param header A header comment to be placed at the top of the file
     *  @param write_all Whether to also write secondary (non-essential) options */
    void
    writeConfigFile(std::filesystem::path const &config_file, std::string const &header = "", bool write_all = true) const;
    /** @brief Update-in-place: parse @p config_file (if it exists) into the registered options, then rewrite
     *  it in canonical form -- stale keys (that no registered parameter consumes) dropped, existing values
     *  preserved, newly-registered parameters emitted with their defaults, comments/order refreshed. A
     *  missing file is created from defaults (same as parseConfigFile). The rewrite is atomic (a temp sibling
     *  is written and then renamed over the target), and never runs during a build into the source tree
     *  because it targets exactly the file it was given.
     *  @param config_file The path of the configuration file to update in place
     *  @param header An optional header comment; empty uses the standard auto-created header
     *  @return true on success (the file was updated, or a missing one was created from its defaults);
     *          false if an error occurred (the exception is logged and swallowed) */
    bool updateConfigFile(std::filesystem::path const &config_file, std::string const &header = "");
    /** @brief Globally enables/disables update-in-place: when enabled, every successful parseConfigFile() that
     *  read an existing file also rewrites it in canonical form (mirrors setCheckUnknownKeys). Call once at startup.
     *  @param enabled Whether parseConfigFile should rewrite the files it reads */
    static void setUpdateInPlace(bool enabled);
    /** @brief Retrieves whether update-in-place is globally enabled
     *  @return true if parseConfigFile rewrites the files it reads, false otherwise */
    static bool updateInPlace();
    /** @brief Globally enables/disables the creation-timestamp line in a generated config's header (default:
     *  enabled). Disable it to produce byte-stable, reproducible output -- e.g. for a config set that is
     *  checked into version control (the config-reference tree), where a changing timestamp would show as a
     *  spurious diff on every regeneration. Call once at startup.
     *  @param enabled Whether the header should carry the creation timestamp */
    static void setEmitTimestamp(bool enabled);
    /** @brief Retrieves whether the header creation-timestamp line is emitted
     *  @return true if a generated config's header carries the creation timestamp, false otherwise */
    static bool emitTimestamp();
    /** @brief Globally enables/disables the unknown-configuration-key diagnostic (default: enabled; warns on config keys no registered parameter consumes).
     *  @param check Whether the unknown-key diagnostic should be enabled */
    static void setCheckUnknownKeys(bool enabled);
    /** @brief Retrieves whether the unknown-configuration-key diagnostic is enabled
     *  @return true if the unknown-key diagnostic is enabled, false otherwise */
    static bool checkUnknownKeys();
    /** @brief Globally selects whether an unknown configuration-file key is an error (true) or a warning (false, the default). Only takes effect when the check is enabled. Call once at startup.
     *  @param is_error Whether an unknown key should throw (true) instead of warning (false) */
    static void setUnknownKeyIsError(bool is_error);
    /** @brief Retrieves whether unknown configuration-file keys are treated as an error
     *  @return true if unknown keys are treated as an error, false if they only warn */
    static bool unknownKeyIsError();
    /** @brief Provides information on the number of file configuration options stored in this class
     *  @return The number of registered file configuration options */
    [[nodiscard]] std::size_t numberOfFileOptions() const;

    /** @brief Parses the commandline for options
     *  @param argc The number of command-line arguments
     *  @param argv The array of command-line argument strings
     *  @param verbose Whether to print the parsed options to the log
     *  @return true if parsing succeeded and execution should continue, false if help was requested */
    bool parseCommandLine(int argc, char **argv, bool verbose = false);
    /** @brief Provides information on the number of command line configuration options stored in this class
     *  @return The number of registered command-line configuration options */
    [[nodiscard]] std::size_t numberOfCLOptions() const;

    /***************************************************************************/
    /**
	  * Allows to retrieve a GFileParsableI-derivative by name and to convert it to
	  * the derived type. This allows us to selectively change properties of these
	  * objects.
	  *
	  * @tparam fileParsableDerivative The concrete GFileParsableI-derived type to cast to
	  * @param option_name The first option name identifying the desired file parameter
	  * @return A shared pointer to the matching parameter cast to the requested type, or an empty pointer if none matches
	  */
    template <typename fileParsableDerivative>
    std::shared_ptr<fileParsableDerivative>
    file_at(std::string const &option_name) { // NOLINT(misc-unused-parameters)
        auto it = findProxyByName_(file_parameter_proxies_, option_name);
        if(it != file_parameter_proxies_.end()) {
            return std::dynamic_pointer_cast<fileParsableDerivative>(*it);
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
        const parameter_type& def_val,
        std::move_only_function<void(parameter_type)> call_back,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment = std::string()
    ) {
#ifdef DEBUG
        assertNotRegistered_(option_name, file_parameter_proxies_, "registerFileParameter(single_parm_ptr)");
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
                << "In GParameterObject::resetFileParameterDefaults(GSingleParmT): Error!"
                << '\n'
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
	  * @tparam par_type1 The type of the first parameter
	  * @tparam par_type2 The type of the second parameter
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
    template <typename par_type1, typename par_type2>
    // NOLINTNEXTLINE(readability-function-size) -- combined two-parameter file registration needs both parameters' name/default/comment plus the shared call-back and label; the parameter count is the API, not splittable
    GParsableI &registerFileParameter(
        std::string const &option_name1,
        std::string const &option_name2,
        const par_type1& def_val1,
        const par_type2& def_val2,
        std::move_only_function<void(par_type1, par_type2)> call_back,
        std::string const &combined_label,
        bool is_essential = Gem::Common::VAR_IS_ESSENTIAL,
        std::string const &comment1 = std::string(),
        std::string const &comment2 = std::string()
    ) {
#ifdef DEBUG
        assertNotRegistered_(option_name1, file_parameter_proxies_, "registerFileParameter(comb_parm_ptr)");
#endif /* DEBUG */

        // Always route through the full constructor -- see registerFileParameter(single_parm_ptr):
        // empty comments must not drop the caller's is_essential choice.
        auto comb_parm_ptr = std::make_shared<GFileCombinedParsableParameterT<par_type1, par_type2>>(
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
	  * @tparam par_type1 The type of the first parameter
	  * @tparam par_type2 The type of the second parameter
	  * @param option_name1 The name of the first option, which identifies the combined parameter
	  * @param def_val1 The new default value for the first parameter
	  * @param def_val2 The new default value for the second parameter
	  */
    template <typename par_type1, typename par_type2>
    void resetFileParameterDefaults(
        std::string const &option_name1,
        par_type1 def_val1,
        par_type2 def_val2
    ) {
        // Retrieve the parameter object with this name
        std::shared_ptr<GCombinedParT<par_type1, par_type2>> const parm_object =
            file_at<GCombinedParT<par_type1, par_type2>>(option_name1);

        // Check that we have indeed received an item
        if(not parm_object) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParameterObject::resetFileParameterDefaults(GCombinedParT): Error!"
                << '\n'
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
        assertNotRegistered_(option_name, file_parameter_proxies_, "registerFileParameter(vec_parm_ptr)");
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
                << "In GParameterObject::resetFileParameterDefaults(GVectorParT): Error!"
                << '\n'
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
        assertNotRegistered_(option_name, file_parameter_proxies_, "registerFileParameter(array_parm_ptr)");
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
            [&stored_reference](std::array<parameter_type, N> v) { stored_reference = std::move(v); },
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
                << "In GParameterObject::resetFileParameterDefaults(GArrayParT): Error!"
                << '\n'
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
        const parameter_type& impl_val = GDefaultValueT<parameter_type>::value()
    ) {
#ifdef DEBUG
        assertNotRegistered_(option_name, cl_parameter_proxies_, "registerCLParameter(ref_parm_ptr)");
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
     *  #ifdef DEBUG block per overload).
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
     *  the file already existed, rewrites it in canonical form.
     *  @param config_file The configuration file to read (and, when do_rewrite, rewrite)
     *  @param captured Optional output pointer receiving the parsed JSON document, or nullptr
     *  @param do_rewrite Whether to rewrite the file in canonical form after reading it
     *  @param rewrite_header The header for a rewrite; empty uses the standard auto-created header
     *  @return true if the file already existed, false if it had to be created from defaults */
    bool doParseConfigFile_(
        std::filesystem::path const &config_file,
        boost::json::value *captured,
        bool do_rewrite,
        std::string const &rewrite_header = ""
    );
    /** @brief Resolves @p config_file against the configured base directory (creating the directory if
     *  absent), creates the file from defaults if it does not yet exist, and validates an existing file
     *  (regular file, ".json" extension). Sets @p file_existed and returns the resolved path. */
    std::filesystem::path resolveConfigPathAndEnsureFile(
        std::filesystem::path const &config_file,
        bool &file_existed
    );
    /** @brief Update-in-place rewrite: reports on-disk keys no registered parameter consumes, then rewrites
     *  @p config_path in canonical form (using @p root's current values / @p rewrite_header). */
    void reportAndRewriteConfigFile(
        boost::json::value const &root,
        std::filesystem::path const &config_path,
        std::string const &rewrite_header
    );
    /** @brief Builds the canonical configuration document (header + one entry per registered file
     *  option: value = the current parsed/default value, default = the registered default). Shared by
     *  writeConfigFile() and the update-in-place rewrite.
     *  @param header The header comment (split on ';' into individual comment lines)
     *  @param write_all Whether to also emit non-essential options
     *  @return The assembled JSON document (a JSON object at the root) */
    [[nodiscard]] boost::json::value buildConfigDocument_(std::string const &header, bool write_all) const;
    /** @brief Atomically replaces @p config_file with the pretty-printed serialization of @p document
     *  (writes a temp sibling, then renames it over the target), bypassing the deliberate no-overwrite
     *  guard of writeConfigFile() -- the update path is the one caller that legitimately overwrites a file.
     *  @param config_file The target configuration file (.json) to replace
     *  @param document The JSON document to serialize */
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
