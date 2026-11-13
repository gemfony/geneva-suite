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

// Integration test: a class's *generated* serialize (from GReflectiveInterfaceT
// + localMembers_) round-trips through the GArchive codecs, exactly as it does
// through a Boost archive. This validates the retargeted policy functions
// (ser_emit / ser_base_object -> archive_named / archive_named_base) and the
// mixin's archive-generic parent-slice serialization, over a real two-level
// hierarchy (so the "gemfonyParent" parent slice is exercised). The Boost path
// is unchanged (the retarget's else-branch) and its regression is covered by the
// full test suite.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>
#include <vector>

#include "weft/GBinaryArchive.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "weft/GJsonArchive.hpp"
#include "common/GMemberReflectionT.hpp"
#include "common/GReflectiveInterfaceT.hpp"

using namespace Gem::Common;

namespace {

// Hierarchy root (its own gemfony_common_root_t): parent is GCommonInterfaceT<PRoot>.
class PRoot : public GReflectiveInterfaceT<PRoot, GCommonInterfaceT<PRoot>> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(make_member("root_i_", self.root_i_), make_member("root_s_", self.root_s_));
    }

public:
    static constexpr std::string_view class_name = "PRoot";
    PRoot() = default;

    int root_i_ = 0;
    std::string root_s_;

protected:
};

// Derived: parent is PRoot (another mixin class) -> exercises the gemfonyParent
// parent-slice path. Also carries a container member and a load-only member.
class PDerived : public GReflectiveInterfaceT<PDerived, PRoot> {
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            make_member("d_d_", self.d_d_),
            make_member("d_v_", self.d_v_),
            make_load_only_member("d_loadonly_", self.d_loadonly_)
        );
    }

public:
    static constexpr std::string_view class_name = "PDerived";
    PDerived() = default;

    double d_d_ = 0.0;
    std::vector<int> d_v_;
    int d_loadonly_ = 0; ///< loaded/compared but NOT serialized (make_load_only_member)

protected:
};

template <typename OArchive, typename IArchive, typename T>
T gem_roundtrip(const T &in) {
    OArchive oa;
    oa &in;
    std::string encoded = oa.str();

    IArchive ia(encoded);
    T out;
    ia &out;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("GArchive integration: generated serialize round-trips a single-level mixin class",
          "[common][archive][integration]") {
    PRoot a;
    a.root_i_ = 7;
    a.root_s_ = "root";

    SECTION("binary") {
        using namespace Gem::Weft;
        PRoot b = gem_roundtrip<GBinaryOArchive, GBinaryIArchive>(a);
        CHECK(b.root_i_ == 7);
        CHECK(b.root_s_ == "root");
    }
    SECTION("json") {
        using namespace Gem::Weft;
        PRoot b = gem_roundtrip<GJsonOArchive, GJsonIArchive>(a);
        CHECK(b.root_i_ == 7);
        CHECK(b.root_s_ == "root");
    }
}

TEST_CASE("GArchive integration: generated serialize round-trips the parent slice of a two-level class",
          "[common][archive][integration]") {
    PDerived a;
    a.root_i_ = 5;             // inherited from PRoot -> travels via the gemfonyParent slice
    a.root_s_ = "inherited";
    a.d_d_ = 2.5;
    a.d_v_ = {1, 2, 3};
    a.d_loadonly_ = 99;        // load-only -> must NOT be serialized

    SECTION("binary") {
        using namespace Gem::Weft;
        PDerived b = gem_roundtrip<GBinaryOArchive, GBinaryIArchive>(a);
        CHECK(b.root_i_ == 5);       // parent slice survived
        CHECK(b.root_s_ == "inherited");
        CHECK(b.d_d_ == 2.5);
        CHECK(b.d_v_ == std::vector<int>{1, 2, 3});
        CHECK(b.d_loadonly_ == 0);   // load-only skipped by serialize -> stays default
    }
    SECTION("json") {
        using namespace Gem::Weft;
        PDerived b = gem_roundtrip<GJsonOArchive, GJsonIArchive>(a);
        CHECK(b.root_i_ == 5);
        CHECK(b.root_s_ == "inherited");
        CHECK(b.d_d_ == 2.5);
        CHECK(b.d_v_ == std::vector<int>{1, 2, 3});
        CHECK(b.d_loadonly_ == 0);
    }
}

TEST_CASE("GArchive integration: JSON output nests the parent slice under gemfonyParent",
          "[common][archive][integration]") {
    using namespace Gem::Weft;
    PDerived a;
    a.root_i_ = 5;
    a.d_d_ = 2.5;

    GJsonOArchive oa;
    oa &a;
    const boost::json::value &v = oa.value();
    REQUIRE(v.is_object());
    // The derived member and the nested parent slice both appear, keyed by name.
    CHECK(v.as_object().contains("d_d_"));
    CHECK(v.as_object().contains("gemfonyParent"));
    REQUIRE(v.as_object().at("gemfonyParent").is_object());
    CHECK(v.as_object().at("gemfonyParent").as_object().contains("root_i_"));
    // The load-only member was skipped, so it must not appear.
    CHECK_FALSE(v.as_object().contains("d_loadonly_"));
}
