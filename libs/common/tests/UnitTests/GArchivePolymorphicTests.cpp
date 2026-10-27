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

// Step-3b integration test: a GArchive (de)serializes a smart pointer to a
// hierarchy root and reconstructs the exact DYNAMIC type on load, through the
// registry-thunk dispatch. Exercises both codecs, both smart-pointer flavours,
// the null pointer, containers of polymorphic pointers, and the parent slice of
// a reconstructed derived.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "weft/GArchivePolymorphic.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GMemberReflectionT.hpp"
#include "common/GReflectiveInterfaceT.hpp"

using namespace Gem::Common;
using Gem::Weft::GBinaryIArchive;
using Gem::Weft::GBinaryOArchive;
using Gem::Weft::GJsonIArchive;
using Gem::Weft::GJsonOArchive;

namespace {

// A small polymorphic hierarchy rooted at PolyBase.
class PolyBase : public GReflectiveInterfaceT<PolyBase, GCommonInterfaceT<PolyBase>> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(make_member("base_id_", self.base_id_));
    }

public:
    static constexpr std::string_view class_name = "PolyBase";
    PolyBase() = default;

    int base_id_ = 0;
    [[nodiscard]] virtual int kind() const { return 0; }

protected:
};

class PolyA : public GReflectiveInterfaceT<PolyA, PolyBase> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(make_member("a_val_", self.a_val_));
    }

public:
    static constexpr std::string_view class_name = "PolyA";
    PolyA() = default;

    double a_val_ = 0.0;
    [[nodiscard]] int kind() const override { return 1; }

protected:
};

class PolyB : public GReflectiveInterfaceT<PolyB, PolyBase> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(make_member("b_str_", self.b_str_));
    }

public:
    static constexpr std::string_view class_name = "PolyB";
    PolyB() = default;

    std::string b_str_;
    [[nodiscard]] int kind() const override { return 2; }

protected:
};

// A derived type whose default constructor is PRIVATE (only de-serialization needs it), befriending
// GReflectiveInterfaceAccess exactly as a real mixin-managed geneva class does. This exercises the
// private-ctor factory path in register_archivable: the registry factory cannot std::make_unique it,
// so it must reconstruct through GReflectiveInterfaceAccess::construct (regression for that seam).
class PolyC : public GReflectiveInterfaceT<PolyC, PolyBase> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(make_member("c_val_", self.c_val_));
    }

public:
    static constexpr std::string_view class_name = "PolyC";
    explicit PolyC(int c) : c_val_(c) { /* public value ctor */ }

    int c_val_ = 0;
    [[nodiscard]] int kind() const override { return 3; }

protected:

private:
    PolyC() = default; ///< Private: only reachable through the befriended access shim (de-serialization).
};

// A type registered IDENTITY-ONLY (GEM_REGISTER_TYPE) rather than archivable: it lands in the
// GPolymorphicRegistry but has no dispatch thunk, so the boot-time completeness self-check must
// flag it. This is the exact mistake (identity-only where GEM_REGISTER_ARCHIVABLE was needed) the
// check exists to catch at startup instead of at the first deserialization.
class PolyIdentityOnly : public GReflectiveInterfaceT<PolyIdentityOnly, PolyBase> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(make_member("io_val_", self.io_val_));
    }

public:
    static constexpr std::string_view class_name = "PolyIdentityOnly";
    PolyIdentityOnly() = default;

    int io_val_ = 0;
    [[nodiscard]] int kind() const override { return 4; }

protected:
};

} // namespace

GEM_REGISTER_ARCHIVABLE(PolyBase)
GEM_REGISTER_ARCHIVABLE(PolyA)
GEM_REGISTER_ARCHIVABLE(PolyB)
GEM_REGISTER_ARCHIVABLE(PolyC)
GEM_REGISTER_TYPE(PolyIdentityOnly) // identity only -- deliberately NOT archive-dispatchable

namespace {

template <typename OArchive, typename IArchive, typename SmartPtr>
SmartPtr ptr_roundtrip(const SmartPtr &in) {
    OArchive oa;
    oa &in;
    std::string encoded = oa.str();

    IArchive ia(encoded);
    SmartPtr out;
    ia &out;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("GArchive polymorphic: a shared_ptr reconstructs the dynamic type", "[common][archive][poly]") {
    auto a = std::make_shared<PolyA>();
    a->base_id_ = 7;   // in the PolyBase slice (travels via gemfonyParent)
    a->a_val_ = 3.5;
    std::shared_ptr<PolyBase> p = a;

    SECTION("binary") {
        std::shared_ptr<PolyBase> q = ptr_roundtrip<GBinaryOArchive, GBinaryIArchive>(p);
        REQUIRE(q);
        CHECK(q->kind() == 1); // dynamic type is PolyA
        CHECK(q->base_id_ == 7);
        auto qa = std::dynamic_pointer_cast<PolyA>(q);
        REQUIRE(qa);
        CHECK(qa->a_val_ == 3.5);
    }
    SECTION("json") {
        std::shared_ptr<PolyBase> q = ptr_roundtrip<GJsonOArchive, GJsonIArchive>(p);
        REQUIRE(q);
        CHECK(q->kind() == 1);
        CHECK(q->base_id_ == 7);
        auto qa = std::dynamic_pointer_cast<PolyA>(q);
        REQUIRE(qa);
        CHECK(qa->a_val_ == 3.5);
    }
}

TEST_CASE("GArchive polymorphic: a unique_ptr reconstructs the dynamic type", "[common][archive][poly]") {
    auto b = std::make_unique<PolyB>();
    b->base_id_ = 4;
    b->b_str_ = "hello";
    std::unique_ptr<PolyBase> p = std::move(b);

    SECTION("binary") {
        std::unique_ptr<PolyBase> q = ptr_roundtrip<GBinaryOArchive, GBinaryIArchive>(p);
        REQUIRE(q);
        CHECK(q->kind() == 2);
        CHECK(q->base_id_ == 4);
        CHECK(dynamic_cast<PolyB *>(q.get())->b_str_ == "hello");
    }
    SECTION("json") {
        std::unique_ptr<PolyBase> q = ptr_roundtrip<GJsonOArchive, GJsonIArchive>(p);
        REQUIRE(q);
        CHECK(q->kind() == 2);
        CHECK(dynamic_cast<PolyB *>(q.get())->b_str_ == "hello");
    }
}

TEST_CASE("GArchive polymorphic: a null pointer round-trips as null", "[common][archive][poly]") {
    std::shared_ptr<PolyBase> p; // null

    CHECK_FALSE(ptr_roundtrip<GBinaryOArchive, GBinaryIArchive>(p));
    CHECK_FALSE(ptr_roundtrip<GJsonOArchive, GJsonIArchive>(p));
}

TEST_CASE("GArchive polymorphic: a container of mixed dynamic types round-trips", "[common][archive][poly]") {
    std::vector<std::shared_ptr<PolyBase>> v;
    auto a = std::make_shared<PolyA>();
    a->a_val_ = 1.25;
    auto b = std::make_shared<PolyB>();
    b->b_str_ = "x";
    v.push_back(a);
    v.push_back(b);
    v.push_back(nullptr);
    v.push_back(std::make_shared<PolyBase>());

    auto check = [](const std::vector<std::shared_ptr<PolyBase>> &w) {
        REQUIRE(w.size() == 4);
        CHECK(w[0]->kind() == 1);
        CHECK(std::dynamic_pointer_cast<PolyA>(w[0])->a_val_ == 1.25);
        CHECK(w[1]->kind() == 2);
        CHECK(std::dynamic_pointer_cast<PolyB>(w[1])->b_str_ == "x");
        CHECK_FALSE(w[2]); // null preserved
        REQUIRE(w[3]);
        CHECK(w[3]->kind() == 0); // exactly PolyBase
    };

    check(ptr_roundtrip<GBinaryOArchive, GBinaryIArchive>(v));
    check(ptr_roundtrip<GJsonOArchive, GJsonIArchive>(v));
}

TEST_CASE("GArchive polymorphic: a private-default-ctor type reconstructs through the friend shim",
          "[common][archive][poly]") {
    // PolyC has NO public default constructor; the registry factory reconstructs it via
    // GReflectiveInterfaceAccess::construct. A round-trip proves the dynamic type is rebuilt.
    auto c = std::make_shared<PolyC>(42);
    c->base_id_ = 5;
    std::shared_ptr<PolyBase> p = c;

    SECTION("binary") {
        std::shared_ptr<PolyBase> q = ptr_roundtrip<GBinaryOArchive, GBinaryIArchive>(p);
        REQUIRE(q);
        CHECK(q->kind() == 3); // dynamic type is PolyC, rebuilt through the private ctor
        CHECK(q->base_id_ == 5);
        auto qc = std::dynamic_pointer_cast<PolyC>(q);
        REQUIRE(qc);
        CHECK(qc->c_val_ == 42);
    }
    SECTION("json") {
        std::shared_ptr<PolyBase> q = ptr_roundtrip<GJsonOArchive, GJsonIArchive>(p);
        REQUIRE(q);
        CHECK(q->kind() == 3);
        CHECK(q->base_id_ == 5);
        auto qc = std::dynamic_pointer_cast<PolyC>(q);
        REQUIRE(qc);
        CHECK(qc->c_val_ == 42);
    }
}

TEST_CASE("GArchive polymorphic: JSON output records the dynamic tag", "[common][archive][poly]") {
    auto a = std::make_shared<PolyA>();
    a->a_val_ = 9.0;
    std::shared_ptr<PolyBase> p = a;

    GJsonOArchive oa;
    oa &p;
    std::string text = oa.str();
    CHECK(text.find("\"tag\":\"PolyA\"") != std::string::npos);
    CHECK(text.find("\"present\":true") != std::string::npos);
}

TEST_CASE("GArchive polymorphic: the completeness self-check flags an identity-only registration",
          "[common][archive][poly]") {
    // The archivable PolyBase/PolyA/PolyB/PolyC are fully dispatchable; PolyIdentityOnly was
    // registered identity-only (GEM_REGISTER_TYPE), so it must surface as a gap.
    const auto gaps = Gem::Weft::archiveRegistrationGaps();

    // At least one hierarchy (PolyBase) has archivable types, so a checker is registered.
    CHECK(Gem::Weft::archiveRegisteredHierarchyCount() >= 1);

    // The identity-only type is reported ...
    const bool flags_identity_only =
        std::any_of(gaps.begin(), gaps.end(),
                    [](const std::string &g) { return g.find("PolyIdentityOnly") != std::string::npos; });
    CHECK(flags_identity_only);

    // ... and the fully-archivable types are NOT.
    for (const char *ok : {"\"PolyA\"", "\"PolyB\"", "\"PolyC\""}) {
        const bool flagged =
            std::any_of(gaps.begin(), gaps.end(),
                        [&](const std::string &g) { return g.find(ok) != std::string::npos; });
        CHECK_FALSE(flagged);
    }

    // The hard assertion form throws while the gap stands.
    CHECK_THROWS(Gem::Weft::verifyArchiveRegistrations());
}
