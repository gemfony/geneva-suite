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

/**
 * @file GImageScalar.hpp
 *
 * Compile-time scalar selector for example 14 (the Mona-Lisa problem). By default the whole
 * example -- genome parameter objects, adaptor, host rasteriser, GPU marshaller, device ABI and the
 * runtime CUDA kernel -- runs in DOUBLE precision. Define the macro GIMAGE_USE_FLOAT (e.g. via the
 * CMake option of the same name, settable from genevaConfig.gcfg) to build the example in FLOAT
 * precision instead, so FP32 vs FP64 can be compared.
 *
 *   gimage_fp_t                  the scalar type used everywhere in the example.
 *   GIMAGE_CONSTRAINED_OBJECT    the constrained parameter object class (in namespace gen).
 *   GIMAGE_GAUSS_ADAPTOR         the Gauss adaptor class (in namespace gen).
 *   GIMAGE_DEFAULT_GPUCONFIG     the default GPU-consumer config (selects the matching kernel).
 */

#ifdef GIMAGE_USE_FLOAT
namespace Gem::Geneva {
using gimage_fp_t = float;
} // namespace Gem::Geneva
#define GIMAGE_CONSTRAINED_OBJECT GConstrainedFloatObject
#define GIMAGE_GAUSS_ADAPTOR      GFloatGaussAdaptor
#define GIMAGE_DEFAULT_GPUCONFIG  "./config/GGPUConsumerFloat.json"
#else
namespace Gem::Geneva {
using gimage_fp_t = double;
} // namespace Gem::Geneva
#define GIMAGE_CONSTRAINED_OBJECT GConstrainedDoubleObject
#define GIMAGE_GAUSS_ADAPTOR      GDoubleGaussAdaptor
#define GIMAGE_DEFAULT_GPUCONFIG  "./config/GGPUConsumer.json"
#endif
