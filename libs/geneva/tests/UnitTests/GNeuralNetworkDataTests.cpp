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
 ********************************************************************************/

/**
 * @file
 * @brief Regression tests for the neural-network training-data containers (trainingSet /
 * networkData) after their conversion from hand-managed raw owning arrays to std::vector
 * (Inv 21). Guards exactly the failure modes of the retired implementation: unsafe
 * self-assignment, a copy path that silently dropped state (init_range_), shallow-vs-deep
 * copy semantics, and the on-disk archive round-trip whose layout the conversion must
 * preserve.
 */

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "geneva/individuals/GNeuralNetworkIndividual.hpp"
#include "weft/GBinaryArchive.hpp"
#include "weft/GJsonArchive.hpp"

using Gem::Geneva::Individuals::networkData;
using Gem::Geneva::Individuals::trainingSet;
using Catch::Approx;

namespace {

/** @brief Builds a small, fully populated training-data object for the tests. */
networkData makeSampleData() {
    networkData n_d(3); // three training-set slots
    n_d.push_back(2);   // network geometry: 2 input nodes ...
    n_d.push_back(1);   // ... and 1 output node
    for(std::size_t pos = 0; pos < 3; pos++) {
        auto t_s = std::make_shared<trainingSet>(2, 1);
        t_s->Input[0] = 0.25 + static_cast<double>(pos);
        t_s->Input[1] = 0.50 + static_cast<double>(pos);
        t_s->Output[0] = (pos % 2 == 0) ? 0.99 : 0.01;
        n_d.addTrainingSet(t_s, pos);
    }
    n_d.setInitRange({{-1., 1.}, {-2., 2.}});
    return n_d;
}

} // namespace

TEST_CASE("trainingSet: copy is deep and self-assignment is safe", "[individuals][nn]") {
    trainingSet t_s(2, 1);
    t_s.Input[0] = 1.;
    t_s.Input[1] = 2.;
    t_s.Output[0] = 3.;

    trainingSet copy(t_s);
    copy.Input[0] = 42.; // must not write through to the original
    CHECK(t_s.Input[0] == 1.);
    CHECK(copy.nInputNodes == 2);
    CHECK(copy.Output[0] == 3.);

    // The retired hand-written operator= was not self-assignment-safe
    trainingSet  const&self = t_s;
    t_s = self;
    CHECK(t_s.Input[0] == 1.);
    CHECK(t_s.Input[1] == 2.);
    CHECK(t_s.Output[0] == 3.);
}

TEST_CASE("networkData: copy and assignment are deep and complete", "[individuals][nn]") {
    const networkData n_d = makeSampleData();

    // Copy construction: deep training sets, geometry and init range all travel
    networkData const copy(n_d);
    CHECK(copy.getNInputNodes() == 2);
    CHECK(copy.getNOutputNodes() == 1);
    REQUIRE(copy.initRangeSet()); // the retired copy path silently dropped init_range_
    CHECK(copy.getInitRange() == n_d.getInitRange());
    auto orig_set = n_d.getTrainingSet(0);
    auto copied_set = copy.getTrainingSet(0);
    REQUIRE(orig_set.has_value());
    REQUIRE(copied_set.has_value());
    CHECK(copied_set.value() != orig_set.value()); // distinct objects ...
    CHECK(copied_set.value()->Input[0] == Approx(orig_set.value()->Input[0])); // ... same values

    // Assignment: same guarantees, plus self-assignment safety
    networkData assigned(1);
    assigned = n_d;
    REQUIRE(assigned.initRangeSet());
    CHECK(assigned.getTrainingSet(2).has_value());
    networkData  const&self = assigned;
    assigned = self;
    CHECK(assigned.getTrainingSet(2).has_value());
    CHECK(assigned.getInitRange() == n_d.getInitRange());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent disk round-trip test: save/load/remove plus the per-training-set verification loop over the SAME loaded object
TEST_CASE("networkData: disk round-trip preserves all data", "[individuals][nn]") {
    const networkData n_d = makeSampleData();

    const std::string file =
        (std::filesystem::temp_directory_path() / "geneva-nn-data-roundtrip.xml").string();
    n_d.saveToDisk(file);
    networkData const loaded(file);
    std::remove(file.c_str());

    CHECK(loaded.getNInputNodes() == 2);
    CHECK(loaded.getNOutputNodes() == 1);
    REQUIRE(loaded.initRangeSet());
    CHECK(loaded.getInitRange() == n_d.getInitRange());
    for(std::size_t pos = 0; pos < 3; pos++) {
        auto t_s = loaded.getTrainingSet(pos);
        REQUIRE(t_s.has_value());
        REQUIRE(t_s.value());
        CHECK(t_s.value()->Input[0] == Approx(0.25 + static_cast<double>(pos)));
        CHECK(t_s.value()->Input[1] == Approx(0.50 + static_cast<double>(pos)));
    }
    CHECK(not loaded.getTrainingSet(3).has_value());
}

// The GArchive codecs (binary + JSON) round-trip the same training data as the disk path. This covers
// the archive-generic serialize that a Boost-free saveToDisk will use: the value vectors via
// archive_named, the GPodContainerT<size_t> geometry base slice, and the std::vector<shared_ptr<
// trainingSet>> whose concrete (non-polymorphic) elements reconstruct element-wise through the
// concrete-pointer path -- the std::vectors serialized directly, with no make_array, explicit count or
// bounds limit. (The training data never crosses the wire -- it is rebuilt from nnTrainingDataStore()
// on individual load -- so this exercises the disk-persistence port specifically.)
TEST_CASE("networkData: GArchive binary + JSON codecs round-trip all data", "[individuals][nn][garchive]") {
    const networkData n_d = makeSampleData();

    auto verify = [&](const networkData &loaded) {
        CHECK(loaded.getNInputNodes() == 2);
        CHECK(loaded.getNOutputNodes() == 1);
        REQUIRE(loaded.initRangeSet());
        CHECK(loaded.getInitRange() == n_d.getInitRange());
        for(std::size_t pos = 0; pos < 3; pos++) {
            auto t_s = loaded.getTrainingSet(pos);
            REQUIRE(t_s.has_value());
            REQUIRE(t_s.value());
            CHECK(t_s.value()->Input[0] == Approx(0.25 + static_cast<double>(pos)));
            CHECK(t_s.value()->Input[1] == Approx(0.50 + static_cast<double>(pos)));
            CHECK(t_s.value()->Output[0] == Approx((pos % 2 == 0) ? 0.99 : 0.01));
        }
        CHECK(not loaded.getTrainingSet(3).has_value());
    };

    // Binary codec.
    {
        Gem::Weft::GBinaryOArchive oa;
        oa &Gem::Weft::make_nvp("nd", n_d);
        networkData loaded(0);
        Gem::Weft::GBinaryIArchive ia(oa.str());
        ia &Gem::Weft::make_nvp("nd", loaded);
        verify(loaded);
    }
    // JSON codec.
    {
        Gem::Weft::GJsonOArchive oa;
        oa &Gem::Weft::make_nvp("nd", n_d);
        networkData loaded(0);
        Gem::Weft::GJsonIArchive ia(oa.str());
        ia &Gem::Weft::make_nvp("nd", loaded);
        verify(loaded);
    }
}
