/**
 * @file GParameterObjectUsagePatterns.cpp
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

// Standard header files go here
#include <algorithm>
#include "weft/GArchivePolymorphic.hpp"
#include "common/GArchiveNamed.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <ranges>
#include <tuple>
#include <vector>

// Boost header files go here
#include <boost/json.hpp>

// Geneva header files go here
#include "common/GConfigEmission.hpp"
#include "common/GJsonIO.hpp"
#include "geneva/Go2.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"

using namespace Gem::Geneva;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

/******************************************************************************/
/**
 * A minimal flat individual, used here only to demonstrate genome inspection /
 * adaption. It carries no extra members, so the CRTP base GGenomeT
 * supplies clone_(); only a constructor (which authors the genome) and a trivial
 * evaluate() remain.
 */
class GDemoIndividual : public gen::GGenomeT<GDemoIndividual> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()

    friend struct Gem::Weft::access;

    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<gen::GGenomeT<GDemoIndividual>>(ar, "GGenomeT", *this);
    }

public:
    GDemoIndividual() {
        // The builder declares only the genome's STRUCTURE; the adaptors live on an OA-owned config
        // (getAdaptionConfig()), authored from the same parameters.
        gen::GGenomeBuilder b;
        b.addDoubleGroup(2, -10., 10.); // two constrained doubles sharing one adaption group
        b.addInt32(0, 0, 5);            // one constrained int32
        this->setGenome(b.build());
        this->randomInit(activityMode::ALLPARAMETERS);
    }

    /** @brief The OA-owned adaption config: a Gauss adaptor on the double group, a flip adaptor on the int. */
    //! [adaptors-mixed-channels]
    std::shared_ptr<oa::GAdaptionConfigBase> getAdaptionConfig() const {
        auto cfg = oa::makeAdaptionConfig<oa::GAdaptionConfigBase>(*this);
        cfg->groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
        cfg->groupInt32(0).flip(1.);
        return cfg;
    }
    //! [adaptors-mixed-channels]

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline(v);
        return {std::ranges::fold_left(
            v | std::views::transform([](double x) { return x * x; }), 0., std::plus{})};
    }
};

GEM_REGISTER_ARCHIVABLE(GDemoIndividual) // NOLINT


/******************************************************************************/
/**
 * Prints the shape of a freshly built genome: the per-channel value arrays and
 * the number of adaption groups registered for each channel.
 */
void printGenome(const std::string &title, const gen::GenomeData &g) {
    std::cout << title << '\n';
    std::cout << "  double values (" << g.dv.size() << "): ";
    for(double const v : g.dv) {
        std::cout << v << ' ';
    }
    std::cout << "\n  float  values (" << g.fv.size() << "): ";
    for(float const v : g.fv) {
        std::cout << v << ' ';
    }
    std::cout << "\n  int32  values (" << g.iv.size() << "): ";
    for(std::int32_t const v : g.iv) {
        std::cout << v << ' ';
    }
    std::cout << "\n  bool   values (" << g.bv.size() << "): ";
    for(std::uint8_t const v : g.bv) {
        std::cout << static_cast<int>(v) << ' ';
    }
    std::cout << "\n  adaption groups -> double: " << g.layout->d.groups.size()
              << ", float: " << g.layout->f.groups.size()
              << ", int32: " << g.layout->i.groups.size()
              << ", bool: " << g.layout->b.groups.size() << "\n\n";
}

/******************************************************************************/
/**
 * This example demonstrates how a flat genome is authored with the GGenomeBuilder
 * and how it is inspected. With the object-tree parameter hierarchy removed, all
 * parameters of an individual are declared once via the builder rather than by
 * pushing back individual parameter objects.
 */
// NOLINTNEXTLINE(readability-function-size) -- single demo main walking through the flat-genome usage patterns end to end (config emission, builder use, inspection)
int main(int argc, char **argv) {
    // --update-configs: materialize the one configuration file this demo owns (the GFunctionIndividual
    // factory's), then exit. Constructing the factory with GParserBuilder in update-in-place mode
    // creates/rewrites ./config/GFunctionIndividual.json from code defaults.
    if(Gem::Common::configEmissionRequested(argc, argv)) {
        Gem::Common::beginConfigEmission();
        gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json").get();
        Gem::Common::finishConfigEmission();
    }

    //===========================================================================
    // 1) A complete individual's genome, dumped to a JSON document.
    //    The factory produces an individual with a full flat genome from its JSON
    //    config; toJSON() serialises that genome's parameters.

    {
        std::shared_ptr<gind::GFunctionIndividualFactory> const gfi_ptr(
            new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
        );

        std::shared_ptr<gen::GGenome> const gfi_test = gfi_ptr->get();

        // Make sure the individual is "clean", i.e. the processed flag is set
        gfi_test->set_processing_status(Gem::Courtier::processingStatus::DO_PROCESS);
        gfi_test->process();

        boost::json::object doc;
        doc["parameterset"] = gfi_test->toJSON();
        Gem::Common::writeJsonFile("result.json", doc);

        // Now run this program and see the file "result.json" for the output
    }

    //===========================================================================
    // 2) Authoring patterns: STRUCTURE with the GGenomeBuilder, ADAPTORS on the config.
    //
    // The genome carries only structure now. The GGenomeBuilder declares the parameter groups and tunes
    // the init policy (.init / .perimeter / .adaptionMode); the *adaptors* live on an OA-owned
    // GAdaptionConfig built from the finished genome and authored through a fluent API that mirrors the
    // builder one-to-one (groupDouble(i).gauss(...) / groupInt32(i).flip(...) / forLabel(...)). The
    // optimization algorithm is then handed that config (via setAdaptionConfig / Go2::registerAdaptionConfig).
    //
    // Three group shapes exist per parameter type:
    //   addX(init[,min,max])      -- one parameter, its own adaption group
    //   addXGroup(n[,min,max])    -- n parameters sharing ONE adaption group (a collection)
    //   addXArray(n[,min,max])    -- n parameters, each its own adaption group

    { // Constrained doubles: a single scalar (group 0), a shared-sigma group (1), and an array (2 & 3).
        gen::GGenomeBuilder b;
        b.addDouble(1., -10., 10.);
        b.addDoubleGroup(3, -5., 5.);
        b.addDoubleArray(2, -1., 1.);
        auto genome = b.build();
        auto cfg = std::make_shared<oa::GAdaptionConfigBase>(*genome.layout);
        for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
            cfg->groupDouble(i).gauss(0.5, 0.8, 1e-3, 2., 1.); // the same Gauss config on each group
        }
        printGenome("Constrained doubles (single + group + array):", genome);
    }

    { // Unbounded ("plain") double collection: min/max set the random-init perimeter and the Gauss step
      // range, but are NOT constraints (no folding takes place).
        gen::GGenomeBuilder b;
        b.addDouble(0.5); // a bare unbounded scalar (init perimeter [0,1]) -- left un-adapted here
        b.addDoublePlainGroup(4, -10., 10.);
        auto genome = b.build();
        auto cfg = std::make_shared<oa::GAdaptionConfigBase>(*genome.layout);
        cfg->groupDouble(1).gauss(0.025, 0.1, 0., 1., 1.); // group 1 is the plain collection
        printGenome("Unbounded (plain) doubles:", genome);
    }

    { // Floats behave exactly like doubles, on their own value channel.
        gen::GGenomeBuilder b;
        b.addFloat(0.f, -2.f, 2.f);
        auto genome = b.build();
        auto cfg = std::make_shared<oa::GAdaptionConfigBase>(*genome.layout);
        cfg->groupFloat(0).gauss(0.5f, 0.8f, 1e-3f, 2.f, 1.f);
        printGenome("Constrained floats:", genome);
    }

    { // A bi-gaussian adaptor (two superimposed gaussians) on a constrained double.
        gen::GGenomeBuilder b;
        b.addDouble(0., -10., 10.);
        auto genome = b.build();
        auto cfg = std::make_shared<oa::GAdaptionConfigBase>(*genome.layout);
        cfg->groupDouble(0).biGauss(
            0.5, 0.8, 1e-3, 2.,  // sigma1 + self-adaption + bounds
            0.5, 0.8, 1e-3, 2.,  // sigma2 + self-adaption + bounds
            0.5, 0.8, 0., 2.,    // delta + self-adaption + bounds
            1.                   // ad_prob
        );
        printGenome("Constrained double with a bi-gaussian adaptor:", genome);
    }

    { // Integer parameters: an integer Gauss adaptor and a flip adaptor.
        gen::GGenomeBuilder b;
        b.addInt32(0, -100, 100);
        b.addInt32Group(3, 0, 9);
        auto genome = b.build();
        auto cfg = std::make_shared<oa::GAdaptionConfigBase>(*genome.layout);
        cfg->groupInt32(0).intGauss(2., 0.8, 1., 5., 1.);
        cfg->groupInt32(1).flip(1.);
        printGenome("Constrained int32 (gauss + flip):", genome);
    }

    { // Boolean parameters: a single switch and a group, both flip-adapted.
        gen::GGenomeBuilder b;
        b.addBool(true);
        b.addBoolGroup(4);
        auto genome = b.build();
        auto cfg = std::make_shared<oa::GAdaptionConfigBase>(*genome.layout);
        cfg->groupBool(0).flip(1.);
        cfg->groupBool(1).flip(0.5);
        printGenome("Booleans (single + group):", genome);
    }

    { // Per-group tuning: the structural policy (.init / .perimeter / adaptionMode(NEVER) -- a frozen,
      // "inactive" group) stays on the builder; the adaptor is authored on the config for the active groups.
        //! [problem-handle-tuning#1]
        gen::GGenomeBuilder b;
        b.addDoubleGroup(2, -10., 10.).init(3.);
        b.addDoubleGroup(2, -10., 10.).perimeter(-1., 1.);
        b.addDoubleGroup(2, -10., 10.).adaptionMode(adaptionMode::NEVER); // frozen parameters
        //! [problem-handle-tuning#1]
        //! [problem-handle-tuning#3]
        auto genome = b.build();
        //! [problem-handle-tuning#3]
        auto cfg = std::make_shared<oa::GAdaptionConfigBase>(*genome.layout);
        // Author the Gauss adaptor on every group; the frozen group (built adaptionMode::NEVER) is
        // inactive, so the adaption kernel skips it regardless.
        for(std::size_t i = 0; i < cfg->doubleGroups().size(); i++) {
            cfg->groupDouble(i).gauss(0.5, 0.8, 1e-3, 2., 1.);
        }
        printGenome("Per-group init / perimeter / frozen:", genome);
    }

    //===========================================================================
    // 3) Inspecting and mutating a flat individual.

    {
        GDemoIndividual ind;

        std::vector<double> dvals;
        ind.streamline(dvals);
        std::cout << "GDemoIndividual double values: ";
        for(double const v : dvals) {
            std::cout << v << ' ';
        }
        std::cout << '\n';

        std::vector<std::int32_t> ivals;
        ind.streamline(ivals);
        std::cout << "GDemoIndividual int32 values: ";
        for(std::int32_t const v : ivals) {
            std::cout << v << ' ';
        }
        std::cout << '\n';

        std::vector<double> lower;
        std::vector<double> upper;
        ind.boundaries(lower, upper);
        std::cout << "Double boundaries:";
        for(auto const& [lo, hi] : std::views::zip(lower, upper)) {
            std::cout << " [" << lo << ", " << hi << "]";
        }
        std::cout << '\n';

        // Mutate the parameters. The adaption state + logic are OA-owned; a standalone
        // individual drives them via a self-owned scratch + the config it authors (StandaloneAdapter).
        // We expect changes.
        oa::StandaloneAdapter(ind, ind.getAdaptionConfig()).adapt(ind);
        std::vector<double> after;
        ind.streamline(after);
        std::cout << "GDemoIndividual double values after adapt(): ";
        for(double const v : after) {
            std::cout << v << ' ';
        }
        std::cout << '\n';

        std::cout << "GDemoIndividual as CSV: " << ind.toCSV() << '\n';
    }

    //===========================================================================

    return 0;
}
