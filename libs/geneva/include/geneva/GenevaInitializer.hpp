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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <exception>

// Boost header files go here

// Geneva headers go here
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GFactoryStore.hpp"
#include "geneva/oa/GInitializerT.hpp"
#include "hap/GRandomFactory.hpp"

namespace Gem::Geneva {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The start of Geneva in a program, and the one place that states in which order the
 * process-wide objects of the six libraries begin and end.
 *
 * A program holds one in main(); GOptimizer holds one as well (GOptimizer::gi_). Its constructor
 * makes sure the random-number factory exists, which hap's lifecycle guard has normally done before
 * main(); its destructor does nothing. It must not finalize the factory: it is not the factory's
 * only holder, and a finalized factory never hands out random numbers again.
 *
 * @par Start
 *  1. Before main(), while the shared libraries are initialized, each after those it depends on
 *     (weft, common, then hap, courtier and dietrich, then geneva, then the program): the
 *     archive registrations in weft's registries, the algorithms in oaFactoryStore(), hap's
 *     lifecycle guard, which creates the random-number factory, and the program's stamp.
 *  2. In main(), courtier's launcher (runOptimization(), GLauncherT::run()): the stop handlers and
 *     the fatal report, the modules (each registers its consumer providers and types), then the
 *     consumer, which it registers in GConsumerRegistryT.
 *  3. On first use: the logger, the recorder and its streams, the thread budget, the factory's
 *     producer threads (at the first draw), the lease pool of random-number proxies, the pools
 *     of the STAGED and QUARANTINE sources with their refill threads, the MPI environment, the
 *     training data of the neural-network individual.
 *
 * @par End
 *  1. In main(), when the launcher's run() ends: the consumer leaves the registry (its threads are
 *     joined when its last holder lets go), the recorder closes its streams. Then GOptimizer and
 *     this object end. The factory still runs: random numbers can be drawn after a run.
 *  2. After main() has returned, the statics made during main() end, the newest first: the lease
 *     pool (its proxies return their packages to the factory), the STAGED and QUARANTINE pools
 *     (their refill threads are joined), the consumer registry (with a consumer a program
 *     registered without the launcher, whose threads are joined here), the training data.
 *  3. Then the statics made before main(), the program's first and each library's before those
 *     of the libraries it depends on. hap's lifecycle guard ends after those of geneva: it
 *     finalizes the factory (closes its buffers, joins its producers and returns their
 *     reservation in the thread budget). Nothing draws random numbers after that. Then the
 *     factory itself is destroyed, when the guard lets go of it.
 *
 * @par Never destroyed
 * The logger, the recorder, the thread budget, the provider stores (algorithms, consumer providers,
 * marshallers, the option stores), weft's registries, the list of loaded modules (a module is never
 * unloaded, since objects built from its code live until the end) and the launcher's record of the
 * item type. Threads and destructors of the end above still use them; the operating system reclaims
 * them. The reason stands at each definition.
 *
 * A new process-wide object states at its definition why it is process-wide and where it falls in
 * this order. The test GLifetimeOrderTest runs this start and end once, in a process of its own,
 * and checks that the factory was destroyed and every reserved thread joined.
 */
class GenevaInitializer { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /** @brief Makes sure the random-number factory exists (see the class comment) */
    GenevaInitializer();

    /** @brief Does nothing; it does not finalize the factory (see the class comment) */
    ~GenevaInitializer();
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
