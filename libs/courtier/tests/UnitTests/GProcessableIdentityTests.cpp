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

#include "GDemoProcessingContainers.hpp"
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
TEST_CASE(
    "GProcessable lineage id: fresh on copy but kept on assign and on serialize",
    "[proc][id]"
) {
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

/******************************************************************************/
/**
 * The marks of a failed evaluation (detail::SubmissionMarks) belong to the object they were set on --
 * the slot of a batch -- like the lineage id: a copy is a new item and carries none, an assignment
 * (a load) and an absorbed return keep the target's own, and they are not serialized. A networked
 * consumer absorbs a deserialized return into the slot, so the submitter's mark must survive that.
 */
TEST_CASE(
    "GProcessable marks: none on a copy and kept on assign and absorb but not serialized",
    "[proc][id][substitute]"
) {
    GSimpleContainer a(42);
    REQUIRE(a.isSubstitutable());
    REQUIRE_FALSE(a.isSubstitute());
    a.setSubstitutable(false);
    REQUIRE_FALSE(a.isSubstitutable());

    // COPY CONSTRUCTION -> a new item without marks.
    GSimpleContainer const copy(a);
    CHECK(copy.isSubstitutable());

    // COPY ASSIGNMENT (load) -> the target keeps its own marks, in both directions.
    GSimpleContainer b(43);
    b = a;
    CHECK(b.isSubstitutable());
    GSimpleContainer c(44);
    c.setSubstitutable(false);
    c = copy;
    CHECK_FALSE(c.isSubstitutable());

    // An ABSORBED return (the networked consumer's check-in) keeps the slot's mark.
    c.absorbResultsFrom(GSimpleContainer(45));
    CHECK_FALSE(c.isSubstitutable());

    // SERIALIZED ROUND-TRIP -> the marks do not travel.
    std::string blob;
    {
        Gem::Weft::GBinaryOArchive oa;
        oa &Gem::Weft::make_nvp("item", a);
        blob = oa.str();
    }
    GSimpleContainer restored(0);
    {
        Gem::Weft::GBinaryIArchive ia(blob);
        ia &Gem::Weft::make_nvp("item", restored);
    }
    CHECK(restored.isSubstitutable());

    // The submitter sets the mark again.
    a.setSubstitutable(true);
    CHECK(a.isSubstitutable());
}
