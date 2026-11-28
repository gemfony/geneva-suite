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

// Standard header files go here
#include <filesystem>
#include <string>

// Geneva header files go here

namespace Gem::Common {

// Only named here: common/GParserBuilder.hpp holds it, and a class that registers options on it
// includes that header where it does
class GParserBuilder;

/******************************************************************************/
/**
 * The configuration facet of an object: it registers its options on a GParserBuilder, and from
 * those writes a configuration file or reads one into itself. A class with options overrides the
 * hook addConfigurationOptions_() and calls its parent's version from there.
 *
 * Every class with the common interface (GCommonInterfaceT) has this facet. The class holds no data
 * and is no template, so its header needs only the name of GParserBuilder; the parser itself is
 * included by the classes that register options.
 */
class GConfigurableI {
public:
    /** @brief Registers the object's configuration options on a parser builder */
    void addConfigurationOptions(Gem::Common::GParserBuilder &gpb);

    /** @brief Writes a configuration file with the object's options and their current values */
    void writeConfigFile(std::filesystem::path const &config_file, std::string const &header);

    /** @brief Reads a configuration file into the object */
    void readConfigFile(std::filesystem::path const &config_file);

protected:
    /***************************************************************************/
    // Defaulted special members: the class holds no data. The destructor is protected and not
    // virtual, as an object is never deleted through this class.

    GConfigurableI() = default;
    GConfigurableI(GConfigurableI const &) = default;
    GConfigurableI(GConfigurableI &&) = default;
    GConfigurableI &operator=(GConfigurableI const &) = default;
    GConfigurableI &operator=(GConfigurableI &&) = default;
    ~GConfigurableI() = default;

    /** @brief Registers the options of this class on the parser builder (the default has none) */
    virtual void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb);
};

/******************************************************************************/

} /* namespace Gem::Common */
