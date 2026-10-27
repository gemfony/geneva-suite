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
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// Capture-target: a log sink that stores every message it receives for the
// test to inspect. Used in place of the console/file loggers.

namespace {

class CapturingTarget : public GBaseLogTarget {
public:
    void log(std::string const &msg) const override {
        std::scoped_lock const lk(m_);
        messages.push_back(msg);
    }
    void logWithSource(std::string const &msg, std::string const &src) const override {
        std::scoped_lock const lk(m_);
        messages.push_back("[" + src + "] " + msg);
    }
    mutable std::mutex m_;
    mutable std::vector<std::string> messages;
};

// Build a unique scratch path for file-based tests.
std::filesystem::path scratch(std::string const &tag) {
    auto base = std::filesystem::temp_directory_path() / "geneva_logger_tests";
    std::filesystem::create_directories(base);
    return base / tag;
}

} // namespace

// ---------------------------------------------------------------------------
// GManipulator

TEST_CASE("GManipulator: stores log type with no accompanying info", "[common][logger]") {
    GManipulator const m(logType::STDOUT);
    CHECK(m.getLogType() == logType::STDOUT);
    CHECK_FALSE(m.hasAccompInfo());
    CHECK(m.getAccompInfo().empty());
}

TEST_CASE("GManipulator: stores accompanying info when provided", "[common][logger]") {
    GManipulator const m("call-site-info", logType::EXCEPTION);
    CHECK(m.getLogType() == logType::EXCEPTION);
    CHECK(m.hasAccompInfo());
    CHECK(m.getAccompInfo() == "call-site-info");
}

// ---------------------------------------------------------------------------
// GLogStreamer

TEST_CASE("GLogStreamer: default ctor leaves content/extension/log-file empty",
          "[common][logger]") {
    GLogStreamer const s;
    CHECK(s.content().empty());
    CHECK_FALSE(s.hasExtension());
    CHECK(s.getExtension().empty());
    CHECK_FALSE(s.hasOneTimeLogFile());
}

TEST_CASE("GLogStreamer: extension ctor populates the extension field",
          "[common][logger]") {
    // Explicit std::string disambiguates against the path ctor (the bare
    // string literal would otherwise be a viable conversion to both).
    GLogStreamer const s{std::string{"module-a"}};
    CHECK(s.hasExtension());
    CHECK(s.getExtension() == "module-a");
    CHECK_FALSE(s.hasOneTimeLogFile());
}

TEST_CASE("GLogStreamer: filesystem-path ctor populates the one-time log file",
          "[common][logger]") {
    // Brace-initialise to avoid most-vexing-parse (the temporary path would
    // otherwise be read as a function declaration).
    std::filesystem::path p{"/tmp/foo.log"};
    GLogStreamer const s{p};
    CHECK(s.hasOneTimeLogFile());
    CHECK(s.getOneTimeLogFile() == p);
    CHECK_FALSE(s.hasExtension());
}

TEST_CASE("GLogStreamer: templated operator<< accumulates streamable values",
          "[common][logger]") {
    GLogStreamer s;
    s << "x=" << 42 << " y=" << 3.5;
    CHECK(s.content().contains("x=42"));
    CHECK(s.content().contains("y=3.5"));
}

TEST_CASE("GLogStreamer: std::endl manipulator goes through the dedicated overload",
          "[common][logger]") {
    GLogStreamer s;
    s << "line1" << std::endl << "line2";
    CHECK(s.content().contains("line1\n"));
    CHECK(s.content().contains("line2"));
}

TEST_CASE("GLogStreamer::reset() drops the accumulated content",
          "[common][logger]") {
    GLogStreamer s;
    s << "some-payload";
    REQUIRE_FALSE(s.content().empty());
    s.reset();
    CHECK(s.content().empty());
}

// ---------------------------------------------------------------------------
// GConsoleLogger / GFileLogger

TEST_CASE("GConsoleLogger::log: callable", "[common][logger]") {
    // log() writes to std::clog. We can't easily intercept that here without
    // redirecting global streams, so we only assert it does not throw.
    GConsoleLogger const l;
    CHECK_NOTHROW(l.log("hello\n"));
    CHECK_NOTHROW(l.logWithSource("hello\n", "source-x"));
}

TEST_CASE("GFileLogger::log: appends the message to the configured file",
          "[common][logger]") {
    auto path = scratch("flog_append.log");
    std::filesystem::remove(path);

    GFileLogger const l(path);
    l.log("first-line\n");
    l.log("second-line\n");

    REQUIRE(std::filesystem::exists(path));
    std::ifstream ifs(path);
    std::string const content{std::istreambuf_iterator<char>(ifs), {}};
    CHECK(content.contains("first-line"));
    CHECK(content.contains("second-line"));

    std::filesystem::remove(path);
}

TEST_CASE("GFileLogger::logWithSource: appends the source suffix to the file name",
          "[common][logger]") {
    auto base_path = scratch("flog_source.log");
    auto src_path  = std::filesystem::path(base_path.string() + "_modA");
    std::filesystem::remove(src_path);

    GFileLogger const l(base_path);
    l.logWithSource("payload-a\n", "modA");
    REQUIRE(std::filesystem::exists(src_path));

    std::ifstream ifs(src_path);
    std::string const content{std::istreambuf_iterator<char>(ifs), {}};
    CHECK(content.contains("payload-a"));

    std::filesystem::remove(src_path);
}

// ---------------------------------------------------------------------------
// GLogger: target registration / dispatch

TEST_CASE("GLogger: addLogTarget+log routes to every registered target",
          "[common][logger]") {
    // We instantiate a private GLogger just so we
    // get its public API without entangling with the singleton.
    GLogger g;
    auto t1 = std::make_shared<CapturingTarget>();
    auto t2 = std::make_shared<CapturingTarget>();
    g.addLogTarget(t1);
    g.addLogTarget(t2);
    CHECK(g.hasLogTargets());

    g.log("hello\n");
    REQUIRE(t1->messages.size() == 1);
    REQUIRE(t2->messages.size() == 1);
    CHECK(t1->messages[0] == "hello\n");
    CHECK(t2->messages[0] == "hello\n");
}

TEST_CASE("GLogger::resetLogTargets clears the registry",
          "[common][logger]") {
    GLogger g;
    g.addLogTarget(std::make_shared<CapturingTarget>());
    REQUIRE(g.hasLogTargets());
    g.resetLogTargets();
    CHECK_FALSE(g.hasLogTargets());
}

TEST_CASE("GLogger::addLogTarget(empty) throws",
          "[common][logger]") {
    GLogger g;
    CHECK_THROWS_AS(g.addLogTarget(std::shared_ptr<GBaseLogTarget>{}),
                    geneva_exception);
}

TEST_CASE("GLogger::setDefaultLogTarget(empty) throws",
          "[common][logger]") {
    GLogger g;
    CHECK_THROWS_AS(g.setDefaultLogTarget(std::shared_ptr<GBaseLogTarget>{}),
                    geneva_exception);
}

TEST_CASE("GLogger: with no custom targets, log() falls back to the default target",
          "[common][logger]") {
    GLogger g;
    auto def = std::make_shared<CapturingTarget>();
    g.setDefaultLogTarget(def);
    CHECK_FALSE(g.hasLogTargets());

    g.log("via-default\n");
    REQUIRE(def->messages.size() == 1);
    CHECK(def->messages[0] == "via-default\n");
}

TEST_CASE("GLogger::logWithSource: forwards to logWithSource on each target",
          "[common][logger]") {
    GLogger g;
    auto t1 = std::make_shared<CapturingTarget>();
    g.addLogTarget(t1);

    g.logWithSource("payload\n", "modX");
    REQUIRE(t1->messages.size() == 1);
    CHECK(t1->messages[0] == "[modX] payload\n");
}

TEST_CASE("GLogger::throwException: throws geneva_exception with the supplied text",
          "[common][logger]") {
    GLogger g;
    try {
        g.throwException("forwarded-error");
        FAIL("expected throw");
    } catch(geneva_exception const &e) {
        CHECK(std::string(e.what()).contains("forwarded-error"));
    }
}

// ---------------------------------------------------------------------------
// LOCATIONSTRING / GEXCEPTION / GWARNING / GLOGGING / GFILE / GSTDOUT / GSTDERR
// macros — only check that they compile and yield the expected log type.

TEST_CASE("GLogger macros: each produces a GManipulator of the expected type",
          "[common][logger][macros]") {
    GManipulator const gex   = GEXCEPTION;     CHECK(gex.getLogType()   == logType::EXCEPTION);
    GManipulator const gterm = GTERMINATION;   CHECK(gterm.getLogType() == logType::TERMINATION);
    GManipulator const gwarn = GWARNING;       CHECK(gwarn.getLogType() == logType::WARNING);
    GManipulator const glog  = GLOGGING;       CHECK(glog.getLogType()  == logType::LOGGING);
    GManipulator const gfile = GFILE;          CHECK(gfile.getLogType() == logType::FILE);
    GManipulator const gso   = GSTDOUT;        CHECK(gso.getLogType()   == logType::STDOUT);
    GManipulator const gse   = GSTDERR;        CHECK(gse.getLogType()   == logType::STDERR);

    // Macros that include a location string should populate accomp info.
    CHECK(gex.hasAccompInfo());
    CHECK(gwarn.hasAccompInfo());

    // Macros that do not should leave it empty.
    CHECK_FALSE(glog.hasAccompInfo());
    CHECK_FALSE(gfile.hasAccompInfo());
}

// ---------------------------------------------------------------------------
// GLogStreamer::operator<<(GManipulator&): switch arms.
//
// These go through the real `glogger` singleton (the Geneva exception path
// writes a `GENEVA-EXCEPTION.log` file as a side-effect; the warning/logging
// paths go to the registered targets — by default the console). We don't
// assert on the file content; we just hit the code so it's instrumented.
// TERMINATION is deliberately NOT exercised — it would call std::terminate().

TEST_CASE("GLogStreamer << GEXCEPTION throws a geneva_exception",
          "[common][logger][manipulator]") {
    // Ensure the side-effect log file does not survive between test runs.
    std::error_code ec;
    std::filesystem::remove("GENEVA-EXCEPTION.log", ec);

    bool caught = false;
    try {
        glogger << "test-payload " << 123 << '\n' << GEXCEPTION;
        FAIL("expected geneva_exception");
    } catch(geneva_exception const &e) {
        caught = true;
        // Message must include the streamed payload.
        CHECK(std::string(e.what()).contains("test-payload"));
    }
    CHECK(caught);

    std::filesystem::remove("GENEVA-EXCEPTION.log", ec);
}

TEST_CASE("GLogStreamer << GWARNING does not throw; emits to the logger sink",
          "[common][logger][manipulator]") {
    CHECK_NOTHROW(glogger << "warning text" << '\n' << GWARNING);
}

TEST_CASE("GLogStreamer << GLOGGING does not throw; emits to the logger sink",
          "[common][logger][manipulator]") {
    CHECK_NOTHROW(glogger << "logging text" << '\n' << GLOGGING);
}

TEST_CASE("GLogStreamer << GSTDOUT does not throw; writes via toStdOut",
          "[common][logger][manipulator]") {
    CHECK_NOTHROW(glogger << "to stdout " << 42 << '\n' << GSTDOUT);
}

TEST_CASE("GLogStreamer << GSTDERR does not throw; writes via toStdErr",
          "[common][logger][manipulator]") {
    CHECK_NOTHROW(glogger << "to stderr " << 42 << '\n' << GSTDERR);
}

TEST_CASE("GLogStreamer << GFILE with a path writes to that file",
          "[common][logger][manipulator]") {
    auto path = scratch("one_time.log");
    std::filesystem::remove(path);

    CHECK_NOTHROW(glogger(path) << "one-time-payload" << '\n' << GFILE);
    REQUIRE(std::filesystem::exists(path));

    std::ifstream ifs(path);
    std::string const content{std::istreambuf_iterator<char>(ifs), {}};
    CHECK(content.contains("one-time-payload"));

    std::filesystem::remove(path);
}

TEST_CASE("GLogStreamer << GFILE without a path throws",
          "[common][logger][manipulator]") {
    // The path-less form must complain: there is no destination to write to.
    CHECK_THROWS_AS(glogger << "should-fail" << '\n' << GFILE, geneva_exception);
}

TEST_CASE("GLogStreamer << GWARNING with extension forwards through logWithSource",
          "[common][logger][manipulator]") {
    // The string-form of glogger() captures an extension; the WARNING arm
    // then routes through logWithSource on every registered target.
    CHECK_NOTHROW(glogger(std::string{"unit-test-module"})
                  << "warning with source" << '\n' << GWARNING);
}

TEST_CASE("GLogStreamer << GLOGGING with extension forwards through logWithSource",
          "[common][logger][manipulator]") {
    CHECK_NOTHROW(glogger(std::string{"unit-test-module"})
                  << "logging with source" << '\n' << GLOGGING);
}

// ---------------------------------------------------------------------------
// terminateApplication() through the GLogger template surface: we do NOT
// call it on the real singleton (it would terminate the process). Instead we
// instantiate a private GLogger and just check that the lock /
// stderr write portion of the function compiles & accepts input.
//
// (The post-stderr std::terminate() call is intentionally unreachable in
// tests; this is documented in COVERAGE.md as an excluded death path.)

TEST_CASE("GLogger::toStdOut / toStdErr emit the supplied message",
          "[common][logger][manipulator]") {
    GLogger g;
    CHECK_NOTHROW(g.toStdOut("via toStdOut\n"));
    CHECK_NOTHROW(g.toStdErr("via toStdErr\n"));
}

// ---------------------------------------------------------------------------
// GFileLogger::logWithSource — first-call header vs. subsequent writes

TEST_CASE("GFileLogger::logWithSource: header line appears on first write only",
          "[common][logger]") {
    auto base_path = scratch("flog_first_flag.log");
    auto src_path  = std::filesystem::path(base_path.string() + "_chk");
    std::filesystem::remove(src_path);

    GFileLogger const l(base_path);
    l.logWithSource("msg1\n", "chk");
    l.logWithSource("msg2\n", "chk");

    REQUIRE(std::filesystem::exists(src_path));
    std::ifstream ifs(src_path);
    std::string const content{std::istreambuf_iterator<char>(ifs), {}};

    // The header "Logging data from source" must appear exactly once.
    auto first_pos = content.find("Logging data from source");
    REQUIRE(first_pos != std::string::npos);
    CHECK(content.find("Logging data from source", first_pos + 1) == std::string::npos);

    // Both payloads must be present.
    CHECK(content.contains("msg1"));
    CHECK(content.contains("msg2"));

    std::filesystem::remove(src_path);
}

// ---------------------------------------------------------------------------
// GLogger::setDefaultLogTarget: success path — verify the default is actually used

TEST_CASE("GLogger::setDefaultLogTarget routes log() through the new default",
          "[common][logger]") {
    GLogger g;
    auto cap = std::make_shared<CapturingTarget>();
    g.setDefaultLogTarget(cap);

    g.log("routed-via-default\n");
    REQUIRE(cap->messages.size() == 1);
    CHECK(cap->messages[0] == "routed-via-default\n");
}

TEST_CASE("GLogger::setDefaultLogTarget routes logWithSource() through the new default",
          "[common][logger]") {
    GLogger g;
    auto cap = std::make_shared<CapturingTarget>();
    g.setDefaultLogTarget(cap);

    g.logWithSource("payload\n", "ext");
    REQUIRE(cap->messages.size() == 1);
    CHECK(cap->messages[0] == "[ext] payload\n");
}
