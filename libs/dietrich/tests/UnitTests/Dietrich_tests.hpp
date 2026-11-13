/**
 * @file Dietrich_tests.hpp
 *
 * INTERNAL — not installed, not part of the public Dietrich API.
 *
 * Helper template functions used by Dietrich's internal test driver
 * (dietrich/tests/UnitTests/GPlotDesignerTests.cpp). They mirror the Geneva
 * standard-tests driver (geneva/tests/UnitTests/Geneva_tests.hpp) but depend ONLY
 * on common -- dietrich sits below geneva in the library stack and must not pull it
 * in. External users should write their own Catch2 drivers directly against the
 * Gem::Common::GSelfTestable methods modify_GUnitTests() / specificTests*_GUnitTests().
 */

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

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <cmath>
#include <concepts>
#include <iostream>
#include <memory>
#include <string>
#include <typeinfo>

// Catch2 headers go here
#include <catch2/catch_test_macros.hpp>

// Common headers go here (dietrich depends only on common)
#include "common/GCommonEnums.hpp"
#include "common/GExceptions.hpp"
#include "common/GSelfTestable.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "common/GUnitTestFrameworkT.hpp"

#include "GEqualityPrinter.hpp"

using namespace Gem::Dietrich;

/*************************************************************************************************/

namespace Gem::Dietrich::Tests {

/*************************************************************************************************/
/**
 * Deduces the CRTP category root of a tested type T -- the type parameter of the
 * Gem::Common::GCommonInterfaceT<Root> base, which is exactly the element_type of the
 * std::shared_ptr returned by the public (inherited) clone() method (GBasePlotter for
 * the plotters, GPlotDesigner / GDecorator / GDecoratorContainer for the others).
 */
template <typename T>
using category_root_t = typename decltype(std::declval<const T &>().clone())::element_type;

/*************************************************************************************************/
/**
 * Performs the standard tests every Gem::Common::GCommonInterfaceT type should pass:
 * construction, copy / clone / load / assignment equivalence, and (de-)serialization
 * round-trips in all three modes through both the object methods and the external
 * Gem::Common helpers. The (de-)serialization blocks run only when modify_GUnitTests()
 * actually changes the object.
 */
template <typename T>
void StandardTests_no_failure_expected() {
    // The self-test facet is an OPT-IN base (Gem::Common::GSelfTestable), not part of the universal
    // interface. Requiring it instead of skipping the hooks when absent is deliberate:
    // modify_GUnitTests() is what makes the round-trip blocks below meaningful, so a type that
    // quietly lost the facet would keep passing while no longer exercising it.
    static_assert(
        std::derived_from<T, Gem::Common::GSelfTestable>,
        "StandardTests_no_failure_expected<T>: T must opt into the Gem::Common::GSelfTestable facet."
    );
    using root_t = category_root_t<T>;
    GEqualityPrinter gep(
        "StandardTests_no_failure_expected",
        pow(10, -7),
        Gem::Common::CE_WITH_MESSAGES
    );

    //---------------------------------------------------------------------------//
    // Tests of construction, loading, cloning, ...

    { // Test default construction and copy construction
        std::shared_ptr<T> T_ptr, T_ptr_cp;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr); // must point somewhere

        // Make sure the object is not in pristine condition
        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());

        // Copy construction
        REQUIRE_NOTHROW(T_ptr_cp = std::make_shared<T>(*T_ptr));

        CHECK(gep.isEqual(*T_ptr_cp, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_cp, *T_ptr));

        CHECK(T_ptr.unique());
        CHECK(T_ptr_cp.unique());

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_cp.reset());
    }

    { // Test cloning to the category root
        std::shared_ptr<root_t> T_ptr, T_ptr_clone;

        // The perturbation runs through the DERIVED type: the category root deliberately does not
        // carry the self-test facet, so the hook is only reachable before the upcast.
        {
            std::shared_ptr<T> T_derived;
            REQUIRE_NOTHROW(T_derived = TFactory_GUnitTests<T>());
            REQUIRE(T_derived);
            REQUIRE_NOTHROW(T_derived->modify_GUnitTests());
            T_ptr = T_derived; // the sole remaining owner once T_derived goes out of scope
        }
        REQUIRE(T_ptr);

        REQUIRE_NOTHROW(T_ptr_clone = T_ptr->clone());

        CHECK(gep.isEqual(*T_ptr_clone, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_clone, *T_ptr));

        CHECK(T_ptr.unique());
        CHECK(T_ptr_clone.unique());

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_clone.reset());
    }

    { // Test cloning to a target type
        std::shared_ptr<T> T_ptr, T_ptr_clone;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);

        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());
        REQUIRE_NOTHROW(T_ptr_clone = T_ptr->template clone<T>());

        CHECK(gep.isEqual(*T_ptr_clone, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_clone, *T_ptr));

        CHECK(T_ptr.unique());
        CHECK(T_ptr_clone.unique());

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_clone.reset());
    }

    { // Test loading through a std::shared_ptr
        std::shared_ptr<T> T_ptr, T_ptr_load;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);

        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());

        REQUIRE_NOTHROW(T_ptr_load = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr_load);

        REQUIRE_NOTHROW(T_ptr_load->load(T_ptr));
        CHECK(gep.isEqual(*T_ptr_load, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_load, *T_ptr));

        CHECK(T_ptr.unique());
        CHECK(T_ptr_load.unique());

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_load.reset());
    }

    { // Test loading through a reference
        std::shared_ptr<T> T_ptr, T_ptr_load;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);

        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());

        REQUIRE_NOTHROW(T_ptr_load = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr_load);
        REQUIRE_NOTHROW(T_ptr_load->load(*T_ptr));
        CHECK(gep.isEqual(*T_ptr_load, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_load, *T_ptr));

        CHECK(T_ptr.unique());
        CHECK(T_ptr_load.unique());

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_load.reset());
    }

    //---------------------------------------------------------------------------//
    // Check (de-)serialization in different modes through object functions

    { // plain text format
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1);
        std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr2);

        if(T_ptr1->modify_GUnitTests()) {
            CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));
            REQUIRE_NOTHROW(T_ptr2->fromString(
                T_ptr1->toString(Gem::Common::serializationMode::GEM_BINARY),
                Gem::Common::serializationMode::GEM_BINARY
            ));
            CHECK(gep.isSimilar(*T_ptr1, *T_ptr2));
        }
        else {
            std::cout << "Internal (de-)serialization test for object with name "
                      << typeid(T).name()
                      << " not run because original objects are identical / GEM_BINARY" << '\n';
        }
    }

    { // XML format
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1);
        std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr2);

        if(T_ptr1->modify_GUnitTests()) {
            CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));
            REQUIRE_NOTHROW(T_ptr2->fromString(
                T_ptr1->toString(Gem::Common::serializationMode::GEM_JSON),
                Gem::Common::serializationMode::GEM_JSON
            ));
            CHECK(gep.isSimilar(*T_ptr1, *T_ptr2));
        }
        else {
            std::cout << "Internal (de-)serialization test for object with name "
                      << typeid(T).name() << " not run because original objects are identical / GEM_JSON"
                      << '\n';
        }
    }

    //---------------------------------------------------------------------------//
    // Check (de-)serialization in different modes through external Gem::Common functions

    { // GEM_BINARY mode
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1);
        std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr2);

        if(T_ptr1->modify_GUnitTests()) {
            CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));
            std::string serializedObject =
                Gem::Common::sharedPtrToString(T_ptr1, Gem::Common::serializationMode::GEM_BINARY);
            T_ptr2 = Gem::Common::sharedPtrFromString<T>(
                serializedObject,
                Gem::Common::serializationMode::GEM_BINARY
            );
            CHECK(gep.isSimilar(*T_ptr1, *T_ptr2));
        }
        else {
            std::cout << "External (de-)serialization test for object with name "
                      << typeid(T).name()
                      << " not run because original objects are identical / GEM_BINARY" << '\n';
        }
    }

    { // GEM_JSON mode
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1);
        std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr2);

        if(T_ptr1->modify_GUnitTests()) {
            CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));
            std::string serializedObject =
                Gem::Common::sharedPtrToString(T_ptr1, Gem::Common::serializationMode::GEM_JSON);
            T_ptr2 = Gem::Common::sharedPtrFromString<T>(
                serializedObject,
                Gem::Common::serializationMode::GEM_JSON
            );
            CHECK(gep.isSimilar(*T_ptr1, *T_ptr2));
        }
        else {
            std::cout << "External (de-)serialization test for object with name "
                      << typeid(T).name() << " not run because original objects are identical / GEM_JSON"
                      << '\n';
        }
    }

    //---------------------------------------------------------------------------//

    { // Run specific tests for the current object type
        std::shared_ptr<T> T_ptr;
        CHECK_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);
        T_ptr->specificTestsNoFailureExpected_GUnitTests();
    }
}

/*************************************************************************************************/
/**
 * Performs the standard tests that should lead to a failure for every
 * Gem::Common::GCommonInterfaceT type. Most notably, self-assignment should throw in
 * DEBUG mode.
 */
template <typename T>
void StandardTests_failures_expected() {
    static_assert(
        std::derived_from<T, Gem::Common::GSelfTestable>,
        "StandardTests_failures_expected<T>: T must opt into the Gem::Common::GSelfTestable facet."
    );
    GEqualityPrinter gep(
        "StandardTests_failures_expected",
        pow(10, -10),
        Gem::Common::CE_WITH_MESSAGES
    );

    {
        // Checks that self-assignment throws in DEBUG mode
#ifdef DEBUG
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1);
        CHECK_THROWS_AS(T_ptr1->load(T_ptr1), geneva_exception);
#endif
    }

    //---------------------------------------------------------------------------//
    // Run specific tests for the current object type
    {
        std::shared_ptr<T> T_ptr = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr);
        CHECK_NOTHROW(T_ptr->specificTestsFailuresExpected_GUnitTests());
    }
}

/*************************************************************************************************/

} /* namespace Gem::Dietrich::Tests */

/*************************************************************************************************/
