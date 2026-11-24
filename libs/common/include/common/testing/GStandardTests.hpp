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
#include <utility>
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
std::shared_ptr<T>
TFactory_GUnitTests() { // NOLINT(readability-identifier-naming): established name
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
        throw gem_exception(g_error_streamer() << "Caught unknown exception" << '\n');
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
    GEqualityPrinter(GEqualityPrinter &&) = delete;
    GEqualityPrinter &operator=(const GEqualityPrinter &) = delete;
    GEqualityPrinter &operator=(GEqualityPrinter &&) = delete;
    ~GEqualityPrinter() = default;

    /***************************************************************************/
    /**
     * @brief The only constructor.
     *
     * @param caller The name of the calling entity
     * @param limit Used in floating point comparisons to check similarity
     * @param emit_messages Determines whether messages should be emitted upon error
     */
    GEqualityPrinter(std::string caller, double limit, bool emit_messages)
      : caller_(std::move(caller))
      , limit_(limit)
      , emit_messages_(emit_messages) { /* nothing */
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
    [[nodiscard]] bool isEqual(const gem_type &x, const gem_type &y) const {
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
    [[nodiscard]] bool isEqual(
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
    [[nodiscard]] bool isSimilar(const gem_type &x, const gem_type &y) const {
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
    [[nodiscard]] bool isSimilar(
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
concept ModifiableForTest = requires(T &t) {
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
    if constexpr(ModifiableForTest<T>) {
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
using category_root_t = decltype(std::declval<const T &>().clone())::element_type;

/******************************************************************************/
/**
 * @brief The steps of the standard tests, one function each. A test program calls
 * StandardTests_no_failure_expected() / StandardTests_failures_expected() below; the steps are
 * public so that a program can run one of them on its own.
 */

/**
 * @brief Creates an object for the standard tests and changes it, if its type allows it.
 * @return The (changed) object
 */
template <typename T>
std::shared_ptr<T> makeChangedForTest() {
    std::shared_ptr<T> t_ptr;
    REQUIRE_NOTHROW(t_ptr = TFactory_GUnitTests<T>());
    REQUIRE(t_ptr);
    REQUIRE_NOTHROW(modifyForTestIfAvailable(*t_ptr));
    return t_ptr;
}

/**
 * @brief Checks that a copy equals its original and is its only owner's object.
 * @param gep The comparator
 * @param copy The copy (a clone, a loaded or an assigned object)
 * @param original The object it was made from
 */
template <typename root_type>
void checkCopyOf(
    GEqualityPrinter const &gep,
    std::shared_ptr<root_type> const &copy,
    std::shared_ptr<root_type> const &original
) {
    CHECK(gep.isEqual(*copy, *original));
    CHECK(gep.isSimilar(*copy, *original));
    CHECK(copy.use_count() == 1);
    CHECK(original.use_count() == 1);
}

/** @brief Copy construction gives an equal object. @param gep The comparator */
template <typename T>
void testCopyConstruction(GEqualityPrinter const &gep) {
    std::shared_ptr<T> const original = makeChangedForTest<T>();
    std::shared_ptr<T> copy;
    REQUIRE_NOTHROW(copy = std::make_shared<T>(*original));
    checkCopyOf(gep, copy, original);
}

/**
 * @brief Cloning through the category root gives an equal object. The change runs through T before
 * the upcast: modifyForTest() is not virtual, so only T's own function changes T's own members.
 * @param gep The comparator
 */
template <typename T>
void testCloneToCategoryRoot(GEqualityPrinter const &gep) {
    std::shared_ptr<category_root_t<T>> const original = makeChangedForTest<T>();
    std::shared_ptr<category_root_t<T>> clone;
    REQUIRE_NOTHROW(clone = original->clone());
    checkCopyOf(gep, clone, original);
}

/** @brief Cloning to T gives an equal object. @param gep The comparator */
template <typename T>
void testCloneToTarget(GEqualityPrinter const &gep) {
    std::shared_ptr<T> const original = makeChangedForTest<T>();
    std::shared_ptr<T> clone;
    REQUIRE_NOTHROW(clone = original->template clone<T>());
    checkCopyOf(gep, clone, original);
}

/** @brief Loading through a std::shared_ptr gives an equal object. @param gep The comparator */
template <typename T>
void testLoadThroughPointer(GEqualityPrinter const &gep) {
    std::shared_ptr<T> const original = makeChangedForTest<T>();
    std::shared_ptr<T> const loaded = TFactory_GUnitTests<T>();
    REQUIRE(loaded);
    REQUIRE_NOTHROW(loaded->load(original));
    checkCopyOf(gep, loaded, original);
}

/** @brief Loading through a reference gives an equal object. @param gep The comparator */
template <typename T>
void testLoadThroughReference(GEqualityPrinter const &gep) {
    std::shared_ptr<T> const original = makeChangedForTest<T>();
    std::shared_ptr<T> const loaded = TFactory_GUnitTests<T>();
    REQUIRE(loaded);
    REQUIRE_NOTHROW(loaded->load(*original));
    checkCopyOf(gep, loaded, original);
}

/**
 * @brief Copy assignment gives an equal object. A copy assignment written by hand restates the member
 * list beside localMembers_(); comparing the assigned object with its source catches a member the
 * assignment forgot. A type without a copy assignment has nothing here to drift.
 * @param gep The comparator
 */
template <typename T>
void testCopyAssignment(GEqualityPrinter const &gep) {
    if constexpr(std::is_copy_assignable_v<T>) {
        std::shared_ptr<T> const original = makeChangedForTest<T>();
        std::shared_ptr<T> const assigned = TFactory_GUnitTests<T>();
        REQUIRE(assigned);
        REQUIRE_NOTHROW(*assigned = *original);
        checkCopyOf(gep, assigned, original);
    }
}

/**
 * @brief Prints why a round-trip was not run.
 * @param way "Internal" (the object's own functions) or "External" (the Gem::Common helpers)
 * @param mode The serialization mode
 */
template <typename T>
void reportSkippedRoundTrip(std::string const &way, Gem::Common::serializationMode mode) {
    std::cout << way << " (de-)serialization test for object with name " << typeid(T).name()
              << " not run because the object could not be changed / mode "
              << static_cast<int>(mode) << '\n';
}

/**
 * @brief A changed object survives a round-trip through its own toString() / fromString(). It needs a
 * changed object: an unchanged one equals a fresh one, so a round-trip that loses everything would pass.
 * @param gep The comparator
 * @param mode The serialization mode
 */
template <typename T>
void testOwnRoundTrip(GEqualityPrinter const &gep, Gem::Common::serializationMode mode) {
    std::shared_ptr<T> const original = TFactory_GUnitTests<T>();
    REQUIRE(original);
    std::shared_ptr<T> const restored = TFactory_GUnitTests<T>();
    REQUIRE(restored);

    if(not modifyForTestIfAvailable(*original)) {
        reportSkippedRoundTrip<T>("Internal", mode);
        return;
    }
    CHECK(not gep.isEqual(*original, *restored));
    REQUIRE_NOTHROW(restored->fromString(original->toString(mode), mode));
    CHECK(gep.isSimilar(*original, *restored));
}

/**
 * @brief A changed object survives a round-trip through the Gem::Common helpers (which courtier uses).
 * @param gep The comparator
 * @param mode The serialization mode
 */
template <typename T>
void testHelperRoundTrip(GEqualityPrinter const &gep, Gem::Common::serializationMode mode) {
    std::shared_ptr<T> const original = TFactory_GUnitTests<T>();
    REQUIRE(original);
    std::shared_ptr<T> const fresh = TFactory_GUnitTests<T>();
    REQUIRE(fresh);

    if(not modifyForTestIfAvailable(*original)) {
        reportSkippedRoundTrip<T>("External", mode);
        return;
    }
    CHECK(not gep.isEqual(*original, *fresh));
    std::string const serialized_object = Gem::Common::sharedPtrToString(original, mode);
    std::shared_ptr<T> const restored =
        Gem::Common::sharedPtrFromString<T>(serialized_object, mode);
    REQUIRE(restored);
    CHECK(gep.isSimilar(*original, *restored));
}

/******************************************************************************/
/**
 * @brief The standard tests that every reflective value type should pass.
 *
 * Construction, copy construction, cloning (to the category root and to T), loading (through a pointer
 * and a reference), copy assignment and (de-)serialization round-trips in both modes (GEM_BINARY,
 * GEM_JSON), through the object's own functions and through the Gem::Common helpers. The round-trips
 * run only when the object could be changed (see ModifiableForTest).
 *
 * @tparam T The tested type
 */
template <typename T>
void StandardTests_no_failure_expected() { // NOLINT(readability-identifier-naming): established name
    // The test cases are generated from type lists and named by index; name the type in every failure.
    INFO("Tested type: " << typeid(T).name());
    GEqualityPrinter const gep(
        "StandardTests_no_failure_expected",
        std::pow(10, -7),
        Gem::Common::CE_WITH_MESSAGES
    );

    testCopyConstruction<T>(gep);
    testCloneToCategoryRoot<T>(gep);
    testCloneToTarget<T>(gep);
    testLoadThroughPointer<T>(gep);
    testLoadThroughReference<T>(gep);
    testCopyAssignment<T>(gep);

    for(auto const mode :
        {Gem::Common::serializationMode::GEM_BINARY, Gem::Common::serializationMode::GEM_JSON}) {
        testOwnRoundTrip<T>(gep, mode);
        testHelperRoundTrip<T>(gep, mode);
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
void StandardTests_failures_expected() { // NOLINT(readability-identifier-naming): established name
    INFO("Tested type: " << typeid(T).name());

    std::shared_ptr<T> const t_ptr = TFactory_GUnitTests<T>();
    REQUIRE(t_ptr);
#ifdef DEBUG
    CHECK_THROWS_AS(t_ptr->load(t_ptr), gem_exception);
#endif
}

/******************************************************************************/

} /* namespace Gem::Common::Testing */

/******************************************************************************/
