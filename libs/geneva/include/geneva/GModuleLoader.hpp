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

// Standard headers go here
#include <cstdint>
#include <filesystem>
#include <string>

// Geneva headers go here
#include "common/GModuleManifest.hpp"       // GenevaModuleManifest (the unified module manifest)
#include "geneva/genome/GIndividualPlugin.hpp" // the individual plugin contract (entry-point names + factory type)

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief Opens a runtime Geneva module from @p module_path, validates its toolchain-compatibility
 * fingerprint (`GenevaCompat`) against this host, keeps it resident and returns its manifest.
 *
 * This is the low-level module primitive -- it dlopens and gates but does NOT dispatch any contribution;
 * @c loadModule() below builds on it. It:
 *  - loads the shared object with @c RTLD_GLOBAL (one symbol namespace: a single GArchive
 *    registry and single Geneva singletons) and @c RTLD_NOW (eager resolution);
 *  - resolves @c geneva_module_manifest() and runs the two gates **before touching any C++ contribution**:
 *    the @c GenevaCompat toolchain fingerprint (a compiler/stdlib/Boost/build-mode mismatch is rejected with
 *    a diagnostic naming the offending axis, rather than being mis-loaded and crashing later) and then the
 *    module-ABI stamp (@c GENEVA_MODULE_ABI_VERSION -- the module and this Geneva must agree on the manifest
 *    layout, the kind numbering and each kind's payload type);
 *  - keeps the library resident for the process lifetime (its code + GArchive registrations back
 *    live objects) and never unloads it.
 *
 * The returned manifest (and everything it points to) lives for the process lifetime.
 *
 * @param module_path The filesystem path to the module shared object
 * @return The module's manifest (never nullptr; throws on any failure, incl. a missing manifest)
 */
const GenevaModuleManifest *openModule(const std::filesystem::path &module_path);

/******************************************************************************/
/**
 * @brief What a loaded module contributed to this process.
 *
 * A module may carry several typed contributions. The store-backed kinds (optimization algorithms and GPU
 * marshallers today, consumers later) are registered into their process-global stores by @c loadModule() as a
 * side effect; the claim-once individual (at most one per process) is handed back here for the caller to claim.
 */
struct LoadedModule {
    /** @brief The module's individual content-creator factory, or null if it contributes no individual. */
    GIndividualFactoryPtr individual;
    /** @brief How many optimization algorithms the module registered into @c oaFactoryStore(). */
    std::uint32_t oa_count = 0;
    /** @brief How many GPU marshallers the module registered into @c marshallerProviderStore(). */
    std::uint32_t marshaller_count = 0;
};

/**
 * @brief Loads a runtime Geneva module and dispatches every contribution it carries by kind.
 *
 * Opens + compat-gates the module (see @c openModule()), then walks its manifest contributions:
 *  - an @c OA contribution is @c setOnce-registered into @c oaFactoryStore() under the algorithm's mnemonic
 *    (its personality nickname); a mnemonic a built-in or another module already holds is a hard error (a
 *    module cannot shadow one);
 *  - the (at most one) @c INDIVIDUAL contribution's factory is returned in @c LoadedModule::individual for
 *    the caller to claim (the single content-creator slot lives in Go2, not here);
 *  - a @c MARSHALLER contribution is registered into the marshaller provider store;
 *  - any other kind -- reserved-but-unwired (monitor / consumer) or unknown -- is REFUSED, naming the kind
 *    and the kinds this Geneva serves; nothing a module advertises is ever silently dropped.
 * Every other defect is a refusal too, each naming the module and what was wrong: no manifest entry point
 * (the former legacy two-symbol plugin fallback has been removed), a null manifest, a stale ABI stamp, an
 * empty contribution list, a contribution without its factory thunk, a factory that yields nothing, and a
 * mnemonic / device target a built-in or another module already holds.
 *
 * @param module_path The filesystem path to the module shared object
 * @return The module's contributions (individual + OA count); throws on any load/compat/collision failure
 */
LoadedModule loadModule(const std::filesystem::path &module_path);

/******************************************************************************/
/** @brief This host's own toolchain-compatibility fingerprint (built from the host's predefined macros).
 *  Exposed for diagnostics and tests. */
const GenevaCompat &thisHostCompat();

/**
 * @brief Returns an empty string if @p moduleCompat is ABI-compatible with this host, otherwise a
 * diagnostic naming the first offending axis. This is the exact check the loader applies to a module's
 * manifest before touching any of its C++ contributions; exposed so tests can verify the gate without a
 * shared object.
 */
std::string moduleCompatMismatch(const GenevaCompat &moduleCompat);

/******************************************************************************/

} /* namespace Gem::Geneva */
