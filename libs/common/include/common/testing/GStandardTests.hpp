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
 * @file GStandardTests.hpp
 *
 * @brief The standard tests every reflective value type of the collection should pass, for Catch2 test
 * programs.
 *
 * A test program -- in this tree or in a project built on an installed collection -- includes this
 * header and calls `StandardTests_no_failure_expected<T>()` / `StandardTests_failures_expected<T>()`
 * inside a Catch2 test case. The tests construct, copy, clone, load, assign and (de-)serialize objects
 * of `T` and compare the results. No library includes this header: Catch2 is needed by the test program
 * that does, never by a library of the collection.
 *
 * A type that has a public member `bool modifyForTest()` is changed with it before the comparisons, so
 * that a copy, clone or round-trip that loses a member shows. A type without one is tested unchanged;
 * the round-trip blocks that need a changed object are then skipped, with a notice.
 */

#pragma once

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <cmath>
#include <concepts>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>

// Catch2 headers go here
#include <catch2/catch_test_macros.hpp>

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GContainerT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"

namespace Gem::Common::Testing {

/******************************************************************************/
/**
 * @brief Creates a new T object for the standard tests.
 *
 * A test program specializes it for a type without a public default constructor, before the
 * standard tests are instantiated for that type.
 *
 * @tparam T The type of the object to be created for testing
 * @return A std::shared_ptr to a newly created T object
 */
template <typename T>
std::shared_ptr<T> TFactory_GUnitTests() {
    std::shared_ptr<T> p;

    try {
        p = std::make_shared<T>();
    }
    catch(const gem_exception &) {
        // Re-throw without slicing: bare `throw;` propagates the dynamic
        // type, whereas `throw g;` would copy-construct a gem_exception
        // and discard any derived-class state.
        throw;
    }
    catch(...) {
        throw gem_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In std::shared_ptr<T> TFactory_GUnitTests(): Error!" << '\n'
            << "Caught unknown exception" << '\n'
        );
    }

    return p;
}

/******************************************************************************/
/**
 * @brief Compares two objects with the reflective interface and prints the difference when they differ.
 */
class GEqualityPrinter {
public:
    GEqualityPrinter(const GEqualityPrinter &) = delete;
    GEqualityPrinter &operator=(const GEqualityPrinter &) = delete;

    /***************************************************************************/
    /**
     * @brief The only constructor.
     *
     * @param caller The name of the calling entity
     * @param limit Used in floating point comparisons to check similarity
     * @param emitMessages Determines whether messages should be emitted upon error
     */
    GEqualityPrinter(const std::string &caller, const double &limit, const bool &emitMessages)
      : caller_(caller)
      , limit_(limit)
      , emit_messages_(emitMessages) { /* nothing */
    }

    /***************************************************************************/
    /**
     * @brief Checks for equality of two objects of one type with the reflective interface, optionally
     * emitting a message.
     *
     * @param x The first parameter to compare
     * @param y The second parameter to compare
     * @return A boolean indicating whether both parameters are equal
     */
    template <typename gem_type>
        requires Gem::Common::gemfony_common_interface<gem_type>
    bool isEqual(const gem_type &x, const gem_type &y) const {
        try {
            x.compare(y, Gem::Common::expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
            return true;
        }
        catch(g_expectation_violation &g) {
            this->report(g);
            return false;
        }
    }

    /***************************************************************************/
    /**
     * @brief Checks for equality of a container type with a std::vector of its value type, optionally
     * emitting a message.
     *
     * @param x The container to compare
     * @param y The std::vector used for the comparison
     * @return A boolean indicating whether both parameters are equal
     */
    template <typename gem_simplecontainer_type>
        requires std::derived_from<
            gem_simplecontainer_type,
            Gem::Common::GPodContainerT<typename gem_simplecontainer_type::value_type>>
    bool isEqual(
        const gem_simplecontainer_type &x,
        const std::vector<typename gem_simplecontainer_type::value_type> &y
    ) const {
        try {
            x.Gem::Common::template GPodContainerT<typename gem_simplecontainer_type::value_type>::
                compare_base(y, Gem::Common::expectation::EQUALITY, CE_DEF_SIMILARITY_DIFFERENCE);
            return true;
        }
        catch(g_expectation_violation &g) {
            this->report(g);
            return false;
        }
    }

    /***************************************************************************/
    /**
     * @brief Checks for similarity of two objects of one type with the reflective interface, optionally
     * emitting a message.
     *
     * @param x The first parameter to compare
     * @param y The second parameter to compare
     * @return A boolean indicating whether both parameters are similar
     */
    template <typename gem_type>
        requires Gem::Common::gemfony_common_interface<gem_type>
    bool isSimilar(const gem_type &x, const gem_type &y) const {
        try {
            x.compare(y, Gem::Common::expectation::FP_SIMILARITY, CE_DEF_SIMILARITY_DIFFERENCE);
            return true;
        }
        catch(g_expectation_violation &g) {
            this->report(g);
            return false;
        }
    }

    /***************************************************************************/
    /**
     * @brief Checks for similarity of a container type with a std::vector of its value type, optionally
     * emitting a message.
     *
     * @param x The container to compare
     * @param y The std::vector used for the comparison
     * @return A boolean indicating whether both parameters are similar
     */
    template <typename gem_simplecontainer_type>
        requires std::derived_from<
            gem_simplecontainer_type,
            Gem::Common::GPodContainerT<typename gem_simplecontainer_type::value_type>>
    bool isSimilar(
        const gem_simplecontainer_type &x,
        const std::vector<typename gem_simplecontainer_type::value_type> &y
    ) const {
        try {
            x.Gem::Common::template GPodContainerT<typename gem_simplecontainer_type::value_type>::
                compare_base(
                    y,
                    Gem::Common::expectation::FP_SIMILARITY,
                    CE_DEF_SIMILARITY_DIFFERENCE
                );
            return true;
        }
        catch(g_expectation_violation &g) {
            this->report(g);
            return false;
        }
    }

    /***************************************************************************/

private:
    /** @brief Prints a violated expectation, if messages were asked for
     *  @param g The violated expectation */
    void report(g_expectation_violation const &g) const {
        if(emit_messages_) {
            std::cout << "\n=========================================\n"
                      << g << "\n=========================================\n";
        }
    }

    std::string caller_; ///< Holds the name of the calling entity
    double limit_;       ///< A limit used to determine similarity in fp comparisons
    bool
        emit_messages_; ///< Specifies whether messages should be emitted if expectations were not met
};

/******************************************************************************/
/**
 * @brief A type whose objects the standard tests can change: it has a public member
 * `bool modifyForTest()`.
 *
 * The member is an ordinary, non-virtual function of the class, always compiled. It changes the object
 * with the class's own means and returns whether it changed something. A class that adds members to a
 * base with such a function declares its own, calls the base's and changes its own members.
 */
template <typename T>
concept modifiable_for_test = requires(T &t) {
    { t.modifyForTest() } -> std::same_as<bool>;
};

/******************************************************************************/
/**
 * @brief Changes an object for the standard tests, if its type allows it.
 *
 * @param t The object to change
 * @return Whether the object was changed (always false for a type without `modifyForTest()`)
 */
template <typename T>
bool modifyForTestIfAvailable(T &t) {
    if constexpr(modifiable_for_test<T>) {
        return t.modifyForTest();
    }
    else {
        return false;
    }
}

/******************************************************************************/
/**
 * @brief The category root of a tested type T.
 *
 * There is no single common base to name: each category of the collection has its own root (in Geneva
 * the genomes bottom out at GGenome, the personality traits at GPersonalityTraits, and so on). The root
 * is the type parameter of the Gem::Common::GCommonInterfaceT<Root> base, which is exactly the
 * element_type of the std::shared_ptr returned by the public (inherited) clone() method.
 */
template <typename T>
using category_root_t = typename decltype(std::declval<const T &>().clone())::element_type;

/******************************************************************************/
/**
 * @brief The standard tests that every reflective value type should pass.
 *
 * Construction, copy construction, cloning (to the category root and to T), loading (through a pointer
 * and a reference), copy assignment and (de-)serialization round-trips in both modes (GEM_BINARY,
 * GEM_JSON), through the object's own functions and through the Gem::Common helpers. The round-trips
 * run only when the object could be changed (see modifiable_for_test).
 *
 * @tparam T The tested type
 */
template <typename T>
void StandardTests_no_failure_expected() {
    // The test cases are generated from type lists and named by index; name the type in every failure.
    INFO("Tested type: " << typeid(T).name());
    using root_t = category_root_t<T>;
    GEqualityPrinter gep(
        "StandardTests_no_failure_expected",
        std::pow(10, -7),
        Gem::Common::CE_WITH_MESSAGES
    );

    //---------------------------------------------------------------------------
    // Tests of construction, loading, cloning, ...

    { // Default construction and copy construction
        std::shared_ptr<T> T_ptr, T_ptr_cp;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);

        // Make sure the object is not in pristine condition
        REQUIRE_NOTHROW(modifyForTestIfAvailable(*T_ptr));

        REQUIRE_NOTHROW(T_ptr_cp = std::make_shared<T>(*T_ptr));

        CHECK(gep.isEqual(*T_ptr_cp, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_cp, *T_ptr));

        CHECK(T_ptr.use_count() == 1);
        CHECK(T_ptr_cp.use_count() == 1);

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_cp.reset());
    }

    { // Cloning to the category root
        std::shared_ptr<root_t> T_ptr, T_ptr_clone;

        // The change runs through T: modifyForTest() is not virtual, so only T's own function changes
        // T's own members.
        {
            std::shared_ptr<T> T_derived;
            REQUIRE_NOTHROW(T_derived = TFactory_GUnitTests<T>());
            REQUIRE(T_derived);
            REQUIRE_NOTHROW(modifyForTestIfAvailable(*T_derived));
            T_ptr = T_derived; // the sole remaining owner once T_derived goes out of scope
        }
        REQUIRE(T_ptr);

        REQUIRE_NOTHROW(T_ptr_clone = T_ptr->clone());

        CHECK(gep.isEqual(*T_ptr_clone, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_clone, *T_ptr));

        CHECK(T_ptr.use_count() == 1);
        CHECK(T_ptr_clone.use_count() == 1);

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_clone.reset());
    }

    { // Cloning to a target type
        std::shared_ptr<T> T_ptr, T_ptr_clone;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);
        REQUIRE_NOTHROW(modifyForTestIfAvailable(*T_ptr));

        REQUIRE_NOTHROW(T_ptr_clone = T_ptr->template clone<T>());

        CHECK(gep.isEqual(*T_ptr_clone, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_clone, *T_ptr));

        CHECK(T_ptr.use_count() == 1);
        CHECK(T_ptr_clone.use_count() == 1);

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_clone.reset());
    }

    { // Loading through a std::shared_ptr
        std::shared_ptr<T> T_ptr, T_ptr_load;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);
        REQUIRE_NOTHROW(modifyForTestIfAvailable(*T_ptr));

        REQUIRE_NOTHROW(T_ptr_load = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr_load);
        REQUIRE_NOTHROW(T_ptr_load->load(T_ptr));

        CHECK(gep.isEqual(*T_ptr_load, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_load, *T_ptr));

        CHECK(T_ptr.use_count() == 1);
        CHECK(T_ptr_load.use_count() == 1);

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_load.reset());
    }

    { // Loading through a reference
        std::shared_ptr<T> T_ptr, T_ptr_load;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);
        REQUIRE_NOTHROW(modifyForTestIfAvailable(*T_ptr));

        REQUIRE_NOTHROW(T_ptr_load = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr_load);
        REQUIRE_NOTHROW(T_ptr_load->load(*T_ptr));

        CHECK(gep.isEqual(*T_ptr_load, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_load, *T_ptr));

        CHECK(T_ptr.use_count() == 1);
        CHECK(T_ptr_load.use_count() == 1);

        REQUIRE_NOTHROW(T_ptr.reset());
        REQUIRE_NOTHROW(T_ptr_load.reset());
    }

    // Copy assignment (operator=). A copy assignment written by hand restates the member list beside
    // localMembers_(); comparing the assigned object with its source is what catches a member the
    // assignment forgot. A type without a copy assignment has nothing here to drift.
    if constexpr(std::is_copy_assignable_v<T>) {
        std::shared_ptr<T> T_ptr, T_ptr_assign;

        REQUIRE_NOTHROW(T_ptr = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr);
        REQUIRE_NOTHROW(modifyForTestIfAvailable(*T_ptr));

        REQUIRE_NOTHROW(T_ptr_assign = TFactory_GUnitTests<T>());
        REQUIRE(T_ptr_assign);
        REQUIRE_NOTHROW(*T_ptr_assign = *T_ptr);

        CHECK(gep.isEqual(*T_ptr_assign, *T_ptr));
        CHECK(gep.isSimilar(*T_ptr_assign, *T_ptr));
    }

    //---------------------------------------------------------------------------
    // (De-)serialization in both modes, through the object's own functions and through the
    // Gem::Common helpers (which courtier uses). Each needs a changed object: an unchanged one equals a
    // fresh one, so a round-trip that loses everything would pass.

    for(auto const mode :
        {Gem::Common::serializationMode::GEM_BINARY, Gem::Common::serializationMode::GEM_JSON}) {
        { // The object's own functions
            std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
            REQUIRE(T_ptr1);
            std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
            REQUIRE(T_ptr2);

            if(modifyForTestIfAvailable(*T_ptr1)) {
                CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));

                REQUIRE_NOTHROW(T_ptr2->fromString(T_ptr1->toString(mode), mode));
                CHECK(gep.isSimilar(*T_ptr1, *T_ptr2));
            }
            else {
                std::cout << "Internal (de-)serialization test for object with name "
                          << typeid(T).name()
                          << " not run because the object could not be changed / mode "
                          << static_cast<int>(mode) << '\n';
            }
        }

        { // The Gem::Common helpers
            std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
            REQUIRE(T_ptr1);
            std::shared_ptr<T> T_ptr2 = TFactory_GUnitTests<T>();
            REQUIRE(T_ptr2);

            if(modifyForTestIfAvailable(*T_ptr1)) {
                CHECK(not gep.isEqual(*T_ptr1, *T_ptr2));

                std::string const serialized_object = Gem::Common::sharedPtrToString(T_ptr1, mode);
                T_ptr2 = Gem::Common::sharedPtrFromString<T>(serialized_object, mode);
                REQUIRE(T_ptr2);
                CHECK(gep.isSimilar(*T_ptr1, *T_ptr2));
            }
            else {
                std::cout << "External (de-)serialization test for object with name "
                          << typeid(T).name()
                          << " not run because the object could not be changed / mode "
                          << static_cast<int>(mode) << '\n';
            }
        }
    }
}

/******************************************************************************/
/**
 * @brief The standard tests that should lead to a failure for every reflective value type. Most
 * notably, loading an object from itself throws in DEBUG mode.
 *
 * @tparam T The tested type
 */
template <typename T>
void StandardTests_failures_expected() {
    INFO("Tested type: " << typeid(T).name());

    std::shared_ptr<T> T_ptr1 = TFactory_GUnitTests<T>();
    REQUIRE(T_ptr1);
#ifdef DEBUG
    CHECK_THROWS_AS(T_ptr1->load(T_ptr1), gem_exception);
#endif
}

/******************************************************************************/

} /* namespace Gem::Common::Testing */

/******************************************************************************/
