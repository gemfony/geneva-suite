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

// NOTE: library-private header. It is only included by gemfony-hap translation
// units (it pulls in the SIMD/CUDA engine headers behind PRIVATE compile
// definitions) and is deliberately NOT installed -- no consumer ever sees it.

// Geneva headers go here
#include "common/GLogger.hpp"
#include "hap/GRandomDefines.hpp"

// Standard headers go here
#include <cstddef>
#include <cstdint>
#include <mutex>

#if defined(HAP_AVX2_BACKEND) || defined(HAP_NEON_BACKEND)
#include "hap/GXoshiro256ppSIMD.hpp" // SIMD bulk-refill engine
#endif
#if defined(HAP_USE_CUDA)
#include "hap/GCUDARng.hpp" // GPU bulk-refill backend (cuRAND host API)
#include <optional>
#endif

namespace Gem::Hap::detail {

/******************************************************************************/
/**
 * @brief The library-private bulk random-number source shared by every source strategy.
 *
 * GFillBackend concentrates the "where do the raw words come from" decision in one place. It
 * exposes @c generate(dst,n), so it duck-types as a bulk URBG for @c random_container::fill_from
 * and is reused unchanged by the QUEUE producer (the factory's producer threads), the
 * GRotatingPool (STAGED / QUARANTINE), and the LOCAL fall-back -- the engine-selection and
 * fall-back policy lives here and nowhere else.
 *
 * @par Algorithm
 * At construction it selects the fastest engine available in this build @e once (for the
 * object's lifetime), then every @c generate() call dispatches to that fixed choice -- there is
 * no per-call branching on a device probe. The selection priority is:
 * @f[
 *   \text{engine} =
 *   \begin{cases}
 *     \text{GPU (cuRAND)} & \text{if built with CUDA } \wedge\; \text{a device is present at run time},\\[2pt]
 *     \text{SIMD xoshiro256++} & \text{else if a SIMD backend (AVX2 / NEON) was compiled in},\\[2pt]
 *     \text{scalar xoshiro256++} & \text{otherwise.}
 *   \end{cases}
 * @f]
 *
 * @par Selection diagram
 * @verbatim
   GFillBackend(seed)            built with CUDA?
        |                       /              \
        |                    yes                no
        |                     |                  \
        |            device present? ---no--+     \
        |                     |              |      \
        |                    yes             v       v
        |                     |     SIMD backend compiled in (HAP_AVX2/NEON)?
        |                     |          /                       \
        |                     |        yes                        no
        v                     v         v                          v
   generate(dst,n) ----> [ cuRAND GPU ] [ SIMD xoshiro256++ ]  [ scalar xoshiro256++ ]
                              (1)              (2)                     (3)
   @endverbatim
 *
 * @par Never a silent fall-back
 * The CUDA decision is logged exactly ONCE for the whole process (a @c std::call_once gate, not
 * once per backend): when a device is present the GPU choice is noted; when the binary was built
 * with the CUDA backend but @e no device is available, a WARNING is emitted so the SIMD/CPU
 * fall-back never comes as a surprise. The runtime device probe means a CUDA-enabled binary still
 * runs correctly (just on the CPU engine) on a machine without a GPU.
 */
class GFillBackend {
public:
    using result_type = std::uint64_t;

    /**
     * @brief Builds the backend, picking the active engine and logging the CUDA decision once.
     *
     * @param seed The seed for this backend's CPU/GPU engine
     */
    explicit GFillBackend(std::uint64_t seed)
#if defined(HAP_AVX2_BACKEND) || defined(HAP_NEON_BACKEND)
        : cpu_(static_cast<xoshiro256pp_simd::result_type>(seed))
#else
        : cpu_(static_cast<G_CPU_BASE_GENERATOR::result_type>(seed))
#endif
    {
#if defined(HAP_USE_CUDA)
        useCuda_ = GCudaRNG::deviceAvailable();
        if(useCuda_) {
            cuda_.emplace(static_cast<std::uint64_t>(seed));
        }

        // Log the CUDA decision exactly ONCE for the whole process (not once per
        // backend). When built with the CUDA backend but no device is present,
        // emit a clear warning so the SIMD/CPU fall-back never comes as a surprise.
        static std::once_flag cuda_decision_logged;
        const bool useCuda = useCuda_;
        std::call_once(cuda_decision_logged, [useCuda]() {
            if(useCuda) {
                glogger << "In GFillBackend: a CUDA device is present;"
                        << " random-number containers are refilled on the GPU via cuRAND." << '\n'
                        << GLOGGING;
            }
            else {
                glogger
                    << "In GFillBackend: this binary was built with the CUDA"
                    << " random-number backend, but no CUDA-capable device is available." << '\n'
                    << "Falling back to the SIMD/CPU bulk-refill engine -- the GPU is NOT being"
                    << " used for random-number generation." << '\n'
                    << GWARNING;
            }
        });
#endif
    }

    /******************************************************************************/
    /**
     * @brief Bulk-fills [dst, dst+n) with uniformly distributed 64-bit words from the active engine.
     *
     * @param dst Start of the destination buffer
     * @param n   Number of 64-bit words to write
     */
    void generate(result_type *dst, std::size_t n) {
#if defined(HAP_USE_CUDA)
        if(useCuda_) {
            cuda_->generate(dst, n);
            return;
        }
#endif
#if defined(HAP_AVX2_BACKEND) || defined(HAP_NEON_BACKEND)
        cpu_.generate(dst, n);
#else
        for(std::size_t i = 0; i < n; ++i) {
            dst[i] = cpu_();
        }
#endif
    }

private:
#if defined(HAP_AVX2_BACKEND) || defined(HAP_NEON_BACKEND)
    xoshiro256pp_simd cpu_; ///< SIMD bulk-refill engine (universal GPU fall-back)
#else
    G_CPU_BASE_GENERATOR cpu_; ///< scalar xoshiro256++ (no SIMD backend compiled in)
#endif
#if defined(HAP_USE_CUDA)
    bool useCuda_ = false; ///< whether a CUDA device was found at run time
    std::optional<GCudaRNG> cuda_; ///< the cuRAND GPU backend (engaged only when useCuda_)
#endif
};

/******************************************************************************/

} /* namespace Gem::Hap::detail */
