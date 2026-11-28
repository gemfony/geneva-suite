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
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/GArchiveNamed.hpp" // archive_named (archive-generic member emitter)
#include "common/GCommonEnums.hpp"
#include "common/GExceptions.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// Tiny GArchive-serializable payload type used to exercise the
// sharedPtrToString / sharedPtrFromString round-trip.

namespace {

class TestPayload {
public:
    TestPayload() = default;
    TestPayload(int i, double d, std::string s)
      : i_(i)
      , d_(d)
      , s_(std::move(s)) {
    }

    [[nodiscard]] int i() const {
        return i_;
    }
    [[nodiscard]] double d() const {
        return d_;
    }
    [[nodiscard]] std::string s() const {
        return s_;
    }

    bool operator==(TestPayload const &o) const = default;

private:
    friend struct Gem::Weft::access;
    template <class archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] unsigned int version) {
        Gem::Common::archive_named(ar, "i", i_);
        Gem::Common::archive_named(ar, "d", d_);
        Gem::Common::archive_named(ar, "s", s_);
    }

    int i_{0};
    double d_{0.0};
    std::string s_;
};

} // namespace

// ---------------------------------------------------------------------------
// sharedPtrToString / sharedPtrFromString round-trips through each archive
// family that the helpers support.

TEST_CASE(
    "GSerializationHelperFunctionsT: shared_ptr round-trip via GEM_BINARY mode",
    "[common][serialization-helpers]"
) {
    auto in = std::make_shared<TestPayload>(17, 3.5, "alpha");

    const std::string s = sharedPtrToString(in, serializationMode::GEM_BINARY);
    REQUIRE_FALSE(s.empty());

    auto out = sharedPtrFromString<TestPayload>(s, serializationMode::GEM_BINARY);
    REQUIRE(out);
    CHECK(*out == *in);
}

TEST_CASE(
    "GSerializationHelperFunctionsT: shared_ptr round-trip via GEM_JSON mode",
    "[common][serialization-helpers]"
) {
    auto in = std::make_shared<TestPayload>(-3, 1.25, "beta");

    const std::string s = sharedPtrToString(in, serializationMode::GEM_JSON);
    REQUIRE_FALSE(s.empty());

    auto out = sharedPtrFromString<TestPayload>(s, serializationMode::GEM_JSON);
    REQUIRE(out);
    CHECK(*out == *in);
}

TEST_CASE(
    "GSerializationHelperFunctionsT: sharedPtrFromString throws on garbage input",
    "[common][serialization-helpers]"
) {
    // A decode error reaches the caller, as with sharedPtrToString(); there is no empty pointer.
    CHECK_THROWS_AS(
        sharedPtrFromString<TestPayload>("not-a-valid-archive-blob", serializationMode::GEM_BINARY),
        Gem::Weft::weft_exception
    );
    CHECK_THROWS_AS(
        sharedPtrFromString<TestPayload>("not-a-valid-archive-blob", serializationMode::GEM_JSON),
        std::exception
    );
}

TEST_CASE(
    "GSerializationHelperFunctionsT: a serialization mode that is not one of the two is refused",
    "[common][serialization-helpers]"
) {
    // sharedPtrFromString() returned an empty pointer for it; both functions now throw alike
    auto const unknown = static_cast<serializationMode>(99);
    CHECK_THROWS_AS(
        sharedPtrToString(std::make_shared<TestPayload>(1, 2.0, "x"), unknown),
        gem_exception
    );
    CHECK_THROWS_AS(sharedPtrFromString<TestPayload>("", unknown), gem_exception);
}

TEST_CASE(
    "GSerializationHelperFunctionsT: an empty shared_ptr round-trips as empty",
    "[common][serialization-helpers]"
) {
    for(auto const mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        std::string const s = sharedPtrToString(std::shared_ptr<TestPayload>{}, mode);
        CHECK_FALSE(sharedPtrFromString<TestPayload>(s, mode));
    }
}

// ---------------------------------------------------------------------------
// archiveToString / archiveFromString and the file pair: a named value of any
// archivable type, in the codec the mode names.

TEST_CASE(
    "GSerializationHelperFunctionsT: a named value round-trips through both modes",
    "[common][serialization-helpers]"
) {
    TestPayload const in(5, -0.75, "gamma");
    std::vector<int> const numbers{1, 2, 3};
    for(auto const mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        INFO("mode " << static_cast<int>(mode));
        TestPayload out;
        archiveFromString("payload", out, archiveToString("payload", in, mode), mode);
        CHECK(out == in);

        std::vector<int> read_numbers;
        archiveFromString("numbers", read_numbers, archiveToString("numbers", numbers, mode), mode);
        CHECK(read_numbers == numbers);
    }

    // The string of sharedPtrToString() is the archive of the pointer under the object name
    auto const ptr = std::make_shared<TestPayload>(in);
    CHECK(
        sharedPtrToString(ptr, serializationMode::GEM_JSON) ==
        archiveToString(detail::OBJECT_STRING_TAG, ptr, serializationMode::GEM_JSON)
    );
}

TEST_CASE(
    "GSerializationHelperFunctionsT: a named value round-trips through a file",
    "[common][serialization-helpers]"
) {
    auto const path =
        std::filesystem::temp_directory_path() / "GSerializationHelperFunctionsT-archive.txt";
    TestPayload const in(11, 2.5, "delta");
    for(auto const mode : {serializationMode::GEM_BINARY, serializationMode::GEM_JSON}) {
        INFO("mode " << static_cast<int>(mode));
        archiveToFile(path, "payload", in, mode);
        TestPayload out;
        archiveFromFile(path, "payload", out, mode);
        CHECK(out == in);
    }

    // A refused write leaves the file as it was
    auto const unknown = static_cast<serializationMode>(99);
    CHECK_THROWS_AS(archiveToFile(path, "payload", TestPayload{}, unknown), gem_exception);
    TestPayload out;
    archiveFromFile(path, "payload", out, serializationMode::GEM_JSON);
    CHECK(out == in);

    std::filesystem::remove(path);
    CHECK_THROWS_AS(
        archiveFromFile(path, "payload", out, serializationMode::GEM_JSON),
        gem_exception
    );
}

TEST_CASE(
    "GSerializationHelperFunctionsT: a named value refuses a mode that is not one of the two",
    "[common][serialization-helpers]"
) {
    auto const unknown = static_cast<serializationMode>(99);
    TestPayload value(1, 2.0, "x");
    CHECK_THROWS_AS(archiveToString("payload", value, unknown), gem_exception);
    std::string const text = archiveToString("payload", value, serializationMode::GEM_BINARY);
    CHECK_THROWS_AS(archiveFromString("payload", value, text, unknown), gem_exception);
}
