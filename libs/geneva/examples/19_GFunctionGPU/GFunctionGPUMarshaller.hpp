/**
 * @file GFunctionGPUMarshaller.hpp
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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

// Geneva headers
#include "geneva/genome/GBaseGPUMarshallerT.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"

namespace gind = Gem::Geneva::Individuals;

namespace Gem::Geneva::FunctionGPU {

/******************************************************************************/
/**
 * @brief The GPU marshaller for the GFunctionIndividual benchmark-function demo.
 *
 * This is the whole GPU-specific contribution a problem makes: it adapts a batch of GFunctionIndividuals
 * to the device kernel. It shows how little a concrete marshaller has to add now that
 * Gem::Geneva::GBaseGPUMarshallerT supplies the mechanical part -- itemDimension() / flatten() / scatter()
 * (and the double->scalar conversion, and the scalar-kind witness for the store) are all inherited. Only
 * two things are genuinely problem-specific:
 *
 *  - @c problemConstants() -- the opaque blob the kernel also needs. Here it is a single int: the benchmark
 *    function id (matching @c Gem::Geneva::Benchmarks::FUNC_*), so one kernel serves any of the demo
 *    functions.
 *  - capturing that function id -- read from the batch (every individual in a run shares one function), so
 *    flatten() is overridden ONLY to sample it before delegating the actual flattening to the base.
 *
 * The kernel replicates the SAME math the CPU path runs (Gem::Geneva::Benchmarks::eval, via
 * GFunctionIndividual::evaluate()), so a GPU run and a CPU run (--consumer stc) agree -- which the demo's
 * parity-check mode verifies. The scalar is double (the benchmark functions are defined in double), so
 * parity is near-exact (float rounding aside).
 */
class GFunctionGPUMarshaller final : public Gem::Geneva::GBaseGPUMarshallerT<double> {
public:
    /***************************************************************************/
    /**
     * @brief Samples the batch's benchmark function id, then delegates the flattening to the base.
     *
     * All individuals in a run evaluate the same benchmark function, so the id is read once from the first
     * item and handed to the kernel through problemConstants(). The base does the row-major streamlining.
     *
     * @param items The batch to flatten
     * @param params_out Filled by the base with items.size() * itemDimension() doubles, row-major
     */
    void flatten(std::span<const item_ptr> items, std::vector<double> &params_out) const override {
        if(not items.empty()) {
            const auto *first = dynamic_cast<const gind::GFunctionIndividual *>(items.front().get());
            if(first != nullptr) {
                func_id_ = static_cast<int>(first->getDemoFunction());
            }
        }
        Gem::Geneva::GBaseGPUMarshallerT<double>::flatten(items, params_out);
    }

    /***************************************************************************/
    /**
     * @brief The opaque problem constant: a single int, the benchmark function id read in flatten().
     *
     * The consumer always calls flatten() before it builds the problem-constants blob, so func_id_ is
     * current here.
     *
     * @return A byte blob holding the int function id
     */
    [[nodiscard]] std::vector<std::byte> problemConstants() const override {
        std::vector<std::byte> blob(sizeof(int));
        std::memcpy(blob.data(), &func_id_, sizeof(int));
        return blob;
    }

private:
    mutable int func_id_ = 0; ///< benchmark function id, refreshed from the batch in flatten()
};

/******************************************************************************/

} /* namespace Gem::Geneva::FunctionGPU */
