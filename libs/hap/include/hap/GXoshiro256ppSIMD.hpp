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


/********************************************************************************
 *
 * GXoshiro256ppSIMD.hpp — SIMD bulk-refill engines for Hap.
 *
 * Vectorised xoshiro256++ that advances several independent streams in parallel
 * SIMD lanes and writes them interleaved into a destination buffer. Used for
 * the producer/refill path (random_container fill), where throughput — not
 * per-call latency — matters.
 *
 *   - AVX2 (x86_64): 4 parallel streams in __m256i   -> xoshiro256pp_simd
 *   - NEON (AArch64): 2 parallel streams in uint64x2_t-> xoshiro256pp_simd
 *
 * Which (if any) variant is compiled is decided at BUILD time by the CMake
 * probes HAP_HAS_AVX2 / HAP_HAS_NEON (-> HAP_AVX2_BACKEND / HAP_NEON_BACKEND
 * compile definitions). On every other ISA this header defines nothing and Hap
 * falls back to the scalar xoshiro256++ (GXoshiro256pp.hpp). No runtime
 * dispatch. The interleaving of N independent streams is statistically sound
 * (each stream is a full xoshiro256++); Hap has no cross-thread determinism
 * requirement.
 *
 * Algorithm: David Blackman & Sebastiano Vigna, xoshiro256++ 1.0, public domain
 * (CC0). This is an independent SIMD re-implementation; no vendored code.
 *
 * This file is part of the Geneva library collection. Apache License 2.0
 * applies to this wrapper. See the NOTICE file in the top-level directory.
 *
 ********************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

// The SIMD variants share the scalar header's detail::splitmix64 seed expansion
#include "hap/GXoshiro256pp.hpp"

#if defined(HAP_AVX2_BACKEND)
#include <immintrin.h>
#elif defined(HAP_NEON_BACKEND)
#include <arm_neon.h>
#endif

namespace Gem::Hap {

/******************************************************************************/
#if defined(HAP_AVX2_BACKEND)

/**
 * AVX2 variant: 4 independent xoshiro256++ streams in __m256i lanes.
 * Drop-in for the scalar engine (result_type, min/max, operator(), seedable,
 * default-constructible) plus a bulk generate() that the refill path uses.
 */
class xoshiro256pp_simd {
public:
    static constexpr int  LANES = 4;
    using result_type           = std::uint64_t;

    /** @brief Default construction uses a fixed non-zero seed (default_seed). */
    xoshiro256pp_simd() noexcept { seed(default_seed); }
    /**
     * @brief Construction from a single 64-bit seed (expanded via splitmix64).
     * @param s The 64-bit seed value used to initialise all LANES streams
     */
    explicit xoshiro256pp_simd(result_type s) noexcept { seed(s); }

    /**
     * @brief Smallest value the generator can produce.
     * @return The minimum result_type value (always 0)
     */
    static constexpr result_type(min)() noexcept { return 0; }
    /**
     * @brief Largest value the generator can produce.
     * @return The maximum result_type value (2^64 - 1)
     */
    static constexpr result_type(max)() noexcept {
        return (std::numeric_limits<result_type>::max)();
    }

    /**
     * @brief (Re-)seed all LANES parallel streams from a single 64-bit value.
     *
     * The single seed is expanded via successive splitmix64 calls into the
     * per-lane, per-word state words, then loaded into the SIMD registers.
     * The single-value buffer is marked empty.
     *
     * @param s The 64-bit seed value used to initialise all LANES streams
     */
    void seed(result_type s) noexcept {
        std::uint64_t              sm = s;
        alignas(32) std::uint64_t  init[4][LANES]; // init[word][lane]
        for (int lane = 0; lane < LANES; ++lane) {
            for (auto & w : init) w[lane] = detail::splitmix64(sm);
}
        for (int w = 0; w < 4; ++w) {
            s_[w] = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(init[w]));
}
        bufpos_ = LANES; // buffer empty
    }

    /**
     * @brief Bulk fill: n values written as LANES interleaved streams.
     *
     * Writes full SIMD blocks directly to the destination and handles a final
     * partial block (tail smaller than LANES) via a stack buffer.
     *
     * @param dst Destination buffer that receives the n generated values; must hold at least n elements
     * @param n The number of 64-bit values to generate and write
     */
    void generate(result_type *dst, std::size_t n) noexcept {
        std::size_t i = 0;
        for (; i + LANES <= n; i += LANES) {
            _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + i), next_lanes());
}
        if (i < n) { // tail < LANES
            alignas(32) result_type tmp[LANES];
            _mm256_store_si256(reinterpret_cast<__m256i *>(tmp), next_lanes());
            for (std::size_t j = 0; i < n; ++i, ++j) dst[i] = tmp[j];
        }
    }

    /**
     * @brief Produce a single 64-bit value (buffered from the 4-wide SIMD block).
     *
     * Refills the internal LANES-wide buffer from a fresh SIMD block when it is
     * exhausted, then returns the next buffered value.
     *
     * @return The next pseudo-random 64-bit value
     */
    result_type operator()() noexcept {
        if (bufpos_ >= LANES) {
            _mm256_store_si256(reinterpret_cast<__m256i *>(buf_), next_lanes());
            bufpos_ = 0;
        }
        return buf_[bufpos_++];
    }

    /**
     * @brief Advance the generator by z single-value steps.
     * @param z The number of generated values to skip
     */
    void discard(unsigned long long z) noexcept { while (z-- > 0) (void)(*this)(); }

private:
    static constexpr result_type default_seed = 0x9e3779b97f4a7c15ULL;

    /**
     * @brief Rotate each 64-bit SIMD lane left by k bits.
     * @tparam k The compile-time number of bit positions to rotate left by
     * @param x The SIMD vector whose lanes are rotated
     * @return The vector with each lane left-rotated by k
     */
    template <int k> static __m256i rotl(__m256i x) noexcept {
        return _mm256_or_si256(_mm256_slli_epi64(x, k), _mm256_srli_epi64(x, 64 - k));
    }

    /**
     * @brief Advance all LANES streams once and return their interleaved outputs.
     * @return A SIMD vector holding the next output of each of the LANES streams
     */
    __m256i next_lanes() noexcept {
        const __m256i res = _mm256_add_epi64(rotl<23>(_mm256_add_epi64(s_[0], s_[3])), s_[0]);
        const __m256i t   = _mm256_slli_epi64(s_[1], 17);
        s_[2] = _mm256_xor_si256(s_[2], s_[0]);
        s_[3] = _mm256_xor_si256(s_[3], s_[1]);
        s_[1] = _mm256_xor_si256(s_[1], s_[2]);
        s_[0] = _mm256_xor_si256(s_[0], s_[3]);
        s_[2] = _mm256_xor_si256(s_[2], t);
        s_[3] = rotl<45>(s_[3]);
        return res;
    }

    __m256i                  s_[4];
    alignas(32) result_type  buf_[LANES]{};
    int                      bufpos_ = LANES;
};

#elif defined(HAP_NEON_BACKEND)

/**
 * NEON variant: 2 independent xoshiro256++ streams in uint64x2_t lanes.
 */
class xoshiro256pp_simd {
public:
    static constexpr int LANES = 2;
    using result_type          = std::uint64_t;

    /** @brief Default construction uses a fixed non-zero seed (default_seed). */
    xoshiro256pp_simd() noexcept { seed(default_seed); }
    /**
     * @brief Construction from a single 64-bit seed (expanded via splitmix64).
     * @param s The 64-bit seed value used to initialise all LANES streams
     */
    explicit xoshiro256pp_simd(result_type s) noexcept { seed(s); }

    /**
     * @brief Smallest value the generator can produce.
     * @return The minimum result_type value (always 0)
     */
    static constexpr result_type(min)() noexcept { return 0; }
    /**
     * @brief Largest value the generator can produce.
     * @return The maximum result_type value (2^64 - 1)
     */
    static constexpr result_type(max)() noexcept {
        return (std::numeric_limits<result_type>::max)();
    }

    /**
     * @brief (Re-)seed all LANES parallel streams from a single 64-bit value.
     *
     * The single seed is expanded via successive splitmix64 calls into the
     * per-lane, per-word state words, then loaded into the NEON registers.
     * The single-value buffer is marked empty.
     *
     * @param s The 64-bit seed value used to initialise all LANES streams
     */
    void seed(result_type s) noexcept {
        std::uint64_t sm = s;
        std::uint64_t init[4][LANES];
        for (int lane = 0; lane < LANES; ++lane)
            for (int w = 0; w < 4; ++w) init[w][lane] = detail::splitmix64(sm);
        for (int w = 0; w < 4; ++w) s_[w] = vld1q_u64(init[w]);
        bufpos_ = LANES;
    }

    /**
     * @brief Bulk fill: n values written as LANES interleaved streams.
     *
     * Writes full SIMD blocks directly to the destination and handles a final
     * partial block (tail smaller than LANES) via a stack buffer.
     *
     * @param dst Destination buffer that receives the n generated values; must hold at least n elements
     * @param n The number of 64-bit values to generate and write
     */
    void generate(result_type *dst, std::size_t n) noexcept {
        std::size_t i = 0;
        for (; i + LANES <= n; i += LANES) vst1q_u64(dst + i, next_lanes());
        if (i < n) {
            result_type tmp[LANES];
            vst1q_u64(tmp, next_lanes());
            for (std::size_t j = 0; i < n; ++i, ++j) dst[i] = tmp[j];
        }
    }

    /**
     * @brief Produce a single 64-bit value (buffered from the LANES-wide block).
     *
     * Refills the internal LANES-wide buffer from a fresh SIMD block when it is
     * exhausted, then returns the next buffered value.
     *
     * @return The next pseudo-random 64-bit value
     */
    result_type operator()() noexcept {
        if (bufpos_ >= LANES) { vst1q_u64(buf_, next_lanes()); bufpos_ = 0; }
        return buf_[bufpos_++];
    }

    /**
     * @brief Advance the generator by z single-value steps.
     * @param z The number of generated values to skip
     */
    void discard(unsigned long long z) noexcept { while (z-- > 0) (void)(*this)(); }

private:
    static constexpr result_type default_seed = 0x9e3779b97f4a7c15ULL;

    /**
     * @brief Rotate each 64-bit NEON lane left by k bits.
     * @tparam k The compile-time number of bit positions to rotate left by
     * @param x The NEON vector whose lanes are rotated
     * @return The vector with each lane left-rotated by k
     */
    template <int k> static uint64x2_t rotl(uint64x2_t x) noexcept {
        return vorrq_u64(vshlq_n_u64(x, k), vshrq_n_u64(x, 64 - k));
    }

    /**
     * @brief Advance all LANES streams once and return their interleaved outputs.
     * @return A NEON vector holding the next output of each of the LANES streams
     */
    uint64x2_t next_lanes() noexcept {
        const uint64x2_t res = vaddq_u64(rotl<23>(vaddq_u64(s_[0], s_[3])), s_[0]);
        const uint64x2_t t   = vshlq_n_u64(s_[1], 17);
        s_[2] = veorq_u64(s_[2], s_[0]);
        s_[3] = veorq_u64(s_[3], s_[1]);
        s_[1] = veorq_u64(s_[1], s_[2]);
        s_[0] = veorq_u64(s_[0], s_[3]);
        s_[2] = veorq_u64(s_[2], t);
        s_[3] = rotl<45>(s_[3]);
        return res;
    }

    uint64x2_t  s_[4];
    result_type buf_[LANES]{};
    int         bufpos_ = LANES;
};

#endif // backend selection

} /* namespace Gem::Hap */
