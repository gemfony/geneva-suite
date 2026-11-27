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
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Boost headers go here
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>

// Geneva header files go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GParserBuilder.hpp"

/******************************************************************************/
/**
 * @file
 * The command-line part of GParserBuilder: the proxies of command-line options and the
 * registration that needs them. A program that registers command-line options includes this
 * header; it brings in Boost.ProgramOptions, which the configuration files of a class do not need,
 * so GParserBuilder.hpp leaves it out.
 */

namespace Gem::Common {

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

    /** @brief Lists this option only under --showAll, not under --help (false: under both again);
     *  it is read from the command line all the same */
    GCLParsableI &setHidden(bool hidden = true) {
        hidden_ = hidden;
        return *this;
    }
    /** @brief Whether this option is listed only under --showAll */
    [[nodiscard]] bool isHidden() const { return hidden_; }

protected:
    /** @brief Registers this option with a Boost.ProgramOptions options description
     *  @param desc The options description to which this option is added */
    virtual void save_to(boost::program_options::options_description &desc) const = 0;

    /** @brief Returns the content of this object as a std::string
     *  @return A human-readable representation of this option's name and value */
    [[nodiscard]] virtual std::string content() const = 0;

private:
    bool hidden_ = false;     ///< Whether --help leaves this option out (--showAll lists it)
    std::size_t section_ = 0; ///< The section of the help text it is listed in (GParserBuilder::beginCLSection())
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
      : GCLParsableI(option_name_var, comment_var)
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
        std::ostringstream result;
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
 * A command-line option whose Boost.ProgramOptions value semantic comes from a function: a flag
 * (GParserBuilder::registerCLFlag()) or an option that may be given several times
 * (GParserBuilder::registerCLRepeatableParameter()).
 */
class GCLSemanticParsableParameter : public GCLParsableI {
public:
    /** @brief Stores the option's name and comment, the function that makes its value semantic
     *  (a new object, which Boost.ProgramOptions owns) and the one that describes its value */
    GCLSemanticParsableParameter(
        std::string const &option_name_var,
        std::string const &comment_var,
        std::function<boost::program_options::value_semantic *()> make_semantic,
        std::function<std::string()> describe_value
    )
      : GCLParsableI(option_name_var, comment_var)
      , make_semantic_(std::move(make_semantic))
      , describe_value_(std::move(describe_value)) { /* nothing */
    }

private:
    /** @brief Registers this option with a Boost.ProgramOptions options description */
    void save_to(boost::program_options::options_description &desc) const override {
        desc.add_options()((this->optionName()).c_str(), make_semantic_(), (this->comment()).c_str());
    }
    /** @brief Returns the option's name and value as a std::string */
    [[nodiscard]] std::string content() const override {
        return this->optionName() + " :\t" + describe_value_();
    }

    std::function<boost::program_options::value_semantic *()>
        make_semantic_; ///< Makes the value semantic the option is registered with
    std::function<std::string()> describe_value_; ///< Describes the current value
};

/******************************************************************************/
/**
 * Adds an option that may be given several times (--module a --module b) to the command line
 * parameters. @p values receives all values in the order they are given, and is empty if the
 * option is not given.
 *
 * @tparam parameter_type The type of one value
 * @param option_name The name of the option
 * @param values The vector that receives the values
 * @param comment A comment to be shown in the command-line help
 * @return A reference to the registered parameter proxy, to allow further configuration
 */
template <typename parameter_type>
GCLParsableI &GParserBuilder::registerCLRepeatableParameter(
    std::string const &option_name,
    std::vector<parameter_type> &values,
    std::string const &comment
) {
    checkNotRegistered_(option_name, cl_parameter_proxies_);
    namespace po = boost::program_options;
    return addCLProxy_(std::make_shared<GCLSemanticParsableParameter>(
        option_name,
        comment,
        [&values] {
            return po::value<std::vector<parameter_type>>(&values)->default_value(
                std::vector<parameter_type>{},
                "" // No text for the empty default in the help
            );
        },
        [&values] {
            std::ostringstream text;
            for(auto const &value : values) {
                text << value << ' ';
            }
            return text.str();
        }
    ));
}

/******************************************************************************/

} /* namespace Gem::Common */
