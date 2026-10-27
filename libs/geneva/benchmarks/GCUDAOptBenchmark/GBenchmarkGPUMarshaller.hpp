/**
 * @file GBenchmarkGPUMarshaller.hpp
 */

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

// Standard headers
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

// Geneva headers
#include "geneva/genome/GBaseGPUMarshallerT.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"

namespace gind = Gem::Geneva::Individuals;

namespace Gem::Geneva::Benchmarks {

/******************************************************************************/
/**
 * A GPU marshaller for the CUDA optimisation benchmark. It adapts a batch of GFunctionIndividuals to the
 * device kernel for the unified courtier GPU consumer (Gem::Courtier::GPU::GGPUConsumerT): the whole
 * population is scored in ONE bulk, runtime-compiled kernel launch (kernels/benchmark_eval.cu via NVRTC).
 *
 * It derives Gem::Geneva::GBaseGPUMarshallerT<double>, so the mechanical part -- itemDimension() /
 * flatten() / scatter(), the double->scalar conversion and the scalar-kind witness -- is inherited. Only
 * the benchmark-specific piece remains: the function id (the same for every individual in a run) is read
 * from the batch and handed to the kernel as the opaque problem constant (a single int), so flatten() is
 * overridden ONLY to sample it before delegating the actual flattening to the base.
 *
 * The GPU consumer is device-only; a CPU run uses the individual's own evaluate() through a CPU consumer
 * (e.g. --consumer stc), which shares the EXACT same function math the device kernel replicates
 * (Gem::Geneva::Benchmarks::eval in GBenchmarkFunctions.hpp), so a CPU run and a GPU run agree and the GPU
 * can be cross-checked.
 */
class GBenchmarkGPUMarshaller final : public Gem::Geneva::GBaseGPUMarshallerT<double> {
public:
    /** @brief Samples the batch's benchmark function id, then delegates the flattening to the base. All
     *  items in a run share the same function, so it is read once from the first item and handed to the
     *  kernel through problemConstants(). */
    void flatten(std::span<const item_ptr> items, std::vector<double> &params_out) const override {
        if(not items.empty()) {
            const auto *first = dynamic_cast<const gind::GFunctionIndividual *>(items.front().get());
            if(first != nullptr) {
                funcId_ = static_cast<int>(first->getDemoFunction());
            }
        }
        Gem::Geneva::GBaseGPUMarshallerT<double>::flatten(items, params_out);
    }

    /** @brief The opaque problem constant is a single int: the benchmark function id. It is read from
     *  the batch in flatten(), which the consumer always calls first, so funcId_ is current here. */
    [[nodiscard]] std::vector<std::byte> problemConstants() const override {
        std::vector<std::byte> b(sizeof(int));
        std::memcpy(b.data(), &funcId_, sizeof(int));
        return b;
    }

private:
    mutable int funcId_ = 0; ///< Benchmark function id, refreshed from the batch in flatten()
};

/******************************************************************************/

} /* namespace Gem::Geneva::Benchmarks */
