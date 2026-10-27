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

#include "common/GParserBuilder.hpp"

// Standard library headers used directly in this translation unit
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <ios>
#include <iostream>
#include <mutex>
#include <ostream>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Other Geneva headers whose symbols are used directly
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GJsonIO.hpp"
#include "common/GLogger.hpp"

// Boost headers used directly in this translation unit
#include <boost/json.hpp>
#include <boost/program_options/errors.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

namespace Gem::Common {

/******************************************************************************/
/**
 * Initialization of static data members
 */
std::mutex Gem::Common::GParserBuilder::configfile_parser_mutex_;
bool Gem::Common::GParserBuilder::unknown_key_is_error_ = false;
bool Gem::Common::GParserBuilder::check_unknown_keys_ = true;
bool Gem::Common::GParserBuilder::update_in_place_ = false;
bool Gem::Common::GParserBuilder::emit_timestamp_ = true;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The standard constructor of the comment level.
 *
 * @param cl The comment level (index into a parameter's comment vector) this object represents
 */
commentLevel::commentLevel(std::size_t cl)
  : comment_level_(cl) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Retrieves the current comment level.
 *
 * @return The comment level held by this object
 */
std::size_t commentLevel::getCommentLevel() const {
    return comment_level_;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A constructor for individual items.
 *
 * @param option_name_var The single option name for this parsable item
 * @param comment_var The single comment describing this parsable item
 */
GParsableI::GParsableI(std::string const &option_name_var, std::string const &comment_var)
  : option_name_(GParsableI::makeVector(option_name_var))
  , comment_(GParsableI::makeVector(comment_var))
  , cl_(0) { /* nothing */
}

/******************************************************************************/
/**
 * @brief A constructor for vectors of option names and comments.
 *
 * @param option_name_vec The vector of option names for this parsable item
 * @param comment_vec The vector of comments (one per comment level) describing this parsable item
 */
GParsableI::GParsableI(
    std::vector<std::string> const &option_name_vec,
    std::vector<std::string> const &comment_vec
)
  : option_name_(option_name_vec)
  , comment_(comment_vec)
  , cl_(0) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Retrieves the option name at the given position.
 *
 * @param pos The index into the option-name vector (defaults are defined in the header)
 * @return The option name stored at position @p pos
 */
std::string GParsableI::optionName(std::size_t pos) const {
    if(option_name_.size() <= pos) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParsableI::optionName(std::size_t): Error!" << '\n'
            << "Tried to access item at position " << pos << '\n'
            << "where the size of the vector is " << option_name_.size() << '\n'
        );
    }

    return option_name_.at(pos);
}

/******************************************************************************/
/**
 * @brief Retrieves the comment that was assigned to this variable.
 *
 * @param pos The index into the comment vector (i.e. the comment level)
 * @return The comment string stored at position @p pos
 */
std::string GParsableI::comment(std::size_t pos) const {
    if(comment_.size() <= pos) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParsableI::comment(std::size_t): Error!" << '\n'
            << "Tried to access item at position " << pos << '\n'
            << "where the size of the vector is " << comment_.size() << '\n'
        );
    }

    return comment_.at(pos);
}

/******************************************************************************/
/**
 * @brief Checks whether comments have indeed been registered.
 *
 * @return true if at least one comment is registered, false otherwise
 */
bool GParsableI::hasComments() const {
    return not comment_.empty();
}

/******************************************************************************/
/**
 * @brief Retrieves the number of comments available.
 *
 * @return The number of registered comments (comment levels)
 */
std::size_t GParsableI::numberOfComments() const {
    return comment_.size();
}

/******************************************************************************/
/**
 * @brief Retrieves the number of option names registered for this parameter.
 *
 * @return The number of registered option names
 */
std::size_t GParsableI::numberOfOptionNames() const {
    return option_name_.size();
}

/******************************************************************************/
/**
 * @brief Appends a std::ostream manipulator to the comment at the current level.
 *
 * Needed for ostringstream.
 *
 * @param val A std::ostream manipulator function (e.g. std::endl) to be appended to the current comment
 * @return A reference to this object (to allow chaining)
 */
GParsableI &GParsableI::operator<<(std::ostream &(*val)(std::ostream &)) {
    std::ostringstream oss; // NOLINT(cppcoreguidelines-init-variables)
    oss << val;
    comment_.at(cl_) += oss.str();
    return *this;
}

/******************************************************************************/
/**
 * @brief Appends a std::ios manipulator to the comment at the current level.
 *
 * Needed for ostringstream.
 *
 * @param val A std::ios manipulator function to be appended to the current comment
 * @return A reference to this object (to allow chaining)
 */
GParsableI &GParsableI::operator<<(std::ios &(*val)(std::ios &)) {
    std::ostringstream oss; // NOLINT(cppcoreguidelines-init-variables)
    oss << val;
    comment_.at(cl_) += oss.str();
    return *this;
}

/******************************************************************************/
/**
 * @brief Appends a std::ios_base manipulator to the comment at the current level.
 *
 * Needed for ostringstream.
 *
 * @param val A std::ios_base manipulator function to be appended to the current comment
 * @return A reference to this object (to allow chaining)
 */
GParsableI &GParsableI::operator<<(std::ios_base &(*val)(std::ios_base &)) {
    std::ostringstream oss; // NOLINT(cppcoreguidelines-init-variables)
    oss << val;
    comment_.at(cl_) += oss.str();
    return *this;
}

/******************************************************************************/
/**
 * @brief Allows to indicate the current comment level.
 *
 * Subsequent streamed text is appended to the comment at the selected level.
 *
 * @param cl A commentLevel object carrying the comment level to switch to
 * @return A reference to this object (to allow chaining)
 */
GParsableI &GParsableI::operator<<(commentLevel const &cl) {
#ifdef DEBUG
    if(comment_.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParsableI::operator<< (commentLevel const& cl): Error!" << '\n'
            << "No comments in vector" << '\n'
        );
    }

    if(comment_.size() <= cl.getCommentLevel()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParsableI::operator<< (commentLevel const& cl): Error!" << '\n'
            << "Invalid comment level " << cl.getCommentLevel()
            << " requested, where the maximum is " << comment_.size() - 1 << '\n'
        );
    }
#endif /* DEBUG */

    cl_ = cl.getCommentLevel();
    return *this;
}

/******************************************************************************/
/**
 * @brief Allows to switch to the next comment level.
 *
 * Increments the current comment level by one; subsequent streamed text is
 * appended to the comment at the new level.
 *
 * @param nc A nextComment tag object that triggers the level increment (unused)
 * @return A reference to this object (to allow chaining)
 */
GParsableI &GParsableI::operator<<([[maybe_unused]] nextComment const & nc) {
#ifdef DEBUG
    if(comment_.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParsableI::operator<< (nextComment const& nc): Error!" << '\n'
            << "No comments in vector" << '\n'
        );
    }

    if(comment_.size() <= (cl_ + 1)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParsableI::operator<< (nextComment const& nc): Error!" << '\n'
            << "Invalid comment level " << cl_ + 1 << " requested, where the maximum is "
            << comment_.size() - 1 << '\n'
        );
    }
#endif /* DEBUG */

    // Increment the comment level
    cl_++;
    return *this;
}

/******************************************************************************/
/**
 * @brief Splits a comment into sub-tokens.
 *
 * The comment will be split in case of newlines and semicolons.
 *
 * @param comment The comment string to be split (the literal "empty" and an empty string yield no tokens)
 * @return A vector of the individual sub-tokens obtained from @p comment
 */
std::vector<std::string> GParsableI::splitComment(std::string const &comment) {
    std::vector<std::string> results;

    if(not comment.empty() && comment != "empty") {
        // Split according to newlines, then break each line into sub-tokens at semicolons
        std::istringstream buffer(comment);
        std::string line; // NOLINT(cppcoreguidelines-init-variables)
        while(std::getline(buffer, line)) {
            for(auto const &t : Gem::Common::splitString(line, ";")) {
                results.push_back(t);
            }
        }
    }

    return results;
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A constructor for individual items.
 *
 * @param option_name_var The single option name for this file-parsable item
 * @param comment_var The single comment describing this file-parsable item
 * @param is_essential_var Whether this is an essential parameter (always written to the config file)
 */
GFileParsableI::GFileParsableI(
    std::string const &option_name_var,
    std::string const &comment_var,
    bool is_essential_var
)
  : GParsableI(option_name_var, comment_var)
  , is_essential_(is_essential_var) { /* nothing */
}

/******************************************************************************/
/**
 * @brief A constructor for vectors of option names and comments.
 *
 * @param option_name_vec The vector of option names for this file-parsable item
 * @param comment_vec The vector of comments (one per comment level) describing this file-parsable item
 * @param is_essential_var Whether this is an essential parameter (always written to the config file)
 */
GFileParsableI::GFileParsableI(
    std::vector<std::string> const &option_name_vec,
    std::vector<std::string> const &comment_vec,
    bool is_essential_var
)
  : GParsableI(option_name_vec, comment_vec)
  , is_essential_(is_essential_var) { /* nothing */
}

/******************************************************************************/
/**
 * @brief Checks whether this is an essential variable.
 *
 * @return true if this parameter is essential, false otherwise
 */
bool GFileParsableI::isEssential() const {
    return is_essential_;
}

/******************************************************************************/
/** @brief Executes a stored callbacl function */
void GFileParsableI::executeCallBackFunction() {
    executeCallBackFunction_();
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A constructor for individual items
 */
GCLParsableI::GCLParsableI(std::string const &option_name_var, std::string const &comment_var)
  : GParsableI(option_name_var, comment_var) { /* nothing */
}

/******************************************************************************/
/**
 * A constructor for vectors
 */
GCLParsableI::GCLParsableI(
    std::vector<std::string> const &option_name_vec,
    std::vector<std::string> const &comment_vec
)
  : GParsableI(option_name_vec, comment_vec) { /* nothing */
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * The default constructor
 */
GParserBuilder::GParserBuilder() {
    auto basename_opt = Gem::Common::environmentVariableAs<std::string>("GENEVA_CONFIG_BASENAME");
    if(basename_opt && !(*basename_opt).empty()) {
        // Read out the directory
        config_base_dir_ = std::filesystem::path(*basename_opt);
    }
}

/******************************************************************************/
/**
 * Applies an already-parsed configuration document to the registered options,
 * without touching the file. Runs the optional unknown-key diagnostic. This is
 * the "apply" half of parseConfigFile, separated so that callers (e.g. GFactoryT)
 * can read + parse a config file once and re-apply the cached document to many
 * freshly created objects.
 */
void GParserBuilder::loadFromDocument(
    boost::json::value const &root,
    std::filesystem::path const &config_path,
    bool run_unknown_key_check
) {
    // A configuration document is always a JSON object at the root. Anything else is malformed.
    if(not root.is_object()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParserBuilder::loadFromDocument(): Error!" << '\n'
            << "Configuration file " << config_path.string() << '\n'
            << "does not contain a JSON object at its root." << '\n'
        );
    }
    boost::json::object const &obj = root.get_object();

    // Diagnostic: detect configuration-file keys that no registered parameter
    // consumes (config/code drift -- a renamed or stale key). Runs only on a
    // genuine file parse (run_unknown_key_check == true): GFactoryT re-applies a
    // cached document to every produced object, and the check must not re-run per
    // object.
    if(run_unknown_key_check && check_unknown_keys_) {
        std::set<std::string> known_keys;
        for(auto const &proxy_ptr : file_parameter_proxies_) {
            // Each parameter reports the top-level JSON key it occupies; combined
            // parameters nest their sub-options under a single group label, so the
            // label -- not the sub-option names -- is the valid top-level key.
            known_keys.insert(proxy_ptr->topLevelConfigKey());
        }
        for(auto const &key_value : obj) {
            const std::string key(key_value.key());
            if(key == "header" || known_keys.contains(key)) {
                continue;
            }
            if(unknown_key_is_error_) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GParserBuilder::loadFromDocument(): Error!" << '\n'
                    << "Configuration file " << config_path.string() << '\n'
                    << "contains the unknown key \"" << key
                    << "\" that no registered parameter consumes." << '\n'
                    << "This usually indicates config/code drift (a renamed or stale key)." << '\n'
                );
            }
            glogger << "In GParserBuilder::loadFromDocument(): Warning!" << '\n'
                    << "Configuration file " << config_path.string() << '\n'
                    << "contains the unknown key \"" << key
                    << "\" that no registered parameter consumes; it will be ignored." << '\n'
                    << GLOGGING;
        }
    }

    // Load the data into our objects and execute the relevant call-back functions
    for(auto const &proxy_ptr : file_parameter_proxies_) {
        proxy_ptr->load_from(obj);
        proxy_ptr->executeCallBackFunction();
    }
}

/******************************************************************************/
/**
 * Shared implementation of parseConfigFile() and updateConfigFile(): reads @p config_file (creating it
 * from defaults if absent), applies its values to the registered options and, when @p do_rewrite is set
 * and the file already existed, rewrites it in canonical form (stale keys dropped, existing values kept,
 * new keys defaulted). The whole operation is serialized by the config-file mutex.
 *
 * @param config_file The path to the configuration file that should be parsed (and, when do_rewrite, rewritten)
 * @param captured An optional pointer to a JSON document that, if non-null, receives the parsed options
 * @param do_rewrite Whether to rewrite the file in canonical form after applying it
 * @param rewrite_header The header for a rewrite; empty uses the standard auto-created header
 * @return true on success (parsed or created without error), false if an exception was caught
 */
bool GParserBuilder::doParseConfigFile_(
    std::filesystem::path const &config_file,
    boost::json::value *captured,
    bool do_rewrite,
    std::string const &rewrite_header
) {
    // Make sure only one entity is parsed at once. This allows us to
    // concurrently create e.g. optimization algorithms, letting them
    // parse the same config file. The update-in-place rewrite happens under
    // the same lock, so it is serialized against every other config access.
    std::scoped_lock const lk(GParserBuilder::configfile_parser_mutex_);

    boost::json::value root; // holds the parsed configuration document

    std::filesystem::path config_path;

    try {
        // Resolve the path (creating the base directory if needed) and ensure the file exists.
        bool file_existed = false;
        config_path = this->resolveConfigPathAndEnsureFile(config_file, file_existed);

        // Parse the configuration document (Boost.JSON's parser does not accept a path directly).
        root = Gem::Common::parseJsonFile(config_path);

        // Optionally hand the parsed document back to the caller (e.g. GFactoryT) so
        // it can cache it and avoid re-reading + re-parsing the file on every
        // produce() call.
        if(captured != nullptr) {
            *captured = root;
        }

        // Apply the parsed values to the registered options. Factored out so a
        // cached document can be re-applied without touching the file again. In update mode the
        // unknown-key diagnostic is suppressed -- stale keys are dropped by design and reported
        // (below) at info level instead of warned about.
        this->loadFromDocument(root, config_path, /* run_unknown_key_check = */ not do_rewrite);

        // Update-in-place: having applied the on-disk values to the registered options (absent keys
        // fell back to their defaults), rewrite the file in canonical form. Only an existing file is
        // rewritten -- a missing one was just created from defaults above, which is already canonical.
        if(do_rewrite && file_existed) {
            this->reportAndRewriteConfigFile(root, config_path, rewrite_header);
        }

        return true; // Success! (a caller reads this as "parsed/created without error", not "existed")
    }
    catch(geneva_exception const &e) {
        glogger << "Caught geneva_exception when parsing configuration file "
                << config_path.string() << ":" << '\n'
                << e.what() << '\n'
                << GLOGGING;
        return false;
    }
    catch(std::exception const &e) {
        glogger << "Caught std::exception when parsing configuration file " << config_path.string()
                << ":" << '\n'
                << e.what() << '\n'
                << GLOGGING;
        return false;
    }
    catch(...) {
        glogger << "Unknown error while parsing the configuration file " << config_path.string()
                << '\n'
                << GLOGGING;
        return false;
    }
}

/******************************************************************************/
/**
 * @brief Resolves @p config_file against the configured base directory (creating the directory if absent),
 * creates the file from defaults if it does not yet exist, and validates an existing file (regular file,
 * ".json" extension). Sets @p file_existed and returns the resolved path.
 */
std::filesystem::path GParserBuilder::resolveConfigPathAndEnsureFile(
    std::filesystem::path const &config_file,
    bool &file_existed
) {
    std::filesystem::path config_path;

    // Assemble a path object from the config file, possibly adding a base directory
    if(not config_base_dir_.empty()) {
        // Make sure the base directory exists, creating it if necessary. As a missing config
        // file is auto-created below, a missing config directory must be created too, so the
        // mechanism works in a fresh run directory regardless of which caller runs first.
        if(not std::filesystem::exists(config_base_dir_)) {
            std::error_code ec;
            std::filesystem::create_directories(config_base_dir_, ec);
            if(ec) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GParserBuilder::parseConfigFile(): Error!" << '\n'
                    << "Base-directory " << config_base_dir_.string()
                    << " does not exist and could not be created: " << ec.message() << '\n'
                );
            }
            glogger << "Note: In GParserBuilder::parseConfigFile():" << '\n'
                    << "The configuration directory " << config_base_dir_.string()
                    << " did not exist and was created for you." << '\n'
                    << GLOGGING;
        }
        else if(not std::filesystem::is_directory(config_base_dir_)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParserBuilder::parseConfigFile(): Error!" << '\n'
                << "Base-directory " << config_base_dir_.string() << " is not a directory"
                << '\n'
            );
        }

        config_path = config_base_dir_ / config_file;
    }
    else {
        config_path = config_file;
    }

    // Check that the configuration file exists.
    // If not, create a default version
    file_existed = std::filesystem::exists(config_path);
    if(not file_existed) {
        glogger << "Note: In GParserBuilder::parseConfigFile():" << '\n'
                << "Configuration file " << config_path.string() << " does not exist."
                << '\n'
                << "We will try to create a file with default values for you." << '\n'
                << GLOGGING;

        std::string const header =
            "This configuration file was automatically created by GParserBuilder;";
        this->writeConfigFile(
            config_path,
            header,
            true // write_all == true
        );
    }
    else { // config_file exists
        // Is it a regular file ?
        if(not std::filesystem::is_regular_file(config_path)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParserBuilder::parseConfigFile(): Error!" << '\n'
                << config_path.string() << " exists but is no regular file." << '\n'
            );
        }

        // We require the file to have the json extension
        if(not std::filesystem::path(config_path).has_extension() ||
           std::filesystem::path(config_path).extension() != ".json") {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParserBuilder::parseConfigFile(): Error!" << '\n'
                << config_path.string() << " does not have the required extension \".json\""
                << '\n'
            );
        }
    }

    return config_path;
}

/******************************************************************************/
/**
 * @brief Update-in-place rewrite: reports on-disk keys that no registered parameter consumes, then
 * rewrites @p config_path in canonical form (header from @p rewrite_header, values from the registered
 * options that were just loaded from @p root).
 */
void GParserBuilder::reportAndRewriteConfigFile(
    boost::json::value const &root,
    std::filesystem::path const &config_path,
    std::string const &rewrite_header
) {
    // Report which on-disk keys are being dropped because no registered parameter consumes them.
    std::set<std::string> known_keys;
    for(auto const &proxy_ptr : file_parameter_proxies_) {
        known_keys.insert(proxy_ptr->topLevelConfigKey());
    }
    std::vector<std::string> dropped;
    if(root.is_object()) {
        for(auto const &key_value : root.get_object()) {
            const std::string key(key_value.key());
            if(key != "header" and not known_keys.contains(key)) {
                dropped.push_back(key);
            }
        }
    }
    if(not dropped.empty()) {
        glogger << "Note: In GParserBuilder::reportAndRewriteConfigFile(): " << config_path.string() << '\n'
                << "dropping " << dropped.size()
                << " stale key(s) no registered parameter consumes: "
                << (dropped | std::views::join_with(std::string(", ")) | std::ranges::to<std::string>())
                << '\n'
                << GLOGGING;
    }

    std::string const header =
        rewrite_header.empty()
            ? std::string("This configuration file was automatically created by GParserBuilder;")
            : rewrite_header;
    this->atomicReplaceConfigFile_(config_path, this->buildConfigDocument_(header, true /* write_all */));
}

/******************************************************************************/
/**
 * Reads and parses a configuration file, applying the values to the registered options. When
 * update-in-place is globally enabled (setUpdateInPlace(true)), an existing file it reads is also
 * rewritten in canonical form.
 *
 * @param config_file The path of the configuration file to read and parse
 * @param captured An optional pointer to a JSON document that, if non-null, receives the parsed options
 * @return true on success, false if an exception was caught
 */
bool GParserBuilder::parseConfigFile(std::filesystem::path const &config_file, boost::json::value *captured) {
    return this->doParseConfigFile_(config_file, captured, /* do_rewrite = */ update_in_place_);
}

/******************************************************************************/
/**
 * Update-in-place: parse @p config_file (if it exists) into the registered options and rewrite it in
 * canonical form -- stale keys dropped, existing values preserved, newly-registered parameters defaulted,
 * comments/order refreshed. Unconditional (independent of the global update-in-place flag). A missing file
 * is created from defaults. The rewrite is atomic.
 *
 * @param config_file The path of the configuration file to update in place
 * @param header An optional header comment; empty uses the standard auto-created header
 * @return true on success, false if an exception was caught
 */
bool GParserBuilder::updateConfigFile(std::filesystem::path const &config_file, std::string const &header) {
    return this->doParseConfigFile_(config_file, nullptr, /* do_rewrite = */ true, header);
}

/******************************************************************************/
/**
 * Globally enables (true) or disables (false, the default) update-in-place. When enabled, every successful
 * parseConfigFile() that read an existing file also rewrites it in canonical form. Intended to be called
 * once at program startup (mirrors setCheckUnknownKeys()).
 *
 * @param enabled Whether parseConfigFile should rewrite the files it reads
 */
void GParserBuilder::setUpdateInPlace(bool enabled) {
    update_in_place_ = enabled;
}

/******************************************************************************/
/**
 * Retrieves whether update-in-place is globally enabled.
 *
 * @return true if parseConfigFile rewrites the files it reads, false otherwise
 */
bool GParserBuilder::updateInPlace() {
    return update_in_place_;
}

/******************************************************************************/
/**
 * Globally enables (true, the default) or disables (false) the creation-timestamp line in a generated
 * config's header. Disabling it produces byte-stable, reproducible output for a config set that is checked
 * into version control (the config-reference tree), where a changing timestamp would otherwise show as a
 * spurious diff on every regeneration. Intended to be called once at program startup.
 *
 * @param enabled Whether the header should carry the creation timestamp
 */
void GParserBuilder::setEmitTimestamp(bool enabled) {
    emit_timestamp_ = enabled;
}

/******************************************************************************/
/**
 * Retrieves whether the header creation-timestamp line is emitted.
 *
 * @return true if a generated config's header carries the creation timestamp, false otherwise
 */
bool GParserBuilder::emitTimestamp() {
    return emit_timestamp_;
}

/******************************************************************************/
/**
 * Writes out a configuration file.
 *
 * @param config_file The name of the configuration file to be written
 * @param header A descriptive comment to be output at the top of the configuration file
 * @param write_all A boolean parameter that indicates whether all or only essential parameters should be written
 */
void GParserBuilder::writeConfigFile(
    std::filesystem::path const &config_file,
    std::string const &header,
    bool write_all
) const {
    // Do some error checking
    {
        // Is config_file a directory ?
        if(std::filesystem::is_directory(config_file)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParserBuilder::writeConfigFile(): Error!" << '\n'
                << config_file.string() << " is a directory." << '\n'
            );
        }

        // We do not allow to overwrite existing files
        if(std::filesystem::exists(config_file) && std::filesystem::is_regular_file(config_file)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParserBuilder::writeConfigFile(): Error!" << '\n'
                << "You have specified an existing file (" << config_file.string() << ")." << '\n'
            );
        }

        // Make sure the target directory exists, creating it if necessary. We auto-create a
        // missing config FILE here (this is the "does not exist" path of parseConfigFile), so we
        // must also create a missing config DIRECTORY -- otherwise, on a fresh run directory with
        // no config/ folder, writing the very first config (e.g. Go2's own) would fail and abort
        // the whole program via GTERMINATION before any config could be created.
        const std::filesystem::path target_dir = std::filesystem::path(config_file).remove_filename();
        if(not target_dir.empty()) {
            if(std::filesystem::exists(target_dir)) {
                if(not std::filesystem::is_directory(target_dir)) {
                    throw geneva_exception(
                        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                        << "In GParserBuilder::writeConfigFile(): Error!" << '\n'
                        << "The target path " << target_dir.string() << " is not a directory."
                        << '\n'
                    );
                }
            }
            else {
                std::error_code ec;
                std::filesystem::create_directories(target_dir, ec);
                if(ec) {
                    throw geneva_exception(
                        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                        << "In GParserBuilder::writeConfigFile(): Error!" << '\n'
                        << "Could not create the target directory " << target_dir.string() << ": "
                        << ec.message() << '\n'
                    );
                }
                glogger << "Note: In GParserBuilder::writeConfigFile():" << '\n'
                        << "The configuration directory " << target_dir.string()
                        << " did not exist and was created for you." << '\n'
                        << GLOGGING;
            }
        }

        // Check that the configuration file has the required extension
        if(not config_file.has_extension() || config_file.extension() != ".json") {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParserBuilder::writeConfigFile(): Error!" << '\n'
                << config_file.string() << " does not have the required extension \".json\"" << '\n'
            );
        }
    }

    // Open the required configuration file
    std::ofstream ofs(config_file);
    if(not ofs) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParserBuilder::writeConfigFile(): Error writing configuration file "
            << config_file.string() << '\n'
        );
    }

    // Assemble the canonical document (shared with the update-in-place rewrite) and write it out.
    Gem::Common::prettyPrintJson(ofs, this->buildConfigDocument_(header, write_all));
    ofs << '\n';

    // Close the file handle
    ofs.close();
}

/******************************************************************************/
/**
 * Builds the canonical configuration property tree: a header (the given header split on ';' into comment
 * lines, plus a creation timestamp) followed by one entry per registered file option, each carrying its
 * comment, registered default and current value. Shared by writeConfigFile() and the update-in-place
 * rewrite so both emit byte-identical structure.
 *
 * @param header A header comment to place at the top of the file (split on ';')
 * @param write_all Whether to also emit non-essential options
 * @return The assembled JSON document (a JSON object at the root)
 */
boost::json::value GParserBuilder::buildConfigDocument_(std::string const &header, bool write_all) const {
    if(file_parameter_proxies_.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParserBuilder::buildConfigDocument_(): No variables found!" << '\n'
        );
    }

    boost::json::object root;

    // Output a header comment array (one line per element). Insertion order is preserved, so the
    // header is emitted first, ahead of every parameter.
    boost::json::array header_lines;
    if(not header.empty()) {
        // Break the header into individual tokens
        for(auto const &h : Gem::Common::splitString(header, ";")) {
            header_lines.emplace_back(h);
        }
    }
    // The creation timestamp is omitted when byte-stable output is requested (e.g. a version-controlled
    // config-reference tree, where a changing timestamp would be a spurious diff on every regeneration).
    if(emit_timestamp_) {
        header_lines.emplace_back(Gem::Common::currentTimeAsString());
    }
    root["header"] = boost::json::object{{"comment", std::move(header_lines)}};

    // Output variables and values
    for(auto const &v_ptr : file_parameter_proxies_) {
        // Only write out the parameter(s) if they are either essential or it
        // has been requested to write out all parameters regardless
        if(not write_all && not v_ptr->isEssential()) {
            continue;
        }

        // Output the actual data of this parameter object to the document
        v_ptr->save_to(root);
    }

    return boost::json::value(std::move(root));
}

/******************************************************************************/
/**
 * Atomically replaces @p config_file with the serialization of @p document: writes a temporary sibling
 * file and then renames it over the target. This is the one caller that legitimately overwrites an
 * existing configuration file (writeConfigFile() deliberately refuses to), so an interrupted update can
 * never truncate a live config, and the .json extension is enforced here too.
 *
 * @param config_file The target configuration file (.json) to replace
 * @param document The JSON document to serialize
 */
void GParserBuilder::atomicReplaceConfigFile_(
    std::filesystem::path const &config_file,
    boost::json::value const &document
) const {
    // Enforce the .json extension (matches writeConfigFile()/parseConfigFile()).
    if(not config_file.has_extension() || config_file.extension() != ".json") {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParserBuilder::atomicReplaceConfigFile_(): Error!" << '\n'
            << config_file.string() << " does not have the required extension \".json\"" << '\n'
        );
    }

    // Write to a temporary sibling (same directory -> same filesystem, so the rename is atomic).
    std::filesystem::path tmp_path = config_file;
    tmp_path += ".tmp";
    {
        std::ofstream ofs(tmp_path);
        if(not ofs) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GParserBuilder::atomicReplaceConfigFile_(): Error writing temporary file "
                << tmp_path.string() << '\n'
            );
        }
        Gem::Common::prettyPrintJson(ofs, document);
        ofs << '\n';
    } // ofs flushed and closed here, before the rename

    std::error_code ec;
    std::filesystem::rename(tmp_path, config_file, ec);
    if(ec) {
        std::error_code ignore;
        std::filesystem::remove(tmp_path, ignore);
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GParserBuilder::atomicReplaceConfigFile_(): Error!" << '\n'
            << "Could not rename " << tmp_path.string() << " over " << config_file.string() << ": "
            << ec.message() << '\n'
        );
    }
}

/******************************************************************************/
/**
 * Provides information on the number of configuration options stored in this class
 *
 * @return The number of configuration options stored in this class
 */
std::size_t GParserBuilder::numberOfFileOptions() const {
    return file_parameter_proxies_.size();
}

/******************************************************************************/
/**
 * Globally enables (true, the default) or disables (false) the diagnostic
 * that detects unknown configuration-file keys. Intended to be called
 * once at program startup.
 */
void GParserBuilder::setCheckUnknownKeys(bool enabled) {
    check_unknown_keys_ = enabled;
}

/******************************************************************************/
/**
 * Retrieves whether the unknown-configuration-key diagnostic is enabled
 */
bool GParserBuilder::checkUnknownKeys() {
    return check_unknown_keys_;
}

/******************************************************************************/
/**
 * Globally selects whether an unknown configuration-file key is treated as an
 * error (true) or merely a warning (false, the default). Only takes effect when
 * the check is enabled via setCheckUnknownKeys(true).
 */
void GParserBuilder::setUnknownKeyIsError(bool is_error) {
    unknown_key_is_error_ = is_error;
}

/******************************************************************************/
/**
 * Retrieves whether unknown configuration-file keys are treated as an error
 */
bool GParserBuilder::unknownKeyIsError() {
    return unknown_key_is_error_;
}

/******************************************************************************/
/**
 * Parses the command line for options
 *
 * @param argc The argument count
 * @param argv The argument vector
 * @param verbose If set to true, the function will emit information about the parsed parameters
 * @return A boolean indicating whether help was requested (true) or not (false)
 */
bool GParserBuilder::parseCommandLine(int argc, char **argv, bool verbose) {
    namespace po = boost::program_options;

    bool result = GCL_NO_HELP_REQUESTED;

    // The options description is needed both for parsing and for the usage message printed on a
    // command-line error, so it is built outside the try block below.
    std::string const usage_string = std::string("Usage: ") + argv[0] + " [options]";
    po::options_description desc(usage_string);

    // We always want --help and -h to be available
    desc.add_options()("help,h", "Emit help message");

    // Add further options from the parameter objects
    for(auto const &p_ptr : cl_parameter_proxies_) {
        p_ptr->save_to(desc);
    }

    try {
        // Do the actual parsing
        po::variables_map vm;
        po::store(po::parse_command_line(argc, (const char *const *)argv, desc), vm);
        po::notify(vm);

        // Emit a help message, if necessary and let the caller of this function know
        if(vm.contains("help")) {
            std::cout << desc << '\n';
            result = GCL_HELP_REQUESTED;
        }
        else {
            if(verbose) {
                std::cout << "GParserBuilder::parseCommandLine():" << '\n'
                          << "Working with the following options:" << '\n';
                for(auto const &p_ptr : cl_parameter_proxies_) {
                    std::cout << p_ptr->content() << '\n';
                }
                std::cout << '\n';
            }
        }
    }
    catch(po::error const &e) {
        // A malformed command line is a USER error, not an internal fault. Report it together with
        // the usage and exit cleanly with a non-zero status via LOGEXIT -- rather than
        // std::terminate()-ing via GTERMINATION (which would dump core on a mere CLI typo).
        std::ostringstream usage; // NOLINT(cppcoreguidelines-init-variables)
        usage << desc;
        glogger << "Error on the command line: " << e.what() << '\n'
                << '\n'
                << usage.str()
                << LOGEXIT(EXIT_FAILURE);
    }
    catch(...) {
        // Anything other than a program_options error here is an unexpected INTERNAL fault, so the
        // hard stop is kept.
        glogger << "In GParserBuilder::parseCommandLine(int argc, char **argv):" << '\n'
                << "Unknown error while parsing the command line" << '\n'
                << GTERMINATION;
    }

    return result;
}

/******************************************************************************/
/**
 * Provides information on the number of command line configuration options
 * stored in this class
 */
std::size_t GParserBuilder::numberOfCLOptions() const {
    return cl_parameter_proxies_.size();
}

/******************************************************************************/

} /* namespace Gem::Common */
