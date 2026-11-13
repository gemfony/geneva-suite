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

#include <sstream>


#include "courtier/GDemoProcessingContainers.hpp"
#include "weft/GBinaryArchive.hpp" // Gem::Weft::GBinary[IO]Archive

using namespace Gem::Courtier;

/******************************************************************************/
/**
 * B0 pin for the D11 stable lineage id (GProcessable::submission_uuid_). The id is a stable, per-individual
 * identity used by the networked consumer to reunite / de-duplicate late returns; the per-dispatch
 * correlation id keeps routing. The contract is the deliberate asymmetry across the three copy paths
 * (encoded once in detail::LineageId): a copy is a NEW individual (fresh id), an assignment / load KEEPS the
 * target's own id, and a serialized round-trip PRESERVES the id. A demo container (a GProcessingContainerT,
 * hence a GProcessable) exercises the base contract without pulling in geneva.
 */
TEST_CASE("GProcessable lineage id: fresh on copy, kept on assign, preserved on serialize", "[proc][id]") {
    GSimpleContainer const a(42);
    GSimpleContainer b(43);

    // Every construction mints a distinct id.
    CHECK(a.getSubmissionUuid() != b.getSubmissionUuid());

    // COPY CONSTRUCTION == a new individual (offspring / refill) -> a FRESH id, not the source's.
    GSimpleContainer const copy(a);
    CHECK(copy.getSubmissionUuid() != a.getSubmissionUuid());

    // COPY ASSIGNMENT (the load(other) path) -> the target KEEPS its own lineage.
    const auto b_id_before = b.getSubmissionUuid();
    b = a;
    CHECK(b.getSubmissionUuid() == b_id_before);
    CHECK(b.getSubmissionUuid() != a.getSubmissionUuid());

    // SERIALIZED ROUND-TRIP (wire / checkpoint) -> the id is PRESERVED (a deserialized object is first
    // default-constructed with a fresh id, then the archived value overwrites it).
    std::string blob;
    {
        Gem::Weft::GBinaryOArchive oa;
        oa &Gem::Weft::make_nvp("item", a);
        blob = oa.str();
    }
    GSimpleContainer restored(0);
    REQUIRE(restored.getSubmissionUuid() != a.getSubmissionUuid()); // distinct before the load
    {
        Gem::Weft::GBinaryIArchive ia(blob);
        ia &Gem::Weft::make_nvp("item", restored);
    }
    CHECK(restored.getSubmissionUuid() == a.getSubmissionUuid());

    // setSubmissionUuid() restores a chosen identity onto a fresh clone -- the mechanism by which a copy
    // stands in for the item it represents.
    GSimpleContainer retention(copy); // a fresh id
    REQUIRE(retention.getSubmissionUuid() != a.getSubmissionUuid());
    retention.setSubmissionUuid(a.getSubmissionUuid());
    CHECK(retention.getSubmissionUuid() == a.getSubmissionUuid());
}
