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
#include <memory>
#include <string>

// Geneva headers
#include "courtier/gpu/GGPUDeviceBackendI.hpp"

namespace Gem::Courtier::GPU {

/******************************************************************************/
/**
 * CUDA device backend. It acquires the kernel at RUN TIME: a `.cu` source file from the config is
 * compiled with NVRTC to PTX and loaded through the CUDA driver API; a prebuilt `.ptx`/`.cubin` is
 * loaded directly. The whole batch is uploaded and evaluated in one cuLaunchKernel (bulk). All CUDA
 * types are hidden behind a pimpl so this header pulls in no CUDA headers.
 *
 * Compiled only when a CUDA toolkit was found at configure time (see the courtier GPU CMakeLists).
 *
 * @tparam scalar_type The flat-buffer element type for parameters and fitness (double for full parity, float for FP32 device speed); defaults to double
 */
template <typename scalar_type = double>
class GCUDABackend final : public GGPUDeviceBackendI<scalar_type> {
public:
    /** @brief Default constructor; creates the pimpl. The CUDA context/module are built in initialize(). */
    GCUDABackend();
    /** @brief Destructor; releases the CUDA context/module held by the pimpl. */
    ~GCUDABackend() override;

    GCUDABackend(const GCUDABackend &) = delete;
    GCUDABackend &operator=(const GCUDABackend &) = delete;

    /**
     * @brief Acquires the kernel described by @p spec (NVRTC-compile a .cu source, or load a prebuilt .ptx/.cubin) and prepares the CUDA context for launches.
     */
    void initialize(const KernelSpec &spec) override;
    /**
     * @brief Uploads the whole batch, launches the kernel once, and reads the per-item fitness back.
     */
    void evaluate(
        const scalar_type *params, int n_items, int dim,
        const std::byte *pconst, std::size_t pconst_size,
        scalar_type *fitness_out, int threads_per_item = 1) override;
    /** @brief A short backend identifier ("cuda"), used for logging and selection. */
    [[nodiscard]] std::string name() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
