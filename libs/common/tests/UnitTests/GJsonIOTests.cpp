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
#include <fstream>
#include <string>

#include <boost/json.hpp>

#include "common/GExceptions.hpp"
#include "common/GJsonIO.hpp"

using namespace Gem::Common;
namespace json = boost::json;

namespace {
std::filesystem::path scratch(std::string const &tag) {
    auto base = std::filesystem::temp_directory_path() / "geneva_json_io_tests";
    std::filesystem::create_directories(base);
    return base / tag;
}
} // namespace

// ---------------------------------------------------------------------------
// prettyPrintJson layout: one member / element per line, four-space indent.

TEST_CASE("prettyPrintJson: objects and arrays print one entry per line",
          "[common][json-io][pretty]") {
    json::object obj;
    obj["comment"] = json::array{"line one", "line two", "line three"};
    obj["value"] = "42";

    const std::string out = prettyPrintJson(json::value(std::move(obj)));

    // Each comment element must be on its own line (hand-editing readability requirement).
    CHECK(out.find("\"line one\"") != std::string::npos);
    CHECK(out.find("\"line two\"") != std::string::npos);
    // The three comment strings appear on three distinct lines, in order.
    const auto p1 = out.find("line one");
    const auto p2 = out.find("line two");
    const auto p3 = out.find("line three");
    REQUIRE(p1 != std::string::npos);
    REQUIRE(p2 != std::string::npos);
    REQUIRE(p3 != std::string::npos);
    CHECK(p1 < p2);
    CHECK(p2 < p3);
    // A newline separates consecutive comment elements.
    CHECK(out.find('\n', p1) < p2);
    // Four-space indentation is used for top-level members.
    CHECK(out.find("\n    \"comment\"") != std::string::npos);
}

TEST_CASE("prettyPrintJson: empty containers are compact",
          "[common][json-io][pretty]") {
    CHECK(prettyPrintJson(json::value(json::object{})) == "{}");
    CHECK(prettyPrintJson(json::value(json::array{})) == "[]");
}

// ---------------------------------------------------------------------------
// Round-trip: pretty-print then parse yields an equal value.

TEST_CASE("parse/prettyPrint round-trip preserves structure and values",
          "[common][json-io][roundtrip]") {
    json::value original = json::parse(R"({
        "name": "geneva",
        "nested": { "a": "1", "b": "2" },
        "vec": ["10", "20", "30"],
        "typed_numbers": [1, 2, 3],
        "flag": true,
        "empty_obj": {},
        "empty_arr": []
    })");

    const std::string pretty = prettyPrintJson(original);
    json::value reparsed = parseJsonString(pretty, "round-trip");

    CHECK(reparsed == original);
}

// ---------------------------------------------------------------------------
// Inv 15 regression guard: the reason we left Boost.PropertyTree. A multi-element
// array must round-trip as N elements. The old ptree config format serialized
// vectors as duplicate "item" keys inside an object; a strict JSON parser collapses
// duplicate object keys to one (silent data loss). Using a JSON array instead, all
// elements survive.

TEST_CASE("multi-element array survives round-trip (no dup-key collapse)",
          "[common][json-io][roundtrip][regression]") {
    json::value v = json::array{"1", "1", "1"};
    json::value back = parseJsonString(prettyPrintJson(v), "vector");
    REQUIRE(back.is_array());
    CHECK(back.get_array().size() == 3);
    CHECK(back == v);
}

// ---------------------------------------------------------------------------
// Parser leniency: comments and trailing commas are tolerated.

TEST_CASE("parseJsonString: tolerates comments and trailing commas",
          "[common][json-io][lenient]") {
    json::value v = parseJsonString(R"({
        // a line comment
        "a": "1",
        "b": "2",   /* a block comment */
    })", "lenient");
    REQUIRE(v.is_object());
    CHECK(v.get_object().at("a").as_string() == "1");
    CHECK(v.get_object().at("b").as_string() == "2");
}

TEST_CASE("parseJsonString: throws a geneva_exception on malformed input",
          "[common][json-io][error]") {
    CHECK_THROWS_AS(parseJsonString("{ not valid ", "bad"), geneva_exception);
}

// ---------------------------------------------------------------------------
// File I/O: writeJsonFile then parseJsonFile round-trips and appends a newline.

TEST_CASE("writeJsonFile / parseJsonFile round-trip",
          "[common][json-io][file]") {
    const auto path = scratch("roundtrip.json");
    json::value v = json::object{{"k", "v"}, {"list", json::array{"a", "b"}}};

    writeJsonFile(path, v);
    CHECK(parseJsonFile(path) == v);

    // The file ends in a single trailing newline.
    std::ifstream ifs(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
    REQUIRE_FALSE(content.empty());
    CHECK(content.back() == '\n');

    std::filesystem::remove(path);
}

TEST_CASE("parseJsonFile: throws a geneva_exception for a missing file",
          "[common][json-io][error]") {
    CHECK_THROWS_AS(parseJsonFile(scratch("does_not_exist.json")), geneva_exception);
}

// ---------------------------------------------------------------------------
// applyConfigOverrides: the "configs derived from code + per-example overrides" merge.
// A config is a JSON object of parameter nodes ({comment, default, value}) and groups
// (objects of nested nodes). An override carries the bare replacement value keyed by name.

namespace {
// A representative generated configuration: a scalar parameter, an array-valued parameter,
// a parameter left at its default, and a nested group (mirroring touched_termination).
json::value sampleConfig() {
    return json::parse(R"({
        "consumer": {
            "comment": ["The consumer to use"],
            "default": "stc",
            "value": "stc"
        },
        "algo_config_files": {
            "comment": ["One per entry"],
            "default": ["a.json", "b.json"],
            "value": ["a.json", "b.json"]
        },
        "max_iteration": {
            "comment": ["The maximum allowed number of iterations"],
            "default": 1000,
            "value": 1000
        },
        "touched_termination": {
            "termination_file": {
                "comment": ["A file to touch"],
                "default": "empty",
                "value": "empty"
            },
            "touched_termination_active": {
                "comment": ["Activates the touched termination"],
                "default": false,
                "value": false
            }
        }
    })");
}
} // namespace

TEST_CASE("applyConfigOverrides: an override replaces only the value, keeping default and comment",
          "[common][json-io][override]") {
    json::value cfg = sampleConfig();
    applyConfigOverrides(cfg, json::parse(R"({ "consumer": "gpu" })"));

    auto const &node = cfg.get_object().at("consumer").get_object();
    CHECK(node.at("value").as_string() == "gpu");      // the override wins
    CHECK(node.at("default").as_string() == "stc");    // the code-owned default is untouched
    CHECK(node.at("comment") == json::array{"The consumer to use"});
}

TEST_CASE("applyConfigOverrides: a value's native type is preserved through the override",
          "[common][json-io][override]") {
    json::value cfg = sampleConfig();
    applyConfigOverrides(cfg, json::parse(R"({ "max_iteration": 10 })"));

    auto const &value = cfg.get_object().at("max_iteration").get_object().at("value");
    REQUIRE(value.is_int64());                          // stays a number, not a string
    CHECK(value.as_int64() == 10);
}

TEST_CASE("applyConfigOverrides: an array value is replaced wholesale",
          "[common][json-io][override]") {
    json::value cfg = sampleConfig();
    applyConfigOverrides(cfg, json::parse(R"({ "algo_config_files": ["x.json"] })"));

    auto const &value = cfg.get_object().at("algo_config_files").get_object().at("value");
    REQUIRE(value.is_array());
    CHECK(value.as_array().size() == 1);
    CHECK(value.as_array().at(0).as_string() == "x.json");
}

TEST_CASE("applyConfigOverrides: recurses into a group node",
          "[common][json-io][override]") {
    json::value cfg = sampleConfig();
    applyConfigOverrides(cfg, json::parse(R"({
        "touched_termination": { "touched_termination_active": true }
    })"));

    auto const &group = cfg.get_object().at("touched_termination").get_object();
    CHECK(group.at("touched_termination_active").get_object().at("value").as_bool() == true);
    // The sibling parameter in the group is left untouched.
    CHECK(group.at("termination_file").get_object().at("value").as_string() == "empty");
}

TEST_CASE("applyConfigOverrides: parameters not named in the override are left untouched",
          "[common][json-io][override]") {
    json::value cfg = sampleConfig();
    json::value const before = cfg;
    applyConfigOverrides(cfg, json::parse(R"({ "consumer": "gpu" })"));

    CHECK(cfg.get_object().at("max_iteration") == before.get_object().at("max_iteration"));
    CHECK(cfg.get_object().at("algo_config_files") == before.get_object().at("algo_config_files"));
}

TEST_CASE("applyConfigOverrides: an unknown parameter name throws (stale/mistyped override)",
          "[common][json-io][override][error]") {
    json::value cfg = sampleConfig();
    CHECK_THROWS_AS(applyConfigOverrides(cfg, json::parse(R"({ "no_such_param": 1 })")),
                    geneva_exception);
}

TEST_CASE("applyConfigOverrides: a shape mismatch against a group throws",
          "[common][json-io][override][error]") {
    json::value cfg = sampleConfig();
    // touched_termination is a group; a scalar override cannot target it.
    CHECK_THROWS_AS(applyConfigOverrides(cfg, json::parse(R"({ "touched_termination": 3 })")),
                    geneva_exception);
}

TEST_CASE("applyConfigOverrides: a non-object override document throws",
          "[common][json-io][override][error]") {
    json::value cfg = sampleConfig();
    CHECK_THROWS_AS(applyConfigOverrides(cfg, json::value(42)), geneva_exception);
}
