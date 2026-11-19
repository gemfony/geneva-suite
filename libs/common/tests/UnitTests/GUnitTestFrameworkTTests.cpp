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

#include <memory>

#include "common/GExceptions.hpp"
#include "common/GUnitTestFrameworkT.hpp"

namespace {

struct Plain {
    int v{99};
};

// A type whose default constructor always throws a gem_exception; used
// to verify that TFactory_GUnitTests re-throws gem exceptions verbatim.
struct ThrowsGem {
    ThrowsGem() {
        throw gem_exception("intentional from ThrowsGem");
    }
};

// A type whose default constructor throws a non-gem exception; the
// factory must wrap it in a gem_exception (the "Caught unknown
// exception" path) before rethrowing.
struct ThrowsOther {
    ThrowsOther() {
        throw std::logic_error("not a gem exception");
    }
};

} // namespace

TEST_CASE(
    "TFactory_GUnitTests<T>: default-constructible T returns a populated shared_ptr",
    "[common][unit-framework]"
) {
    auto p = TFactory_GUnitTests<Plain>();
    REQUIRE(p);
    CHECK(p->v == 99);
}

TEST_CASE(
    "TFactory_GUnitTests<T>: gem_exception from T's ctor propagates unchanged",
    "[common][unit-framework]"
) {
    CHECK_THROWS_AS(TFactory_GUnitTests<ThrowsGem>(), gem_exception);
}

TEST_CASE(
    "TFactory_GUnitTests<T>: non-gem exception is wrapped in gem_exception",
    "[common][unit-framework]"
) {
    // The factory catches unknown exceptions and re-throws as gem_exception
    // with a "Caught unknown exception" message.
    try {
        TFactory_GUnitTests<ThrowsOther>();
        FAIL("expected gem_exception to be thrown");
    }
    catch(gem_exception const &g) {
        std::string const what = g.what();
        CHECK(what.contains("Caught unknown exception"));
    }
    catch(...) {
        FAIL("threw a non-gem exception type");
    }
}
