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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <ranges>
#include <vector>


#include "common/GExceptions.hpp"
#include "weft/GArchivePolymorphic.hpp" // GEM_REGISTER_ARCHIVABLE + archive-generic dispatch
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/oa/GAdaption.hpp"
#include "geneva/oa/GAdaptionConfig.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Geneva;
using namespace Gem::Geneva::Genome;
namespace oa = Gem::Geneva::OptimizationAlgorithms;

namespace Gem::Tests {

/******************************************************************************/
/**
 * A mixed flat individual used to exercise the OA-owned adaption config + free functions: two FP groups
 * sharing the interned label "position", one unlabelled FP group, an int32 group with an integer Gauss
 * adaptor labelled "count", and a bool group with a flip adaptor.
 */
class AdaptCfgIndividual : public GGenomeT<AdaptCfgIndividual> {
public:
    using gemfony_flat_individual = void; // b2: genome-only flat leaf -- opt into GGenomeT's empty localMembers_()
public:
    AdaptCfgIndividual() { buildGenome(); }
    AdaptCfgIndividual(const AdaptCfgIndividual &) = default;

protected:
    std::vector<double> evaluate() override {
        std::vector<double> v;
        this->streamline<double>(v);
        return {std::ranges::fold_left(
            v | std::views::transform([](double x) { return x * x; }), 0., std::plus{})};
    }

private:
    void buildGenome() {
        // Structure + labels only; the adaptors are authored on the OA config (see authoredConfig()).
        GGenomeBuilder b;
        b.addDoubleGroup(3, -5., 5.).label("position"); // d group 0
        b.addDoubleGroup(2, -5., 5.).label("position"); // d group 1
        b.addDouble(0., -5., 5.);                       // d group 2 (unlabelled)
        b.addInt32Group(2, -10, 10).label("count");     // i group 0
        b.addBoolGroup(2);                              // b group 0
        this->setGenome(b.build());
    }

    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, [[maybe_unused]] const unsigned int version) {
        Gem::Common::archive_named_base<GGenomeT<AdaptCfgIndividual>>(ar, "GGenomeT", *this);
    }
};

} // namespace Gem::Tests

GEM_REGISTER_ARCHIVABLE(Gem::Tests::AdaptCfgIndividual) // NOLINT

using Gem::Tests::AdaptCfgIndividual;

/******************************************************************************/
/**
 * Builds the OA config for AdaptCfgIndividual: a Gauss adaptor (seed 0.5) on each of the three FP groups,
 * an integer Gauss adaptor on the int group and a flip adaptor on the bool group -- the settings the
 * genome formerly baked into its layout, now authored on the (structure-derived) config.
 */
static oa::GEAAdaptionConfig authoredConfig(const AdaptCfgIndividual &ind) {
    oa::GEAAdaptionConfig cfg(ind); // structure skeleton from the genome
    cfg.groupDouble(0).gauss(0.5, 0.8, 1e-3, 2., 1.);
    cfg.groupDouble(1).gauss(0.5, 0.8, 1e-3, 2., 1.);
    cfg.groupDouble(2).gauss(0.5, 0.8, 1e-3, 2., 1.);
    cfg.groupInt32(0).intGauss(0.5, 0.8, 1e-3, 2., 1.);
    cfg.groupBool(0).flip(1.0);
    return cfg;
}

/******************************************************************************/
TEST_CASE("GAdaptionConfig: built from a genome, addresses existing groups", "[flat][adaptcfg]") {
    AdaptCfgIndividual const ind;
    auto cfg = authoredConfig(ind);

    // Mirrors the genome's group structure.
    CHECK(cfg.doubleGroups().size() == 3);
    CHECK(cfg.int32Groups().size() == 1);
    CHECK(cfg.boolGroups().size() == 1);

    // Authoring a group that exists is fine; an out-of-range index throws.
    CHECK_NOTHROW(cfg.groupDouble(0).gauss(0.4, 0.8, 1e-3, 2., 1.));
    CHECK_THROWS_AS(cfg.groupDouble(3), geneva_exception);
    CHECK_THROWS_AS(cfg.groupInt32(5), geneva_exception);
}

/******************************************************************************/
TEST_CASE("GAdaptionConfig: label resolution is one-to-many", "[flat][adaptcfg]") {
    AdaptCfgIndividual const ind;
    auto cfg = authoredConfig(ind);

    // "position" tags the two FP groups; "count" the one int group; an absent label resolves to none.
    CHECK(cfg.groupsForLabel("position").size() == 2);
    CHECK(cfg.groupsForLabel("count").size() == 1);
    CHECK(cfg.groupsForLabel("absent").empty());

    // Authoring by an absent label throws; by a present one spans every tagged group.
    CHECK_THROWS_AS(cfg.forLabel("absent"), geneva_exception);
    CHECK_NOTHROW(cfg.forLabel("position").gauss(0.3, 0.8, 1e-3, 2., 1.));

    // The setting reached BOTH "position" groups (group 0 and group 1), not just the first.
    CHECK(cfg.doubleGroups()[0].start_sigma == 0.3);
    CHECK(cfg.doubleGroups()[1].start_sigma == 0.3);
    CHECK(cfg.doubleGroups()[2].start_sigma == 0.5); // the unlabelled group is untouched
}

/******************************************************************************/
TEST_CASE("GAdaptionConfig: checkConsistency accepts the authoring genome, rejects a different one", "[flat][adaptcfg]") {
    AdaptCfgIndividual const ind;
    auto cfg = authoredConfig(ind);

    // Same structure -> ok.
    CHECK_NOTHROW(cfg.checkConsistency(ind));

    // A structurally different genome (1 double, no other channels) -> throws.
    GGenomeBuilder other;
    other.addDouble(0., -1., 1.); // structure only
    std::shared_ptr<const GGenomeLayout> const other_layout = other.buildLayout();
    CHECK_THROWS_AS(cfg.checkConsistency(*other_layout), geneva_exception);
}

/******************************************************************************/
// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- one coherent stochastic robustness test: the fold/write-fold invariants over the SAME per-rep before/after state are tightly coupled and would only be scattered by splitting
TEST_CASE("GAdaption: runAdaptionKernels mutates within bounds (config-driven)", "[flat][adaptcfg]") {
    Gem::Hap::GRandomT<Gem::Hap::randomSource::QUEUE> gr;

    // Robust over repeats -- stochastic adaption must keep producing in-bounds changes.
    for(int rep = 0; rep < 20; ++rep) {
        AdaptCfgIndividual ind;
        auto cfg = authoredConfig(ind);

        // The per-group adaption STATE is OA-owned scratch (on the slot in a live run); here a standalone
        // store, seeded from the config.
        GAuxiliaryStore scratch;
        cfg.installInto(scratch);

        std::vector<double> before;
        ind.streamline<double>(before);

        std::size_t total = 0;
        for(int it = 0; it < 5; ++it) {
            total += oa::runAdaptionKernels(ind, scratch, cfg, gr);
        }
        CHECK(total > 0); // ad_prob == 1 over several iterations -> something adapts

        // The folded (external) double values stay strictly within [-5, 5).
        std::vector<double> after;
        ind.streamline<double>(after);
        REQUIRE(after.size() == before.size());
        for(double const x : after) {
            CHECK(x >= -5.);
            CHECK(x < 5.);
        }
        // Integer values fold into the closed range [-10, 10].
        std::vector<std::int32_t> iv;
        ind.streamline<std::int32_t>(iv);
        for(std::int32_t const x : iv) {
            CHECK(x >= -10);
            CHECK(x <= 10);
        }
        CHECK(before != after); // values actually moved

        // WRITE-FOLD invariant (normalized-genome architecture §2.2): the kernels add their step to the
        // raw internal value, then foldConstrainedValuesInPlace() folds each bounded value back into the
        // canonical internal interval [-0.5, 0.5). The external value is its affine image (box [-5, 5):
        // scale = 10, anchor = 0, so external == internal * 10). Verify the raw store is in the canonical
        // interval and maps to the external value.
        std::span<const double> const store_d = ind.internalDoubleValues();
        REQUIRE(store_d.size() == after.size());
        for(std::size_t k = 0; k < after.size(); ++k) {
            CHECK(store_d[k] >= -0.5);
            CHECK(store_d[k] < 0.5);
            const double tol = 8. * std::numeric_limits<double>::epsilon() * 10.;
            CHECK(std::abs(after[k] - store_d[k] * 10.) <= tol); // external == internal * scale (anchor 0)
        }
        std::span<const std::int32_t> const store_i = ind.internalInt32Values();
        REQUIRE(store_i.size() == iv.size());
        for(std::size_t k = 0; k < iv.size(); ++k) {
            CHECK(store_i[k] == iv[k]); // internal == external for ints too
        }
    }
}

/******************************************************************************/
TEST_CASE("GAdaption: readAdaptionSigmas + resetAdaptionState round-trip", "[flat][adaptcfg]") {
    Gem::Hap::GRandomT<Gem::Hap::randomSource::QUEUE> gr;

    AdaptCfgIndividual ind;
    auto cfg = authoredConfig(ind);

    // The per-group adaption STATE is OA-owned scratch (on the slot in a live run); here a standalone
    // store, seeded from the config.
    GAuxiliaryStore scratch;
    cfg.installInto(scratch);

    // One sigma per Gauss group (3 FP groups here); all start at the seed 0.5.
    std::vector<double> const sig0 = oa::readAdaptionSigmas(scratch, cfg, "GDoubleGaussAdaptor");
    REQUIRE(sig0.size() == 3);
    for(double const s : sig0) {
        CHECK(s == 0.5);
    }
    // The integer Gauss adaptor reports its single group's sigma too.
    CHECK(oa::readAdaptionSigmas(scratch, cfg, "GInt32GaussAdaptor").size() == 1);

    // Drive sigma away from its seed, then reset it back.
    bool moved = false;
    for(int it = 0; it < 50 && not moved; ++it) {
        oa::runAdaptionKernels(ind, scratch, cfg, gr);
        std::vector<double> const s = oa::readAdaptionSigmas(scratch, cfg, "GDoubleGaussAdaptor");
        for(double const v : s) {
            if(v != 0.5) {
                moved = true;
            }
        }
    }
    CHECK(moved); // sigma self-adapted away from the seed

    oa::resetAdaptionState(scratch, cfg);
    std::vector<double> const sig_reset = oa::readAdaptionSigmas(scratch, cfg, "GDoubleGaussAdaptor");
    REQUIRE(sig_reset.size() == 3);
    for(double const s : sig_reset) {
        CHECK(s == 0.5); // back to the configured seed
    }
}

/******************************************************************************/
TEST_CASE("GAdaptionConfig: installInto seeds the per-group state", "[flat][adaptcfg]") {
    AdaptCfgIndividual const ind;
    auto cfg = authoredConfig(ind);

    // Author a distinct seed, install it into a scratch store, and read it back through the sigma reader.
    cfg.groupDouble(0).gauss(1.25, 0.8, 1e-3, 2., 1.);
    GAuxiliaryStore scratch;
    cfg.installInto(scratch);

    std::vector<double> sig = oa::readAdaptionSigmas(scratch, cfg, "GDoubleGaussAdaptor");
    REQUIRE(sig.size() == 3);
    CHECK(sig[0] == 1.25); // the re-seeded group
    CHECK(sig[1] == 0.5);  // the others keep their original seed
    CHECK(sig[2] == 0.5);
}

/******************************************************************************/
// API-gap fix (2026-07-18): the label handle used to offer gauss/intGauss/flip/adaptionMode but
// silently missed biGauss, so a labelled bi-Gauss group was unauthorable by name. Pin the
// forwarding: biGauss through forLabel() must author every FP group carrying the label.
TEST_CASE("GAdaptionConfig: biGauss is authorable by label", "[flat][adaptcfg]") {
    AdaptCfgIndividual const ind;
    auto cfg = authoredConfig(ind);

    CHECK_NOTHROW(cfg.forLabel("position").biGauss(
        /*sigma1*/ 0.25, /*sigma_sigma1*/ 0.8, /*min_sigma1*/ 1e-3, /*max_sigma1*/ 2.,
        /*sigma2*/ 0.15, /*sigma_sigma2*/ 0.8, /*min_sigma2*/ 1e-3, /*max_sigma2*/ 2.,
        /*delta*/ 0.05, /*sigma_delta*/ 0.8, /*min_delta*/ 0., /*max_delta*/ 0.5,
        /*ad_prob*/ 1.
    ));

    // Both "position" groups became bi-Gauss groups with the requested start sigmas ...
    CHECK(cfg.doubleGroups()[0].has_bigauss);
    CHECK(cfg.doubleGroups()[1].has_bigauss);
    CHECK(cfg.doubleGroups()[0].start_sigma1 == 0.25);
    CHECK(cfg.doubleGroups()[1].start_sigma2 == 0.15);

    // ... and the unlabelled group is untouched.
    CHECK_FALSE(cfg.doubleGroups()[2].has_bigauss);
}
