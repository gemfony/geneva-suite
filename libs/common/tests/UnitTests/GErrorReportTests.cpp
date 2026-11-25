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

// Where an error is reported: once, at the place where it is finally handled -- not where it is
// thrown, and not again on each rethrow. An error that a caller catches and handles prints nothing;
// one that ends the program is written once to the standard error stream and once to
// GEM-EXCEPTION.log (the fatal report); one that ends a task is reported once by GERROR, by the
// handler that ends it (here the thread pool's bulk fork/join).

#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GFatalReport.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GThreadPool.hpp"

using namespace Gem::Common;

namespace {

/** @brief A log target that keeps what it is given */
class CapturingTarget : public GBaseLogTarget {
public:
    using GBaseLogTarget::log;
    using GBaseLogTarget::logWithSource;
    void log(std::string const &message) const override {
        std::scoped_lock const lk(m_);
        messages.push_back(message);
    }
    void logWithSource(std::string const &message, std::string const &extension) const override {
        std::scoped_lock const lk(m_);
        messages.push_back("[" + extension + "] " + message);
    }
    mutable std::mutex m_;
    mutable std::vector<std::string> messages;
};

/** @brief Runs the test in a working directory of its own, where GEM-EXCEPTION.log is written */
class ScratchDirectory {
public:
    explicit ScratchDirectory(std::string const &tag)
      : previous_(std::filesystem::current_path())
      , path_(
            std::filesystem::temp_directory_path() /
            ("gem_error_report_" + tag + "_" + std::to_string(::getpid()))
        ) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
        std::filesystem::current_path(path_);
    }
    ScratchDirectory(ScratchDirectory const &) = delete;
    ScratchDirectory(ScratchDirectory &&) = delete;
    ScratchDirectory &operator=(ScratchDirectory const &) = delete;
    ScratchDirectory &operator=(ScratchDirectory &&) = delete;
    ~ScratchDirectory() {
        std::error_code ec;
        std::filesystem::current_path(previous_, ec);
        std::filesystem::remove_all(path_, ec);
    }

private:
    std::filesystem::path previous_;
    std::filesystem::path path_;
};

/** @brief Captures what glogger hands to its targets while the object lives */
class CapturedLog {
public:
    CapturedLog() {
        glogger.addLogTarget(target_);
    }
    CapturedLog(CapturedLog const &) = delete;
    CapturedLog(CapturedLog &&) = delete;
    CapturedLog &operator=(CapturedLog const &) = delete;
    CapturedLog &operator=(CapturedLog &&) = delete;
    ~CapturedLog() {
        glogger.resetLogTargets();
    }
    [[nodiscard]] std::vector<std::string> messages() const {
        std::scoped_lock const lk(target_->m_);
        return target_->messages;
    }

private:
    std::shared_ptr<CapturingTarget> target_ = std::make_shared<CapturingTarget>();
};

/** @brief The content of a file; empty if there is none */
std::string contentOf(std::filesystem::path const &file) {
    std::ifstream in(file);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/** @brief How often @p what occurs in @p text */
std::size_t occurrences(std::string_view text, std::string_view what) {
    std::size_t n = 0;
    for(std::size_t pos = text.find(what); pos != std::string_view::npos;
        pos = text.find(what, pos + what.size())) {
        ++n;
    }
    return n;
}

constexpr std::string_view EXCEPTION_LOG = "GEM-EXCEPTION.log";

/** @brief Throws an error whose message is to name this function */
[[noreturn]] void throwingFunction() {
    throw gem_exception(g_error_streamer() << "the reason" << '\n');
}

} // namespace

TEST_CASE(
    "An error thrown and caught by a caller produces no error output",
    "[common][error-report]"
) {
    ScratchDirectory const directory("caught");
    CapturedLog const log;

    bool handled = false;
    try {
        throw gem_exception(g_error_streamer() << "a caught and handled error" << '\n');
    }
    catch(gem_exception const &e) {
        handled = std::string(e.what()).contains("a caught and handled error");
    }

    CHECK(handled);
    CHECK_FALSE(std::filesystem::exists(EXCEPTION_LOG));
    CHECK(log.messages().empty());
}

TEST_CASE(
    "The message of an error names the file and line and function of the throw",
    "[common][error-report]"
) {
    std::string what;
    try {
        throwingFunction();
    }
    catch(gem_exception const &e) {
        what = e.what();
    }
    CHECK(what.contains("GErrorReportTests.cpp at line "));
    CHECK(what.contains("throwingFunction"));
    CHECK(what.contains("Recorded on "));
    CHECK(what.ends_with("the reason\n"));
}

TEST_CASE(
    "An error that ends the program is written once to stderr and once to GEM-EXCEPTION.log",
    "[common][error-report]"
) {
    ScratchDirectory const directory("fatal");

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if(child == 0) {
        // The child: its stderr to a file, the fatal report as its terminate handler, and an
        // error that is caught, wrapped and rethrown before nobody handles it. An exception that
        // leaves the function of a thread ends the program (std::terminate()).
        if(std::freopen("stderr.txt", "w", stderr) == nullptr) {
            std::_Exit(98);
        }
        installFatalReport();
        std::thread([] {
            try {
                throw gem_exception(g_error_streamer() << "the inner cause" << '\n');
            }
            catch(gem_exception const &e) {
                throw gem_exception(g_error_streamer() << "the outer context" << '\n' << e.what());
            }
        }).join();
        std::_Exit(99); // not reached
    }

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == EXIT_FAILURE);

    const std::string err = contentOf("stderr.txt");
    CHECK(occurrences(err, "the inner cause") == 1);
    CHECK(occurrences(err, "the outer context") == 1);

    const std::string recorded = contentOf(std::filesystem::path(EXCEPTION_LOG));
    CHECK(occurrences(recorded, "the inner cause") == 1);
    CHECK(occurrences(recorded, "the outer context") == 1);
}

TEST_CASE(
    "GERROR reports an error once to the log and once to GEM-EXCEPTION.log without a throw",
    "[common][error-report]"
) {
    ScratchDirectory const directory("gerror");
    CapturedLog const log;

    CHECK_NOTHROW(glogger << "a task ended" << '\n' << GERROR);

    const auto messages = log.messages();
    REQUIRE(messages.size() == 1);
    CHECK(messages.front().contains("a task ended"));
    CHECK(messages.front().contains("ERROR"));
    CHECK(occurrences(contentOf(std::filesystem::path(EXCEPTION_LOG)), "a task ended") == 1);
}

TEST_CASE(
    "A task of the thread pool that ends with an error is reported once where it ends",
    "[common][error-report]"
) {
    ScratchDirectory const directory("pool");
    CapturedLog const log;

    Concurrency::GThreadPool pool(2);
    std::vector<int> items{0, 1, 2, 3};
    pool.blocking_for_each(std::span<int>(items), [](int &item) {
        if(item == 2) {
            throw gem_exception(g_error_streamer() << "item two failed" << '\n');
        }
        item += 10;
    });

    CHECK(items == std::vector<int>{10, 11, 2, 13});
    const auto messages = log.messages();
    REQUIRE(messages.size() == 1);
    CHECK(messages.front().contains("item two failed"));
    CHECK(occurrences(contentOf(std::filesystem::path(EXCEPTION_LOG)), "item two failed") == 1);
}
