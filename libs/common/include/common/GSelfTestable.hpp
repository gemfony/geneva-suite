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

// Boost header files go here

// Geneva header files go here

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief The opt-in facet through which a class contributes its own unit tests.
 *
 * A class that has self-tests to offer inherits this interface **unconditionally** and
 * overrides the hooks it actually implements; a class that has none simply does not inherit
 * it. The three hooks are the ones Geneva's standard-test harnesses drive:
 *
 * - `modify_GUnitTests()` perturbs the object so that the generic clone / copy /
 *   (de-)serialization round-trip tests can check for inequality. Returning `false` tells the
 *   harness that the object could not be modified, and the round-trip block is skipped.
 * - `specificTestsNoFailureExpected_GUnitTests()` runs the class's own checks that must pass.
 * - `specificTestsFailuresExpected_GUnitTests()` runs the class's own checks that must throw.
 *
 * @par Why this is a separate interface, not part of GCommonInterfaceT
 * These hooks used to sit on `GCommonInterfaceT`, the universal reflective-value base, which
 * gave that class two unrelated meanings and put three test-only slots into the vtable of
 * *every* Geneva object -- including user-written individuals compiled into runtime-loadable
 * modules, which are built with `-UGEM_TESTING` and can never use them. Worse, the arrangement
 * was only ABI-safe because the hooks were *declared* unconditionally: a preprocessor-gated
 * variant would have made the core's and the module's vtables disagree and broken `dlopen`.
 * Splitting the facet off replaces that discipline-based safety argument with a structural one
 * -- `GSelfTestable` has no conditional members at all, and a class that does not opt in has
 * no test-related slots to disagree about.
 *
 * @par Bodies stay GEM_TESTING-conditional
 * Inheriting this interface is unconditional; *implementing* a hook is not. An override's body
 * is written inside `#ifdef GEM_TESTING`, with the non-testing branch calling
 * `Gem::Common::condnotset()`. That keeps Catch2 out of production builds (in particular out of
 * loadable modules) while the class layout stays identical in both configurations.
 *
 * @par Hierarchy roots do not inherit this interface
 * Category roots (`GGenome`, `GPersonalityTraits`, `GOptimizationAlgorithmBase`, ...) and CRTP
 * container mixins deliberately do **not** derive from `GSelfTestable`; otherwise every derived
 * user class would carry the slots again and nothing would be gained. Their test bodies are
 * protected, non-virtual helpers instead, and an opted-in derivative chains to them by qualified
 * call (`GGenome::modify_GUnitTests_()`).
 *
 * @par Polymorphic use
 * Code that walks a heterogeneous collection tests an element with
 * `if(auto *st = dynamic_cast<GSelfTestable *>(ptr); st != nullptr) { st->modify_GUnitTests(); }`.
 * An element without the facet is silently skipped -- the same outcome the old no-op default
 * produced.
 */
class GSelfTestable {
public:
    /***************************************************************************/
    /**
     * @brief Applies modifications to this object. This is needed for testing purposes
     *
     * @return A boolean indicating whether modifications were actually made
     */
    bool modify_GUnitTests() { return this->modify_GUnitTests_(); }

    /***************************************************************************/
    /** @brief Performs self tests that are expected to succeed. This is needed for testing purposes */
    void specificTestsNoFailureExpected_GUnitTests() {
        this->specificTestsNoFailureExpected_GUnitTests_();
    }

    /***************************************************************************/
    /** @brief Performs self tests that are expected to fail. This is needed for testing purposes */
    void specificTestsFailuresExpected_GUnitTests() {
        this->specificTestsFailuresExpected_GUnitTests_();
    }

protected:
    /***************************************************************************/
    // Defaulted special members. The destructor is protected and NON-virtual on purpose: this
    // interface is a facet, never an owner -- nothing is ever deleted through a GSelfTestable
    // pointer, and `protected` makes that impossible from the outside. The class is still
    // polymorphic (it has virtual hooks), so dynamic_cast to it works.

    GSelfTestable() = default;
    GSelfTestable(GSelfTestable const &) = default;
    GSelfTestable(GSelfTestable &&) = default;
    ~GSelfTestable() = default;

    GSelfTestable &operator=(GSelfTestable const &) = default;
    GSelfTestable &operator=(GSelfTestable &&) = default;

    /***************************************************************************/
    /**
     * @brief Applies modifications to this object. This is needed for testing purposes
     *
     * The default is a no-op rather than a throwing stub, because the two say different things:
     * "this class adds no modification of its own" is a legitimate state, whereas "a test hook
     * was called in a non-testing build" (what `condnotset()` reports from a guarded body) is a
     * harness error.
     *
     * @return A boolean indicating whether modifications were actually made
     */
    virtual bool modify_GUnitTests_() {
        return false; // no local modifications
    }

    /***************************************************************************/
    /** @brief Performs self tests that are expected to succeed. This is needed for testing purposes */
    virtual void specificTestsNoFailureExpected_GUnitTests_() {
        // no local tests
    }

    /***************************************************************************/
    /** @brief Performs self tests that are expected to fail. This is needed for testing purposes */
    virtual void specificTestsFailuresExpected_GUnitTests_() {
        // no local tests
    }
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */
