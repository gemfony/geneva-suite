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

// The text of the fatal report (common/GFatalReport.hpp) for each kind of exception. The handler
// itself ends the process; the programs that end through it are tested where they run
// (GSimpleOptimizerConfigError, GCourtierMpiFatalEndTest).

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <stdexcept>
#include <string>

#include "common/GExceptions.hpp"
#include "common/GFatalReport.hpp"

TEST_CASE("the fatal report names the error and the exit code", "[common][fatal]") {
    const std::string exit_line = "\nThe program ends with the exit code 1.\n";
    CHECK(
        Gem::Common::fatalMessage(
            std::make_exception_ptr(Gem::Common::gem_exception("bad entry"))
        ) == "\nError: bad entry" + exit_line
    );
    CHECK(
        Gem::Common::fatalMessage(std::make_exception_ptr(std::runtime_error("no file"))) ==
        "\nError: no file (a standard exception)" + exit_line
    );
    CHECK(
        Gem::Common::fatalMessage(std::make_exception_ptr(42)) ==
        "\nError: an exception of unknown type" + exit_line
    );
    CHECK(
        Gem::Common::fatalMessage(nullptr) ==
        "\nError: std::terminate() was called without an exception\nThe program aborts.\n"
    );
}
