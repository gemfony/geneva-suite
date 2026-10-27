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

#include "common/GGlobalDefines.hpp"

#include <cstddef>
#include <cstdint>

namespace Gem::Hap {

/**
 * @brief GPU-based bulk random-number backend built on the cuRAND host API.
 *
 * GPU-based bulk random-number backend built on the cuRAND host API. It serves
 * as a refill backend for the random-number containers: a single curandGenerate
 * call fills a device buffer which is copied to the host. It exposes the same
 * bulk interface (generate(dst, n)) as the SIMD CPU refill engines, so the
 * producer can use it interchangeably.
 *
 * The cuRAND generator, CUDA stream and device buffer are held as void* so this
 * header pulls in no CUDA headers and can be included from ordinary C++
 * translation units (the producer). All CUDA code lives in GCUDARng.cu.
 */
class GCudaRNG {
public:
    using result_type = std::uint64_t;

    /** @brief Creates a cuRAND generator seeded from the given value. @param seed The seed value for the cuRAND generator. */
    explicit GCudaRNG(std::uint64_t seed);
    /** @brief Destroys the generator, device buffer and CUDA stream. */
    ~GCudaRNG();

    GCudaRNG(GCudaRNG const &)            = delete;
    GCudaRNG &operator=(GCudaRNG const &) = delete;

    /** @brief Fills dst[0..n) with n 64-bit values (one device generate + copy).
     *  @param dst Destination host buffer holding at least n 64-bit values. @param n The number of values to generate. */
    void generate(result_type *dst, std::size_t n);

    /** @brief True iff at least one usable CUDA device is present at runtime.
     *  @return true if a CUDA device is available, false otherwise. */
    static bool deviceAvailable() noexcept;

private:
    // d_buf_ / d_words_ is the "device pointer + host-side capacity, grown on demand" pair, which
    // courtier's CUDA backend also spells out (GCUDABackend's d_* / cap_* members). Kept separate
    // on purpose: the two libraries share no CUDA layer, and a common facility would drag the
    // driver API across a library boundary for six lines of bookkeeping. See the note there.
    void       *gen_{nullptr};    ///< curandGenerator_t
    void       *stream_{nullptr}; ///< cudaStream_t
    void       *d_buf_{nullptr};  ///< device buffer (32-bit words)
    std::size_t d_words_{0};      ///< current capacity of d_buf_ in 32-bit words
};

} /* namespace Gem::Hap */
