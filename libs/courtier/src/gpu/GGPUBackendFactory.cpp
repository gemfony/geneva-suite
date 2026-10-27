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

// Geneva headers
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/gpu/GGPUBackendFactory.hpp"
#include "courtier/gpu/GGPUDeviceBackendI.hpp"

#ifdef GPUGEN_HAVE_CUDA
#include "courtier/gpu/GCUDABackend.hpp"
#endif

namespace Gem::Courtier::GPU {

/******************************************************************************/
// This is the extensible backend picker: backendAvailable() / makeBackend() switch over BackendKind
// and select the device backend compiled into this build. A new backend (e.g. HIP or SYCL) slots in
// here -- add a BackendKind, include its header behind a new GPUGEN_HAVE_<X> guard, and add the
// matching case to both functions below; the rest of the GPU consumer is backend-agnostic.

/******************************************************************************/
/**
 * @brief Reports whether a given backend was compiled into this build.
 *
 * The GPU consumer is device-only: CUDA is available only when its toolkit was found at configure time
 * (guarded by GPUGEN_HAVE_CUDA). There is no CPU fallback backend.
 *
 * @param kind The backend to query
 * @return true if the backend is available in this build, false otherwise
 */
bool backendAvailable(BackendKind kind) {
    switch(kind) {
    case BackendKind::CUDA:
#ifdef GPUGEN_HAVE_CUDA
        return true;
#else
        return false;
#endif
    }
    return false;
}

/******************************************************************************/
/**
 * @brief Constructs the requested device backend for the given scalar type.
 *
 * The GPU consumer is device-only: the only backend is CUDA, constructible only when its toolkit was
 * compiled in.
 *
 * @tparam scalar_type The floating-point scalar the backend operates on (double or float)
 * @param kind The backend to construct
 * @return An owning pointer to the constructed backend
 * @throws geneva_exception if the requested backend was not compiled into this build
 */
template <typename scalar_type>
std::unique_ptr<GGPUDeviceBackendI<scalar_type>> makeBackend(BackendKind kind) {
    switch(kind) {
    case BackendKind::CUDA:
#ifdef GPUGEN_HAVE_CUDA
        return std::make_unique<GCUDABackend<scalar_type>>();
#else
        break;
#endif
    }
    throw geneva_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "Gem::Courtier::GPU::makeBackend(): backend '" << toString(kind)
        << "' was not compiled into this build (its toolkit was not found at configure time)." << '\n');
}

/******************************************************************************/
// Explicit instantiations: this .cpp is where the CUDA backend header and its toolkit guard are
// available, so the templated backends are materialised here for the supported scalars.
template std::unique_ptr<GGPUDeviceBackendI<double>> makeBackend<double>(BackendKind);
template std::unique_ptr<GGPUDeviceBackendI<float>> makeBackend<float>(BackendKind);

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
