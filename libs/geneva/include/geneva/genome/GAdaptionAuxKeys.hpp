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

// Geneva headers go here
#include "geneva/genome/GAuxiliaryStore.hpp"

namespace Gem::Geneva::Genome {

/******************************************************************************/
/**
 * The auxiliary-store keys under which a flat genome keeps its per-group adaption state, one block per
 * adaptor kind and channel. They form the contract between the side that INSTALLS / SEEDS the state
 * (the OA-owned GAdaptionConfigBase::installInto()) and the side that READS it (the data-oriented
 * adaption kernels in GAdaption.hpp). Defining them here, rather than privately in one translation
 * unit, keeps both sides addressing the very same blocks.
 *
 * Note: the int32 channel's adaption-fp type is double, so AUXKEY_GAUSS_INT addresses GaussState<double>.
 */
constexpr AuxKey AUXKEY_GAUSS_DOUBLE = 1;   ///< GaussState<double>  over the double channel
constexpr AuxKey AUXKEY_GAUSS_FLOAT = 2;    ///< GaussState<float>   over the float channel
constexpr AuxKey AUXKEY_BIGAUSS_DOUBLE = 3; ///< BiGaussState<double> over the double channel
constexpr AuxKey AUXKEY_BIGAUSS_FLOAT = 4;  ///< BiGaussState<float>  over the float channel
constexpr AuxKey AUXKEY_FLIP_INT = 5;       ///< FlipState           over the int32 channel
constexpr AuxKey AUXKEY_FLIP_BOOL = 6;      ///< FlipState           over the bool channel
constexpr AuxKey AUXKEY_GAUSS_INT = 7;      ///< GaussState<double>  over the int32 channel

/******************************************************************************/

} /* namespace Gem::Geneva::Genome */
