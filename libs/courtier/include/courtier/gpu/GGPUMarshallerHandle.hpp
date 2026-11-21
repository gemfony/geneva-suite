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
#include <memory>
#include <string>

// Geneva header files go here
#include "common/GProviderStoreT.hpp"
#include "common/GProviderT.hpp"

namespace Gem::Courtier::GPU {

/******************************************************************************/
/**
 * @brief The device scalar (flat-buffer element) a GPU marshaller was built for. The scalar is a
 * compile-time template parameter of the marshaller and the consumer; this is its run-time
 * witness, so that one store holds marshallers of either precision and the GPU consumer's
 * provider recovers the typed one.
 */
enum class GPUScalarKind {
    Float,
    Double
};

/******************************************************************************/
/**
 * @brief A scalar-agnostic handle to a GPU marshaller: a GGPUEvaluableI<item, float> and a
 * GGPUEvaluableI<item, double> share no base, so a marshaller derives from this one as well. The
 * GPU consumer's provider reads the scalar kind and casts the handle back to the GGPUEvaluableI of
 * its item type and that scalar -- a marshaller for another item type does not cast.
 */
class GGPUMarshallerHandle { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /** @brief The (defaulted) destructor */
    virtual ~GGPUMarshallerHandle() = default;
    /** @brief The device scalar this marshaller flattens and scatters in @return Its kind */
    [[nodiscard]] virtual GPUScalarKind scalarKind() const = 0;
};

/******************************************************************************/
/**
 * @brief A provider of one GPU marshaller, registered under a device target ("cuda"); it hands out
 * the marshaller (provide()) and names the configuration file of the GPU consumer (backend and
 * kernel) the marshaller's kernel needs.
 */
class GGPUMarshallerProviderBase : public Gem::Common::GProviderT<GGPUMarshallerHandle> {
public:
    /** @brief The GPU consumer's configuration file @return Its path */
    [[nodiscard]] virtual const std::string &gpuConfigFile() const = 0;
};

/******************************************************************************/
/**
 * @brief The process-global store of GPU marshaller providers, keyed by device target. The problem
 * contributes its marshaller here (a program, or a module through its library's handler); the GPU
 * consumer's provider looks it up when the consumer is chosen. One problem per process: at most
 * one marshaller per target.
 * @return The store (never null)
 */
[[nodiscard]] inline auto marshallerProviderStore() {
    return Gem::Common::providerStore<GGPUMarshallerHandle>();
}

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
