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
 * @file
 * @brief The one place where Geneva's built-in optimization algorithms make themselves selectable.
 *
 * Every algorithm the library ships registers its factory here, at library-load time, through the same
 * function a runtime-loaded algorithm module goes through: Gem::Geneva::registerOptimizationAlgorithm().
 * There is no per-algorithm registration translation unit any more -- this file is the complete list of
 * mnemonics a stock Geneva offers, and it doubles as the build's algorithm inventory (the optional
 * algorithms appear here exactly under the GENEVA_ALGO_* switch that puts their sources into the library).
 *
 * Geneva is always built as a shared library, so these load-time initializers are never stripped.
 */

// Geneva headers go here
#include "geneva/oa/GInitializerT.hpp"
#include "geneva/oa/GOptimizationAlgorithmFactoryT.hpp"

#include "geneva/oa/GAntColonyOptimization.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "geneva/oa/GGeneralizedSimulatedAnnealing.hpp"
#include "geneva/oa/GSepCmaEvolutionStrategy.hpp"
#include "geneva/oa/GStandardPSO2011.hpp"

#ifdef GENEVA_ALGO_CGD
#include "geneva/oa/GConjugateGradientDescent.hpp"
#endif
#ifdef GENEVA_ALGO_NM
#include "geneva/oa/GNelderMead.hpp"
#endif
#ifdef GENEVA_ALGO_PS
#include "geneva/oa/GParameterScanFactory.hpp"
#endif
#ifdef GENEVA_ALGO_SA
#include "geneva/oa/GSimulatedAnnealing.hpp"
#endif
#ifdef GENEVA_ALGO_SWARM
#include "geneva/oa/GSwarmAlgorithm.hpp"
#endif

namespace Gem::Geneva::OptimizationAlgorithms {

namespace {

/******************************************************************************/
//! [algo-overview-registration]
// The algorithms that are always part of Geneva. Each factory is the GOptimizationAlgorithmFactoryT
// scaffold instantiated for one algorithm -- that instantiation is the whole factory, so it needs no
// name and no derived class of its own (the algorithm's own personality traits are the scaffold's
// default second argument).

const GInitializerT<GOptimizationAlgorithmFactoryT<GAntColonyOptimization>> g_aco_registrant;
//! [geneva.oa.registration#2]
const GInitializerT<GOptimizationAlgorithmFactoryT<GEvolutionaryAlgorithm>> g_ea_registrant;
//! [geneva.oa.registration#2]
const GInitializerT<GOptimizationAlgorithmFactoryT<GGeneralizedSimulatedAnnealing>> g_gsa_registrant;
const GInitializerT<GOptimizationAlgorithmFactoryT<GSepCmaEvolutionStrategy>> g_sepcma_registrant;
const GInitializerT<GOptimizationAlgorithmFactoryT<GStandardPSO2011>> g_pso_registrant;

/******************************************************************************/
// The algorithms a slim-core build may leave out. Each switch is set by the build for exactly the
// algorithms whose sources went into the library, so an omitted algorithm's mnemonic simply does not
// exist at runtime.

#ifdef GENEVA_ALGO_CGD
const GInitializerT<GOptimizationAlgorithmFactoryT<GConjugateGradientDescent>> g_cgd_registrant;
#endif
#ifdef GENEVA_ALGO_NM
const GInitializerT<GOptimizationAlgorithmFactoryT<GNelderMead>> g_nm_registrant;
#endif
#ifdef GENEVA_ALGO_PS
// The one algorithm whose factory genuinely adds something (the parameter-spec command-line option),
// so it is a class of its own rather than the bare scaffold instantiation.
//! [geneva.oa.registration#3]
const GInitializerT<GParameterScanFactory> g_ps_registrant;
//! [geneva.oa.registration#3]
#endif
#ifdef GENEVA_ALGO_SA
const GInitializerT<GOptimizationAlgorithmFactoryT<GSimulatedAnnealing>> g_sa_registrant;
#endif
#ifdef GENEVA_ALGO_SWARM
const GInitializerT<GOptimizationAlgorithmFactoryT<GSwarmAlgorithm>> g_swarm_registrant;
#endif
//! [algo-overview-registration]

/******************************************************************************/

} // anonymous namespace

} /* namespace Gem::Geneva::OptimizationAlgorithms */
