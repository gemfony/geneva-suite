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

// NOTE: library-private header. It is only included by gemfony-geneva translation units
// (GModuleLoader.cpp and GConsumerSetup.cpp) and is deliberately NOT installed -- no consumer ever
// sees it.

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <filesystem>

// Geneva headers go here
#include "geneva/GModuleLoader.hpp" // LoadedModule

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief Loads a module with Geneva's handlers, without loading the optional consumer modules
 * first. For loadOptionalConsumerModules() only, which runs inside std::call_once and would
 * enter itself through loadModule().
 *
 * @param module_path The module
 * @return What it contributed
 */
LoadedModule loadModuleWithGenevaHandlers(const std::filesystem::path &module_path);

/**
 * @brief Loads Geneva's optional consumer modules, once per process (GConsumerSetup.cpp). Must
 * not be called from a contribution handler or from anything a module's factory runs.
 */
void loadOptionalConsumerModules();

/******************************************************************************/

} /* namespace Gem::Geneva */
