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
 * Standalone exerciser for the FATAL reconciliation paths of the courtier submission policies.
 * These paths exit the process via LOGEXIT (std::exit, a clean non-zero exit -- no core dump),
 * which still cannot be observed in-process by Catch2, so each scenario is driven from CTest as a
 * subprocess and asserted via the process exit status (see the CMakeLists). Usage:
 *
 *   GCourtierFatalExerciser --scenario=<scenario>
 *
 * Scenarios:
 *   ok                 -- a clean batch under full-success-or-fatal: returns 0 (control case).
 *   fatal-unfixable    -- a deterministically failing slot under full-success-or-fatal: must exit non-zero.
 *   fatal-none-fatal   -- every slot fails under full-success-or-fatal: zero-usable floor, must exit non-zero.
 *   fatal-none-clone   -- every slot fails under clone-on-partial-return: nothing to clone from, must exit non-zero.
 */

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <span>

#include "common/GParserBuilder.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"

using namespace Gem::Courtier;
using Gem::Courtier::fault_mode;
using Gem::Courtier::GFaultyContainer;

namespace {

using item_ptr = std::unique_ptr<GFaultyContainer>;

std::vector<item_ptr> make_batch(std::size_t n, fault_mode fm_for_all = fault_mode::NONE) {
    std::vector<item_ptr> v;
    v.reserve(n);
    for(std::size_t i = 0; i < n; ++i) {
        v.push_back(std::make_unique<GFaultyContainer>(i, fm_for_all));
    }
    return v;
}

} /* anonymous namespace */

int main(int argc, char **argv) {
    std::string scenario;
    Gem::Common::GParserBuilder gpb;
    gpb.registerCLParameter(
        "scenario", scenario, std::string(""),
        "which fatal path to exercise: ok | fatal-unfixable | fatal-none-fatal | fatal-none-clone"
    );
    if(gpb.parseCommandLine(argc, argv) == Gem::Common::GCL_HELP_REQUESTED) {
        return 0;
    }
    if(scenario.empty()) {
        std::cerr << "Error: --scenario is required "
                     "(ok | fatal-unfixable | fatal-none-fatal | fatal-none-clone)\n";
        return 2;
    }

    auto consumer = std::make_shared<GStdThreadConsumerT<GFaultyContainer>>(4);
    auto workOn = [&consumer](std::vector<item_ptr> &batch, const GSubmissionPolicy &policy) {
        consumer->processBatch(std::span<item_ptr>(batch.data(), batch.size()), policy);
    };

    if(scenario == "ok") {
        auto batch = make_batch(8, fault_mode::NONE);
        workOn(batch, GSubmissionPolicy::full_success_or_fatal());
        std::cout << "ok: returned normally\n";
        return 0;
    }
    if(scenario == "fatal-unfixable") {
        // One deterministically-throwing slot, no retry budget -> unresolved -> FATAL.
        auto batch = make_batch(8, fault_mode::NONE);
        batch[3] = std::make_unique<GFaultyContainer>(3, fault_mode::THROW_PROCESSING);
        workOn(batch, GSubmissionPolicy::full_success_or_fatal());
        std::cout << "fatal-unfixable: ERROR -- returned without terminating\n";
        return 0; // should be unreachable
    }
    if(scenario == "fatal-none-fatal") {
        auto batch = make_batch(8, fault_mode::THROW_PROCESSING);
        workOn(batch, GSubmissionPolicy::full_success_or_fatal());
        std::cout << "fatal-none-fatal: ERROR -- returned without terminating\n";
        return 0; // should be unreachable
    }
    if(scenario == "fatal-none-clone") {
        // Even the tolerant clone policy cannot continue if NOTHING came back to clone from.
        auto batch = make_batch(8, fault_mode::THROW_PROCESSING);
        workOn(batch, GSubmissionPolicy::clone_on_partial_return());
        std::cout << "fatal-none-clone: ERROR -- returned without terminating\n";
        return 0; // should be unreachable
    }

    std::cerr << "Unknown scenario: " << scenario << '\n';
    return 2;
}
