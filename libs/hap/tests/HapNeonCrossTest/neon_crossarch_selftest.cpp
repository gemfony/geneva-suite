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


// Standalone AArch64/NEON self-test for Hap's GXoshiro256ppSIMD.hpp.
// Cross-compiled for aarch64 and run under qemu-aarch64 (user mode), since the
// build host is x86_64. Validates the NEON SIMD engine WITHOUT the full Geneva
// stack.
//
//   aarch64-linux-gnu-g++ -std=c++20 -O2 -static -I hap/include \
//       neon_selftest.cpp -o neon_selftest
//   qemu-aarch64 ./neon_selftest
//
// Core check (known-answer): the NEON engine seeds lane 0 from the first four
// splitmix64 outputs of the seed -- exactly what the scalar xoshiro256pp does.
// So lane 0 of the NEON output MUST equal scalar xoshiro256pp(seed) bit-for-bit.
// A wrong rotate/shift/lane-crossing in the NEON path breaks this immediately.

#define HAP_NEON_BACKEND 1

#include <cstdint>
#include <cstdio>
#include <vector>

#include "hap/GXoshiro256pp.hpp"     // scalar reference
#include "hap/GXoshiro256ppSIMD.hpp" // NEON engine under test

using namespace Gem::Hap;
using u64 = std::uint64_t;

int main() {
    const u64 seed   = 0xDEADBEEFCAFEF00DULL;
    int       errors = 0;

    static_assert(xoshiro256pp_simd::LANES == 2, "NEON build must expose 2 lanes");

    // ---- 1. Known-answer: NEON lane 0 == scalar xoshiro256pp(seed) ----------
    xoshiro256pp_simd e(seed);
    std::vector<u64>  buf(2000);
    e.generate(buf.data(), buf.size()); // interleaved: even=lane0, odd=lane1
    xoshiro256pp ref(seed);
    int laneMismatch = 0;
    for (int k = 0; k < 1000; ++k) {
        u64 r = ref();
        if (buf[2 * k] != r) {
            if (laneMismatch < 3)
                std::printf("  lane0 mismatch @%d: %016llx != %016llx\n", k,
                            (unsigned long long)buf[2 * k], (unsigned long long)r);
            ++laneMismatch;
        }
    }
    if (laneMismatch) { std::printf("FAIL known-answer: %d/1000 lane0 mismatches\n", laneMismatch); ++errors; }
    else              std::printf("PASS known-answer: lane0 == scalar xoshiro256pp(seed) over 1000 draws\n");

    // ---- 2. Lane independence: lane1 must not equal lane0 -------------------
    bool independent = false;
    for (int k = 0; k < 1000 && !independent; ++k)
        if (buf[2 * k + 1] != buf[2 * k]) independent = true;
    if (!independent) { std::printf("FAIL independence: lane1 == lane0\n"); ++errors; }
    else              std::printf("PASS independence: lane1 is a distinct stream\n");

    // ---- 3. operator() (buffered single draw) matches generate() ordering ---
    xoshiro256pp_simd e2(seed);
    int opMismatch = 0;
    for (int i = 0; i < 200; ++i)
        if (e2() != buf[i]) ++opMismatch;
    if (opMismatch) { std::printf("FAIL operator(): %d/200 mismatches vs generate()\n", opMismatch); ++errors; }
    else            std::printf("PASS operator(): buffered single-draw matches generate()\n");

    // ---- 4. Monobit sanity over a larger sample ----------------------------
    xoshiro256pp_simd e3(0x12345678ULL);
    std::vector<u64>  big(1 << 20); // ~1M values
    e3.generate(big.data(), big.size());
    u64 ones = 0;
    for (u64 v : big) ones += (u64)__builtin_popcountll((unsigned long long)v);
    double frac = (double)ones / ((double)big.size() * 64.0);
    if (frac < 0.49 || frac > 0.51) { std::printf("FAIL monobit: bit fraction %.5f\n", frac); ++errors; }
    else                            std::printf("PASS monobit: bit fraction %.5f (~0.5)\n", frac);

    std::printf(errors ? "\nNEON self-test: FAILED (%d)\n" : "\nNEON self-test: ALL PASS\n", errors);
    return errors ? 1 : 0;
}
