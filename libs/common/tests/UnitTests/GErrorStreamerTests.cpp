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

#include <iomanip>
#include <string>

#include "common/GErrorStreamer.hpp"

TEST_CASE("g_error_streamer: DO_LOG / NO_LOG constants have the expected values",
          "[common][error-streamer]") {
    static_assert(DO_LOG == true);
    static_assert(NO_LOG == false);
}

TEST_CASE("g_error_streamer: default construction yields an empty content",
          "[common][error-streamer]") {
    g_error_streamer const s;
    CHECK(s.content().empty());

    // Implicit conversion path (without logging) should also be empty.
    std::string const converted = static_cast<std::string>(s);
    CHECK(converted.empty());
}

TEST_CASE("g_error_streamer: templated operator<< accumulates streamable values",
          "[common][error-streamer]") {
    g_error_streamer s;
    s << "alpha=" << 1 << " beta=" << 2.5 << " gamma=" << std::string{"x"};

    const std::string out = s.content();
    CHECK(out.contains("alpha=1"));
    CHECK(out.contains("beta=2.5"));
    CHECK(out.contains("gamma=x"));
}

TEST_CASE("g_error_streamer: stream manipulators are honoured",
          "[common][error-streamer]") {
    g_error_streamer s;
    // '\n' is a plain char forwarded through the generic templated
    // operator<< to the internal ostringstream.
    s << "line1" << '\n' << "line2" << '\n';

    const std::string out = s.content();
    CHECK(out.contains("line1\n"));
    CHECK(out.contains("line2\n"));
}

TEST_CASE("g_error_streamer: ios / ios_base manipulators compile and apply",
          "[common][error-streamer]") {
    // std::hex is std::ios_base&(*)(std::ios_base&); std::boolalpha as well.
    // std::resetiosflags returns an `_Setiosflags` proxy that goes through
    // the templated operator<< — both forms must coexist without ambiguity.
    g_error_streamer s;
    s << std::hex << 255 << " " << std::boolalpha << true;

    const std::string out = s.content();
    CHECK(out.contains("ff"));
    CHECK(out.contains("true"));
}

TEST_CASE("g_error_streamer: explicit ctor stores where-and-when prefix but content() omits it",
          "[common][error-streamer]") {
    // Only the implicit-to-string conversion uses where_and_when_ — and even
    // then only when do_log is true. The cheap, allocation-only content()
    // accessor must not surface it.
    g_error_streamer s(NO_LOG, "Recorded somewhere/sometime\n");
    s << "payload";

    CHECK(s.content() == "payload");

    std::string converted = static_cast<std::string>(s);
    CHECK(converted == "payload");
}

TEST_CASE("g_error_streamer: content() is allocation-only and repeatable",
          "[common][error-streamer]") {
    // content() returns a fresh std::string each call without disturbing the
    // wrapped stringstream — calling it twice must produce identical output.
    g_error_streamer s;
    s << "stable";

    const std::string a = s.content();
    const std::string b = s.content();
    CHECK(a == "stable");
    CHECK(a == b);
}

TEST_CASE("Gem::Common::timeAndPlace(): returns a string containing the call-site location",
          "[common][error-streamer]") {
    // timeAndPlace() formats a `Recorded on … in File X at line N (func) :\n`
    // string from std::source_location. Sanity-check the framing without pinning
    // the exact line number (which would shift with every edit above).
    const std::string tp = Gem::Common::timeAndPlace();
    CHECK(tp.contains("Recorded on "));
    CHECK(tp.contains("in File "));
    CHECK(tp.contains("at line "));
    CHECK(tp.contains("GErrorStreamerTests.cpp"));
}
