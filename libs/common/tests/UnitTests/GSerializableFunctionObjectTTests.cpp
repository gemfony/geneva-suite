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
#include "common/GSerializableFunctionObjectT.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// Concrete function-object subclass over a tiny "processable" type.

namespace {

struct Item {
    int value{0};
};

// Doubles the carried value and reports success. Used to exercise operator()
// and the load/compare/clone surface inherited from the base.
class Doubler : public GSerializableFunctionObjectT<Item> {
public:
    Doubler() = default;
    explicit Doubler(int marker) : marker_(marker) {}

    [[nodiscard]] int marker() const { return marker_; }

protected:
    bool process_(Item &p) override {
        p.value *= 2;
        return true;
    }

private:
    [[nodiscard]] Doubler *clone_() const override { return new Doubler(*this); }

    int marker_{0};
};

// A function object that always reports failure — used to verify the
// operator()'s boolean return.
class AlwaysFail : public GSerializableFunctionObjectT<Item> {
protected:
    bool process_([[maybe_unused]] Item & p) override { return false; }

private:
    [[nodiscard]] AlwaysFail *clone_() const override { return new AlwaysFail(*this); }
};

} // namespace

// ---------------------------------------------------------------------------
// operator() dispatches to the override.

TEST_CASE("GSerializableFunctionObjectT::operator(): calls the derived process_",
          "[common][serializable-fobj]") {
    Doubler d;
    Item i{5};
    CHECK(d(i) == true);
    CHECK(i.value == 10);
}

TEST_CASE("GSerializableFunctionObjectT::operator(): returns false when process_ does",
          "[common][serializable-fobj]") {
    AlwaysFail af;
    Item i{1};
    CHECK_FALSE(af(i));
    CHECK(i.value == 1);   // not mutated
}

// ---------------------------------------------------------------------------
// clone() through the base interface.

TEST_CASE("GSerializableFunctionObjectT::clone: returns a fresh derived instance",
          "[common][serializable-fobj]") {
    Doubler src(42);
    auto cp = src.clone<Doubler>();
    REQUIRE(cp);
    CHECK(cp.get() != &src);
    CHECK(cp->marker() == 42);
}

// ---------------------------------------------------------------------------
// compare() — two empty function-object instances should be equal.

TEST_CASE("GSerializableFunctionObjectT::compare: equal objects + EQUALITY passes",
          "[common][serializable-fobj]") {
    Doubler const a;
    Doubler const b;
    CHECK_NOTHROW(a.compare(b, expectation::EQUALITY, 0.));
}

TEST_CASE("GSerializableFunctionObjectT::compare: equal objects + INEQUALITY violates",
          "[common][serializable-fobj]") {
    // The base "has no local data" branch documents that INEQUALITY can
    // never be met between two empty function objects of the same type.
    Doubler const a;
    Doubler const b;
    CHECK_THROWS_AS(a.compare(b, expectation::INEQUALITY, 0.), g_expectation_violation);
}

// ---------------------------------------------------------------------------
// name() — inherits the base name override.

TEST_CASE("GSerializableFunctionObjectT::name: non-empty",
          "[common][serializable-fobj]") {
    Doubler const d;
    CHECK_FALSE(d.name().empty());
}

