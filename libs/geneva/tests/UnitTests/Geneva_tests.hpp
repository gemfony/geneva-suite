/**
 * @file Geneva_tests.hpp
 *
 * INTERNAL — not installed, not part of the public Geneva API.
 *
 * Helper template functions used by Geneva's internal test drivers
 * (libs/geneva/tests/UnitTests/ and libs/geneva/examples/.../Tests/UnitTests/).
 * External users should write their own Catch2 test drivers directly
 * against the Gem::Common::GSelfTestable methods specificTestsNoFailureExpected_GUnitTests()
 * and specificTestsFailuresExpected_GUnitTests() — see the project's
 * test driver in tests/geneva/UnitTests/GenevaStandardTests.cpp for the
 * pattern.
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
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <concepts>
#include <string>
#include <tuple>
#include <typeinfo>
#include <vector>

// Catch2 headers go here
#include <catch2/catch_test_macros.hpp>

using namespace Gem::Hap;
using namespace Gem::Geneva;

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GSelfTestable.hpp"
#include "common/GExceptions.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "common/GTupleIO.hpp"
#include "common/GUnitTestFrameworkT.hpp"

#include "GEqualityPrinter.hpp"

/*************************************************************************************************/

namespace Gem::Geneva::Tests {

/*************************************************************************************************/
/**
 * Deduces the CRTP category root of a tested type T.
 *
 * There is no single common base to hard-code: each logical category has its own CRTP
 * root (personality traits bottom out at GPersonalityTraits, genomes at GGenome, and so
 * on). The category root is the type parameter of the Gem::Common::GCommonInterfaceT<Root>
 * base, and it is exactly the element_type of the std::shared_ptr returned by the public
 * (inherited) clone() method. Deducing it this way works uniformly for every root, so the
 * standard-test template stays category-agnostic.
 */
template <typename T>
using category_root_t = typename decltype(std::declval<const T &>().clone())::element_type;

/*************************************************************************************************/
/**
 * This function performs common tests that need to be passed by every core Geneva class and
 * should be passed by user individuals as well. Most notably, this includes (de-)serialization
 * in different modes.
 */
template <typename T>
void StandardTests_no_failure_expected() {
    // The self-test facet is an OPT-IN base (Gem::Common::GSelfTestable), not a member of the
    // universal interface. Requiring it here rather than skipping the hooks when absent is
    // deliberate: modify_GUnitTests() is what makes the (de-)serialization round-trip blocks below
    // meaningful, so a type that quietly lost the facet would keep passing this test while no longer
    // exercising it. Failing to compile names the problem instead.
    static_assert(
        std::derived_from<T, Gem::Common::GSelfTestable>,
        "StandardTests_no_failure_expected<T>: T must opt into the Gem::Common::GSelfTestable facet "
        "(inherit it and override modify_GUnitTests_() -- chaining to the category root's protected "
        "helper if the root is where the perturbation lives)."
    );
    // The CRTP category root of the tested type (GGenome for individuals,
    // GPersonalityTraits for personality traits, and so on).
    using root_t = category_root_t<T>;
    // Prepare printing of error messages in object comparisons
    GEqualityPrinter gep(
        "StandardTests_no_failure_expected",
        pow(10, -7),
        Gem::Common::CE_WITH_MESSAGES
    );

    //---------------------------------------------------------------------------//
    // Tests of construction, loading, cloning, ...

    { // Test default construction and copy construction
        std::shared_ptr<T> T_ptr, T_ptr_cp;

        // Default construction
        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr); // must point somewhere

        // Make sure the object is not in pristine condition
        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());

        // Copy construction
        REQUIRE_NOTHROW(T_ptr_cp = std::make_shared<T>(*T_ptr));

        // Check for equivalence and similarity
        CHECK(gep.isEqual(*T_ptr_cp, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_cp, *T_ptr));

        // Check that the smart pointers are unique
        CHECK(T_ptr.unique());
        CHECK(T_ptr_cp.unique());

        // Check destruction. Resetting the smart pointer will delete
        // the stored object if it was the last remaining reference to it.
        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_cp.reset());
    }

    { // Test cloning to the category root
        std::shared_ptr<root_t> T_ptr, T_ptr_clone;

        // Default construction. The perturbation runs through the DERIVED type: the category root
        // deliberately does not carry the self-test facet (that is what keeps it out of the vtable
        // of every user class), so the hook is only reachable before the upcast.
        {
            std::shared_ptr<T> T_derived;
            REQUIRE_NOTHROW(T_derived = TFactory_GUnitTests<T>());
            REQUIRE(T_derived); // must point somewhere

            // Make sure the object is not in pristine condition
            REQUIRE_NOTHROW(T_derived->modify_GUnitTests());

            T_ptr = T_derived; // the sole remaining owner once T_derived goes out of scope
        }
        REQUIRE(T_ptr); // must point somewhere

        // Cloning
        REQUIRE_NOTHROW(T_ptr_clone = T_ptr->clone());

        // Check for equivalence and similarity
        CHECK(gep.isEqual(*T_ptr_clone, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_clone, *T_ptr));

        // Check that the smart pointers are unique
        CHECK(T_ptr.unique());
        CHECK(T_ptr_clone.unique());

        // Check destruction. Resetting the smart pointer will delete
        // the stored object if it was the last remaining reference to it.
        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_clone.reset());
    }

    { // Test cloning to a target type
        std::shared_ptr<T> T_ptr, T_ptr_clone;

        // Default construction
        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr); // must point somewhere

        // Make sure the object is not in pristine condition
        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());

        // Cloning
        REQUIRE_NOTHROW(T_ptr_clone = T_ptr->template clone<T>());

        // Check for equivalence and similarity
        CHECK(gep.isEqual(*T_ptr_clone, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_clone, *T_ptr));

        // Check that the smart pointers are unique
        CHECK(T_ptr.unique());
        CHECK(T_ptr_clone.unique());

        // Check destruction. Resetting the smart pointer will delete
        // the stored object if it was the last remaining reference to it.
        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_clone.reset());
    }

    { // Test loading through a std::shared_ptr
        std::shared_ptr<T> T_ptr, T_ptr_load;

        // Default construction
        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr); // must point somewhere

        // Make sure the object is not in pristine condition
        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());

        // Loading
        REQUIRE_NOTHROW(T_ptr_load = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr_load); // must point somewhere

        REQUIRE_NOTHROW(T_ptr_load->load(T_ptr));
        // Check for equivalence and similarity
        CHECK(gep.isEqual(*T_ptr_load, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_load, *T_ptr));

        // Check that the smart pointers are unique
        CHECK(T_ptr.unique());
        CHECK(T_ptr_load.unique());

        // Check destruction. Resetting the smart pointer will delete
        // the stored object if it was the last remaining reference to it.
        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_load.reset());
    }

    { // Test loading through a reference
        std::shared_ptr<T> T_ptr, T_ptr_load;

        // Default construction
        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr); // must point somewhere

        // Make sure the object is not in pristine condition
        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());

        // Loading
        REQUIRE_NOTHROW(T_ptr_load = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr_load); // must point somewhere
        REQUIRE_NOTHROW(T_ptr_load->load(*T_ptr));
        // Check for equivalence and similarity
        CHECK(gep.isEqual(*T_ptr_load, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_load, *T_ptr));

        // Check that the smart pointers are unique
        CHECK(T_ptr.unique());
        CHECK(T_ptr_load.unique());

        // Check destruction. Resetting the smart pointer will delete
        // the stored object if it was the last remaining reference to it.
        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_load.reset());
    }

    { // Check assignment using operator=
        std::shared_ptr<T> T_ptr, T_ptr_assign;

        // Default construction
        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr); // must point somewhere

        // Make sure the object is not in pristine condition
        REQUIRE_NOTHROW(T_ptr->modify_GUnitTests());

        // Assignment
        REQUIRE_NOTHROW(T_ptr_assign = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr_assign); // must point somewhere
        REQUIRE_NOTHROW(T_ptr_assign->load(*T_ptr));

        // Check for equivalence and similarity
        CHECK(gep.isEqual(*T_ptr_assign, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_assign, *T_ptr));

        // Check that the smart pointers are unique
        CHECK(T_ptr.unique());
        CHECK(T_ptr_assign.unique());

        // Check destruction. Resetting the smart pointer will delete
        // the stored object if it was the last remaining reference to it.
        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_assign.reset());
    }

    //---------------------------------------------------------------------------//
    // Check (de-)serialization in different modes through object functions

    { // GEM_BINARY format
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1); // must point somewhere
        std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr2); // must point somewhere

        // Modify and check inequality
        if(T_ptr1->modify_GUnitTests()) { // Has the object been modified ?
            CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));

            // Serialize T_ptr1 and load into T_ptr1, check equalities and similarities
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

    { // GEM_JSON format
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1); // must point somewhere
        std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr2); // must point somewhere

        // Modify and check inequality
        if(T_ptr1->modify_GUnitTests()) {
            CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));

            // Serialize T_ptr1 and load into T_ptr1, check equalities and similarities
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
    // These are particularly used in the Courtier library

    { // GEM_BINARY mode
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1); // must point somewhere
        std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr2); // must point somewhere

        // Modify and check inequality
        if(T_ptr1->modify_GUnitTests()) { // Has the object been modified ?
            CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));

            // Serialize T_ptr1 and load into T_ptr2, check equalities and similarities
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
        REQUIRE(T_ptr1); // must point somewhere
        std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr2); // must point somewhere

        // Modify and check inequality
        if(T_ptr1->modify_GUnitTests()) { // Has the object been modified ?
            CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));

            // Serialize T_ptr1 and load into T_ptr2, check equalities and similarities
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
        REQUIRE(T_ptr); // must point somewhere
        T_ptr->specificTestsNoFailureExpected_GUnitTests();
    }
}

/*************************************************************************************************/
/**
 * This function performs common tests that should lead to a failure for every core Geneva class as
 * as user individuals. Most notably, self-assignment should fail.
 */
template <typename T>
void StandardTests_failures_expected() {
    static_assert(
        std::derived_from<T, Gem::Common::GSelfTestable>,
        "StandardTests_failures_expected<T>: T must opt into the Gem::Common::GSelfTestable facet."
    );
    // Prepare printing of error messages in object comparisons
    GEqualityPrinter gep(
        "StandardTests_failures_expected",
        pow(10, -10),
        Gem::Common::CE_WITH_MESSAGES
    );

    {
        // Checks that self-assignment throws in DEBUG mode
#ifdef DEBUG
        std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr1); // must point somewhere
        CHECK_THROWS_AS(T_ptr1->load(T_ptr1), geneva_exception);
#endif
    }

    //---------------------------------------------------------------------------//
    // Run specific tests for the current object type
    {
        std::shared_ptr<T> T_ptr = TFactory_GUnitTests<T>();
        REQUIRE(T_ptr); // must point somewhere
        CHECK_NOTHROW(T_ptr->specificTestsFailuresExpected_GUnitTests());
    }
}

/*************************************************************************************************/

} /* namespace Gem::Geneva::Tests */

/*************************************************************************************************/
