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

#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

#include <string>

#include "common/GGlobalOptionsT.hpp" // real never-destroy singleton (GGlobalOptionsT<T>)
#include "common/GLogger.hpp"         // real never-destroy singleton (GLogger)
#include "common/GSingletonT.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// Helper payload types. Each TEST_CASE uses a fresh type so the static
// per-template singleton state is isolated — tests cannot pollute each other.

namespace {

struct Payload_Default {
    int marker = 42;
};

struct Payload_Instance {};

struct Payload_Reset {};

struct Payload_Concurrent {
    static std::atomic<int> ctor_count;
    Payload_Concurrent() {
        ++ctor_count;
    }
};
std::atomic<int> Payload_Concurrent::ctor_count{0};

// A type without a default constructor — only reachable via a specialised
// TFactory_GSingletonT<>. Tests the documented customisation point.
struct Payload_FactorySpecialised {
    explicit Payload_FactorySpecialised(int v) : value(v) {}
    int value;
};

// Two payloads differing only in their lifetime policy, used to check the
// never-destroy (leaky) semantics: Payload_Leaky opts in below, Payload_LeakyOff
// keeps the default (destroyed-at-exit) behaviour as a baseline for comparison.
struct Payload_Leaky {};
struct Payload_LeakyOff {};

// Detection concept for "GSingletonT<T>::reset() is callable". T is a template
// parameter here, so the constraint check on reset() happens in a dependent
// context and yields false for never-destroy types (rather than the hard error
// a non-dependent requires-expression on a concrete type would produce).
template <typename T>
concept resettable_singleton = requires { GSingletonT<T>::reset(); };

} // namespace

// Custom factory for the no-default-ctor type. Must be declared in the same
// namespace as the primary template so ADL/standard-lookup can find it.
namespace Gem::Common {
template <>
std::shared_ptr<Payload_FactorySpecialised> TFactory_GSingletonT<Payload_FactorySpecialised>() {
    return std::make_shared<Payload_FactorySpecialised>(7);
}

// Opt Payload_Leaky into never-destroy semantics (Payload_LeakyOff stays at the
// default false_type). The specialisation must live in the trait's namespace.
template <>
struct gsingleton_never_destroy<Payload_Leaky> : std::true_type {};
} // namespace Gem::Common

// ---------------------------------------------------------------------------
// Static interface contract

TEST_CASE("GSingletonT: is non-instantiable and exposes STYPE", "[common][singleton]") {
    static_assert(not std::is_default_constructible_v<GSingletonT<Payload_Default>>);
    static_assert(not std::is_copy_constructible_v<GSingletonT<Payload_Default>>);
    static_assert(not std::is_move_constructible_v<GSingletonT<Payload_Default>>);
    static_assert(std::is_same_v<GSingletonT<Payload_Default>::STYPE, Payload_Default>);
}

// ---------------------------------------------------------------------------
// instance(): default-construct on first call, reuse afterwards

TEST_CASE("GSingletonT::instance() returns a non-null pointer to a default-constructed instance",
          "[common][singleton]") {
    auto sp = GSingletonT<Payload_Instance>::instance();
    REQUIRE(sp);
    CHECK(sp.use_count() >= 1);

    auto sp2 = GSingletonT<Payload_Instance>::instance();
    REQUIRE(sp2);
    CHECK(sp.get() == sp2.get());      // same object across calls
    CHECK(sp == sp2);
}

// ---------------------------------------------------------------------------
// reset(): release the stored instance; instance() afterwards builds a fresh one

TEST_CASE("GSingletonT::reset() drops the stored pointer; instance() afterwards builds a fresh one",
          "[common][singleton]") {
    auto first = GSingletonT<Payload_Reset>::instance();
    REQUIRE(first);

    GSingletonT<Payload_Reset>::reset();

    // External users still keep the old object alive — singleton storage is
    // released, but `first` keeps its referent valid.
    CHECK(first);

    auto fresh = GSingletonT<Payload_Reset>::instance();
    REQUIRE(fresh);
    CHECK(fresh.get() != first.get()); // brand-new object after the reset
}

// ---------------------------------------------------------------------------
// Customisation point: TFactory_GSingletonT<T> specialisation

TEST_CASE("GSingletonT: honours TFactory_GSingletonT<T> specialisation",
          "[common][singleton]") {
    auto sp = GSingletonT<Payload_FactorySpecialised>::instance();
    REQUIRE(sp);
    CHECK(sp->value == 7);   // produced by the user-supplied factory above
}

// ---------------------------------------------------------------------------
// Concurrency: a horde of threads racing on the first instance() call must
// see exactly one constructor invocation, and all returned pointers must
// alias the same object.

TEST_CASE("GSingletonT::instance() constructs exactly once under concurrent first access",
          "[common][singleton][concurrency]") {
    // Reset any residual state from previous runs of this TU — the singleton
    // storage is per-type and survives between TEST_CASEs.
    GSingletonT<Payload_Concurrent>::reset();
    Payload_Concurrent::ctor_count.store(0);

    // Workers must not invoke Catch CHECK/REQUIRE — Catch's output-redirect
    // assertion is not thread-safe across simultaneous test threads. Collect
    // pointers from each worker and validate on the main thread.
    constexpr int kThreads = 16;
    std::vector<std::shared_ptr<Payload_Concurrent>> results(kThreads);
    std::vector<std::thread> ts;
    ts.reserve(kThreads);

    // A start gate increases the chance that several threads hit the DCLP
    // window simultaneously, exercising the lock path rather than the cheap
    // post-init load path.
    std::atomic<bool> go{false};
    for(int i = 0; i < kThreads; ++i) {
        ts.emplace_back([i, &results, &go] {
            while(not go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[i] = GSingletonT<Payload_Concurrent>::instance();
        });
    }
    go.store(true, std::memory_order_release);
    for(auto &t : ts) {
        t.join();
    }

    CHECK(Payload_Concurrent::ctor_count.load() == 1);

    REQUIRE(results[0]);
    for(int i = 1; i < kThreads; ++i) {
        REQUIRE(results[i]);
        CHECK(results[i].get() == results[0].get());
    }
}

// ---------------------------------------------------------------------------
// Never-destroy (leaky) lifetime policy: opt-in trait, disabled reset(), and
// the extra leaked owning reference that pins the instance for the whole run.

TEST_CASE("GSingletonT: gsingleton_never_destroy trait wiring is correct",
          "[common][singleton]") {
    // Opt-in payload vs. default baseline.
    static_assert(gsingleton_never_destroy<Payload_Leaky>::value);
    static_assert(not gsingleton_never_destroy<Payload_LeakyOff>::value);

    // The real singletons we enrolled.
    static_assert(gsingleton_never_destroy<GLogger>::value);
    static_assert(gsingleton_never_destroy<GGlobalOptionsT<std::string>>::value);
    static_assert(gsingleton_never_destroy<GGlobalOptionsT<int>>::value); // partial spec covers any T

    SUCCEED("compile-time trait checks passed");
}

TEST_CASE("GSingletonT::reset() is disabled for never-destroy singletons",
          "[common][singleton]") {
    // reset() carries a requires-clause, so for a never-destroy type it is not a
    // viable candidate: calling it is ill-formed (a compile error). The
    // resettable_singleton concept detects this. Resettable types stay callable.
    static_assert(resettable_singleton<Payload_LeakyOff>);
    static_assert(not resettable_singleton<Payload_Leaky>);

    // The real never-destroy singletons must likewise reject reset().
    static_assert(not resettable_singleton<GLogger>);
    static_assert(not resettable_singleton<GGlobalOptionsT<std::string>>);

    SUCCEED("compile-time reset() availability checks passed");
}

TEST_CASE("GSingletonT: never-destroy instance is a stable singleton with an extra pinned reference",
          "[common][singleton]") {
    // Singleton identity still holds.
    auto on = GSingletonT<Payload_Leaky>::instance();
    REQUIRE(on);
    CHECK(GSingletonT<Payload_Leaky>::instance().get() == on.get());

    // The never-destroy variant carries exactly one extra owning reference (the
    // deliberately leaked pin) compared to a default singleton built the same
    // way: both are held by their storage plus our single local, but the leaky
    // one additionally has the pin -> use_count is higher by exactly one.
    auto off = GSingletonT<Payload_LeakyOff>::instance();
    REQUIRE(off);
    CHECK(on.use_count() == off.use_count() + 1);

    // The pin survives dropping all external references: a later instance()
    // still returns the very same object, and the extra reference is still there.
    auto *raw = on.get();
    on.reset();
    auto again = GSingletonT<Payload_Leaky>::instance();
    REQUIRE(again);
    CHECK(again.get() == raw);                       // never rebuilt -> same object
    CHECK(again.use_count() == off.use_count() + 1); // pin still present
}
