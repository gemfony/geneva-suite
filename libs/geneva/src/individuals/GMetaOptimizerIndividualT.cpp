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

#include "geneva/individuals/GMetaOptimizerIndividualT.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE (GArchive polymorphic-pointer dispatch)
#include "common/GCommonEnums.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include <istream>
#include <ostream>
#include <utility>

GEM_REGISTER_ARCHIVABLE(
    Gem::Geneva::Individuals::GMetaOptimizerIndividualT<Gem::Geneva::Individuals::GFunctionIndividual>
) // NOLINT
GEM_REGISTER_ARCHIVABLE(
    Gem::Geneva::Individuals::GOptOptMonitorT<Gem::Geneva::Individuals::GFunctionIndividual>
) // NOLINT
namespace Gem::Geneva::Individuals {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::Individuals */
