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
 * A lightweight runtime-init helper, retained for backward compatibility (Go2 holds one as a member,
 * and it may still be instantiated directly). It is now largely vestigial: the global random-number
 * factory's lifecycle is owned by a single process-lifetime guard in the hap library, which brings the
 * factory online before main() and finalizes it once after main() returns (see
 * GRandomFactoryLifecycleGuard in hap/src/GRandomFactory.cpp). GenevaInitializer therefore neither owns
 * nor tears down that shared factory; its constructor merely touches it (a no-op if already up) and its
 * destructor does nothing. Crucially it must NOT finalize the factory -- it is not the factory's
 * exclusive owner, so doing so would starve every later random-number consumer.
 */
class GenevaInitializer { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /**
     * @brief The default constructor; touches the global random-number factory.
     *
     * Ensures the shared factory exists (a no-op if the hap-library lifecycle guard already brought it
     * online, which it normally has by this point). It does not take ownership of the factory's lifetime.
     */
    GenevaInitializer();

    /**
     * @brief The destructor.
     *
     * Deliberately does NOT finalize the global random-number factory. That factory is a
     * process-global singleton shared by every Geneva facility and outlives any individual
     * GenevaInitializer (it is embedded in every Go2 as Go2::gi_); it is torn down once by its own
     * singleton destructor at process exit. Finalizing it here would permanently starve every later
     * random-number consumer. See the implementation comment for the full rationale.
     */
    ~GenevaInitializer();

    /***************************************************************************/
    // Note: optimization-algorithm factories register themselves with the global factory store at
    // library-load time (see the self-registration helpers in each factory's .cpp). Consumers are no
    // longer registered in a store -- they are built on demand by the courtier setup layer.
    // GenevaInitializer therefore exposes no registration API; it only performs the runtime
    // init / finalize of the random factory (see ctor / dtor).

    /***************************************************************************/
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
