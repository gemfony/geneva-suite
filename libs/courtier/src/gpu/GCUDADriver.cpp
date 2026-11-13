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

// Standard headers
#include <exception>
#include <type_traits>

// Boost headers go here
// Boost.DLL on std::filesystem, as in GModuleLoader, so no libboost_filesystem is needed.
#define BOOST_DLL_USE_STD_FS 1
#include <boost/dll/shared_library.hpp>

// Geneva headers
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "GCUDADriver.hpp"

namespace Gem::Courtier::GPU {

namespace {

/** The driver library's name, as the NVIDIA driver installs it */
constexpr const char *CUDA_DRIVER_LIBRARY = "libcuda.so.1";

/**
 * The exported name of cuGetProcAddress in the signature cuda.h declares (cuda.h maps the plain name
 * to this one with a macro, which a symbol lookup by string does not see).
 */
constexpr const char *CUDA_GET_PROC_ADDRESS = "cuGetProcAddress_v2";

/**
 * @brief Loads the driver library and fills the function table.
 * @return The function table
 */
GCUDADriver loadDriver_() {
    // The library is never unloaded: CUDA contexts and device buffers may still be released while the
    // process shuts down, after any static object holding the handle would have been destroyed. Hence
    // a deliberately leaked handle rather than a static one.
    auto *library = new boost::dll::shared_library(); // NOLINT(cppcoreguidelines-owning-memory)
    try {
        library->load(CUDA_DRIVER_LIBRARY, boost::dll::load_mode::search_system_folders);
    } catch(const std::exception &e) {
        delete library; // NOLINT(cppcoreguidelines-owning-memory)
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "GCUDADriver: the CUDA driver library " << CUDA_DRIVER_LIBRARY
            << " could not be loaded (" << e.what() << ")." << '\n'
            << "The GPU consumer needs an NVIDIA driver on this machine; the CPU consumers do not."
            << '\n');
    }

    using get_proc_address_t = std::remove_pointer_t<decltype(&::cuGetProcAddress)>;
    auto &getProcAddress = library->get<get_proc_address_t>(CUDA_GET_PROC_ADDRESS);

    GCUDADriver driver;
    auto resolve = [&getProcAddress](const char *name, auto &function) {
        void *address = nullptr;
        CUdriverProcAddressQueryResult status = CU_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
        const CUresult result =
            getProcAddress(name, &address, CUDA_VERSION, CU_GET_PROC_ADDRESS_DEFAULT, &status);
        if(result != CUDA_SUCCESS || status != CU_GET_PROC_ADDRESS_SUCCESS || address == nullptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "GCUDADriver: the installed CUDA driver does not provide " << name
                << " in the version of CUDA " << CUDA_VERSION / 1000 << '.' << (CUDA_VERSION % 1000) / 10
                << "; the driver is probably older than the toolkit Geneva was built with." << '\n');
        }
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address); // NOLINT
    };

    resolve("cuGetErrorName", driver.getErrorName);
    resolve("cuGetErrorString", driver.getErrorString);
    resolve("cuInit", driver.init);
    resolve("cuDeviceGetCount", driver.deviceGetCount);
    resolve("cuDeviceGet", driver.deviceGet);
    resolve("cuDeviceGetAttribute", driver.deviceGetAttribute);
    resolve("cuDevicePrimaryCtxRetain", driver.devicePrimaryCtxRetain);
    resolve("cuDevicePrimaryCtxRelease", driver.devicePrimaryCtxRelease);
    resolve("cuCtxPushCurrent", driver.ctxPushCurrent);
    resolve("cuCtxPopCurrent", driver.ctxPopCurrent);
    resolve("cuCtxSynchronize", driver.ctxSynchronize);
    resolve("cuModuleLoadData", driver.moduleLoadData);
    resolve("cuModuleGetFunction", driver.moduleGetFunction);
    resolve("cuModuleUnload", driver.moduleUnload);
    resolve("cuMemAlloc", driver.memAlloc);
    resolve("cuMemFree", driver.memFree);
    resolve("cuMemcpyHtoD", driver.memcpyHtoD);
    resolve("cuMemcpyDtoH", driver.memcpyDtoH);
    resolve("cuMemsetD8", driver.memsetD8);
    resolve("cuLaunchKernel", driver.launchKernel);

    return driver;
}

} // namespace

/******************************************************************************/
/**
 * @brief The CUDA driver, loaded on the first call and kept for the life of the process.
 *
 * A function-local static: thread-safe initialization, and if loading throws, the next call tries
 * again.
 *
 * @return The driver's function table
 */
const GCUDADriver &cudaDriver() {
    static const GCUDADriver driver = loadDriver_();
    return driver;
}

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
