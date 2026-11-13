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

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <vector>

#include "geneva/genome/GAdaptionKernels.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Geneva;
using namespace Gem::Geneva::Genome;

namespace {
Gem::Hap::GRandomT<Gem::Hap::randomSource::QUEUE> gr;
}

/******************************************************************************/
TEST_CASE("adaptGaussGroup: mode NEVER leaves values and state untouched", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::NEVER;
    GaussState<double> st;
    std::vector<double> v{1., 2., 3.};
    const auto before = v;

    const auto n = adaptGaussGroup<double>(cfg, st, std::span<double>(v), gr);
    CHECK(n == 0);
    CHECK(v == before);
}

/******************************************************************************/
TEST_CASE("adaptGaussGroup: mode ALWAYS adapts every value (ULP-guaranteed change)", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.min_sigma = 1e-6;
    cfg.max_sigma = 10.;
    GaussState<double> st;
    st.sigma = 0.5;

    std::vector<double> v(50, 0.);
    const auto before = v;
    const auto n = adaptGaussGroup<double>(cfg, st, std::span<double>(v), gr);
    CHECK(n == v.size());
    for(std::size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i] != before[i]);
    }
}

/******************************************************************************/
TEST_CASE("adaptGaussGroup: WITHPROBABILITY honours the ad_prob extremes", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::WITHPROBABILITY;

    // ad_prob == 1 => every value is adapted
    {
        GaussState<double> st;
        st.ad_prob = 1.;
        std::vector<double> v(40, 0.);
        CHECK(adaptGaussGroup<double>(cfg, st, std::span<double>(v), gr) == v.size());
    }
    // ad_prob == 0 => no value is adapted
    {
        GaussState<double> st;
        st.ad_prob = 0.;
        std::vector<double> v(40, 0.);
        const auto before = v;
        CHECK(adaptGaussGroup<double>(cfg, st, std::span<double>(v), gr) == 0);
        CHECK(v == before);
    }
}

/******************************************************************************/
TEST_CASE("adaptGaussGroup: sigma self-adapts and stays within [min,max]", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.adaption_threshold = 1;
    cfg.min_sigma = 0.01;
    cfg.max_sigma = 2.0;
    cfg.sigma_sigma = 0.8;
    GaussState<double> st;
    st.sigma = 0.5;
    const double first = st.sigma;
    bool changed = false;

    std::vector<double> v(1, 0.);
    for(int i = 0; i < 200; ++i) {
        adaptGaussGroup<double>(cfg, st, std::span<double>(v), gr);
        CHECK(st.sigma >= cfg.min_sigma);
        CHECK(st.sigma <= cfg.max_sigma);
        if(st.sigma != first) {
            changed = true;
        }
    }
    CHECK(changed);
}

/******************************************************************************/
TEST_CASE("adaptGaussGroup: ad_prob self-adapts and stays within [min,max]", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.adaption_threshold = 1;
    cfg.adapt_ad_prob = 0.1;
    cfg.min_ad_prob = 0.05;
    cfg.max_ad_prob = 1.0;
    GaussState<double> st;
    st.ad_prob = 0.5;

    std::vector<double> v(1, 0.);
    for(int i = 0; i < 200; ++i) {
        adaptGaussGroup<double>(cfg, st, std::span<double>(v), gr);
        CHECK(st.ad_prob >= cfg.min_ad_prob);
        CHECK(st.ad_prob <= cfg.max_ad_prob);
    }
}

/******************************************************************************/
TEST_CASE("adaptGaussGroup: works in float precision", "[kernel]") {
    GaussConfig<float> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.min_sigma = 1e-4f;
    cfg.max_sigma = 5.f;
    GaussState<float> st;
    st.sigma = 0.5f;

    std::vector<float> v(20, 0.f);
    const auto before = v;
    CHECK(adaptGaussGroup<float>(cfg, st, std::span<float>(v), gr) == v.size());
    for(std::size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i] != before[i]);
    }
}

/******************************************************************************/
// Flip kernels (int32 / bool)
/******************************************************************************/

TEST_CASE("adaptFlipIntGroup: mode NEVER leaves values untouched", "[kernel]") {
    FlipConfig cfg;
    cfg.mode = adaptionMode::NEVER;
    FlipState st;
    std::vector<std::int32_t> v{1, 2, 3};
    const auto before = v;

    CHECK(adaptFlipIntGroup(cfg, st, std::span<std::int32_t>(v), gr) == 0);
    CHECK(v == before);
}

/******************************************************************************/
TEST_CASE("adaptFlipIntGroup: mode ALWAYS flips every value by exactly +/-1", "[kernel]") {
    FlipConfig cfg;
    cfg.mode = adaptionMode::ALWAYS;
    FlipState st;

    std::vector<std::int32_t> v(64, 100);
    const auto before = v;
    CHECK(adaptFlipIntGroup(cfg, st, std::span<std::int32_t>(v), gr) == v.size());
    for(std::size_t i = 0; i < v.size(); ++i) {
        CHECK((v[i] == before[i] + 1 || v[i] == before[i] - 1));
    }
}

/******************************************************************************/
TEST_CASE("adaptFlipIntGroup: WITHPROBABILITY honours the ad_prob extremes", "[kernel]") {
    FlipConfig cfg;
    cfg.mode = adaptionMode::WITHPROBABILITY;

    { // ad_prob == 1 => every value flips
        FlipState st;
        st.ad_prob = 1.;
        std::vector<std::int32_t> v(40, 0);
        CHECK(adaptFlipIntGroup(cfg, st, std::span<std::int32_t>(v), gr) == v.size());
    }
    { // ad_prob == 0 => nothing flips
        FlipState st;
        st.ad_prob = 0.;
        std::vector<std::int32_t> v(40, 0);
        const auto before = v;
        CHECK(adaptFlipIntGroup(cfg, st, std::span<std::int32_t>(v), gr) == 0);
        CHECK(v == before);
    }
}

/******************************************************************************/
TEST_CASE("adaptFlipBoolGroup: mode ALWAYS toggles every value", "[kernel]") {
    FlipConfig cfg;
    cfg.mode = adaptionMode::ALWAYS;
    FlipState st;

    std::vector<std::uint8_t> v{0, 1, 0, 1, 1, 0};
    const auto before = v;
    CHECK(adaptFlipBoolGroup(cfg, st, std::span<std::uint8_t>(v), gr) == v.size());
    for(std::size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i] == (before[i] ? std::uint8_t(0) : std::uint8_t(1)));
    }
}

/******************************************************************************/
TEST_CASE("adaptFlipBoolGroup: WITHPROBABILITY ad_prob == 0 toggles nothing", "[kernel]") {
    FlipConfig cfg;
    cfg.mode = adaptionMode::WITHPROBABILITY;
    FlipState st;
    st.ad_prob = 0.;

    std::vector<std::uint8_t> v{0, 1, 0, 1};
    const auto before = v;
    CHECK(adaptFlipBoolGroup(cfg, st, std::span<std::uint8_t>(v), gr) == 0);
    CHECK(v == before);
}

/******************************************************************************/
TEST_CASE("adaptFlip*Group: ad_prob self-adapts and stays within [min,max]", "[kernel]") {
    FlipConfig cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.adapt_ad_prob = 0.1;
    cfg.min_ad_prob = 0.05;
    cfg.max_ad_prob = 1.0;
    FlipState st;
    st.ad_prob = 0.5;

    std::vector<std::int32_t> v(1, 0);
    for(int i = 0; i < 200; ++i) {
        adaptFlipIntGroup(cfg, st, std::span<std::int32_t>(v), gr);
        CHECK(st.ad_prob >= cfg.min_ad_prob);
        CHECK(st.ad_prob <= cfg.max_ad_prob);
    }
}

/******************************************************************************/
// Bi-gaussian kernel
/******************************************************************************/

TEST_CASE("adaptBiGaussGroup: mode NEVER leaves values untouched", "[kernel]") {
    BiGaussConfig<double> cfg;
    cfg.mode = adaptionMode::NEVER;
    BiGaussState<double> st;
    std::vector<double> v{1., 2., 3.};
    const auto before = v;

    CHECK(adaptBiGaussGroup<double>(cfg, st, std::span<double>(v), gr) == 0);
    CHECK(v == before);
}

/******************************************************************************/
TEST_CASE("adaptBiGaussGroup: mode ALWAYS adapts every value (ULP-guaranteed change)", "[kernel]") {
    BiGaussConfig<double> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.min_sigma1 = 1e-6;
    cfg.max_sigma1 = 10.;
    cfg.min_sigma2 = 1e-6;
    cfg.max_sigma2 = 10.;
    BiGaussState<double> st;
    st.sigma1 = 0.5;
    st.sigma2 = 0.5;
    st.delta = 0.5;

    std::vector<double> v(50, 0.);
    const auto before = v;
    CHECK(adaptBiGaussGroup<double>(cfg, st, std::span<double>(v), gr) == v.size());
    for(std::size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i] != before[i]);
    }
}

/******************************************************************************/
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent kernel test: the 200-rep self-adaption loop checks sigma1/sigma2/delta bounds together against the SAME evolving state
TEST_CASE("adaptBiGaussGroup: sigma1/sigma2/delta self-adapt within their bounds", "[kernel]") {
    BiGaussConfig<double> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.adaption_threshold = 1;
    cfg.min_sigma1 = 0.01;
    cfg.max_sigma1 = 2.0;
    cfg.min_sigma2 = 0.01;
    cfg.max_sigma2 = 2.0;
    cfg.min_delta = 0.0;
    cfg.max_delta = 2.0;
    cfg.use_symmetric_sigmas = false;
    BiGaussState<double> st;
    st.sigma1 = 0.5;
    st.sigma2 = 0.5;
    st.delta = 0.5;
    const double first_sigma1 = st.sigma1;
    bool changed = false;

    std::vector<double> v(1, 0.);
    for(int i = 0; i < 200; ++i) {
        adaptBiGaussGroup<double>(cfg, st, std::span<double>(v), gr);
        CHECK(st.sigma1 >= cfg.min_sigma1);
        CHECK(st.sigma1 <= cfg.max_sigma1);
        CHECK(st.sigma2 >= cfg.min_sigma2);
        CHECK(st.sigma2 <= cfg.max_sigma2);
        CHECK(st.delta >= cfg.min_delta);
        CHECK(st.delta <= cfg.max_delta);
        if(st.sigma1 != first_sigma1) {
            changed = true;
        }
    }
    CHECK(changed);
}

/******************************************************************************/
TEST_CASE("adaptBiGaussGroup: works in float precision", "[kernel]") {
    BiGaussConfig<float> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.min_sigma1 = 1e-4f;
    cfg.max_sigma1 = 5.f;
    cfg.min_sigma2 = 1e-4f;
    cfg.max_sigma2 = 5.f;
    BiGaussState<float> st;
    st.sigma1 = 0.5f;
    st.sigma2 = 0.5f;
    st.delta = 0.5f;

    std::vector<float> v(20, 0.f);
    const auto before = v;
    CHECK(adaptBiGaussGroup<float>(cfg, st, std::span<float>(v), gr) == v.size());
    for(std::size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i] != before[i]);
    }
}

/******************************************************************************/
// Integer Gauss kernel
/******************************************************************************/

TEST_CASE("adaptGaussIntGroup: mode NEVER leaves values and state untouched", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::NEVER;
    GaussState<double> st;
    std::vector<std::int32_t> v{1, 2, 3};
    const auto before = v;

    const auto n = adaptGaussIntGroup(cfg, st, std::span<std::int32_t>(v), 100, gr);
    CHECK(n == 0);
    CHECK(v == before);
}

/******************************************************************************/
TEST_CASE("adaptGaussIntGroup: mode ALWAYS changes every value (minimal +/-1 guarantee)", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    GaussState<double> st;
    st.sigma = 0.5;

    std::vector<std::int32_t> v(64, 1000);
    const auto before = v;
    const auto n = adaptGaussIntGroup(cfg, st, std::span<std::int32_t>(v), 100, gr);
    CHECK(n == v.size());
    for(std::size_t i = 0; i < v.size(); ++i) {
        CHECK(v[i] != before[i]); // the zero-addition case is nudged to +/-1
    }
}

/******************************************************************************/
TEST_CASE("adaptGaussIntGroup: WITHPROBABILITY honours the ad_prob extremes", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::WITHPROBABILITY;

    { // ad_prob == 1 => every value adapts
        GaussState<double> st;
        st.ad_prob = 1.;
        st.sigma = 0.5;
        std::vector<std::int32_t> v(40, 0);
        CHECK(adaptGaussIntGroup(cfg, st, std::span<std::int32_t>(v), 100, gr) == v.size());
    }
    { // ad_prob == 0 => nothing adapts
        GaussState<double> st;
        st.ad_prob = 0.;
        std::vector<std::int32_t> v(40, 0);
        const auto before = v;
        CHECK(adaptGaussIntGroup(cfg, st, std::span<std::int32_t>(v), 100, gr) == 0);
        CHECK(v == before);
    }
}

/******************************************************************************/
TEST_CASE("adaptGaussIntGroup: sigma self-adapts and stays within [min,max]", "[kernel]") {
    GaussConfig<double> cfg;
    cfg.mode = adaptionMode::ALWAYS;
    cfg.sigma_sigma = 0.3;
    cfg.min_sigma = 0.01;
    cfg.max_sigma = 3.0;
    cfg.adaption_threshold = 1;
    GaussState<double> st;
    st.sigma = 0.5;

    std::vector<std::int32_t> v(1, 0);
    for(int i = 0; i < 200; ++i) {
        adaptGaussIntGroup(cfg, st, std::span<std::int32_t>(v), 100, gr);
        CHECK(st.sigma >= cfg.min_sigma);
        CHECK(st.sigma <= cfg.max_sigma);
    }
}
