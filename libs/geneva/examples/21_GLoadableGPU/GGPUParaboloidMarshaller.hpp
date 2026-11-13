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

// Geneva headers
#include "geneva/genome/GBaseGPUMarshallerT.hpp"

/******************************************************************************/
/**
 * @brief The GPU marshaller for GGPUParaboloid -- shipped in the SAME loadable module as the individual.
 *
 * The paraboloid kernel (kernels/paraboloid.cu) needs no problem-specific constants and every item flattens
 * the same way, so this marshaller adds nothing to the base scaffolding: @c GBaseGPUMarshallerT<double>
 * already supplies @c itemDimension() / @c flatten() / @c scatter() / @c scalarKind() in pure host code
 * (the device math is the runtime-compiled kernel, not this class). It exists only so the module has a
 * concrete marshaller type to contribute to the marshaller store under the "cuda" device target, which is
 * what lets Go2 build the GPU consumer around this problem for @c --consumer @c gpu.
 */
class GGPUParaboloidMarshaller final : public Gem::Geneva::GBaseGPUMarshallerT<double> {};

/******************************************************************************/
