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

#include <filesystem>
#include <string>

#include "common/GConfigurableI.hpp"
#include "common/GExceptions.hpp"
#include "common/GParserBuilder.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// The configuration facet: an object writes a file from its options and reads one into itself

namespace {

/** @brief An object with one configuration option, held in a member */
class OneOption : public GConfigurableI {
public:
    explicit OneOption(int value)
      : value_(value) {
    }

    [[nodiscard]] int value() const {
        return value_;
    }

protected:
    void addConfigurationOptions_(GParserBuilder &gpb) override {
        GConfigurableI::addConfigurationOptions_(gpb);
        gpb.registerFileParameter<int>("value", value_, value_);
    }

private:
    int value_;
};

/** @brief An object without configuration options */
class NoOption : public GConfigurableI {};

/** @brief The path of a configuration file in the test directory that does not exist yet */
std::filesystem::path fresh_config(std::string const &tag) {
    auto const out =
        std::filesystem::temp_directory_path() / "gem_configurable_tests" / (tag + ".json");
    std::filesystem::create_directories(out.parent_path());
    std::filesystem::remove(out);
    return out;
}

} // namespace

TEST_CASE("GConfigurableI writes a file from its options and reads it back", "[common][config]") {
    auto const path = fresh_config("round_trip");

    OneOption writer(7);
    writer.writeConfigFile(path, "a header");
    REQUIRE(std::filesystem::exists(path));

    OneOption reader(0);
    reader.readConfigFile(path);
    CHECK(reader.value() == 7);
}

TEST_CASE(
    "GConfigurableI refuses to write a file for an object without options",
    "[common][config]"
) {
    auto const path = fresh_config("no_options");

    NoOption object;
    CHECK_THROWS_AS(object.writeConfigFile(path, "a header"), gem_exception);
    CHECK_FALSE(std::filesystem::exists(path));
}
