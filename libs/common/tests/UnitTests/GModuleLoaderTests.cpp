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

/**
 * @file
 * @brief Tests of common's module loader (common/GModuleLoader.hpp), without Geneva.
 *
 * A real module is loaded: GTwoContributionsModule (modules/), built with the recipe of a genuine
 * one. Its two contributions are both good; which one is "bad" is decided by the handlers a case
 * passes.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/GExceptions.hpp"
#include "common/GModuleLoader.hpp"
#include "common/GModuleManifest.hpp"
#include "modules/GTwoContributionsModule.hpp"

namespace {

using Gem::Common::GContributionContext;
using Gem::Common::GContributionHandler;
using Gem::Tests::GTestPayloadPtr;
using Gem::Tests::TEST_KIND_FIRST;
using Gem::Tests::TEST_KIND_SECOND;

/** @brief What the handlers of one case saw, in call order. */
struct Recorder {
    std::vector<std::string> events; ///< "check <name>" and "take <name>", in call order
    std::vector<std::string> taken;  ///< "<name>/<value>" for every contribution that was taken
    std::vector<GContributionContext> contexts; ///< The context of every take
};

/** @brief A check that records that it ran and accepts the contribution.
 *  @param recorder Where the call is recorded
 *  @return The check function */
GContributionHandler::function_type recordingCheck(Recorder &recorder) {
    return [&recorder](
               const GenevaContribution &contribution,
               const GContributionContext & /*context*/
           ) { recorder.events.push_back(std::string("check ") + contribution.name_or_mnemonic); };
}

/** @brief A take that calls the factory and records the name and the value it handed over.
 *  @param recorder Where the call is recorded
 *  @return The take function */
GContributionHandler::function_type recordingTake(Recorder &recorder) {
    return [&recorder](
               const GenevaContribution &contribution,
               const GContributionContext &context
           ) {
        const auto payload =
            Gem::Common::takeContribution<GTestPayloadPtr>(contribution, context, "test payload");
        recorder.events.push_back(std::string("take ") + contribution.name_or_mnemonic);
        recorder.taken.push_back(
            std::string(contribution.name_or_mnemonic) + "/" + std::to_string(*payload)
        );
        recorder.contexts.push_back(context);
    };
}

/** @brief Loads @p path with @p handlers, requires that it throws, and returns the diagnostic text.
 *  @param path The module to load
 *  @param handlers The handlers to pass
 *  @return The exception's message (empty if -- contrary to the requirement -- nothing was
 *  thrown) */
std::string rejectionMessage(
    const std::filesystem::path &path,
    std::span<const GContributionHandler> handlers
) {
    try {
        Gem::Common::loadModule(path, handlers);
    }
    catch(const std::exception &e) {
        return e.what();
    }
    return {};
}

/** @brief Whether @p haystack contains @p needle.
 *  @param haystack The diagnostic text
 *  @param needle The phrase that must be there
 *  @return true if the phrase is present */
bool mentions(const std::string &haystack, const std::string &needle) {
    return haystack.contains(needle);
}

/** @brief A path that does not exist, so that a load of it can only fail when the file is
 *  opened. */
std::filesystem::path noSuchModule() {
    return std::filesystem::temp_directory_path() / "geneva_no_such_module_for_loader_tests.so";
}

} // anonymous namespace

/******************************************************************************/

TEST_CASE("Module loader: both kinds served", "[common][moduleloader]") {
    Recorder recorder;
    const std::array<GContributionHandler, 2> handlers{{
        {.kind = TEST_KIND_FIRST,
         .noun = "first thing",
         .check = recordingCheck(recorder),
         .take = recordingTake(recorder)},
        {.kind = TEST_KIND_SECOND,
         .noun = "second thing",
         .check = recordingCheck(recorder),
         .take = recordingTake(recorder)},
    }};

    REQUIRE_NOTHROW(Gem::Common::loadModule(GENEVA_TEST_MODULE_TWO_CONTRIBUTIONS, handlers));

    CHECK(recorder.taken == std::vector<std::string>{"first/1", "second/2"});
    // Every check ran before any take
    CHECK(
        recorder.events ==
        std::vector<std::string>{"check first", "check second", "take first", "take second"}
    );

    REQUIRE(recorder.contexts.size() == 2);
    for(std::size_t i = 0; i < recorder.contexts.size(); ++i) {
        const GContributionContext &context = recorder.contexts[i];
        CHECK(context.index == i);
        CHECK(context.path == std::filesystem::path(GENEVA_TEST_MODULE_TWO_CONTRIBUTIONS));
        CHECK(mentions(context.description, GENEVA_TEST_MODULE_TWO_CONTRIBUTIONS));
        CHECK(mentions(context.description, "GTwoContributionsModule"));
    }
}

TEST_CASE(
    "Module loader: good first, bad second -- no handler for the second kind",
    "[common][moduleloader]"
) {
    Recorder recorder;
    const std::array<GContributionHandler, 1> handlers{{
        {.kind = TEST_KIND_FIRST,
         .noun = "first thing",
         .check = recordingCheck(recorder),
         .take = recordingTake(recorder)},
    }};

    const std::string msg = rejectionMessage(GENEVA_TEST_MODULE_TWO_CONTRIBUTIONS, handlers);
    REQUIRE_FALSE(msg.empty());
    CHECK(mentions(msg, "kind 1002"));
    CHECK(mentions(msg, "second"));
    CHECK(mentions(msg, "GTwoContributionsModule"));
    CHECK(mentions(msg, "Supported kinds"));
    CHECK(mentions(msg, "1001 (first thing)"));

    // The module was refused as a whole: the good first contribution was not taken either
    CHECK(recorder.taken.size() == 0); // NOLINT(readability-container-size-empty): shows the count
}

TEST_CASE(
    "Module loader: good first, bad second -- the second's check refuses",
    "[common][moduleloader]"
) {
    Recorder recorder;
    const std::array<GContributionHandler, 2> handlers{{
        {.kind = TEST_KIND_FIRST,
         .noun = "first thing",
         .check = recordingCheck(recorder),
         .take = recordingTake(recorder)},
        {.kind = TEST_KIND_SECOND,
         .noun = "second thing",
         .check =
             [](const GenevaContribution & /*contribution*/,
                const GContributionContext & /*context*/) {
                 throw std::runtime_error("the second contribution is refused by its check");
             },
         .take = recordingTake(recorder)},
    }};

    const std::string msg = rejectionMessage(GENEVA_TEST_MODULE_TWO_CONTRIBUTIONS, handlers);
    CHECK(msg == "the second contribution is refused by its check");   // passes through unchanged
    CHECK(recorder.events == std::vector<std::string>{"check first"}); // the first check ran

    // The module was refused as a whole: the good first contribution was not taken either
    CHECK(recorder.taken.size() == 0); // NOLINT(readability-container-size-empty): shows the count
}

TEST_CASE("Module loader: the kinds in the message are the handlers'", "[common][moduleloader]") {
    Recorder recorder;
    const std::array<GContributionHandler, 2> handlers{{
        {.kind = 2001u, .noun = "apple", .check = {}, .take = recordingTake(recorder)},
        {.kind = 2002u, .noun = "pear", .check = {}, .take = recordingTake(recorder)},
    }};

    const std::string msg = rejectionMessage(GENEVA_TEST_MODULE_TWO_CONTRIBUTIONS, handlers);
    REQUIRE_FALSE(msg.empty());
    CHECK(mentions(msg, "Supported kinds: 2001 (apple), 2002 (pear)"));
    CHECK(recorder.taken.empty());
}

TEST_CASE(
    "Module loader: unusable handlers are refused before the module is opened",
    "[common][moduleloader]"
) {
    // The path does not exist: a text other than "Could not load the module" shows that the
    // handlers were looked at first.
    Recorder recorder;

    SECTION("no handler") {
        const std::string msg = rejectionMessage(noSuchModule(), {});
        CHECK(mentions(msg, "No contribution handler was given"));
        CHECK_FALSE(mentions(msg, "Could not load the module"));
    }

    SECTION("a handler without a take function") {
        const std::array<GContributionHandler, 1> handlers{{
            {.kind = TEST_KIND_FIRST,
             .noun = "first thing",
             .check = recordingCheck(recorder),
             .take = {}},
        }};
        const std::string msg = rejectionMessage(noSuchModule(), handlers);
        CHECK(mentions(msg, "The handler for kind 1001 (first thing) has no take function."));
        CHECK_FALSE(mentions(msg, "Could not load the module"));
    }

    SECTION("the same kind twice") {
        const std::array<GContributionHandler, 2> handlers{{
            {.kind = TEST_KIND_FIRST,
             .noun = "first thing",
             .check = {},
             .take = recordingTake(recorder)},
            {.kind = TEST_KIND_FIRST,
             .noun = "first again",
             .check = {},
             .take = recordingTake(recorder)},
        }};
        const std::string msg = rejectionMessage(noSuchModule(), handlers);
        CHECK(mentions(msg, "More than one handler was given for kind 1001."));
        CHECK_FALSE(mentions(msg, "Could not load the module"));
    }

    CHECK(recorder.events.empty());
}

TEST_CASE("Module loader: the gates work without Geneva", "[common][moduleloader]") {
    SECTION("a shared library that is not a module") {
        std::string msg;
        try {
            (void)Gem::Common::openModule(GENEVA_TEST_NOT_A_MODULE);
        }
        catch(const std::exception &e) {
            msg = e.what();
        }
        CHECK(mentions(msg, "is not a Geneva module"));
        CHECK(mentions(msg, GENEVA_TEST_NOT_A_MODULE));
    }

    SECTION("a path that does not exist") {
        std::string msg;
        try {
            (void)Gem::Common::openModule(noSuchModule());
        }
        catch(const std::exception &e) {
            msg = e.what();
        }
        CHECK(mentions(msg, "Could not load the module"));
    }

    SECTION("the test module") {
        const GenevaModuleManifest *manifest =
            Gem::Common::openModule(GENEVA_TEST_MODULE_TWO_CONTRIBUTIONS);
        REQUIRE(manifest != nullptr);
        CHECK(manifest->contributions_count == 2);
    }
}

/******************************************************************************/
// Toolchain-compatibility gate (GenevaCompat). The loader validates a module's fingerprint before
// touching any of its C++ contributions; here we exercise that gate directly (a .so is not needed)
// via the exposed comparator. Regression guard for the silent-crash hole where a
// same-GENEVA_VERSION module built with an incompatible compiler/stdlib/Boost/build-mode used to
// pass and then corrupt the process.
TEST_CASE(
    "Module compat gate accepts this host's own fingerprint",
    "[common][moduleloader][compat]"
) {
    const GenevaCompat self = Gem::Common::thisHostCompat();
    CHECK(Gem::Common::moduleCompatMismatch(self).empty());
}

TEST_CASE(
    "Module compat gate rejects a mismatched toolchain, naming the axis",
    "[common][moduleloader][compat]"
) {
    using Gem::Common::moduleCompatMismatch;
    const GenevaCompat host = Gem::Common::thisHostCompat();

    SECTION("compiler version") {
        GenevaCompat m = host;
        m.compiler_major += 1;
        const std::string d = moduleCompatMismatch(m);
        CHECK_FALSE(d.empty());
        CHECK(d.contains("compiler major"));
    }
    SECTION("standard-library version") {
        GenevaCompat m = host;
        m.stdlib_version += 1;
        const std::string d = moduleCompatMismatch(m);
        CHECK_FALSE(d.empty());
        CHECK(d.contains("standard-library version"));
    }
    SECTION("build-mode ABI flags") {
        GenevaCompat m = host;
        m.abi_flags ^= GENEVA_ABI_FLAG_ASAN; // pretend the module was built with AddressSanitizer
        const std::string d = moduleCompatMismatch(m);
        CHECK_FALSE(d.empty());
        CHECK(d.contains("ABI flags"));
    }
    SECTION("Geneva version") {
        GenevaCompat m = host;
        m.geneva_version += 1;
        const std::string d = moduleCompatMismatch(m);
        CHECK_FALSE(d.empty());
        CHECK(d.contains("Geneva version"));
    }
}
