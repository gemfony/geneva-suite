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
 * Standalone exerciser for failed evaluations under the algorithms whose submission policy is
 * strict, and for the parents of the evolutionary algorithm. A run that cannot go on ends the
 * process through the consumer's fatal exit (std::exit), which Catch2 cannot observe in-process, so
 * each scenario runs as a subprocess and CTest matches its output (see the CMakeLists). Usage:
 *
 *   GFailedEvaluationExerciser --algorithm=<nm | cgd | ps | ea-parent> --retries=<n>
 *
 * The first evaluation fails once (for ea-parent: the first evaluation of a parent). With
 * max_failed_retries >= 1 it is evaluated again and the run completes; with 0 the run ends. The
 * evolutionary algorithm has no such setting: its parent may not be substituted, so the run ends.
 */

#include <atomic>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include "common/GFatalReport.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/GFaultInjector.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/oa/GAdaption.hpp" // makeAdaptionConfig
#include "geneva/oa/GAdaptionConfig.hpp"
#include "geneva/oa/GConjugateGradientDescent.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GNelderMead.hpp"
#include "geneva/oa/GParameterScan.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE

using namespace Gem::Geneva;
using namespace Gem::Geneva::Genome;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

namespace Gem::Tests {

/******************************************************************************/
/** A sphere over two doubles in [-5, 5), started at (1, 1). */
class FailedEvaluationSphere : public GGenomeT<FailedEvaluationSphere> {
public:
    using genome_only = void; // a genome-only flat leaf: GGenomeT's empty localMembers_()

    FailedEvaluationSphere() {
        GGenomeBuilder b;
        b.addDoubleGroup(2, -5., 5.).init(1.0);
        this->setGenome(b.build());
    }
    FailedEvaluationSphere(const FailedEvaluationSphere &) = default;

protected:
    /** @brief The sphere @return Its value */
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        return {std::ranges::fold_left(
            v | std::views::transform([](double x) { return x * x; }),
            0.,
            std::plus{}
        )};
    }

private:
    friend struct Gem::Weft::access;
    template <typename archive_type>
    void serialize(archive_type &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<FailedEvaluationSphere>>(ar, "GGenomeT", *this);
    }
};

/******************************************************************************/
/** Fails one evaluation: the first one, or the first of an item that may not be substituted. */
class FailOnce : public GFaultInjector {
public:
    /** @param parents_only Whether only an item that may not be substituted is failed */
    explicit FailOnce(bool parents_only)
      : parents_only_(parents_only) {
    }
    Fault evaluate(const GGenome &item, Gem::Hap::GRandomBase & /*gr*/) override {
        if(parents_only_ && item.isSubstitutable()) {
            return Fault::NONE;
        }
        return failed_.exchange(true) ? Fault::NONE : Fault::FLAG_ERROR;
    }
    /** @return Whether the failure was injected */
    [[nodiscard]] bool failed() const {
        return failed_.load();
    }

private:
    bool parents_only_;
    std::atomic<bool> failed_{false};
};

} // namespace Gem::Tests
GEM_REGISTER_ARCHIVABLE(Gem::Tests::FailedEvaluationSphere) // NOLINT

using Gem::Tests::FailedEvaluationSphere;
using Gem::Tests::FailOnce;

/******************************************************************************/
/**
 * Builds the algorithm of a scenario, short (Invariant 22).
 * @param algorithm The scenario's algorithm
 * @param retries The value of max_failed_retries
 * @return The algorithm, holding one individual; empty for an unknown name
 */
std::shared_ptr<oa::GOptimizationAlgorithmBase>
makeAlgorithm(const std::string &algorithm, std::size_t retries) {
    if(algorithm == "nm") {
        auto nm = std::make_shared<oa::GNelderMead>();
        nm->setMaxFailedRetries(retries);
        return nm;
    }
    if(algorithm == "cgd") {
        auto cgd = std::make_shared<oa::GConjugateGradientDescent>();
        cgd->setNStartingPoints(1);
        cgd->setMaxFailedRetries(retries);
        return cgd;
    }
    if(algorithm == "ps") {
        auto ps = std::make_shared<oa::GParameterScan>();
        ps->setParameterSpecs("d(0, -5., 5., 3), d(1, -5., 5., 3)");
        ps->setMaxFailedRetries(retries);
        return ps;
    }
    if(algorithm == "ea-parent") {
        auto ea = std::make_shared<oa::GEvolutionaryAlgorithm>();
        ea->setPopulationSizes(8, 2);
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(FailedEvaluationSphere());
        cfg->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        ea->setAdaptionConfig(cfg);
        return ea;
    }
    return {};
}

int main(int argc, char **argv) {
    Gem::Common::installFatalReport(); // an error nobody handles: its message and the exit code 1
    std::string algorithm;
    std::size_t retries = 0;
    Gem::Common::GParserBuilder gpb;
    gpb.registerCLParameter(
        "algorithm",
        algorithm,
        std::string(""),
        "the algorithm to run: nm | cgd | ps | ea-parent"
    );
    gpb.registerCLParameter(
        "retries",
        retries,
        std::size_t(0),
        "max_failed_retries of nm, cgd and ps"
    );
    if(gpb.parseCommandLine(argc, argv) == Gem::Common::GCL_HELP_REQUESTED) {
        return 0;
    }

    auto algo = makeAlgorithm(algorithm, retries);
    if(not algo) {
        std::cerr << "Unknown or missing --algorithm (nm | cgd | ps | ea-parent)\n";
        return 2;
    }
    algo->setMaxIteration(2);
    algo->setMaxStallIteration(0);
    algo->setReportIteration(100000);
    algo->setEmitTerminationReason(false);
    algo->push_back(FailedEvaluationSphere().clone());

    auto injector = std::make_unique<FailOnce>(algorithm == "ea-parent");
    const FailOnce *const watch = injector.get();
    GFaultInjectorRegistry::set(std::move(injector));
    algo->optimize();
    const bool failed = watch->failed(); // read before clearing: the registry owns the injector
    GFaultInjectorRegistry::clear();

    std::cout << "The run completed" << (failed ? " after one failed evaluation" : "") << ".\n";
    return 0;
}
