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

#include <atomic>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "weft/GBinaryArchive.hpp"
#include "weft/GWeftError.hpp"

using Gem::Weft::base_object;
using Gem::Weft::GBinaryIArchive;
using Gem::Weft::GBinaryOArchive;

// ---------------------------------------------------------------------------
// Round-trip helper: save v, load into a fresh T, assert the stream was fully
// consumed, and return the loaded value for an EQUAL check by the caller.

namespace {

template <typename T>
T roundtrip(const T &v) {
    GBinaryOArchive oa;
    oa &v;
    std::string bytes = oa.str();

    GBinaryIArchive ia(bytes);
    T out{};
    ia &out;
    CHECK(ia.exhausted()); // a well-formed full read consumes every byte
    return out;
}

// A serializable value class with a *public* serialize.
struct Point {
    std::int32_t x = 0;
    double y = 0.0;
    std::string name;

    template <typename Archive>
    void serialize(Archive &ar, unsigned) {
        ar &Gem::Weft::make_nvp("x", x);
        ar &Gem::Weft::make_nvp("y", y);
        ar &Gem::Weft::make_nvp("name", name);
    }
    bool operator==(const Point &) const = default;
};

// A serializable value class with a *private* serialize reached via the access
// shim -- the common real-world case (Boost-style friending).
class Secret {
public:
    Secret() = default;
    Secret(int a, std::string b) : a_(a), b_(std::move(b)) {}
    bool operator==(const Secret &) const = default;

private:
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, unsigned) {
        ar &Gem::Weft::make_nvp("a", a_);
        ar &Gem::Weft::make_nvp("b", b_);
    }
    int a_ = 0;
    std::string b_;
};

// Inheritance, to exercise base_object.
struct Base {
    int b = 0;
    template <typename Archive>
    void serialize(Archive &ar, unsigned) {
        ar &Gem::Weft::make_nvp("b", b);
    }
    bool operator==(const Base &) const = default;
};

struct Derived : Base {
    int d = 0;
    template <typename Archive>
    void serialize(Archive &ar, unsigned) {
        ar &base_object<Base>(*this);
        ar &Gem::Weft::make_nvp("d", d);
    }
    bool operator==(const Derived &) const = default;
};

enum class Color : std::uint8_t { red = 1, green = 2, blue = 250 };

} // namespace

// ---------------------------------------------------------------------------
// Integer widths and signedness.

TEST_CASE("GBinaryArchive: integer widths round-trip exactly", "[common][archive][binary]") {
    CHECK(roundtrip<std::int8_t>(-42) == -42);
    CHECK(roundtrip<std::uint8_t>(250) == 250);
    CHECK(roundtrip<std::int16_t>(-30000) == -30000);
    CHECK(roundtrip<std::uint16_t>(60000) == 60000);
    CHECK(roundtrip<std::int32_t>(-2000000000) == -2000000000);
    CHECK(roundtrip<std::uint32_t>(4000000000u) == 4000000000u);
    CHECK(roundtrip<std::int64_t>(std::numeric_limits<std::int64_t>::min()) ==
          std::numeric_limits<std::int64_t>::min());
    CHECK(roundtrip<std::uint64_t>(std::numeric_limits<std::uint64_t>::max()) ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(roundtrip<char>('Q') == 'Q');
}

TEST_CASE("GBinaryArchive: natural width is honoured on the wire", "[common][archive][binary]") {
    GBinaryOArchive oa;
    std::uint8_t byte = 7;
    std::uint64_t word = 7;
    oa &byte;
    oa &word;
    CHECK(oa.str().size() == 1 + 8); // one byte for the u8, eight for the u64
}

TEST_CASE("GBinaryArchive: bool and enum round-trip", "[common][archive][binary]") {
    CHECK(roundtrip<bool>(true) == true);
    CHECK(roundtrip<bool>(false) == false);
    CHECK(roundtrip<Color>(Color::blue) == Color::blue);
    CHECK(roundtrip<Color>(Color::red) == Color::red);
}

// ---------------------------------------------------------------------------
// Floating point, including the long-double sharp edge.

TEST_CASE("GBinaryArchive: float and double round-trip bit-exactly", "[common][archive][binary]") {
    CHECK(roundtrip<float>(3.14159f) == 3.14159f);
    CHECK(roundtrip<double>(2.718281828459045) == 2.718281828459045);
    CHECK(roundtrip<double>(std::numeric_limits<double>::min()) == std::numeric_limits<double>::min());
    CHECK(roundtrip<double>(std::numeric_limits<double>::denorm_min()) ==
          std::numeric_limits<double>::denorm_min());
    // Negative zero must survive as negative zero (bit pattern, not just == value).
    double back = roundtrip<double>(-0.0);
    CHECK(std::bit_cast<std::uint64_t>(back) == std::bit_cast<std::uint64_t>(-0.0));
}

TEST_CASE("GBinaryArchive: long double round-trips bit-exactly (normalized-genome edge)",
          "[common][archive][binary]") {
    // The normalized-coordinate model composes external values in long double;
    // the codec must reproduce them exactly within one binary.
    long double a = 3.14159265358979323846264338327950288L;
    long double b = std::numeric_limits<long double>::min();
    long double c = std::numeric_limits<long double>::max();
    long double d = -1.0L / 3.0L;
    CHECK(roundtrip<long double>(a) == a);
    CHECK(roundtrip<long double>(b) == b);
    CHECK(roundtrip<long double>(c) == c);
    CHECK(roundtrip<long double>(d) == d);
}

// long double is encoded as a canonical, architecture-portable IEEE-754 binary128 (16 bytes),
// NOT as the platform's raw long double slot (which is 80-bit-in-16-with-padding on x86-64 and
// not cross-arch portable). The encoded width proves the canonical form.
TEST_CASE("GBinaryArchive: long double is a canonical 16-byte binary128", "[common][archive][binary]") {
    const long double v = 3.14159265358979323846264338327950288L;
    GBinaryOArchive oa;
    oa &v;
    CHECK(oa.str().size() == 16); // exactly one binary128, regardless of the platform long double format

    long double back{};
    GBinaryIArchive ia(oa.str());
    ia &back;
    CHECK(back == v);

    // The full 64-bit significand of an x86-80-bit value survives the 80->128->80 round trip.
    const long double e = std::nextafter(1.0L, 2.0L); // 1 + 1 ULP: exercises the low significand bit
    CHECK(roundtrip<long double>(e) == e);
}

// ---------------------------------------------------------------------------
// Strings and paths.

TEST_CASE("GBinaryArchive: string round-trips (including empty and embedded NUL)", "[common][archive][binary]") {
    CHECK(roundtrip<std::string>("") == "");
    CHECK(roundtrip<std::string>("hello world") == "hello world");
    std::string withNul("a\0b", 3);
    CHECK(roundtrip<std::string>(withNul) == withNul);
}

TEST_CASE("GBinaryArchive: filesystem::path round-trips", "[common][archive][binary]") {
    std::filesystem::path p{"/opt/geneva/config/GEvolutionaryAlgorithm.json"};
    CHECK(roundtrip<std::filesystem::path>(p) == p);
}

// ---------------------------------------------------------------------------
// Containers.

TEST_CASE("GBinaryArchive: sequence containers round-trip", "[common][archive][binary]") {
    CHECK(roundtrip<std::vector<int>>({}) == std::vector<int>{});
    CHECK(roundtrip<std::vector<int>>({1, 2, 3, 4, 5}) == std::vector<int>{1, 2, 3, 4, 5});
    CHECK(roundtrip<std::vector<double>>({-1.5, 0.0, 2.25}) == std::vector<double>{-1.5, 0.0, 2.25});
    CHECK(roundtrip<std::vector<std::string>>({"a", "bb", "ccc"}) ==
          std::vector<std::string>{"a", "bb", "ccc"});
}

TEST_CASE("GBinaryArchive: nested containers round-trip", "[common][archive][binary]") {
    std::vector<std::vector<int>> nested{{1, 2}, {}, {3, 4, 5}};
    CHECK(roundtrip(nested) == nested);
}

TEST_CASE("GBinaryArchive: set and map round-trip", "[common][archive][binary]") {
    std::set<int> s{5, 3, 9, 1};
    CHECK(roundtrip(s) == s);
    std::map<std::string, int> m{{"one", 1}, {"two", 2}, {"three", 3}};
    CHECK(roundtrip(m) == m);
}

TEST_CASE("GBinaryArchive: pair, tuple and array round-trip", "[common][archive][binary]") {
    std::pair<int, std::string> p{7, "seven"};
    CHECK(roundtrip(p) == p);
    std::tuple<int, double, std::string> t{1, 2.5, "x"};
    CHECK(roundtrip(t) == t);
    std::array<int, 4> a{10, 20, 30, 40};
    CHECK(roundtrip(a) == a);
}

// ---------------------------------------------------------------------------
// std::atomic.

TEST_CASE("GBinaryArchive: std::atomic round-trips its held value", "[common][archive][binary]") {
    GBinaryOArchive oa;
    std::atomic<int> in{12345};
    oa &in;
    GBinaryIArchive ia(oa.str());
    std::atomic<int> out{0};
    ia &out;
    CHECK(ia.exhausted());
    CHECK(out.load() == 12345);
}

// ---------------------------------------------------------------------------
// Serializable classes: public serialize, private-via-access, and base_object.

TEST_CASE("GBinaryArchive: a value class with a public serialize round-trips", "[common][archive][binary]") {
    Point p{42, 3.5, "origin"};
    CHECK(roundtrip(p) == p);
}

TEST_CASE("GBinaryArchive: a class with a private serialize round-trips via access", "[common][archive][binary]") {
    Secret s{99, "classified"};
    CHECK(roundtrip(s) == s);
}

TEST_CASE("GBinaryArchive: base_object serializes the base slice", "[common][archive][binary]") {
    Derived d;
    d.b = 11;
    d.d = 22;
    Derived back = roundtrip(d);
    CHECK(back.b == 11);
    CHECK(back.d == 22);
    CHECK(back == d);
}

TEST_CASE("GBinaryArchive: a class holding containers of classes round-trips", "[common][archive][binary]") {
    std::vector<Point> pts{{1, 1.0, "a"}, {2, 2.0, "b"}, {3, 3.0, "c"}};
    CHECK(roundtrip(pts) == pts);
    std::map<int, Point> byId{{1, {1, 1.0, "a"}}, {2, {2, 2.0, "b"}}};
    CHECK(roundtrip(byId) == byId);
}

// ---------------------------------------------------------------------------
// Failure mode: reading past the end throws rather than reading garbage.

TEST_CASE("GBinaryArchive: truncated stream throws on underflow", "[common][archive][binary]") {
    GBinaryOArchive oa;
    std::uint64_t v = 0xDEADBEEFCAFEBABEULL;
    oa &v;
    std::string truncated = oa.str().substr(0, 3); // fewer than 8 bytes
    GBinaryIArchive ia(truncated);
    std::uint64_t out = 0;
    CHECK_THROWS_AS(ia &out, Gem::Weft::weft_exception);
}
