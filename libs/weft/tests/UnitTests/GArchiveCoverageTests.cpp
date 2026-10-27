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

// Parity coverage: the std types Boost.Serialization ships support for that go
// beyond the primitives/containers already covered by GBinaryArchiveTests /
// GJsonArchiveTests -- optional, variant, complex, bitset, forward_list, the
// multi-key associative containers, and the caller-managed raw ranges
// make_array / make_binary (the two Geneva actually serializes today).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bitset>
#include <chrono>
#include <complex>
#include <cstdint>
#include <forward_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "weft/GBinaryArchive.hpp"
#include "weft/GJsonArchive.hpp"

using namespace Gem::Weft;

namespace {

template <typename OArchive, typename IArchive, typename T>
T rt(const T &in) {
    OArchive oa;
    oa &in;
    IArchive ia(oa.str());
    T out{};
    ia &out;
    return out;
}

// Round-trip a value through both codecs and assert equality each way. A helper
// (not a macro) so brace-init arguments with commas pass through cleanly.
template <typename T>
void both_codecs(const T &in) {
    CHECK((rt<GBinaryOArchive, GBinaryIArchive>(in)) == in);
    CHECK((rt<GJsonOArchive, GJsonIArchive>(in)) == in);
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("GArchive coverage: std::optional (empty and engaged)", "[common][archive][coverage]") {
    both_codecs(std::optional<int>{});
    both_codecs(std::optional<int>{42});
    both_codecs(std::optional<std::string>{"engaged"});
    both_codecs(std::optional<std::vector<int>>{{1, 2, 3}});
}

TEST_CASE("GArchive coverage: std::chrono::duration (raw count)", "[common][archive][coverage]") {
    both_codecs(std::chrono::duration<double>(123.5));   // the OA max/min run-time members
    both_codecs(std::chrono::duration<double>(0.0));
    both_codecs(std::chrono::milliseconds(2500));        // an integer-rep duration
    both_codecs(std::chrono::seconds(-7));
}

TEST_CASE("GArchive coverage: std::complex", "[common][archive][coverage]") {
    both_codecs(std::complex<double>(1.5, -2.25));
    both_codecs(std::complex<float>(0.0f, 3.0f));
}

TEST_CASE("GArchive coverage: std::bitset", "[common][archive][coverage]") {
    both_codecs(std::bitset<8>(0b10110011));
    both_codecs(std::bitset<64>(0xDEADBEEFCAFEBABEULL));
}

TEST_CASE("GArchive coverage: std::variant (each alternative)", "[common][archive][coverage]") {
    using V = std::variant<int, std::string, double>;
    both_codecs(V{7});
    both_codecs(V{std::string{"middle"}});
    both_codecs(V{3.5});
}

TEST_CASE("GArchive coverage: std::forward_list", "[common][archive][coverage]") {
    both_codecs(std::forward_list<int>{});
    both_codecs(std::forward_list<int>{1, 2, 3, 4});
    both_codecs(std::forward_list<std::string>{"a", "b", "c"});
}

TEST_CASE("GArchive coverage: multi-key associative containers (duplicates survive)",
          "[common][archive][coverage]") {
    std::multiset<int> ms{1, 1, 2, 3, 3, 3};
    CHECK((rt<GBinaryOArchive, GBinaryIArchive>(ms)) == ms);
    CHECK((rt<GJsonOArchive, GJsonIArchive>(ms)) == ms);

    std::multimap<int, std::string> mm{{1, "a"}, {1, "b"}, {2, "c"}};
    CHECK((rt<GBinaryOArchive, GBinaryIArchive>(mm)) == mm);
    CHECK((rt<GJsonOArchive, GJsonIArchive>(mm)) == mm);

    std::unordered_multiset<int> ums{5, 5, 6};
    CHECK((rt<GBinaryOArchive, GBinaryIArchive>(ums)) == ums);

    std::unordered_multimap<int, int> umm{{1, 10}, {1, 11}, {2, 20}};
    CHECK((rt<GBinaryOArchive, GBinaryIArchive>(umm)) == umm);
}

// ---------------------------------------------------------------------------
// Caller-managed raw ranges: make_array (elements) and make_binary (bytes).
// The count is supplied by the caller both ways; the buffer is pre-sized.

namespace {

template <typename OArchive, typename IArchive>
void check_make_array() {
    std::vector<double> src{1.5, -2.5, 3.75, 4.0};
    OArchive oa;
    oa &make_array(src.data(), src.size());

    std::vector<double> dst(src.size(), 0.0); // pre-sized by the caller
    IArchive ia(oa.str());
    ia &make_array(dst.data(), dst.size());
    CHECK(dst == src);
}

template <typename OArchive, typename IArchive>
void check_make_binary() {
    std::array<unsigned char, 5> src{0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    OArchive oa;
    oa &make_binary(src.data(), src.size());

    std::array<unsigned char, 5> dst{};
    IArchive ia(oa.str());
    ia &make_binary(dst.data(), dst.size());
    CHECK(dst == src);
}

} // namespace

TEST_CASE("GArchive coverage: make_array round-trips a caller-sized element range", "[common][archive][coverage]") {
    check_make_array<GBinaryOArchive, GBinaryIArchive>();
    check_make_array<GJsonOArchive, GJsonIArchive>();
}

TEST_CASE("GArchive coverage: make_binary round-trips a caller-sized byte block", "[common][archive][coverage]") {
    check_make_binary<GBinaryOArchive, GBinaryIArchive>();
    check_make_binary<GJsonOArchive, GJsonIArchive>();
}

TEST_CASE("GArchive coverage: make_array writes no length prefix (binary is raw)", "[common][archive][coverage]") {
    // 3 doubles, no count in the stream -> exactly 3 * 8 bytes.
    std::vector<double> src{1.0, 2.0, 3.0};
    GBinaryOArchive oa;
    oa &make_array(src.data(), src.size());
    CHECK(oa.str().size() == src.size() * sizeof(double));
}

// ---------------------------------------------------------------------------
// Non-intrusive class support: a POD-clean struct that carries NO serialize
// member and instead supplies a free gem_archive_serialize() in its namespace,
// found by ADL from the class dispatch arm (the analogue of a Boost
// non-intrusive free serialize()). This is the entry point the layout structs
// (GaussConfig / ChannelLayout / GGenomeLayout) use.

namespace {

struct PodPoint {
    int x = 0;
    double y = 0.0;
    std::string tag;
    bool operator==(const PodPoint &) const = default;
};

// The free serializer, in the SAME namespace as PodPoint so ADL finds it.
template <typename Archive>
void gem_archive_serialize(Archive &ar, PodPoint &p) {
    ar &make_nvp("x", p.x);
    ar &make_nvp("y", p.y);
    ar &make_nvp("tag", p.tag);
}

} // namespace

TEST_CASE("GArchive coverage: non-intrusive free gem_archive_serialize round-trips", "[common][archive][coverage]") {
    both_codecs(PodPoint{-7, 3.5, "hello"});

    // Also exercise it nested inside a container (vector of non-intrusive structs).
    std::vector<PodPoint> v{{1, 1.5, "a"}, {2, -2.5, "b"}, {3, 0.0, ""}};
    CHECK((rt<GBinaryOArchive, GBinaryIArchive>(v)) == v);
    CHECK((rt<GJsonOArchive, GJsonIArchive>(v)) == v);
}

// ---------------------------------------------------------------------------
// Owning smart pointers to a CONCRETE, non-hierarchy pointee (no
// gemfony_common_root_t): serialized inline by value (present flag + pointee),
// the analogue of Boost serializing a shared_ptr<Concrete>. This is the shape of
// the individual's non-polymorphic owned members (GProblemPolicy, GAuxiliaryStore).
// The polymorphic-pointer path (hierarchy pointee via the registry) is covered by
// GArchivePolymorphicTests.

namespace {

// A concrete pointee whose default constructor is PRIVATE (kept only for de-serialization),
// befriending Weft::access exactly as GIndividualFactory does. The non-polymorphic owned-pointer
// load path must reconstruct it through access::construct_raw, not a public new.
class PrivateCtorPod {
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] unsigned int version) {
        ar &make_nvp("v", v_);
    }

public:
    explicit PrivateCtorPod(int v) : v_(v) {}
    bool operator==(const PrivateCtorPod &) const = default;
    int v_ = 0;

private:
    PrivateCtorPod() = default; ///< Reachable only through the befriended access shim.
};

template <typename OArchive, typename IArchive, typename Ptr>
void check_concrete_ptr(const Ptr &in) {
    OArchive oa;
    oa &make_nvp("p", const_cast<Ptr &>(in));
    Ptr out;
    IArchive ia(oa.str());
    ia &make_nvp("p", out);
    if (in) {
        REQUIRE(static_cast<bool>(out));
        CHECK(*out == *in);
    } else {
        CHECK_FALSE(static_cast<bool>(out));
    }
}

} // namespace

TEST_CASE("GArchive coverage: non-polymorphic owned pointer (unique_ptr / shared_ptr to concrete)",
          "[common][archive][coverage]") {
    // unique_ptr, engaged and null.
    check_concrete_ptr<GBinaryOArchive, GBinaryIArchive>(std::make_unique<PodPoint>(PodPoint{9, -1.25, "u"}));
    check_concrete_ptr<GJsonOArchive, GJsonIArchive>(std::make_unique<PodPoint>(PodPoint{9, -1.25, "u"}));
    check_concrete_ptr<GBinaryOArchive, GBinaryIArchive>(std::unique_ptr<PodPoint>{});
    check_concrete_ptr<GJsonOArchive, GJsonIArchive>(std::unique_ptr<PodPoint>{});

    // shared_ptr, engaged and null.
    check_concrete_ptr<GBinaryOArchive, GBinaryIArchive>(std::make_shared<PodPoint>(PodPoint{-3, 4.5, "s"}));
    check_concrete_ptr<GJsonOArchive, GJsonIArchive>(std::make_shared<PodPoint>(PodPoint{-3, 4.5, "s"}));
    check_concrete_ptr<GBinaryOArchive, GBinaryIArchive>(std::shared_ptr<PodPoint>{});
    check_concrete_ptr<GJsonOArchive, GJsonIArchive>(std::shared_ptr<PodPoint>{});
}

TEST_CASE("GArchive coverage: non-polymorphic owned pointer to a PRIVATE-ctor concrete pointee",
          "[common][archive][coverage]") {
    // The load path reconstructs the pointee through access::construct_raw (the concrete-pointer
    // analogue of the polymorphic factory seam) -- the shape of the meta-optimizer's ind_factory_.
    check_concrete_ptr<GBinaryOArchive, GBinaryIArchive>(std::make_shared<PrivateCtorPod>(77));
    check_concrete_ptr<GJsonOArchive, GJsonIArchive>(std::make_shared<PrivateCtorPod>(77));
    check_concrete_ptr<GBinaryOArchive, GBinaryIArchive>(std::make_unique<PrivateCtorPod>(-5));
    check_concrete_ptr<GJsonOArchive, GJsonIArchive>(std::make_unique<PrivateCtorPod>(-5));
}
