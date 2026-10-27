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

// CUDA driver API declarations only: every function is reached through a pointer obtained at run
// time, so this header creates no link-time dependency on the driver library.
#include <cuda.h>

namespace Gem::Courtier::GPU {

/******************************************************************************/
/**
 * The part of the CUDA driver API the CUDA backend uses, loaded at run time.
 *
 * The driver library (libcuda.so.1) comes with the NVIDIA driver, not with the CUDA toolkit, so it
 * exists only on machines that have a driver installed. Linking it would keep every program that
 * contains the GPU consumer from starting anywhere else -- even for a run on the CPU consumers. The
 * library is therefore loaded with Boost.DLL (as GModuleLoader loads optimization modules) the first
 * time a CUDA backend is initialized, and the functions are taken from it by cuGetProcAddress, which
 * returns the version matching the cuda.h this file was compiled against.
 *
 * Private to courtier; not installed.
 */
struct GCUDADriver {
    decltype(&::cuGetErrorName) getErrorName = nullptr;
    decltype(&::cuGetErrorString) getErrorString = nullptr;
    decltype(&::cuInit) init = nullptr;
    decltype(&::cuDeviceGetCount) deviceGetCount = nullptr;
    decltype(&::cuDeviceGet) deviceGet = nullptr;
    decltype(&::cuDeviceGetAttribute) deviceGetAttribute = nullptr;
    decltype(&::cuDevicePrimaryCtxRetain) devicePrimaryCtxRetain = nullptr;
    decltype(&::cuDevicePrimaryCtxRelease) devicePrimaryCtxRelease = nullptr;
    decltype(&::cuCtxPushCurrent) ctxPushCurrent = nullptr;
    decltype(&::cuCtxPopCurrent) ctxPopCurrent = nullptr;
    decltype(&::cuCtxSynchronize) ctxSynchronize = nullptr;
    decltype(&::cuModuleLoadData) moduleLoadData = nullptr;
    decltype(&::cuModuleGetFunction) moduleGetFunction = nullptr;
    decltype(&::cuModuleUnload) moduleUnload = nullptr;
    decltype(&::cuMemAlloc) memAlloc = nullptr;
    decltype(&::cuMemFree) memFree = nullptr;
    decltype(&::cuMemcpyHtoD) memcpyHtoD = nullptr;
    decltype(&::cuMemcpyDtoH) memcpyDtoH = nullptr;
    decltype(&::cuMemsetD8) memsetD8 = nullptr;
    decltype(&::cuLaunchKernel) launchKernel = nullptr;
};

/**
 * @brief The CUDA driver, loaded on the first call and kept for the life of the process.
 *
 * Throws a geneva_exception naming the reason if the driver library cannot be loaded or lacks a
 * function; a later call tries again.
 *
 * @return The driver's function table
 */
const GCUDADriver &cudaDriver();

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
