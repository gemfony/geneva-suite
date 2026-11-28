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

#include "common/GConfigurableI.hpp"

// Geneva header files go here
#include "common/GParserBuilder.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * Registers the object's configuration options on a parser builder, through the hook of the most
 * derived class.
 *
 * @param gpb The GParserBuilder object to which configuration options should be added
 */
void GConfigurableI::addConfigurationOptions(Gem::Common::GParserBuilder &gpb) {
    this->addConfigurationOptions_(gpb);
}

/******************************************************************************/
/**
 * Writes a configuration file with the object's options and their current values. An existing
 * file is refused, and so is an object without options (GParserBuilder::writeConfigFile()).
 *
 * @param config_file The name of the configuration file to be written
 * @param header A header to be prepended to the configuration file
 * @throws gem_exception if the file exists, cannot be written, or no option is registered
 */
void GConfigurableI::writeConfigFile(
    std::filesystem::path const &config_file,
    std::string const &header
) {
    // Destroyed at the end of this function, together with its references to this object
    Gem::Common::GParserBuilder gpb;
    this->addConfigurationOptions(gpb);
    gpb.writeConfigFile(config_file, header, true);
}

/******************************************************************************/
/**
 * Reads a configuration file into the object. A missing file is created from the registered
 * defaults (GParserBuilder::parseConfigFile()).
 *
 * @param config_file The name of the configuration file to be parsed
 * @throws gem_exception if the file cannot be read or applied
 */
void GConfigurableI::readConfigFile(std::filesystem::path const &config_file) {
    // Destroyed at the end of this function, together with its references to this object
    Gem::Common::GParserBuilder gpb;
    this->addConfigurationOptions(gpb);
    gpb.parseConfigFile(config_file);
}

/******************************************************************************/
/**
 * Registers the options of this class on the parser builder. This class has none.
 *
 * @param gpb The GParserBuilder object to which configuration options should be added
 */
void GConfigurableI::addConfigurationOptions_([[maybe_unused]] Gem::Common::GParserBuilder &gpb) {
    /* nothing */
}

/******************************************************************************/

} /* namespace Gem::Common */
