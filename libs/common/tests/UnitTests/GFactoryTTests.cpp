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
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/GExceptions.hpp"
#include "common/GFactoryT.hpp"

using namespace Gem::Common;

// ---------------------------------------------------------------------------
// Tiny product + factory pair. The factory increments per-call counters so
// we can observe globalInit / postProcess / getObject behaviour.

namespace {

struct Product {
    int payload{0};
};

class TestFactory : public GFactoryT<Product> {
public:
    explicit TestFactory(std::filesystem::path const &p) : GFactoryT<Product>(p) {}

    int init_calls       = 0;
    int describe_calls   = 0;
    int post_process_calls = 0;
    int get_object_calls = 0;

protected:
    void init_() override {
        ++init_calls;
    }

    void describeLocalOptions_([[maybe_unused]] GParserBuilder & gpb) override {
        ++describe_calls;
    }

    void postProcess_(std::shared_ptr<Product> &p) override {
        ++post_process_calls;
        if(p) {
            p->payload = 42;
        }
    }

private:
    std::shared_ptr<Product>
    getObject_([[maybe_unused]] GParserBuilder & gpb) override {
        ++get_object_calls;
        return std::make_shared<Product>();
    }
};

// Concrete-clone factory — overrides clone() so the "trap" path is no longer
// in play. Used to exercise the clone() success path of the base.
class ClonableFactory : public TestFactory {
public:
    explicit ClonableFactory(std::filesystem::path const &p) : TestFactory(p) {}

    std::shared_ptr<GFactoryT<Product>> clone() const override {
        return std::make_shared<ClonableFactory>(*this);
    }
};

// Creates and returns a path to a config file with the given content. The
// content does not need to be parseable for tests of write/path behaviour.
std::filesystem::path make_config(std::string const &tag,
                                  std::string const &content = "{}") {
    auto base = std::filesystem::temp_directory_path() / "geneva_factory_tests";
    std::filesystem::create_directories(base);
    auto p = base / (tag + ".json");   // GParserBuilder enforces .json
    {
        std::ofstream o(p);
        o << content;
    }
    return p;
}

// A config path that can be neither read nor created, whoever runs the test: its parent is a
// regular file, so no directory can be made there -- not even by root, as in a container build
// (a merely absent directory such as /no/such/path would simply be created for root).
std::filesystem::path unreachable_config(std::string const &file_name) {
    return make_config("not_a_directory") / file_name;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / accessors

TEST_CASE("GFactoryT: stores the config path verbatim",
          "[common][factory]") {
    auto p = make_config("ctor");
    TestFactory const f(p);
    CHECK(f.getConfigFilePath() == p);
    CHECK(f.getConfigFileName() == p.string());
}

TEST_CASE("GFactoryT::setConfigFile updates the stored path",
          "[common][factory]") {
    auto p1 = make_config("set_a");
    auto p2 = make_config("set_b");
    TestFactory f(p1);
    f.setConfigFile(p2.string());
    CHECK(f.getConfigFilePath() == p2);
}

// ---------------------------------------------------------------------------
// get() / operator() — exercise the full pipeline. The test config file
// exists and is empty, which is enough because the factory's `describeLocalOptions_`
// is a no-op.

TEST_CASE("GFactoryT::get() runs init→describe→getObject→postProcess",
          "[common][factory]") {
    auto p = make_config("pipeline");
    TestFactory f(p);

    auto prod = f.get();
    REQUIRE(prod);
    CHECK(prod->payload == 42);              // postProcess_ wrote the marker

    CHECK(f.init_calls         == 1);
    CHECK(f.describe_calls     == 1);
    CHECK(f.get_object_calls   == 1);
    CHECK(f.post_process_calls == 1);

    auto prod2 = f.get();
    REQUIRE(prod2);
    CHECK(f.init_calls         == 1);          // init only happens once
    CHECK(f.describe_calls     == 2);
    CHECK(f.get_object_calls   == 2);
    CHECK(f.post_process_calls == 2);
}

TEST_CASE("GFactoryT::operator() forwards to get()",
          "[common][factory]") {
    auto p = make_config("op_call");
    TestFactory f(p);
    auto prod = f();
    REQUIRE(prod);
    CHECK(prod->payload == 42);
}

TEST_CASE("GFactoryT::get() throws when the config path cannot be opened",
          "[common][factory]") {
    // Point at a directory that cannot exist + lacks the .json extension. The
    // wrapper's parse step throws either way.
    TestFactory f(unreachable_config("geneva_does_not_exist.cfg"));
    CHECK_THROWS_AS(f.get(), geneva_exception);
}

// ---------------------------------------------------------------------------
// get_as<T>(): downcast convenience

struct PolyProduct {
    virtual ~PolyProduct() = default;
    int payload{0};
};
struct DerivedProduct : PolyProduct { int extra = 7; };

class DerivedFactory : public GFactoryT<PolyProduct> {
public:
    explicit DerivedFactory(std::filesystem::path const &p) : GFactoryT<PolyProduct>(p) {}
protected:
    void postProcess_([[maybe_unused]] std::shared_ptr<PolyProduct> &p) override {}

private:
    std::shared_ptr<PolyProduct> getObject_([[maybe_unused]] GParserBuilder & gpb) override {
        return std::make_shared<DerivedProduct>();
    }
};

TEST_CASE("GFactoryT::get_as: downcasts the produced product to a derived type",
          "[common][factory]") {
    auto p = make_config("derived");
    DerivedFactory f(p);
    auto derived = f.get_as<DerivedProduct>();
    REQUIRE(derived);
    CHECK(derived->extra == 7);
}

// ---------------------------------------------------------------------------
// clone(): default impl is a "trap"; an overriding subclass succeeds.

TEST_CASE("GFactoryT::clone() in the base throws (intentional trap)",
          "[common][factory]") {
    auto p = make_config("clone_trap");
    TestFactory const f(p);
    CHECK_THROWS_AS(f.clone(), geneva_exception);
}

TEST_CASE("GFactoryT::clone() in a subclass that overrides it returns a copy",
          "[common][factory]") {
    auto p = make_config("clone_ok");
    ClonableFactory const f(p);
    auto cp = f.clone();
    REQUIRE(cp);
    CHECK(cp->getConfigFilePath() == p);
}

// ---------------------------------------------------------------------------
// load(): copies path / initialized flags from another factory.

TEST_CASE("GFactoryT::load() copies path/initialized from a sibling factory",
          "[common][factory]") {
    auto p1 = make_config("load_src");
    auto p2 = make_config("load_dst");

    auto src = std::make_shared<TestFactory>(p1);
    src->get();   // trigger initialized_ = true

    TestFactory dst(p2);
    dst.load(src);
    CHECK(dst.getConfigFilePath() == p1);
    // After load(), a subsequent get() must not re-run init_() (initialized_ copied).
    dst.get();
    // dst's init_calls counter is still zero — init_calls is counted in src.
    CHECK(dst.init_calls == 0);
}

// ---------------------------------------------------------------------------
// Copy / move semantics — init_mutex_ requires user-defined ops.

TEST_CASE("GFactoryT: copy construction preserves config path / counters semantics",
          "[common][factory]") {
    auto p = make_config("copy_ctor");
    TestFactory const src(p);
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) -- intentional copy: this test checks copy construction
    TestFactory const dst(src);
    CHECK(dst.getConfigFilePath() == p);
}

TEST_CASE("GFactoryT: move construction transfers the config path",
          "[common][factory]") {
    auto p = make_config("move_ctor");
    TestFactory src(p);
    TestFactory const dst(std::move(src));
    CHECK(dst.getConfigFilePath() == p);
}

TEST_CASE("GFactoryT: copy assignment is supported",
          "[common][factory]") {
    auto p1 = make_config("assign_a");
    auto p2 = make_config("assign_b");
    TestFactory const a(p1);
    TestFactory b(p2);
    b = a;
    CHECK(b.getConfigFilePath() == p1);
}

TEST_CASE("GFactoryT: move assignment is supported",
          "[common][factory]") {
    auto p1 = make_config("massign_a");
    auto p2 = make_config("massign_b");
    TestFactory a(p1);
    TestFactory b(p2);
    b = std::move(a);
    CHECK(b.getConfigFilePath() == p1);
}

// ---------------------------------------------------------------------------
// The transient parse cache: parsed once, but never carried across a re-keying

namespace {

/** @brief A factory that reads one integer option, so the applied configuration is observable. */
class ValueReadingFactory : public GFactoryT<Product> {
public:
    explicit ValueReadingFactory(std::filesystem::path const &p) : GFactoryT<Product>(p) {}

    int last_value = -1; ///< the value the most recent get() saw in the configuration

protected:
    void describeLocalOptions_(GParserBuilder &gpb) override {
        gpb.registerFileParameter<int>("observed", -1, [this](int v) { last_value = v; })
            << "An observable integer option";
    }
    void postProcess_([[maybe_unused]] std::shared_ptr<Product> &p) override {}

private:
    std::shared_ptr<Product> getObject_([[maybe_unused]] GParserBuilder &gpb) override {
        return std::make_shared<Product>();
    }
};

/** @brief Builds a config file holding a single "value" entry, in GParserBuilder's own layout. */
std::filesystem::path make_value_config(std::string const &tag, int value) {
    const std::string v = std::to_string(value);
    return make_config(tag, R"({"observed": {"default": )" + v + R"(, "value": )" + v + "}}");
}

} // namespace

TEST_CASE("GFactoryT: the config document is parsed once and re-applied to every product",
          "[common][factory]") {
    auto p = make_value_config("cache_reuse", 11);
    ValueReadingFactory f(p);

    (void)f.get();
    CHECK(f.last_value == 11);

    // The second get() must apply the SAME configuration although the file is no longer read.
    f.last_value = -1;
    (void)f.get();
    CHECK(f.last_value == 11);
}

TEST_CASE("GFactoryT: assignment re-keys the factory and drops the parse cache",
          "[common][factory]") {
    auto p_a = make_value_config("cache_key_a", 1);
    auto p_b = make_value_config("cache_key_b", 2);

    SECTION("copy assignment") {
        ValueReadingFactory target(p_a);
        (void)target.get();
        REQUIRE(target.last_value == 1); // warms the cache with A's document

        ValueReadingFactory const source(p_b);
        target = source;
        (void)target.get();
        CHECK(target.last_value == 2); // A's cached document must not survive the re-keying
    }

    SECTION("move assignment") {
        ValueReadingFactory target(p_a);
        (void)target.get();
        REQUIRE(target.last_value == 1);

        ValueReadingFactory source(p_b);
        target = std::move(source);
        (void)target.get();
        CHECK(target.last_value == 2);
    }

    // Regression: setConfigFile() promises to take effect for the next produced object. With a warm
    // parse cache keyed to the OLD path it used to have no effect at all -- the document parsed from
    // the previous file kept being re-applied.
    SECTION("setConfigFile") {
        ValueReadingFactory f(p_a);
        (void)f.get();
        REQUIRE(f.last_value == 1);

        f.setConfigFile(p_b.string());
        (void)f.get();
        CHECK(f.last_value == 2);
    }
}

TEST_CASE("GFactoryT: a failed parse is not cached -- the next get() retries",
          "[common][factory]") {
    // A file that does not exist: the parse throws out of the fill, which must leave the cache
    // empty rather than record a failure. (The load-once cell's throwing-producer contract.)
    ValueReadingFactory f(unreachable_config("geneva_does_not_exist.json"));
    CHECK_THROWS_AS(f.get(), geneva_exception);
    CHECK_THROWS_AS(f.get(), geneva_exception);

    // Once the factory is pointed at a readable file, the very next get() succeeds.
    auto p = make_value_config("retry_after_failure", 5);
    f.setConfigFile(p.string());
    (void)f.get();
    CHECK(f.last_value == 5);
}

// ---------------------------------------------------------------------------
// Concurrency: globalInit() must call init_() exactly once even under racing
// first-get() calls. Use a custom counter type to observe this.

namespace {

class CountedInitFactory : public GFactoryT<Product> {
public:
    explicit CountedInitFactory(std::filesystem::path const &p) : GFactoryT<Product>(p) {}
    std::atomic<int> init_count{0};
protected:
    void init_() override { ++init_count; }
    void postProcess_([[maybe_unused]] std::shared_ptr<Product> &p) override {}

private:
    std::shared_ptr<Product> getObject_([[maybe_unused]] GParserBuilder & gpb) override {
        return std::make_shared<Product>();
    }
};

} // namespace

TEST_CASE("GFactoryT::globalInit: init_() called exactly once under concurrent first-get()",
          "[common][factory][concurrency]") {
    auto p = make_config("concurrent");
    CountedInitFactory f(p);

    constexpr int N = 16;
    std::vector<std::thread> ts;
    std::atomic<bool> go{false};

    for(int i = 0; i < N; ++i) {
        ts.emplace_back([&] {
            while(not go.load()) {
                std::this_thread::yield();
            }
            try { f.get(); } catch(...) { /* swallow parse errors — we only care about init_() */ }
        });
    }
    go.store(true);
    for(auto &t : ts) t.join();

    CHECK(f.init_count.load() == 1);
}
