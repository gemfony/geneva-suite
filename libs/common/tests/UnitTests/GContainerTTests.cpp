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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <deque>
#include <list>
#include <memory>
#include <ranges>
#include <sstream>
#include <type_traits>
#include <vector>


#include "common/GContainerT.hpp"
#include "common/GArchiveNamed.hpp" // archive_named
#include "weft/GBinaryArchive.hpp"
#include "weft/GJsonArchive.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"

/******************************************************************************/
// File-scope serializable element type for GPtrContainerT serialization tests.
// Must be at file scope (not anonymous namespace): the GArchive smart-pointer arm
// tracking uses external linkage. SerBase is non-polymorphic (no virtual
// destructor), so shared_ptr<SerBase> needs no polymorphic-type registration.

struct SerBase : Gem::Common::gemfony_common_interface_indicator {
    int v = 0;
    SerBase() = default;
    explicit SerBase(int x) : v(x) {}
    ~SerBase() = default;

    template <typename TargetType = SerBase>
    std::shared_ptr<TargetType> clone() const {
        return std::make_shared<TargetType>(*static_cast<const TargetType *>(this));
    }

    void load(const std::shared_ptr<SerBase>& cp) { v = cp->v; }

    bool operator==(const SerBase &o) const { return v == o.v; }

    void compare(const SerBase &cp, Gem::Common::expectation e, [[maybe_unused]] double limit) const {
        if((e == Gem::Common::expectation::EQUALITY ||
            e == Gem::Common::expectation::FP_SIMILARITY) && v != cp.v) {
            throw g_expectation_violation("SerBase: values differ");
        }
        if(e == Gem::Common::expectation::INEQUALITY && v == cp.v) {
            throw g_expectation_violation("SerBase: values equal");
        }
    }

    template <class Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named(ar, "v", v);
    }
};

/******************************************************************************/
// Anonymous-namespace helpers

namespace {

/******************************************************************************/
// Minimal test class hierarchy for SharedPtrStorage tests

struct TestBase : Gem::Common::gemfony_common_interface_indicator {
    int val = 0;

    TestBase() = default;
    explicit TestBase(int v)
      : val(v) {}
    virtual ~TestBase() = default;

    // clone(), as a real GCommonInterfaceT type provides; virtual so it clones the dynamic type
    // (used by the UniquePtrStorage deep-copy path).
    [[nodiscard]] virtual std::unique_ptr<TestBase> clone() const {
        return std::make_unique<TestBase>(val);
    }

    // Typed counterpart, mirroring GCommonInterfaceT::clone<clone_type>(): clones the dynamic
    // type (via the virtual overload above) and hands back a unique_ptr to the requested static type.
    template <typename TargetType>
    [[nodiscard]] std::unique_ptr<TargetType> clone() const {
        std::unique_ptr<TestBase> base = this->clone();
        auto *converted = dynamic_cast<TargetType *>(base.get());
        if(converted == nullptr) {
            throw std::runtime_error("TestBase::clone<TargetType>(): dynamic_cast failed");
        }
        base.release();
        return std::unique_ptr<TargetType>(converted);
    }

    void load(const std::shared_ptr<TestBase>& cp) {
        val = cp->val;
    }

    // load-in-place from a borrow, as GCommonInterfaceT::load(const load_type&) provides.
    void load(const TestBase &cp) {
        val = cp.val;
    }

    virtual bool operator==(const TestBase &o) const {
        return val == o.val;
    }

    // Required by Gem::Common::compare_t for shared_ptr containers
    void compare(
        const TestBase &cp,
        Gem::Common::expectation e,
        [[maybe_unused]] double limit
    ) const {
        if(e == Gem::Common::expectation::EQUALITY || e == Gem::Common::expectation::FP_SIMILARITY) {
            if(val != cp.val) {
                throw g_expectation_violation("TestBase::compare: values differ");
            }
        }
        else if(e == Gem::Common::expectation::INEQUALITY) {
            if(val == cp.val) {
                throw g_expectation_violation("TestBase::compare: values are equal");
            }
        }
    }
};

struct TestDerived : TestBase {
    int derivedVal = 0;

    TestDerived() = default;
    explicit TestDerived(int v, int dv)
      : TestBase(v)
      , derivedVal(dv) {}

    bool operator==(const TestBase &o) const override {
        const auto *od = dynamic_cast<const TestDerived *>(&o);
        return od && TestBase::operator==(o) && derivedVal == od->derivedVal;
    }

    [[nodiscard]] std::unique_ptr<TestBase> clone() const override {
        return std::make_unique<TestDerived>(val, derivedVal);
    }
};

/******************************************************************************/
// Concrete GPodContainerT subclass for tests (GContainerT is abstract)

class ConcretePodVec : public Gem::Common::GPodContainerT<int> {
public:
    ConcretePodVec() = default;
    explicit ConcretePodVec(std::size_t n, int v = 0)
      : Gem::Common::GPodContainerT<int>(n, v) {}
    ~ConcretePodVec() override = default;
};

class ConcretePodVecDouble : public Gem::Common::GPodContainerT<double> {
public:
    ConcretePodVecDouble() = default;
    explicit ConcretePodVecDouble(std::size_t n, double v = 0.0)
      : Gem::Common::GPodContainerT<double>(n, v) {}
    ~ConcretePodVecDouble() override = default;
};

class ConcretePodDeque
    : public Gem::Common::GContainerT<int, Gem::Common::PodStorage<int, std::deque<int>>> {
public:
    using Base = Gem::Common::GContainerT<int, Gem::Common::PodStorage<int, std::deque<int>>>;
    ConcretePodDeque() = default;
    ~ConcretePodDeque() override = default;
};

class ConcretePtrVec : public Gem::Common::GPtrContainerT<TestBase> {
public:
    ConcretePtrVec() = default;
    ~ConcretePtrVec() override = default;
};

// Concrete GUniquePtrContainerT subclass for the unique-container tests. The explicit
// (defaulted) special members keep the container movable -- declaring the destructor would
// otherwise suppress the implicit move operations.
class ConcreteUniquePtrVec : public Gem::Common::GUniquePtrContainerT<TestBase> {
public:
    ConcreteUniquePtrVec() = default;
    ConcreteUniquePtrVec(const ConcreteUniquePtrVec &) = default;
    ConcreteUniquePtrVec(ConcreteUniquePtrVec &&) noexcept = default;
    ConcreteUniquePtrVec &operator=(const ConcreteUniquePtrVec &) = default;
    ConcreteUniquePtrVec &operator=(ConcreteUniquePtrVec &&) noexcept = default;
    ~ConcreteUniquePtrVec() override = default;
};

// std::list-backed POD container
class ConcretePodList
    : public Gem::Common::GContainerT<int, Gem::Common::PodStorage<int, std::list<int>>> {
public:
    ConcretePodList() = default;
    ~ConcretePodList() override = default;
};

// GPtrContainerT backed by the serializable SerBase type
class ConcretePtrSerializable : public Gem::Common::GPtrContainerT<SerBase> {
public:
    ConcretePtrSerializable() = default;
    ~ConcretePtrSerializable() override = default;
};

/******************************************************************************/

} // namespace

/******************************************************************************/
// ─────────────────────────── [pod][vector] ────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- a flat catalogue of independent, self-contained per-operation micro-checks (each SECTION builds its own fixture); the length is coverage breadth over one backend's contract, not branching complexity, and splitting into standalone TEST_CASEs would only multiply the count with no shared setup to consolidate
TEST_CASE("GContainerT: GPodContainerT<int> with std::vector backend", "[GContainerT][pod][vector]") {
    SECTION("Default construction") {
        ConcretePodVec const c;
        CHECK(c.empty());
        CHECK(c.empty());
    }

    SECTION("Construction with count and value") {
        ConcretePodVec c(5, 42);
        REQUIRE(c.size() == 5u);
        for(std::size_t i = 0; i < 5; ++i) {
            CHECK(c[i] == 42);
        }
    }

    SECTION("Copy construction — values are equal, independent storage") {
        ConcretePodVec src(3, 7);
        ConcretePodVec dst(src);
        REQUIRE(dst.size() == 3u);
        // Independence check
        src[0] = 99;
        CHECK(dst[0] == 7);
    }

    SECTION("Move construction") {
        ConcretePodVec const src(4, 1);
        ConcretePodVec dst(std::move(src));
        CHECK(dst.size() == 4u);
        CHECK(dst[0] == 1);
    }

    SECTION("Copy assignment") {
        ConcretePodVec const src(3, 5);
        ConcretePodVec dst(2, 0);
        dst = src;
        REQUIRE(dst.size() == 3u);
        CHECK(dst[0] == 5);
    }

    SECTION("Move assignment") {
        ConcretePodVec const src(3, 5);
        ConcretePodVec dst;
        dst = std::move(src);
        CHECK(dst.size() == 3u);
    }

    SECTION("assign() — count + value") {
        ConcretePodVec c;
        c.assign(4u, 99);
        REQUIRE(c.size() == 4u);
        CHECK(c.front() == 99);
        CHECK(c.back() == 99);
    }

    SECTION("assign() — iterator range") {
        std::vector<int> src = {1, 2, 3, 4};
        ConcretePodVec c;
        c.assign(src.begin(), src.end());
        REQUIRE(c.size() == 4u);
        CHECK(c[2] == 3);
    }

    SECTION("assign() — initializer_list") {
        ConcretePodVec c;
        c.assign({10, 20, 30});
        REQUIRE(c.size() == 3u);
        CHECK(c[1] == 20);
    }

    SECTION("push_back / pop_back") {
        ConcretePodVec c;
        c.push_back(1);
        c.push_back(2);
        REQUIRE(c.size() == 2u);
        c.pop_back();
        REQUIRE(c.size() == 1u);
        CHECK(c.back() == 1);
    }

    SECTION("emplaceBack") {
        ConcretePodVec c;
        c.emplace_back(7);
        c.emplace_back(8);
        REQUIRE(c.size() == 2u);
        CHECK(c[0] == 7);
        CHECK(c[1] == 8);
    }

    SECTION("insert — single") {
        ConcretePodVec c;
        c.assign({1, 3});
        auto it = c.insert(c.begin() + 1, 2);
        REQUIRE(c.size() == 3u);
        CHECK(*it == 2);
        CHECK(c[1] == 2);
    }

    SECTION("insert — count") {
        ConcretePodVec c;
        c.assign({1, 5});
        c.insert(c.begin() + 1, 2u, 99);
        REQUIRE(c.size() == 4u);
        CHECK(c[1] == 99);
        CHECK(c[2] == 99);
    }

    SECTION("insert — range") {
        ConcretePodVec c;
        c.assign({0, 4});
        std::vector<int> extra = {1, 2, 3};
        c.insert(c.begin() + 1, extra.begin(), extra.end());
        REQUIRE(c.size() == 5u);
        CHECK(c[1] == 1);
        CHECK(c[3] == 3);
    }

    SECTION("insert — initializer_list") {
        ConcretePodVec c;
        c.assign({0, 4});
        c.insert(c.begin() + 1, {1, 2, 3});
        REQUIRE(c.size() == 5u);
        CHECK(c[2] == 2);
    }

    SECTION("emplace") {
        ConcretePodVec c;
        c.assign({1, 3});
        auto it = c.emplace(c.begin() + 1, 2);
        REQUIRE(c.size() == 3u);
        CHECK(*it == 2);
    }

    SECTION("erase — single element") {
        ConcretePodVec c;
        c.assign({1, 2, 3});
        auto it = c.erase(c.begin() + 1);
        REQUIRE(c.size() == 2u);
        CHECK(*it == 3);
        CHECK(c[0] == 1);
    }

    SECTION("erase — range") {
        ConcretePodVec c;
        c.assign({1, 2, 3, 4});
        c.erase(c.begin() + 1, c.begin() + 3);
        REQUIRE(c.size() == 2u);
        CHECK(c[1] == 4);
    }

    SECTION("at() throws on out-of-bounds") {
        ConcretePodVec c(2, 0);
        CHECK_THROWS_AS(c.at(5), std::out_of_range);
        CHECK_THROWS_AS(std::as_const(c).at(5), std::out_of_range);
    }

    SECTION("operator[] and front/back") {
        ConcretePodVec c;
        c.assign({10, 20, 30});
        CHECK(c[0] == 10);
        CHECK(c.front() == 10);
        CHECK(c.back() == 30);
    }

    SECTION("const operator[] and front/back") {
        const ConcretePodVec c(3, 5);
        CHECK(c[0] == 5);
        CHECK(c.front() == 5);
        CHECK(c.back() == 5);
    }

    SECTION("data() — available and correct type") {
        ConcretePodVec c(3, 7);
        int  const*ptr = c.data();
        REQUIRE(ptr != nullptr);
        CHECK(ptr[0] == 7);
        CHECK(ptr[2] == 7);
        // const version
        const auto &cc = c;
        const int *cptr = cc.data();
        CHECK(cptr[0] == 7);
    }

    SECTION("Iterators: begin/end/cbegin/cend/rbegin/rend/crbegin/crend") {
        ConcretePodVec c;
        c.assign({1, 2, 3});

        // forward
        int val = 1;
        for(auto it = c.begin(); it != c.end(); ++it, ++val) {
            CHECK(*it == val);
        }
        // const forward
        val = 1;
        for(auto it = c.cbegin(); it != c.cend(); ++it, ++val) {
            CHECK(*it == val);
        }
        // reverse
        val = 3;
        for(auto it = c.rbegin(); it != c.rend(); ++it, --val) {
            CHECK(*it == val);
        }
        // const reverse
        val = 3;
        for(auto it = c.crbegin(); it != c.crend(); ++it, --val) {
            CHECK(*it == val);
        }
    }

    SECTION("size / empty / maxSize / clear") {
        ConcretePodVec c(3, 1);
        CHECK(c.size() == 3u);
        CHECK_FALSE(c.empty());
        CHECK(c.max_size() > 0u);
        c.clear();
        CHECK(c.empty());
        CHECK(c.empty());
    }

    SECTION("capacity / reserve / shrinkToFit") {
        ConcretePodVec c;
        c.reserve(100u);
        CHECK(c.capacity() >= 100u);
        c.assign(5u, 0);
        c.shrink_to_fit();
        // After shrink capacity >= size
        CHECK(c.capacity() >= c.size());
    }

    SECTION("resize (no value)") {
        ConcretePodVec c(3, 5);
        c.resize(5u);
        REQUIRE(c.size() == 5u);
        CHECK(c[3] == 0); // zero-initialised
        c.resize(2u);
        REQUIRE(c.size() == 2u);
    }

    SECTION("resize with value") {
        ConcretePodVec c(2, 1);
        c.resize(4u, 9);
        REQUIRE(c.size() == 4u);
        CHECK(c[2] == 9);
        CHECK(c[3] == 9);
    }

    SECTION("count") {
        ConcretePodVec c;
        c.assign({1, 2, 2, 3});
        CHECK(c.count(2) == 2u);
        CHECK(c.count(5) == 0u);
    }

    SECTION("find") {
        ConcretePodVec c;
        c.assign({1, 2, 3});
        auto it = c.find(2);
        REQUIRE(it != c.end());
        CHECK(*it == 2);
        CHECK(c.find(99) == c.end());
    }

    SECTION("crossOver — equal length") {
        ConcretePodVec a;
        a.assign({1, 2, 3, 4});
        ConcretePodVec b;
        b.assign({5, 6, 7, 8});
        a.crossOver(b, 2);
        // a[0..1] unchanged, a[2..3] swapped with b[2..3]
        CHECK(a[0] == 1);
        CHECK(a[1] == 2);
        CHECK(a[2] == 7);
        CHECK(a[3] == 8);
        CHECK(b[2] == 3);
        CHECK(b[3] == 4);
    }

    SECTION("crossOver — different length") {
        ConcretePodVec a;
        a.assign({1, 2, 3, 4, 5});
        ConcretePodVec b;
        b.assign({6, 7});
        // minSize = 2, swap at pos 1: a[1]<->b[1]; a[2..4] move to b
        a.crossOver(b, 1);
        CHECK(a.size() == 2u);
        CHECK(b.size() == 5u);
        CHECK(a[1] == 7);
        CHECK(b[1] == 2);
        CHECK(b[2] == 3);
    }

    SECTION("compare_base — equality") {
        ConcretePodVec a;
        a.assign({1, 2, 3});
        ConcretePodVec b;
        b.assign({1, 2, 3});
        CHECK_NOTHROW(a.compare_base(b, Gem::Common::expectation::EQUALITY, 0.0));
    }

    SECTION("compare_base — inequality expectation met") {
        ConcretePodVec a;
        a.assign({1, 2});
        ConcretePodVec b;
        b.assign({3, 4});
        CHECK_NOTHROW(a.compare_base(b, Gem::Common::expectation::INEQUALITY, 0.0));
    }

    SECTION("compare_base — equality expectation violated throws") {
        ConcretePodVec a;
        a.assign({1, 2});
        ConcretePodVec b;
        b.assign({3, 4});
        CHECK_THROWS(a.compare_base(b, Gem::Common::expectation::EQUALITY, 0.0));
    }

    SECTION("getDataCopy") {
        ConcretePodVec c;
        c.assign({1, 2, 3});
        std::vector<int> copy;
        c.getDataCopy(copy);
        REQUIRE(copy.size() == 3u);
        CHECK(copy[0] == 1);
        CHECK(copy[2] == 3);
        // Independence
        copy[0] = 99;
        CHECK(c[0] == 1);
    }

    SECTION("operator<=> — less") {
        ConcretePodVec a;
        a.assign({1, 2});
        ConcretePodVec b;
        b.assign({1, 3});
        CHECK((a <=> b) < 0);
    }

    SECTION("operator<=> — equal") {
        ConcretePodVec a;
        a.assign({1, 2});
        ConcretePodVec b;
        b.assign({1, 2});
        CHECK((a <=> b) == 0);
    }

    SECTION("operator<=> — greater") {
        ConcretePodVec a;
        a.assign({2, 2});
        ConcretePodVec b;
        b.assign({1, 2});
        CHECK((a <=> b) > 0);
    }

    SECTION("std::ranges compatibility") {
        ConcretePodVec c;
        c.assign({1, 2, 3});
        // std::ranges::range satisfied
        auto rb = std::ranges::begin(c);
        auto re = std::ranges::end(c);
        CHECK(std::ranges::distance(rb, re) == 3);
        CHECK(std::ranges::size(c) == 3u);
    }

    SECTION("push_front/popFront NOT available — compile-time concept check") {
        // This is verified by checking HasFrontInsertion<std::vector<int>> == false
        CHECK_FALSE(Gem::Common::HasFrontInsertion<std::vector<int>>);
    }

    SECTION("copy self-assignment is safe") {
        ConcretePodVec c;
        c.assign({1, 2, 3});
        // The copy-assignment operator guards against self-assignment (if-branch)
        auto *self = &c;
        c = *self;
        REQUIRE(c.size() == 3u);
        CHECK(c[0] == 1);
        CHECK(c[2] == 3);
    }

    SECTION("crossOver — cp longer than this") {
        ConcretePodVec a;
        a.assign({1, 2});
        ConcretePodVec b;
        b.assign({6, 7, 8, 9, 10});
        // minSize=2, swap at pos 1: a[1]<->b[1]; b[2..4] move to a
        a.crossOver(b, 1);
        CHECK(a.size() == 5u);
        CHECK(b.size() == 2u);
        CHECK(a[1] == 7);
        CHECK(b[1] == 2);
        CHECK(a[2] == 8);
        CHECK(a[4] == 10);
    }
}

/******************************************************************************/
// ─────────────────────────── [pod][deque] ────────────────────────────────

TEST_CASE("GContainerT: GPodContainerT<int> with std::deque backend", "[GContainerT][pod][deque]") {
    SECTION("Default construction") {
        ConcretePodDeque const c;
        CHECK(c.empty());
    }

    SECTION("push_back and pop_back") {
        ConcretePodDeque c;
        c.push_back(1);
        c.push_back(2);
        CHECK(c.size() == 2u);
        c.pop_back();
        CHECK(c.size() == 1u);
        CHECK(c.back() == 1);
    }

    SECTION("push_front / emplaceFront / popFront — available") {
        ConcretePodDeque c;
        c.push_back(2);
        c.push_front(1);
        CHECK(c.front() == 1);
        CHECK(c.back() == 2);
        c.emplace_front(0);
        CHECK(c.front() == 0);
        CHECK(c.size() == 3u);
        c.pop_front();
        CHECK(c.front() == 1);
    }

    SECTION("data() NOT available — concept check") {
        CHECK_FALSE(Gem::Common::HasContiguousStorage<std::deque<int>>);
    }

    SECTION("capacity / reserve / shrinkToFit NOT available — concept check") {
        CHECK_FALSE(Gem::Common::HasCapacity<std::deque<int>>);
    }

    SECTION("crossOver — available (deque has random access)") {
        ConcretePodDeque a;
        a.assign({1, 2, 3, 4});
        ConcretePodDeque b;
        b.assign({5, 6, 7, 8});
        a.crossOver(b, 2);
        CHECK(a[2] == 7);
        CHECK(b[2] == 3);
    }

    SECTION("assign range") {
        ConcretePodDeque c;
        std::deque<int> src = {10, 20, 30};
        c.assign(src.begin(), src.end());
        REQUIRE(c.size() == 3u);
        CHECK(c[1] == 20);
    }

    SECTION("compare_base — equality") {
        ConcretePodDeque a;
        a.assign({1, 2, 3});
        ConcretePodDeque b;
        b.assign({1, 2, 3});
        CHECK_NOTHROW(a.compare_base(b, Gem::Common::expectation::EQUALITY, 0.0));
    }

    SECTION("compare_base — equality expectation violated throws") {
        ConcretePodDeque a;
        a.assign({1, 2});
        ConcretePodDeque b;
        b.assign({3, 4});
        CHECK_THROWS(a.compare_base(b, Gem::Common::expectation::EQUALITY, 0.0));
    }

    SECTION("crossOver — invalid position throws in debug build") {
        ConcretePodDeque a;
        a.assign({1, 2, 3});
        ConcretePodDeque b;
        b.assign({4, 5, 6});
#ifdef DEBUG
        CHECK_THROWS_AS(a.crossOver(b, 3), geneva_exception);
#else
        CHECK_NOTHROW(a.crossOver(b, 2));
#endif
    }

    SECTION("crossOver — this longer than cp") {
        ConcretePodDeque a;
        a.assign({1, 2, 3, 4, 5});
        ConcretePodDeque b;
        b.assign({6, 7});
        a.crossOver(b, 1);
        CHECK(a.size() == 2u);
        CHECK(b.size() == 5u);
        CHECK(a[1] == 7);
        CHECK(b[1] == 2);
        CHECK(b[2] == 3);
    }

    SECTION("crossOver — cp longer than this") {
        ConcretePodDeque a;
        a.assign({1, 2});
        ConcretePodDeque b;
        b.assign({6, 7, 8, 9});
        a.crossOver(b, 1);
        CHECK(a.size() == 4u);
        CHECK(b.size() == 2u);
        CHECK(a[1] == 7);
        CHECK(b[1] == 2);
        CHECK(a[2] == 8);
    }

    SECTION("GArchive round-trip via GContainerT::serialize()") {
        ConcretePodDeque src;
        src.assign({4, 5, 6});
        src[1] = 99;

        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePodDeque loaded;
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        REQUIRE(loaded.size() == 3u);
        CHECK(loaded[0] == 4);
        CHECK(loaded[1] == 99);
        CHECK(loaded[2] == 6);
    }
}

/******************************************************************************/
// ─────────────────────────── [ptr][vector] ────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- a flat catalogue of independent, self-contained per-operation micro-checks (each SECTION builds its own fixture); the length is coverage breadth over one backend's contract, not branching complexity, and splitting into standalone TEST_CASEs would only multiply the count with no shared setup to consolidate
TEST_CASE("GContainerT: GPtrContainerT<TestBase> with std::vector backend", "[GContainerT][ptr][vector]") {
    SECTION("Default construction") {
        ConcretePtrVec const c;
        CHECK(c.empty());
        CHECK(c.empty());
    }

    SECTION("Copy construction — deep clone") {
        ConcretePtrVec src;
        auto item = std::make_shared<TestBase>(42);
        src.push_back_noclone(item);
        ConcretePtrVec dst(src);
        REQUIRE(dst.size() == 1u);
        CHECK(dst[0]->val == 42);
        // Addresses must differ (deep copy)
        CHECK(dst[0].get() != item.get());
        // Modifying original does not affect dst
        item->val = 99;
        CHECK(dst[0]->val == 42);
    }

    SECTION("Move construction") {
        ConcretePtrVec src;
        src.push_back_noclone(std::make_shared<TestBase>(1));
        ConcretePtrVec const dst(std::move(src));
        CHECK(dst.size() == 1u);
    }

    SECTION("push_backClone — independence") {
        ConcretePtrVec c;
        auto item = std::make_shared<TestBase>(10);
        c.push_back_clone(item);
        item->val = 99; // Change after push
        CHECK(c[0]->val == 10); // Clone is unaffected
    }

    SECTION("push_backNoclone — shared ownership") {
        ConcretePtrVec c;
        auto item = std::make_shared<TestBase>(10);
        c.push_back_noclone(item);
        item->val = 99; // Change after push
        CHECK(c[0]->val == 99); // Reflects change (shared)
    }

    SECTION("push_backClone — null pointer throws") {
        ConcretePtrVec c;
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.push_back_clone(null), geneva_exception);
    }

    SECTION("push_backNoclone — null pointer throws") {
        ConcretePtrVec c;
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.push_back_noclone(null), geneva_exception);
    }

    SECTION("insertClone — single") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        c.push_back_noclone(std::make_shared<TestBase>(3));
        auto item = std::make_shared<TestBase>(2);
        c.insert_clone(c.begin() + 1, item);
        REQUIRE(c.size() == 3u);
        CHECK(c[1]->val == 2);
        // Clone independence
        item->val = 99;
        CHECK(c[1]->val == 2);
    }

    SECTION("insertClone — count") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(0));
        auto item = std::make_shared<TestBase>(5);
        c.insert_clone(c.begin() + 1, 3u, item);
        REQUIRE(c.size() == 4u);
        CHECK(c[1]->val == 5);
        CHECK(c[2]->val == 5);
        CHECK(c[3]->val == 5);
    }

    SECTION("insertClone — null pointer throws") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(0));
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.insert_clone(c.begin(), null), geneva_exception);
        CHECK_THROWS_AS(c.insert_clone(c.begin(), 2u, null), geneva_exception);
    }

    SECTION("insertNoclone — single") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        auto item = std::make_shared<TestBase>(2);
        c.insert_noclone(c.begin(), item);
        REQUIRE(c.size() == 2u);
        CHECK(c[0]->val == 2);
    }

    SECTION("insertNoclone single — null pointer throws") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.insert_noclone(c.begin(), null), geneva_exception);
    }

    SECTION("insertClone — count, mid-position contiguity") {
        // Verifies that the count-overload pre-builds the inserted range and
        // produces `count` independent clones contiguously at the insertion
        // position. Would fail if the implementation re-used a stale offset
        // and ended up shifting later clones outside the target window.
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(0));
        c.push_back_noclone(std::make_shared<TestBase>(9));
        auto item = std::make_shared<TestBase>(5);

        c.insert_clone(c.begin() + 1, 3u, item);

        REQUIRE(c.size() == 5u);
        CHECK(c[0]->val == 0);
        CHECK(c[1]->val == 5);
        CHECK(c[2]->val == 5);
        CHECK(c[3]->val == 5);
        CHECK(c[4]->val == 9);

        // No clone aliases the prototype; mutating the prototype must not
        // bleed into the container.
        for(std::size_t i = 1; i <= 3; ++i) {
            CHECK(c[i].get() != item.get());
        }
        item->val = 42;
        CHECK(c[1]->val == 5);
        CHECK(c[2]->val == 5);
        CHECK(c[3]->val == 5);
    }

    SECTION("insertClone — count == 0 is a no-op") {
        // Regression guard: a count of 0 must leave the container unchanged.
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        c.push_back_noclone(std::make_shared<TestBase>(2));
        auto item = std::make_shared<TestBase>(99);
        const std::size_t before = c.size();

        CHECK_NOTHROW(c.insert_clone(c.begin() + 1, std::size_t(0), item));

        CHECK(c.size() == before);
        CHECK(c[0]->val == 1);
        CHECK(c[1]->val == 2);
    }

    SECTION("insertNoclone — count, original at pos, clones after") {
        // Pins down the contract: the user-supplied `item` ends up at exactly
        // `pos`, followed by (count - 1) independent clones, with the prior
        // element shifted right by `count`.
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(0));
        c.push_back_noclone(std::make_shared<TestBase>(9));
        auto item = std::make_shared<TestBase>(7);

        c.insert_noclone(c.begin() + 1, 3u, item);

        REQUIRE(c.size() == 5u);
        // Same address as `item` appears exactly once, and at the insertion position.
        CHECK(c[1].get() == item.get());
        std::size_t same_addr = 0;
        for(auto & i : c) {
            if(i.get() == item.get()) ++same_addr;
        }
        CHECK(same_addr == 1u);
        // All inserted slots compare equal by value; the trailing original is preserved.
        CHECK(c[0]->val == 0);
        CHECK(c[1]->val == 7);
        CHECK(c[2]->val == 7);
        CHECK(c[3]->val == 7);
        CHECK(c[4]->val == 9);
    }

    SECTION("insertNoclone — count == 0 is a no-op") {
        // Regression guard for the previous `count - 1` unsigned underflow:
        // before the fix, this would loop indefinitely / exhaust memory.
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        c.push_back_noclone(std::make_shared<TestBase>(2));
        auto item = std::make_shared<TestBase>(99);
        const std::size_t before = c.size();

        CHECK_NOTHROW(c.insert_noclone(c.begin() + 1, std::size_t(0), item));

        CHECK(c.size() == before);
        CHECK(c[0]->val == 1);
        CHECK(c[1]->val == 2);
        // `item` must NOT have been inserted into the container.
        for(auto & i : c) {
            CHECK(i.get() != item.get());
        }
    }

    SECTION("insertNoclone count — null pointer throws") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.insert_noclone(c.begin(), 2u, null), geneva_exception);
    }

    SECTION("resizeClone") {
        ConcretePtrVec c;
        auto item = std::make_shared<TestBase>(7);
        c.resize_clone(3u, item);
        REQUIRE(c.size() == 3u);
        CHECK(c[0]->val == 7);
        CHECK(c[0].get() != item.get()); // Cloned
        c.resize_clone(1u, item);
        CHECK(c.size() == 1u);
    }

    SECTION("resizeClone — null pointer throws on grow") {
        ConcretePtrVec c;
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.resize_clone(3u, null), geneva_exception);
    }

    SECTION("resizeNoclone") {
        ConcretePtrVec c;
        auto item = std::make_shared<TestBase>(8);
        c.resize_noclone(3u, item);
        REQUIRE(c.size() == 3u);
        CHECK(c[0]->val == 8);
    }

    SECTION("resizeNoclone — shrink path") {
        ConcretePtrVec c;
        auto item = std::make_shared<TestBase>(8);
        c.resize_noclone(4u, item);
        REQUIRE(c.size() == 4u);
        // Now shrink: the dataCnt_.resize(amount) branch
        c.resize_noclone(2u, item);
        CHECK(c.size() == 2u);
        CHECK(c[0]->val == 8);
    }

    SECTION("resizeNoclone — null pointer throws on grow") {
        ConcretePtrVec c;
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.resize_noclone(3u, null), geneva_exception);
    }

    SECTION("resizeEmpty") {
        ConcretePtrVec c;
        c.resize_empty(4u);
        REQUIRE(c.size() == 4u);
        for(auto &p : c) {
            CHECK_FALSE(p); // All null
        }
        c.resize_empty(2u);
        CHECK(c.size() == 2u);
    }

    SECTION("cloneAt — returns independent clone") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(42));
        auto cloned = c.clone_at<TestBase>(0);
        REQUIRE(cloned);
        CHECK(cloned->val == 42);
        CHECK(cloned.get() != c[0].get()); // Different pointer
    }

    SECTION("count — value comparison") {
        ConcretePtrVec c;
        auto a = std::make_shared<TestBase>(5);
        auto b = std::make_shared<TestBase>(5);
        auto d = std::make_shared<TestBase>(9);
        c.push_back_noclone(a);
        c.push_back_noclone(b);
        c.push_back_noclone(d);
        auto ref = std::make_shared<TestBase>(5);
        CHECK(c.count<TestBase>(ref) == 2u);
    }

    SECTION("find — value comparison") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        c.push_back_noclone(std::make_shared<TestBase>(2));
        auto ref = std::make_shared<TestBase>(2);
        auto it = c.find<TestBase>(ref);
        REQUIRE(it != c.end());
        CHECK((*it)->val == 2);
    }

    SECTION("count — null pointer throws") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.count<TestBase>(null), geneva_exception);
    }

    SECTION("find — null pointer throws") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        std::shared_ptr<TestBase> const null;
        CHECK_THROWS_AS(c.find<TestBase>(null), geneva_exception);
    }

    SECTION("crossOver — equal length") {
        ConcretePtrVec a;
        a.push_back_noclone(std::make_shared<TestBase>(1));
        a.push_back_noclone(std::make_shared<TestBase>(2));
        a.push_back_noclone(std::make_shared<TestBase>(3));
        ConcretePtrVec b;
        b.push_back_noclone(std::make_shared<TestBase>(4));
        b.push_back_noclone(std::make_shared<TestBase>(5));
        b.push_back_noclone(std::make_shared<TestBase>(6));
        a.crossOver(b, 1);
        CHECK(a[0]->val == 1);
        CHECK(a[1]->val == 5);
        CHECK(b[1]->val == 2);
    }

    SECTION("crossOver — this longer than cp") {
        ConcretePtrVec a;
        a.push_back_noclone(std::make_shared<TestBase>(1));
        a.push_back_noclone(std::make_shared<TestBase>(2));
        a.push_back_noclone(std::make_shared<TestBase>(3));
        ConcretePtrVec b;
        b.push_back_noclone(std::make_shared<TestBase>(4));
        b.push_back_noclone(std::make_shared<TestBase>(5));
        a.crossOver(b, 1);
        // a[0] stays, a[1] swaps with b[1]; a[2] moves to b
        CHECK(a.size() == 2u);
        CHECK(b.size() == 3u);
        CHECK(a[1]->val == 5);
        CHECK(b[1]->val == 2);
        CHECK(b[2]->val == 3);
    }

    SECTION("crossOver — invalid position throws in debug build") {
        ConcretePtrVec a;
        a.push_back_noclone(std::make_shared<TestBase>(1));
        a.push_back_noclone(std::make_shared<TestBase>(2));
        ConcretePtrVec b;
        b.push_back_noclone(std::make_shared<TestBase>(4));
        b.push_back_noclone(std::make_shared<TestBase>(5));
#ifdef DEBUG
        CHECK_THROWS_AS(a.crossOver(b, 2), geneva_exception);
#else
        CHECK_NOTHROW(a.crossOver(b, 1));
#endif
    }

    SECTION("crossOver — cp longer than this") {
        ConcretePtrVec a;
        a.push_back_noclone(std::make_shared<TestBase>(1));
        a.push_back_noclone(std::make_shared<TestBase>(2));
        ConcretePtrVec b;
        b.push_back_noclone(std::make_shared<TestBase>(4));
        b.push_back_noclone(std::make_shared<TestBase>(5));
        b.push_back_noclone(std::make_shared<TestBase>(6));
        a.crossOver(b, 1);
        // a[0] stays, a[1] swaps with b[1]; b[2] moves to a
        CHECK(a.size() == 3u);
        CHECK(b.size() == 2u);
        CHECK(a[1]->val == 5);
        CHECK(b[1]->val == 2);
        CHECK(a[2]->val == 6);
    }

    SECTION("compare_base — equality") {
        ConcretePtrVec a;
        a.push_back_noclone(std::make_shared<TestBase>(1));
        ConcretePtrVec const b(a); // deep copy
        CHECK_NOTHROW(a.compare_base(b, Gem::Common::expectation::EQUALITY, 0.0));
    }

    SECTION("getDataCopy — independent clones") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(7));
        c.push_back_noclone(std::make_shared<TestBase>(8));
        std::vector<std::shared_ptr<TestBase>> copy;
        c.getDataCopy(copy);
        REQUIRE(copy.size() == 2u);
        CHECK(copy[0]->val == 7);
        CHECK(copy[0].get() != c[0].get()); // Independent clone
    }

    SECTION("attachViewTo — filters by DerivedType") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        auto d = std::make_shared<TestDerived>();
        d->val = 2;
        d->derivedVal = 10;
        c.push_back_noclone(d);
        c.push_back_noclone(std::make_shared<TestBase>(3));

        std::vector<std::shared_ptr<TestDerived>> view;
        c.attachViewTo<TestDerived>(view);
        REQUIRE(view.size() == 1u);
        CHECK(view[0]->derivedVal == 10);
    }

    SECTION("filteredView — range over derived type only") {
        ConcretePtrVec c;
        c.push_back_noclone(std::make_shared<TestBase>(1));
        auto d1 = std::make_shared<TestDerived>();
        d1->derivedVal = 42;
        c.push_back_noclone(d1);
        c.push_back_noclone(std::make_shared<TestBase>(3));
        auto d2 = std::make_shared<TestDerived>();
        d2->derivedVal = 99;
        c.push_back_noclone(d2);

        auto view = c.filteredView<TestDerived>();
        std::vector<int> derivedVals;
        for(auto ptr : view) {
            derivedVals.push_back(ptr->derivedVal);
        }
        REQUIRE(derivedVals.size() == 2u);
        CHECK(derivedVals[0] == 42);
        CHECK(derivedVals[1] == 99);
    }
}

/******************************************************************************/
// ─────────────────────────── [pod][list] ─────────────────────────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- a flat catalogue of independent, self-contained per-operation micro-checks (each SECTION builds its own fixture); the length is coverage breadth over one backend's contract, not branching complexity, and splitting into standalone TEST_CASEs would only multiply the count with no shared setup to consolidate
TEST_CASE("GContainerT: GPodContainerT<int> with std::list backend", "[GContainerT][pod][list]") {
    // std::list satisfies HasFrontInsertion but not HasRandomAccess,
    // HasContiguousStorage, or HasCapacity. crossOver and operator<=>
    // are therefore not available. Element access uses front()/back()
    // and std::next(begin(), n) rather than operator[].

    SECTION("Default construction") {
        ConcretePodList const c;
        CHECK(c.empty());
        CHECK(c.empty());
    }

    SECTION("push_back and pop_back") {
        ConcretePodList c;
        c.push_back(10);
        c.push_back(20);
        c.push_back(30);
        REQUIRE(c.size() == 3u);
        CHECK(c.back() == 30);
        c.pop_back();
        CHECK(c.size() == 2u);
        CHECK(c.back() == 20);
    }

    SECTION("push_front and pop_front — HasFrontInsertion satisfied") {
        ConcretePodList c;
        c.push_back(2);
        c.push_front(1);
        CHECK(c.front() == 1);
        CHECK(c.back() == 2);
        CHECK(c.size() == 2u);
        c.pop_front();
        CHECK(c.front() == 2);
        CHECK(c.size() == 1u);
    }

    SECTION("emplace_back") {
        ConcretePodList c;
        c.emplace_back(42);
        REQUIRE(c.size() == 1u);
        CHECK(c.front() == 42);
    }

    SECTION("emplace_front — HasFrontInsertion + PodStorage") {
        ConcretePodList c;
        c.push_back(2);
        c.emplace_front(1);
        CHECK(c.front() == 1);
        CHECK(c.size() == 2u);
    }

    SECTION("assign from initializer_list") {
        ConcretePodList c;
        c.assign({5, 6, 7, 8});
        REQUIRE(c.size() == 4u);
        CHECK(c.front() == 5);
        CHECK(c.back() == 8);
    }

    SECTION("assign from iterator range") {
        std::list<int> src = {10, 20, 30};
        ConcretePodList c;
        c.assign(src.begin(), src.end());
        REQUIRE(c.size() == 3u);
        CHECK(*std::next(c.begin(), 1) == 20);
    }

    SECTION("assign from count + value") {
        ConcretePodList c;
        c.assign(4u, 99);
        REQUIRE(c.size() == 4u);
        for(auto v : c) { CHECK(v == 99); }
    }

    SECTION("insert single and erase") {
        ConcretePodList c;
        c.assign({1, 3});
        auto it = std::next(c.begin(), 1); // points to 3
        c.insert(it, 2);                   // now {1, 2, 3}
        REQUIRE(c.size() == 3u);
        CHECK(*std::next(c.begin(), 1) == 2);
        // erase the inserted element
        c.erase(std::next(c.begin(), 1));
        REQUIRE(c.size() == 2u);
        CHECK(*std::next(c.begin(), 1) == 3);
    }

    SECTION("erase range") {
        ConcretePodList c;
        c.assign({1, 2, 3, 4, 5});
        auto first = std::next(c.begin(), 1);
        auto last  = std::next(c.begin(), 4);
        c.erase(first, last); // removes {2, 3, 4}
        REQUIRE(c.size() == 2u);
        CHECK(c.front() == 1);
        CHECK(c.back() == 5);
    }

    SECTION("insert count copies") {
        ConcretePodList c;
        c.push_back(0);
        c.push_back(0);
        c.insert(c.begin(), 3u, 7); // insert 3 copies of 7 at front
        REQUIRE(c.size() == 5u);
        CHECK(c.front() == 7);
        CHECK(*std::next(c.begin(), 2) == 7);
        CHECK(*std::next(c.begin(), 3) == 0);
    }

    SECTION("iterators: begin/end/cbegin/cend/rbegin/rend") {
        ConcretePodList c;
        c.assign({1, 2, 3});
        CHECK(*c.begin() == 1);
        CHECK(*c.cbegin() == 1);
        CHECK(*c.rbegin() == 3);
        CHECK(*c.crbegin() == 3);
        int sum = 0;
        for(int  const& it : c) sum += it;
        CHECK(sum == 6);
    }

    SECTION("size / empty / clear") {
        ConcretePodList c;
        CHECK(c.empty());
        c.assign({1, 2, 3});
        CHECK(c.size() == 3u);
        CHECK_FALSE(c.empty());
        c.clear();
        CHECK(c.empty());
    }

    SECTION("resize (no value)") {
        ConcretePodList c;
        c.assign({1, 2, 3});
        c.resize(5u);
        CHECK(c.size() == 5u);
        c.resize(2u);
        CHECK(c.size() == 2u);
    }

    SECTION("resize with value") {
        ConcretePodList c;
        c.push_back(1);
        c.resize(4u, 9);
        REQUIRE(c.size() == 4u);
        CHECK(*std::next(c.begin(), 1) == 9);
        CHECK(c.back() == 9);
    }

    SECTION("max_size") {
        ConcretePodList const c;
        CHECK(c.max_size() > 0u);
    }

    SECTION("count") {
        ConcretePodList c;
        c.assign({1, 2, 2, 3});
        CHECK(c.count(2) == 2u);
        CHECK(c.count(9) == 0u);
    }

    SECTION("find") {
        ConcretePodList c;
        c.assign({10, 20, 30});
        auto it = c.find(20);
        REQUIRE(it != c.end());
        CHECK(*it == 20);
        CHECK(c.find(99) == c.end());
    }

    SECTION("compare_base — equality") {
        ConcretePodList a;
        a.assign({1, 2, 3});
        ConcretePodList b;
        b.assign({1, 2, 3});
        CHECK_NOTHROW(a.compare_base(b, Gem::Common::expectation::EQUALITY, 0.0));
    }

    SECTION("compare_base — inequality expectation met") {
        ConcretePodList a;
        a.assign({1, 2, 3});
        ConcretePodList b;
        b.assign({4, 5, 6});
        CHECK_NOTHROW(a.compare_base(b, Gem::Common::expectation::INEQUALITY, 0.0));
    }

    SECTION("compare_base — equality expectation violated throws") {
        ConcretePodList a;
        a.assign({1, 2});
        ConcretePodList b;
        b.assign({3, 4});
        CHECK_THROWS(a.compare_base(b, Gem::Common::expectation::EQUALITY, 0.0));
    }

    SECTION("copy construction — independent storage") {
        ConcretePodList src;
        src.assign({7, 8, 9});
        ConcretePodList dst(src);
        REQUIRE(dst.size() == 3u);
        src.front() = 99;
        CHECK(dst.front() == 7); // independent
    }

    SECTION("move construction") {
        ConcretePodList src;
        src.assign({1, 2, 3});
        ConcretePodList const dst(std::move(src));
        CHECK(dst.size() == 3u);
    }

    SECTION("copy assignment") {
        ConcretePodList a;
        a.assign({1, 2, 3});
        ConcretePodList b;
        b = a;
        REQUIRE(b.size() == 3u);
        CHECK(b.front() == 1);
    }

    SECTION("move assignment") {
        ConcretePodList a;
        a.assign({1, 2, 3});
        ConcretePodList b;
        b = std::move(a);
        CHECK(b.size() == 3u);
    }

    SECTION("GArchive round-trip via GContainerT::serialize()") {
        ConcretePodList src;
        src.assign({11, 22, 33, 44});
        *std::next(src.begin(), 2) = 99;

        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePodList loaded;
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        REQUIRE(loaded.size() == 4u);
        CHECK(loaded.front() == 11);
        CHECK(*std::next(loaded.begin(), 2) == 99);
        CHECK(loaded.back() == 44);
    }
}

/******************************************************************************/
// ─────────────────────────── [edge] ────────────────────────────────────────

TEST_CASE("GContainerT: edge cases", "[GContainerT][edge]") {
    SECTION("Empty container queries") {
        ConcretePodVec const c;
        CHECK(c.empty());
        CHECK(c.empty());
        CHECK(c.cbegin() == c.cend());
        CHECK(c.crbegin() == c.crend());
    }

    SECTION("resize(0) on empty container") {
        ConcretePodVec c;
        CHECK_NOTHROW(c.resize(0u));
        CHECK(c.empty());
    }

    SECTION("clear() on empty container") {
        ConcretePodVec c;
        CHECK_NOTHROW(c.clear());
        CHECK(c.empty());
    }

    SECTION("crossOver at pos == 0") {
        ConcretePodVec a;
        a.assign({1, 2, 3});
        ConcretePodVec b;
        b.assign({4, 5, 6});
        // All elements from pos 0 onward are swapped
        a.crossOver(b, 0);
        CHECK(a[0] == 4);
        CHECK(b[0] == 1);
    }

    SECTION("crossOver at pos == size-1 (equal-length)") {
        ConcretePodVec a;
        a.assign({1, 2, 3});
        ConcretePodVec b;
        b.assign({4, 5, 6});
        a.crossOver(b, 2);
        CHECK(a[2] == 6);
        CHECK(b[2] == 3);
        // First two elements unchanged
        CHECK(a[0] == 1);
        CHECK(b[0] == 4);
    }

    SECTION("HasContiguousStorage is false for std::deque") {
        CHECK_FALSE(Gem::Common::HasContiguousStorage<std::deque<int>>);
    }

    SECTION("HasCapacity is false for std::deque") {
        CHECK_FALSE(Gem::Common::HasCapacity<std::deque<int>>);
    }

    SECTION("HasContiguousStorage is true for std::vector") {
        CHECK(Gem::Common::HasContiguousStorage<std::vector<int>>);
    }

    SECTION("HasCapacity is true for std::vector") {
        CHECK(Gem::Common::HasCapacity<std::vector<int>>);
    }

    SECTION("HasFrontInsertion is true for std::deque") {
        CHECK(Gem::Common::HasFrontInsertion<std::deque<int>>);
    }

    SECTION("HasFrontInsertion is false for std::vector") {
        CHECK_FALSE(Gem::Common::HasFrontInsertion<std::vector<int>>);
    }

    SECTION("crossOver — invalid position throws in debug build") {
        ConcretePodVec a;
        a.assign({1, 2, 3});
        ConcretePodVec b;
        b.assign({4, 5, 6});
#ifdef DEBUG
        // pos >= minSize(3) must throw in debug builds
        CHECK_THROWS_AS(a.crossOver(b, 3), geneva_exception);
        CHECK_THROWS_AS(a.crossOver(b, 99), geneva_exception);
#else
        // In release builds the guard is compiled out; just verify no crash at boundary
        CHECK_NOTHROW(a.crossOver(b, 2));
#endif
    }

    SECTION("SharedPtrStorage emplaceBack not available — checked via type trait") {
        constexpr bool podHasEmplace = std::same_as<
            Gem::Common::PodStorage<int>::StoredType,
            Gem::Common::PodStorage<int>::ValueType>;
        constexpr bool ptrHasEmplace = std::same_as<
            Gem::Common::SharedPtrStorage<TestBase>::StoredType,
            Gem::Common::SharedPtrStorage<TestBase>::ValueType>;
        CHECK(podHasEmplace);
        CHECK_FALSE(ptrHasEmplace);
    }

    SECTION("const compare_base — double instantiation") {
        const ConcretePodVecDouble a(3, 1.5);
        const ConcretePodVecDouble b(3, 1.5);
        CHECK_NOTHROW(a.compare_base(b, Gem::Common::expectation::EQUALITY, 0.0));
    }

    SECTION("HasFrontInsertion is true for std::list") {
        CHECK(Gem::Common::HasFrontInsertion<std::list<int>>);
    }

    SECTION("HasContiguousStorage is false for std::list") {
        CHECK_FALSE(Gem::Common::HasContiguousStorage<std::list<int>>);
    }

    SECTION("HasCapacity is false for std::list") {
        CHECK_FALSE(Gem::Common::HasCapacity<std::list<int>>);
    }

    SECTION("HasRandomAccess is false for std::list") {
        CHECK_FALSE(Gem::Common::HasRandomAccess<std::list<int>>);
    }
}

/******************************************************************************/
// ─────────────────────────── [serialization] ──────────────────────────────

TEST_CASE("GContainerT: GArchive round-trips", "[GContainerT][serialization]") {
    // GContainerT is abstract. ConcretePodVec inherits GContainerT<int, PodStorage<int>>'s
    // template serialize() method which archives data_cnt_ via archive_named.
    // These tests verify the full serialize/deserialize cycle for GPodContainerT.
    //
    // GPtrContainerT serialization (with cloneable, pointer-held elements) is covered indirectly
    // by GenevaStandardTests: the optimization algorithms hold their individuals through
    // GUniquePtrContainerT and are exercised by the full serialization suite there.

    SECTION("GPodContainerT<int> — TEXT archive round-trip") {
        ConcretePodVec src(5, 42);
        src[2] = 99;
        src[4] = -1;

        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePodVec loaded;
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        REQUIRE(loaded.size() == 5u);
        CHECK(loaded[0] == 42);
        CHECK(loaded[1] == 42);
        CHECK(loaded[2] == 99);
        CHECK(loaded[3] == 42);
        CHECK(loaded[4] == -1);
    }

    SECTION("GPodContainerT<int> — XML archive round-trip") {
        ConcretePodVec src;
        src.assign({10, 20, 30, 40});

        std::string blob;
        {
            Gem::Weft::GJsonOArchive oa;
            oa &Gem::Weft::make_nvp("container", src);
            blob = oa.str();
        }
        ConcretePodVec loaded;
        {
            Gem::Weft::GJsonIArchive ia(blob);
            ia &Gem::Weft::make_nvp("container", loaded);
        }
        REQUIRE(loaded.size() == 4u);
        CHECK(loaded[0] == 10);
        CHECK(loaded[3] == 40);
    }

    SECTION("GPodContainerT<int> — binary archive round-trip") {
        ConcretePodVec src;
        src.assign({-5, 0, 5, 100, -100});

        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePodVec loaded;
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        REQUIRE(loaded.size() == 5u);
        CHECK(loaded[0] == -5);
        CHECK(loaded[2] == 5);
        CHECK(loaded[4] == -100);
    }

    SECTION("GPodContainerT<double> — TEXT archive round-trip") {
        ConcretePodVecDouble src(3, 3.14);
        src[1] = 2.718;

        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePodVecDouble loaded;
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        REQUIRE(loaded.size() == 3u);
        CHECK(loaded[0] == Catch::Approx(3.14));
        CHECK(loaded[1] == Catch::Approx(2.718));
        CHECK(loaded[2] == Catch::Approx(3.14));
    }

    SECTION("Empty container round-trip preserves zero size") {
        ConcretePodVec const src;
        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePodVec loaded;
        loaded.assign({1, 2, 3}); // pre-populate to ensure deserialization clears
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        CHECK(loaded.empty());
    }

    // GPtrContainerT<SerBase> round-trips.
    // SerBase is non-polymorphic (no virtual destructor), so the GArchive codec
    // handles shared_ptr<SerBase> directly (no polymorphic registration needed).

    SECTION("GPtrContainerT<SerBase> — TEXT archive round-trip") {
        ConcretePtrSerializable src;
        src.push_back_noclone(std::make_shared<SerBase>(10));
        src.push_back_noclone(std::make_shared<SerBase>(20));
        src.push_back_noclone(std::make_shared<SerBase>(30));

        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePtrSerializable loaded;
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        REQUIRE(loaded.size() == 3u);
        CHECK(loaded[0]->v == 10);
        CHECK(loaded[1]->v == 20);
        CHECK(loaded[2]->v == 30);
        CHECK(loaded[0].get() != src[0].get()); // independent objects after round-trip
    }

    SECTION("GPtrContainerT<SerBase> — XML archive round-trip") {
        ConcretePtrSerializable src;
        src.push_back_noclone(std::make_shared<SerBase>(5));
        src.push_back_noclone(std::make_shared<SerBase>(-5));

        std::string blob;
        {
            Gem::Weft::GJsonOArchive oa;
            oa &Gem::Weft::make_nvp("container", src);
            blob = oa.str();
        }
        ConcretePtrSerializable loaded;
        {
            Gem::Weft::GJsonIArchive ia(blob);
            ia &Gem::Weft::make_nvp("container", loaded);
        }
        REQUIRE(loaded.size() == 2u);
        CHECK(loaded[0]->v == 5);
        CHECK(loaded[1]->v == -5);
    }

    SECTION("GPtrContainerT<SerBase> — binary archive round-trip") {
        ConcretePtrSerializable src;
        src.push_back_noclone(std::make_shared<SerBase>(100));
        src.push_back_noclone(std::make_shared<SerBase>(200));

        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePtrSerializable loaded;
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        REQUIRE(loaded.size() == 2u);
        CHECK(loaded[0]->v == 100);
        CHECK(loaded[1]->v == 200);
    }

    SECTION("GPtrContainerT<SerBase> — empty container round-trip") {
        ConcretePtrSerializable const src;
        std::string blob;
        {
            Gem::Weft::GBinaryOArchive oa;
            oa &Gem::Weft::make_nvp("c", src);
            blob = oa.str();
        }
        ConcretePtrSerializable loaded;
        loaded.push_back_noclone(std::make_shared<SerBase>(99));
        {
            Gem::Weft::GBinaryIArchive ia(blob);
            ia &Gem::Weft::make_nvp("c", loaded);
        }
        CHECK(loaded.empty());
    }
}

/******************************************************************************/
// The UniquePtrStorage policy and the unique_ptr overloads of the deep-copy helpers, exercised in
// isolation (nothing in geneva uses the unique container yet).

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- a flat catalogue of independent, self-contained per-operation micro-checks (each SECTION builds its own fixture); the length is coverage breadth, not branching complexity, and splitting into standalone TEST_CASEs would only multiply the count with no shared setup to consolidate
TEST_CASE("GContainerT: UniquePtrStorage + unique_ptr deep-copy helpers", "[GContainerT][ptr][unique]") {
    using Vec = std::vector<std::unique_ptr<TestBase>>;

    SECTION("clone clones the dynamic type into a unique_ptr") {
        std::unique_ptr<TestBase> b = std::make_unique<TestBase>(7);
        std::unique_ptr<TestBase> d = std::make_unique<TestDerived>(3, 9);
        auto bc = b->clone();
        auto dc = d->clone();
        REQUIRE(bc);
        REQUIRE(dc);
        CHECK(bc->val == 7);
        auto *dcd = dynamic_cast<TestDerived *>(dc.get());
        REQUIRE(dcd != nullptr); // polymorphic clone, not sliced
        CHECK(dcd->val == 3);
        CHECK(dcd->derivedVal == 9);
        CHECK(dc.get() != d.get()); // independent object
    }

    SECTION("copyCloneableSmartPointer loads in place on a type match, clones otherwise") {
        std::unique_ptr<TestBase> const from = std::make_unique<TestBase>(5);
        std::unique_ptr<TestBase> to = std::make_unique<TestBase>(0);
        TestBase  const*to_raw = to.get();
        Gem::Common::copyCloneableSmartPointer(from, to);
        CHECK(to->val == 5);
        CHECK(to.get() == to_raw); // loaded in place, no reallocation

        std::unique_ptr<TestBase> const empty;
        Gem::Common::copyCloneableSmartPointer(empty, to);
        CHECK(!to); // null source resets the target

        std::unique_ptr<TestBase> const der = std::make_unique<TestDerived>(1, 2);
        Gem::Common::copyCloneableSmartPointer(der, to);
        REQUIRE(to);
        CHECK(dynamic_cast<TestDerived *>(to.get()) != nullptr); // type mismatch -> clone
    }

    SECTION("copyCloneableSmartPointerContainer: equal size loads in place") {
        Vec from;
        from.push_back(std::make_unique<TestBase>(1));
        from.push_back(std::make_unique<TestBase>(2));
        Vec to;
        to.push_back(std::make_unique<TestBase>(0));
        to.push_back(std::make_unique<TestBase>(0));
        TestBase  const*slot0 = to[0].get();
        Gem::Common::copyCloneableSmartPointerContainer(from, to);
        REQUIRE(to.size() == 2);
        CHECK(to[0]->val == 1);
        CHECK(to[1]->val == 2);
        CHECK(to[0].get() == slot0);          // reused slot
        CHECK(to[0].get() != from[0].get());  // but a deep, independent copy
    }

    SECTION("copyCloneableSmartPointerContainer: growth clones the extra elements") {
        Vec from;
        for(int i = 0; i < 3; ++i) {
            from.push_back(std::make_unique<TestBase>(i + 1));
        }
        Vec to;
        to.push_back(std::make_unique<TestBase>(0));
        Gem::Common::copyCloneableSmartPointerContainer(from, to);
        REQUIRE(to.size() == 3);
        CHECK(to[0]->val == 1);
        CHECK(to[2]->val == 3);
        for(std::size_t i = 0; i < 3; ++i) {
            CHECK(to[i].get() != from[i].get());
        }
    }

    SECTION("copyCloneableSmartPointerContainer: shrink drops the extra elements") {
        Vec from;
        from.push_back(std::make_unique<TestBase>(9));
        Vec to;
        for(int i = 0; i < 4; ++i) {
            to.push_back(std::make_unique<TestBase>(0));
        }
        Gem::Common::copyCloneableSmartPointerContainer(from, to);
        REQUIRE(to.size() == 1);
        CHECK(to[0]->val == 9);
    }

    SECTION("UniquePtrStorage::deepCopy is deep and independent") {
        using Policy = Gem::Common::UniquePtrStorage<TestBase>;
        Policy::ContainerType src;
        src.push_back(std::make_unique<TestDerived>(4, 8));
        Policy::ContainerType dst;
        Policy::deepCopy(src, dst);
        REQUIRE(dst.size() == 1);
        auto *d = dynamic_cast<TestDerived *>(dst[0].get());
        REQUIRE(d != nullptr);
        CHECK(d->val == 4);
        CHECK(d->derivedVal == 8);
        dst[0]->val = 100; // mutating the copy must not touch the source
        CHECK(src[0]->val == 4);
    }
}

/******************************************************************************/
// A populated GUniquePtrContainerT: the pointer API now works for unique_ptr
// storage -- clone-based ops clone via clone(), move-based ops move the sole-owned handle.

TEST_CASE("GContainerT: GUniquePtrContainerT populated container", "[GContainerT][ptr][unique]") {
    SECTION("push (move-in and clone-in), size, element access") {
        ConcreteUniquePtrVec v;
        v.push_back_noclone(std::make_unique<TestBase>(1)); // move-in (sole ownership)
        auto proto = std::make_unique<TestBase>(2);
        v.push_back_clone(proto);                           // clone-in via clone()
        v.push_back(std::make_unique<TestBase>(3));         // push_back(StoredType&&)
        REQUIRE(v.size() == 3);
        CHECK(v[0]->val == 1);
        CHECK(v.at(1)->val == 2);
        CHECK(v[2]->val == 3);
        CHECK(proto->val == 2);            // clone-in did not consume the prototype
        CHECK(v[1].get() != proto.get());  // it is an independent clone
    }

    SECTION("clone-in keeps the dynamic type (no slicing)") {
        ConcreteUniquePtrVec v;
        std::unique_ptr<TestBase> const base_handle = std::make_unique<TestDerived>(5, 6);
        v.push_back_clone(base_handle);
        REQUIRE(v.size() == 1);
        auto *dv = dynamic_cast<TestDerived *>(v[0].get());
        REQUIRE(dv != nullptr);
        CHECK(dv->val == 5);
        CHECK(dv->derivedVal == 6);
    }

    SECTION("deep copy construction is independent") {
        ConcreteUniquePtrVec a;
        a.push_back_noclone(std::make_unique<TestBase>(10));
        a.push_back_noclone(std::make_unique<TestBase>(20));
        ConcreteUniquePtrVec b = a; // copy-ctor -> StoragePolicy::deepCopy
        REQUIRE(b.size() == 2);
        CHECK(b[0]->val == 10);
        CHECK(b[0].get() != a[0].get()); // distinct objects
        b[0]->val = 99;                  // mutating the copy ...
        CHECK(a[0]->val == 10);          // ... does not touch the original
    }

    SECTION("deep copy assignment resizes and stays independent") {
        ConcreteUniquePtrVec a;
        a.push_back_noclone(std::make_unique<TestBase>(7));
        ConcreteUniquePtrVec b;
        b.push_back_noclone(std::make_unique<TestBase>(0));
        b.push_back_noclone(std::make_unique<TestBase>(0));
        b = a; // operator= -> deepCopy (shrinks 2 -> 1)
        REQUIRE(b.size() == 1);
        CHECK(b[0]->val == 7);
        CHECK(b[0].get() != a[0].get());
    }

    SECTION("move construction transfers ownership without cloning") {
        ConcreteUniquePtrVec a;
        a.push_back_noclone(std::make_unique<TestBase>(42));
        TestBase  const*raw = a[0].get();
        ConcreteUniquePtrVec b = std::move(a);
        REQUIRE(b.size() == 1);
        CHECK(b[0].get() == raw); // the very same object, moved, not cloned
    }

    SECTION("getDataCopy produces a deep, independent copy") {
        ConcreteUniquePtrVec a;
        a.push_back_noclone(std::make_unique<TestDerived>(1, 2));
        std::vector<std::unique_ptr<TestBase>> out;
        a.getDataCopy(out);
        REQUIRE(out.size() == 1);
        CHECK(dynamic_cast<TestDerived *>(out[0].get()) != nullptr);
        CHECK(out[0].get() != a[0].get());
    }

    SECTION("clear / empty") {
        ConcreteUniquePtrVec v;
        v.push_back_noclone(std::make_unique<TestBase>(1));
        CHECK(!v.empty());
        v.clear();
        CHECK(v.empty());
        CHECK(v.size() == 0);
    }
}
