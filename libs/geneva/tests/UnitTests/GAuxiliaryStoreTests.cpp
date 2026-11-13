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
#include <memory>

#include "common/GExpectationChecksT.hpp"
#include "courtier/GWireSerializationContext.hpp" // the wire scope + layout registry (scratch omission)
#include "geneva/genome/GAuxiliaryStore.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/individuals/GTestIndividual3.hpp"

using namespace Gem::Geneva::Genome;

namespace {

/** @brief A representative per-group EA-Gauss metadata record (POD). */
struct AuxGaussRecord {
    float sigma = 0.f;
    float adProb = 0.f;
    std::uint32_t counter = 0;
};

constexpr AuxKey KEY = 0x1234u;

} // namespace

/******************************************************************************/
TEST_CASE("GAuxiliaryStore POD blocks: install / typed access / copy-independence / clear", "[aux]") {
    GAuxiliaryStore s;

    CHECK_FALSE(s.hasAux(KEY));
    s.installAuxBlock<AuxGaussRecord>(KEY, 3);
    REQUIRE(s.hasAux(KEY));

    auto recs = s.metaRecords<AuxGaussRecord>(KEY);
    REQUIRE(recs.size() == 3);
    CHECK(recs[0].sigma == 0.f); // zero-initialised
    CHECK(recs[0].counter == 0u);

    recs[1].sigma = 0.7f;
    recs[1].counter = 5;
    CHECK(s.metaRecords<AuxGaussRecord>(KEY)[1].sigma == 0.7f);
    CHECK(s.metaRecords<AuxGaussRecord>(KEY)[1].counter == 5u);

    // A copy is independent (deep byte copy)
    GAuxiliaryStore t(s);
    REQUIRE(t.hasAux(KEY));
    CHECK(t.metaRecords<AuxGaussRecord>(KEY)[1].sigma == 0.7f);
    t.metaRecords<AuxGaussRecord>(KEY)[1].sigma = 0.1f;
    CHECK(s.metaRecords<AuxGaussRecord>(KEY)[1].sigma == 0.7f); // s unaffected

    // clearScratch drops the blocks
    s.clearScratch();
    CHECK_FALSE(s.hasAux(KEY));
}

/******************************************************************************/
TEST_CASE("Individual scratch: clone copies it; compare ignores it; resetPersonality drops it", "[aux]") {
    using Gem::Geneva::Individuals::GTestIndividual3;

    // The per-group POD adaption scratch is OA-owned and rides on the individual itself (in its
    // GAuxiliaryStore). Its clone-copies / compare-ignores / boundary-clears contract is therefore a
    // property of the individual.
    auto ind = std::make_unique<GTestIndividual3>();

    // A clone taken BEFORE installing scratch -> an identical-genome twin with no scratch.
    auto bare = ind->clone();
    REQUIRE_FALSE(bare->scratch().hasAux(KEY));

    // Install per-group metadata on the individual's scratch only.
    ind->scratch().installAuxBlock<AuxGaussRecord>(KEY, 2);
    ind->scratch().metaRecords<AuxGaussRecord>(KEY)[0].sigma = 1.5f;
    REQUIRE(ind->scratch().hasAux(KEY));

    // A clone taken AFTER installing copies the scratch, independently.
    auto twin = ind->clone();
    REQUIRE(twin->scratch().hasAux(KEY));
    CHECK(twin->scratch().metaRecords<AuxGaussRecord>(KEY)[0].sigma == 1.5f);
    twin->scratch().metaRecords<AuxGaussRecord>(KEY)[0].sigma = 9.f;
    CHECK(ind->scratch().metaRecords<AuxGaussRecord>(KEY)[0].sigma == 1.5f); // independent

    // compare() ignores the scratch: ind (with scratch) equals bare (without).
    CHECK_NOTHROW(ind->compare(
        *bare,
        Gem::Common::expectation::EQUALITY,
        Gem::Common::CE_DEF_SIMILARITY_DIFFERENCE
    ));

    // resetPersonality() drops the whole OA-owned scratch (personality + POD blocks).
    ind->resetPersonality();
    CHECK_FALSE(ind->scratch().hasAux(KEY));
}

/******************************************************************************/
// The scratch rides make_wire_omitted_ptr_member: serialized by value on a checkpoint/file, but omitted
// on the wire. Crucially, a wire load must leave the target's default (non-null) store in place rather
// than nulling scratch_ -- scratch() dereferences it unguarded, so nulling it would be a latent crash.
TEST_CASE("Individual scratch: kept on a checkpoint, omitted on the wire (target keeps its default)", "[aux][wire]") {
    using Gem::Geneva::Individuals::GTestIndividual3;
    using mode = Gem::Common::serializationMode;
    namespace c2 = Gem::Courtier;

    auto withScratch = []() {
        auto ind = std::make_shared<GTestIndividual3>();
        ind->scratch().installAuxBlock<AuxGaussRecord>(KEY, 2);
        ind->scratch().metaRecords<AuxGaussRecord>(KEY)[0].sigma = 1.5f;
        REQUIRE(ind->scratch().hasAux(KEY));
        return ind;
    };

    // --- checkpoint / file (no wire scope): the scratch travels by value ---
    {
        auto original = withScratch();
        GTestIndividual3 restored;
        restored.fromString(original->toString(mode::GEM_BINARY), mode::GEM_BINARY);
        REQUIRE(restored.scratch().hasAux(KEY)); // scratch survived the checkpoint
        CHECK(restored.scratch().metaRecords<AuxGaussRecord>(KEY)[0].sigma == 1.5f);
    }

    // --- wire (an enabled scope on both ends): the scratch is omitted; the target keeps its own default
    //     (non-null, empty) store -- NOT null. scratch() below would crash if the omitted member had
    //     nulled scratch_, so this pins the non-null invariant the wire-omitted-ptr policy preserves. ---
    {
        auto original = withScratch();

        c2::GWireBlobRegistry send_reg;
        c2::GWireSerializationContext send_ctx;
        send_ctx.enabled = true;
        send_ctx.registry = &send_reg;
        std::string wire;
        {
            c2::GWireSerializationScope const scope(&send_ctx);
            wire = original->toString(mode::GEM_BINARY); // first send: carries the layout, omits the scratch
        }

        c2::GWireBlobRegistry recv_reg;
        c2::GWireSerializationContext recv_ctx;
        recv_ctx.enabled = true;
        recv_ctx.registry = &recv_reg;
        GTestIndividual3 restored;
        {
            c2::GWireSerializationScope const scope(&recv_ctx);
            restored.fromString(wire, mode::GEM_BINARY);
        }
        CHECK_FALSE(restored.scratch().hasAux(KEY)); // scratch omitted; restored keeps its empty default (no crash)
    }
}
