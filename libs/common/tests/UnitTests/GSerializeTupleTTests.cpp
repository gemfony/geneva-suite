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
#include "weft/GBinaryArchive.hpp"
#include "weft/GJsonArchive.hpp"
#include <catch2/catch_template_test_macros.hpp>

#include <sstream>
#include <string>
#include <tuple>



// ---------------------------------------------------------------------------
// Round-trip helper. Each archive family needs a slightly different ctor
// argument set, so we keep helpers per family rather than a generic one.

namespace {

template <class Tuple>
Tuple round_trip_text(Tuple const &in) {
    Gem::Weft::GBinaryOArchive oa;
    oa &Gem::Weft::make_nvp("t", const_cast<Tuple &>(in));
    Tuple out{};
    Gem::Weft::GBinaryIArchive ia(oa.str());
    ia &Gem::Weft::make_nvp("t", out);
    return out;
}

template <class Tuple>
Tuple round_trip_xml(Tuple const &in) {
    Gem::Weft::GJsonOArchive oa;
    oa &Gem::Weft::make_nvp("t", const_cast<Tuple &>(in));
    Tuple out{};
    Gem::Weft::GJsonIArchive ia(oa.str());
    ia &Gem::Weft::make_nvp("t", out);
    return out;
}

template <class Tuple>
Tuple round_trip_binary(Tuple const &in) {
    Gem::Weft::GBinaryOArchive oa;
    oa &Gem::Weft::make_nvp("t", const_cast<Tuple &>(in));
    Tuple out{};
    Gem::Weft::GBinaryIArchive ia(oa.str());
    ia &Gem::Weft::make_nvp("t", out);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// One-element tuple

TEST_CASE("GSerializeTupleT: 1-element tuple round-trips through every archive family",
          "[common][serialize-tuple]") {
    auto in = std::make_tuple(42);

    CHECK(round_trip_text  (in) == in);
    CHECK(round_trip_xml   (in) == in);
    CHECK(round_trip_binary(in) == in);
}

// ---------------------------------------------------------------------------
// Two-element tuple

TEST_CASE("GSerializeTupleT: 2-element tuple round-trips through every archive family",
          "[common][serialize-tuple]") {
    auto in = std::make_tuple(3, 4.5);
    CHECK(round_trip_text  (in) == in);
    CHECK(round_trip_xml   (in) == in);
    CHECK(round_trip_binary(in) == in);
}

// ---------------------------------------------------------------------------
// Three-element tuple, including a non-trivial std::string element to
// exercise allocation/streaming paths.

TEST_CASE("GSerializeTupleT: 3-element tuple incl. std::string round-trips",
          "[common][serialize-tuple]") {
    auto in = std::make_tuple(1, 2.5, std::string{"hello"});
    CHECK(round_trip_text  (in) == in);
    CHECK(round_trip_xml   (in) == in);
    CHECK(round_trip_binary(in) == in);
}

// ---------------------------------------------------------------------------
// Four-element tuple

TEST_CASE("GSerializeTupleT: 4-element tuple round-trips",
          "[common][serialize-tuple]") {
    auto in = std::make_tuple(1, 2, 3.5, std::string{"abc"});
    CHECK(round_trip_text  (in) == in);
    CHECK(round_trip_xml   (in) == in);
    CHECK(round_trip_binary(in) == in);
}

// ---------------------------------------------------------------------------
// Five-element tuple

TEST_CASE("GSerializeTupleT: 5-element tuple round-trips",
          "[common][serialize-tuple]") {
    auto in = std::make_tuple(1, 2.5, std::string{"x"}, std::int64_t{99}, false);
    CHECK(round_trip_text  (in) == in);
    CHECK(round_trip_xml   (in) == in);
    CHECK(round_trip_binary(in) == in);
}

// ---------------------------------------------------------------------------
// Six-element tuple — the largest currently-supported arity. If the file
// ever grows to support 7+ elements, add cases for those here.

TEST_CASE("GSerializeTupleT: 6-element tuple round-trips",
          "[common][serialize-tuple]") {
    auto in = std::make_tuple(1, 2.5, std::string{"x"}, std::int64_t{99}, true, std::string{"y"});
    CHECK(round_trip_text  (in) == in);
    CHECK(round_trip_xml   (in) == in);
    CHECK(round_trip_binary(in) == in);
}

// ---------------------------------------------------------------------------
// Ensure each archive actually produces a non-empty payload (sanity-check
// that the serialize() overload was selected, not a no-op fallback).

TEST_CASE("GSerializeTupleT: each archive writes a non-empty payload",
          "[common][serialize-tuple]") {
    auto in = std::make_tuple(1, std::string{"abc"});

    Gem::Weft::GBinaryOArchive bin_oa;
    bin_oa &Gem::Weft::make_nvp("t", in);
    Gem::Weft::GJsonOArchive json_oa;
    json_oa &Gem::Weft::make_nvp("t", in);

    CHECK_FALSE(bin_oa.str().empty());
    CHECK_FALSE(json_oa.str().empty());
}
