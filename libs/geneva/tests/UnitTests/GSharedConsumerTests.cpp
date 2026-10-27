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
 * The single-consumer model: a process has at most ONE work consumer (GConsumerRegistry), shared by
 * every algorithm. This file pins three things: (1) the registry holds exactly one consumer and
 * ensureConsumer builds it only once; (2) the meta-EA evaluates its umbrella-individuals on its OWN
 * orchestration pool while their inner sub-EAs all share the one work consumer (the deadlock-free way
 * to nest optimizations); (3) several independent algorithms submitting at once fan in to that one
 * consumer. (A naive nested EA-in-EA that ran BOTH tiers on the single work pool would re-enter the pool
 * and deadlock -- which is exactly why nesting goes through the meta-EA's separate pool.)
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "courtier/GConsumerRegistry.hpp"
#include "courtier/consumers/GStdThreadConsumerT.hpp"
#include "geneva/GConsumerSetup.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GMetaEvolutionaryAlgorithm.hpp"

namespace gen = Gem::Geneva::Genome;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

namespace {

/** @brief A tiny flat sphere: 3 constrained doubles in [-5, 5), started at 3.0. The inner problem. */
class InnerSphere : public gen::GGenomeT<InnerSphere> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    InnerSphere() {
        gen::GGenomeBuilder b;
        b.addDoubleGroup(3, -5., 5.).init(3.0);
        this->setGenome(b.build());
    }
    InnerSphere(const InnerSphere &) = default;

    std::shared_ptr<oa::GAdaptionConfigBase> buildAdaptionConfig() const {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        cfg->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        return cfg;
    }

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        double s = 0.;
        for(double const x : v) {
            s += x * x;
        }
        return {s};
    }
};

/**
 * @brief A meta individual whose evaluation RUNS an inner EA over InnerSphere through the default
 * (un-injected) submission path -- i.e. exactly the nesting the invariant targets. Its own genome is
 * one constrained double (so the outer EA has something to adapt); the value is unused.
 */
class MetaSphere : public gen::GGenomeT<MetaSphere> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    MetaSphere() {
        gen::GGenomeBuilder b;
        b.addDoubleGroup(1, -5., 5.).init(3.0);
        this->setGenome(b.build());
    }
    MetaSphere(const MetaSphere &) = default;

    std::shared_ptr<oa::GAdaptionConfigBase> buildAdaptionConfig() const {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        cfg->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        return cfg;
    }

protected:
    std::vector<double> evaluate() override {
        auto inner = std::make_shared<oa::GEvolutionaryAlgorithm>();
        inner->setPopulationSizes(4, 2);
        inner->setMaxIteration(2);
        inner->setReportIteration(100000);
        InnerSphere const src;
        inner->push_back(src.clone());
        inner->setAdaptionConfig(src.buildAdaptionConfig());
        // Deliberately NO setLocalConsumer(): init() defaults to the thread-pool consumer, which is the
        // un-injected path the registry must funnel into the single shared consumer.
        inner->optimize();

        auto best = inner->getBestGlobalIndividual<InnerSphere>();
        std::vector<double> v;
        best->streamline<double>(v);
        double s = 0.;
        for(double const x : v) {
            s += x * x;
        }
        return {s};
    }
};

} /* anonymous namespace */

/******************************************************************************/

TEST_CASE("Consumer registry holds one consumer", "[consumer][sharing][registry]") {
    using Registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>;
    using consumer_t = Gem::Courtier::GStdThreadConsumerT<gen::GGenome>;

    auto &reg = Registry::instance();
    reg.clear();

    CHECK(reg.consumer() == nullptr);

    // ensureConsumer builds exactly once; later (and concurrent-shaped) calls share the same consumer.
    std::size_t factory_calls = 0;
    auto factory = [&factory_calls]() -> Registry::consumer_ptr {
        ++factory_calls;
        return std::make_shared<consumer_t>();
    };
    auto first = reg.ensureConsumer(factory);
    auto second = reg.ensureConsumer(factory);
    CHECK(factory_calls == 1);     // the second call did NOT build a new consumer
    CHECK(first == second);        // it returned the same consumer
    CHECK(reg.consumer() == first);

    // setConsumer replaces it (e.g. Go2 registering the consumer it built); clear empties.
    auto replacement = std::make_shared<consumer_t>();
    reg.setConsumer(replacement);
    CHECK(reg.consumer() == replacement);
    reg.clear();
    CHECK(reg.consumer() == nullptr);
}

/******************************************************************************/

TEST_CASE(
    "Concurrent algorithms fan in to one shared thread-pool consumer",
    "[consumer][sharing][stress]") {
    using StcConsumer = Gem::Courtier::GStdThreadConsumerT<gen::GGenome>;

    // The shared thread-pool consumer is meant to serve several algorithms at once (the fan-in case):
    // each submitter waits on its OWN per-batch counter in dispatch_, not a global pool drain. Here K
    // independent EAs run on K separate threads, all un-injected so all converge on the ONE registered
    // thread-pool consumer. This exercises concurrent dispatch_ calls on the shared pool and asserts
    // (a) every algorithm still converges and (b) only ONE pool was built (bounded threads, no
    // per-algorithm oversubscription). The submitting threads are NOT pool workers, so they block on
    // their batches while the pool drains them -- no pool-reentrancy (a nested EA-in-EA on the SAME pool
    // would instead be split across kinds; see the two-tier meta-optimization model).
    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().clear();
    StcConsumer::instances_constructed().store(0);

    constexpr int K = 4;
    std::vector<double> results(K, 1.0e9); // each thread writes only its own slot -> no data race

    std::vector<std::thread> threads;
    threads.reserve(K);
    for(int t = 0; t < K; ++t) {
        threads.emplace_back([t, &results]() {
            auto ea = std::make_shared<oa::GEvolutionaryAlgorithm>();
            ea->setPopulationSizes(12, 4);
            ea->setMaxIteration(40);
            ea->setReportIteration(100000);
            InnerSphere const src;
            ea->push_back(src.clone());
            ea->setAdaptionConfig(src.buildAdaptionConfig());
            // No setLocalConsumer(): the default thread-pool consumer is resolved (and shared) via the registry.
            ea->optimize();

            auto best = ea->getBestGlobalIndividual<InnerSphere>();
            std::vector<double> v;
            best->streamline<double>(v);
            double s = 0.;
            for(double const x : v) {
                s += x * x;
            }
            results[t] = s;
        });
    }
    for(auto &th : threads) {
        th.join();
    }

    // Assertions on the main thread only (Catch2 macros are not thread-safe).
    for(double const s : results) {
        CHECK(s < 20.0); // every concurrent algorithm converged (well below the f=27 start)
    }
    CHECK(StcConsumer::instances_constructed().load() == 1); // one shared pool for all K algorithms

    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().clear();
}

/******************************************************************************/

TEST_CASE(
    "Meta-EA evaluates umbrella-individuals on its own pool; sub-EAs share the one work consumer",
    "[consumer][sharing][metaea]") {
    using StcConsumer = Gem::Courtier::GStdThreadConsumerT<gen::GGenome>;

    // The meta-EA evaluates its umbrella-individuals (MetaSphere -- each runs an inner EA) on its OWN
    // orchestration pool, NOT the work consumer. The inner EAs, un-injected, all converge on the one
    // shared work consumer. Because the orchestration pool and the work consumer are distinct pools, an
    // umbrella-individual blocking on its inner EA never starves the pool it runs on -- no deadlock. (A
    // plain EA here would instead evaluate the umbrellas on the work consumer, whose workers would then
    // block on inner work posted back to the same pool -- the deadlock the meta-EA exists to avoid.)
    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().clear();
    StcConsumer::instances_constructed().store(0);

    auto meta = std::make_shared<oa::GMetaEvolutionaryAlgorithm>();
    meta->setPopulationSizes(4, 2);
    meta->setMaxIteration(1);
    meta->setReportIteration(100000);
    meta->setNOrchestrationThreads(2);
    MetaSphere const src;
    meta->push_back(src.clone());
    meta->setAdaptionConfig(src.buildAdaptionConfig());
    meta->optimize();

    auto best = meta->getBestGlobalIndividual<MetaSphere>();
    REQUIRE(best); // completed without deadlock
    // Exactly one work consumer was built and shared by every inner EA; the meta-EA's own orchestration
    // pool is not a consumer, so it does not add to this count.
    CHECK(StcConsumer::instances_constructed().load() == 1);

    Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().clear();
}

/******************************************************************************/

TEST_CASE("buildConsumerSetup registers the process consumer", "[consumer][sharing][registry]") {
    using Registry = Gem::Courtier::GConsumerRegistryT<gen::GGenome>;

    auto &reg = Registry::instance();
    reg.clear();

    auto stc = Gem::Geneva::buildConsumerSetup(Gem::Geneva::ConsumerSpec{.mnemonic = "stc"});
    REQUIRE(stc.consumer);
    CHECK(reg.consumer() == stc.consumer); // registered, so un-injected algorithms submit through it

    // A later build replaces the single process consumer (the process has one work endpoint).
    auto stc2 = Gem::Geneva::buildConsumerSetup(
        Gem::Geneva::ConsumerSpec{.mnemonic = "stc", .n_threads = 1});
    REQUIRE(stc2.consumer);
    CHECK(stc2.consumer != stc.consumer);  // a genuinely new consumer was built
    CHECK(reg.consumer() == stc2.consumer);

    reg.clear();
}

// ---------------------------------------------------------------------------
// The consumer catalog is data in the shared provider store, not a hard-coded table + switch.

TEST_CASE("Consumer provider store exposes the built-in consumer catalog", "[consumer][sharing][registry]") {
    // The built-in local + socket consumers are always registered; unknown mnemonics are not (the retired
    // "sc" serial consumer is now "stc" with one thread, so "sc" is no longer a known mnemonic).
    CHECK(Gem::Geneva::isKnownConsumer("stc"));
    CHECK(Gem::Geneva::isKnownConsumer("asio"));
    CHECK(Gem::Geneva::isKnownConsumer("beast"));
    CHECK_FALSE(Gem::Geneva::isKnownConsumer("sc"));
    CHECK_FALSE(Gem::Geneva::isKnownConsumer("does_not_exist"));

    // The local consumer needs no networked client; the socket consumers do. An unknown mnemonic never does.
    CHECK_FALSE(Gem::Geneva::consumerNeedsClient("stc"));
    CHECK(Gem::Geneva::consumerNeedsClient("asio"));
    CHECK(Gem::Geneva::consumerNeedsClient("beast"));
    CHECK_FALSE(Gem::Geneva::consumerNeedsClient("does_not_exist"));

    // The listing names each registered consumer, and the count covers at least the three always-built ones.
    const std::string listing = Gem::Geneva::consumerListing();
    CHECK(listing.contains("stc:"));
    CHECK(listing.contains("asio:"));
    CHECK(listing.contains("beast:"));
    CHECK_FALSE(listing.contains("sc:"));
    CHECK(Gem::Geneva::consumerCount() >= 3);

    // The always-built consumers take their client/server role from --client (known before the consumer is
    // built), so they do NOT determine it at runtime. This is the generic capability generic drivers query
    // instead of naming a specific consumer. An unknown mnemonic is never role-at-runtime.
    CHECK_FALSE(Gem::Geneva::consumerDeterminesRoleAtRuntime("stc"));
    CHECK_FALSE(Gem::Geneva::consumerDeterminesRoleAtRuntime("asio"));
    CHECK_FALSE(Gem::Geneva::consumerDeterminesRoleAtRuntime("beast"));
    CHECK_FALSE(Gem::Geneva::consumerDeterminesRoleAtRuntime("does_not_exist"));
    // The MPI consumer, when built into this binary, self-assigns the role from its process rank.
    if(Gem::Geneva::isKnownConsumer("mpi")) {
        CHECK(Gem::Geneva::consumerDeterminesRoleAtRuntime("mpi"));
    }
}
