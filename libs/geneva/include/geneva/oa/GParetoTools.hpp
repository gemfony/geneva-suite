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

#include "common/GGlobalDefines.hpp"

// Standard headers go here
#include <algorithm>
#include <cstddef>
#include <limits>
#include <numeric>
#include <vector>

// Geneva headers go here
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/GenevaHelperFunctions.hpp" // isWorse(double, double, maxMode)
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * @brief Shared multi-objective (Pareto) ranking utilities, used by every optimization algorithm that
 * offers a Pareto / multi-objective selection mode (the evolutionary algorithm and the sep-CMA evolution
 * strategy today, and any future NSGA-II-style algorithm). Factored here so the dominance test and the
 * NSGA-II ranking exist exactly once.
 */

/******************************************************************************/
/**
 * @brief Strict Pareto dominance: `a` dominates `b` iff `a` is no worse than `b` on @e every fitness
 * criterion and strictly better on @e at least one. Uses each criterion's transformed fitness and the
 * individual's maxMode (so it is correct for both minimisation and maximisation).
 *
 * @param a The candidate dominator
 * @param b The candidate dominated individual
 * @return true iff `a` strictly Pareto-dominates `b`
 */
inline bool paretoDominates(
    const Gem::Geneva::Genome::GGenome &a,
    const Gem::Geneva::Genome::GGenome &b
) {
    const std::size_t n_crit = a.getNStoredResults();
    const maxMode m = a.getMaxMode();

    bool strictly_better_somewhere = false;
    for(std::size_t c = 0; c < n_crit; ++c) {
        const double va = a.transformed_fitness(c);
        const double vb = b.transformed_fitness(c);
        if(isWorse(va, vb, m)) {
            return false; // a is worse on some criterion -> cannot dominate
        }
        if(isWorse(vb, va, m)) {
            strictly_better_somewhere = true; // a is strictly better here
        }
    }
    return strictly_better_somewhere;
}

/******************************************************************************/
/**
 * @brief NSGA-II ranking (best-first): a fast non-dominated sort partitions the population into
 * non-domination fronts, and within each front the individuals are ordered by @e decreasing crowding
 * distance (boundary points get infinite crowding so they are always retained). Concatenating the fronts
 * yields a best-first order suitable as a selection / recombination key. (Deb, Pratap, Agarwal,
 * Meyarivan, "NSGA-II", IEEE Trans. Evol. Comput. 6(2), 2002.)
 *
 * @param pop The individuals to rank (non-owning pointers; must all expose the same number of criteria)
 * @return Indices into @p pop, ordered best-first
 */
namespace detail {

/** @brief nonDominatedRank() step 1: fast non-dominated sort into best-first non-domination fronts. */
inline std::vector<std::vector<std::size_t>> paretoFronts(
    const std::vector<const Gem::Geneva::Genome::GGenome *> &pop
) {
    const std::size_t sz = pop.size();
    std::vector<std::vector<std::size_t>> dominated(sz); // who each individual dominates
    std::vector<std::size_t> dom_count(sz, 0);           // how many dominate each individual
    std::vector<std::vector<std::size_t>> fronts(1);

    for(std::size_t p = 0; p < sz; ++p) {
        for(std::size_t q = 0; q < sz; ++q) {
            if(p == q) {
                continue;
            }
            if(paretoDominates(*pop[p], *pop[q])) {
                dominated[p].push_back(q);
            }
            else if(paretoDominates(*pop[q], *pop[p])) {
                ++dom_count[p];
            }
        }
        if(dom_count[p] == 0) {
            fronts[0].push_back(p);
        }
    }

    std::size_t fi = 0;
    while(fi < fronts.size() && not fronts[fi].empty()) {
        std::vector<std::size_t> next_front;
        for(std::size_t const p : fronts[fi]) {
            for(std::size_t const q : dominated[p]) {
                if(--dom_count[q] == 0) {
                    next_front.push_back(q);
                }
            }
        }
        if(next_front.empty()) {
            break;
        }
        fronts.push_back(std::move(next_front));
        ++fi;
    }

    return fronts;
}

/** @brief nonDominatedRank() step 2: append one front's indices to @p order, by decreasing crowding
 *  distance (boundary points get infinite crowding so they are always retained). */
inline void appendFrontByCrowding(
    const std::vector<const Gem::Geneva::Genome::GGenome *> &pop,
    const std::vector<std::size_t> &front,
    std::size_t n_crit,
    std::vector<std::size_t> &order
) {
    const std::size_t fs = front.size();
    std::vector<double> crowd(fs, 0.);
    for(std::size_t c = 0; c < n_crit; ++c) {
        // Sort this front by criterion c.
        std::vector<std::size_t> by_c(fs);
        std::iota(by_c.begin(), by_c.end(), 0);
        auto val = [&](std::size_t local) { return pop[front[local]]->transformed_fitness(c); };
        std::ranges::sort(by_c, std::ranges::less{}, val);
        // Boundary points get infinite crowding (always retained).
        crowd[by_c.front()] = std::numeric_limits<double>::infinity();
        crowd[by_c.back()] = std::numeric_limits<double>::infinity();
        const double span = val(by_c.back()) - val(by_c.front());
        if(span <= 0.) {
            continue;
        }
        for(std::size_t j = 1; j + 1 < fs; ++j) {
            crowd[by_c[j]] += (val(by_c[j + 1]) - val(by_c[j - 1])) / span;
        }
    }
    // Order this front by decreasing crowding distance.
    std::vector<std::size_t> local_order(fs);
    std::iota(local_order.begin(), local_order.end(), 0);
    std::ranges::sort(local_order, std::ranges::greater{}, [&](std::size_t i) { return crowd[i]; });
    for(std::size_t const local : local_order) {
        order.push_back(front[local]);
    }
}

} // namespace detail

inline std::vector<std::size_t> nonDominatedRank(
    const std::vector<const Gem::Geneva::Genome::GGenome *> &pop
) {
    const std::size_t sz = pop.size();
    const std::size_t n_crit = sz > 0 ? pop[0]->getNStoredResults() : 1;

    const std::vector<std::vector<std::size_t>> fronts = detail::paretoFronts(pop);

    std::vector<std::size_t> order;
    order.reserve(sz);
    for(const auto &front : fronts) {
        detail::appendFrontByCrowding(pop, front, n_crit, order);
    }
    return order;
}

/******************************************************************************/

} // namespace Gem::Geneva::OptimizationAlgorithms
