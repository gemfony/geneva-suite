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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Geneva header files go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GModuleManifest.hpp"

namespace Gem::Common {

/******************************************************************************/
/**
 * @brief Opens a runtime module from @p module_path, validates its toolchain-compatibility
 * fingerprint (`GenevaCompat`) against this program, keeps it resident and returns its manifest.
 *
 * This is the low-level module primitive -- it dlopens and gates but does NOT hand any contribution
 * to anyone; @c loadModule() below builds on it. It:
 *  - loads the shared object with @c RTLD_GLOBAL (one symbol namespace: a single GArchive
 *    registry and single library singletons) and @c RTLD_NOW (eager resolution);
 *  - resolves @c geneva_module_manifest() and runs the two gates **before touching any C++
 *    contribution**: the @c GenevaCompat toolchain fingerprint (a compiler/stdlib/Boost/build-mode
 *    mismatch is rejected with a diagnostic naming the offending axis, rather than being mis-loaded
 *    and crashing later) and then the module-ABI stamp (@c GENEVA_MODULE_ABI_VERSION -- the module
 *    and this program must agree on the manifest layout, the kind numbering and each kind's payload
 *    type);
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
/** @brief What the loader tells a handler about the module a contribution comes from. */
struct GContributionContext {
    std::filesystem::path path; ///< The path the module was loaded from
    std::string description;    ///< The module as messages name it: path, name and version
    std::size_t index = 0;      ///< The position of the contribution in the module's manifest
};

/******************************************************************************/
/**
 * @brief What a program does with the contributions of one kind.
 *
 * The loader calls check() for every contribution of a module before it calls take() for any
 * of them. check() refuses (throws) what cannot be taken and must not call the contribution's
 * make_factory; it may be empty. take() calls make_factory (through takeContribution()) and
 * takes the result over; it must not be empty.
 *
 * The loader holds its lock while a handler runs, and the lock is not recursive: neither
 * function may call openModule() or loadModule(). Such a call would wait for the lock its own
 * caller holds.
 */
struct GContributionHandler {
    /// The signature of check and take
    using function_type =
        std::function<void(const GenevaContribution &, const GContributionContext &)>;

    std::uint32_t kind = 0; ///< The kind this handler serves (GENEVA_CONTRIBUTION_*)
    std::string noun;       ///< What a contribution of the kind is, for messages ("consumer")
    function_type check;    ///< Refuses a contribution without calling into the module
    function_type take;     ///< Calls the contribution's factory and takes the result over
};

/******************************************************************************/
/**
 * @brief Loads a module and hands each of its contributions to the handler of its kind.
 *
 * The caller states at the call which kinds it accepts. The module is opened and gated as by
 * openModule(). Then, in the order of the manifest:
 *   1. every contribution is checked: it has a factory entry point, a handler for its kind was
 *      given, and that handler's check() accepts it. Nothing of the module is called;
 *   2. take() of its handler is called for every contribution.
 * A module with a contribution that fails a check of 1. is refused as a whole: nothing of it was
 * taken. A take() that throws ends the load; what earlier contributions of the same module
 * handed over stays (a mnemonic that is already in use is found there, because only the object
 * the factory returns knows its mnemonic).
 *
 * May be called from several threads; the calls run one after the other. See
 * GContributionHandler for what a handler must not do.
 *
 * @param module_path The module (a shared object)
 * @param handlers One handler per kind the caller accepts; at least one, no kind twice, each
 *        with a take function. A std::array or std::vector of handlers converts to the span.
 * @throws geneva_exception for unusable handlers (before the module is opened), for a module
 *         that cannot be opened or does not pass a gate, for a manifest without contributions,
 *         and for a contribution that is refused; whatever a handler throws passes through
 */
void loadModule(
    const std::filesystem::path &module_path,
    std::span<const GContributionHandler> handlers
);

/******************************************************************************/
/**
 * @brief For a handler's take(): calls the factory of @p contribution and moves the result out
 * of the holder the module allocated.
 *
 * The cast from the factory's void * is unchecked: @p ptr_type must be exactly the type the module's
 * author helper allocates for the kind. The module ABI version is what guards that.
 *
 * @tparam ptr_type The smart-pointer type the factory allocates (e.g. a shared pointer to a provider)
 * @param contribution The contribution whose factory is called
 * @param context The module, for the messages
 * @param noun What the contribution is, for the messages
 * @return The pointer the module handed over; never empty
 * @throws geneva_exception if the factory returns null or an empty pointer
 */
template <typename ptr_type>
[[nodiscard]] ptr_type takeContribution(
    const GenevaContribution &contribution,
    const GContributionContext &context,
    std::string_view noun
) {
    void *raw = contribution.make_factory();
    if(raw == nullptr) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Common::takeContribution(): Error!" << '\n'
            << "The module " << context.description << " produced a null " << noun
            << " (contribution " << context.index << ")." << '\n'
        );
    }
    // Adopt the heap-allocated holder into RAII immediately (Invariant 21), then move its payload
    // out
    const std::unique_ptr<ptr_type> holder(static_cast<ptr_type *>(raw));
    ptr_type result = std::move(*holder);
    if(not result) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Common::takeContribution(): Error!" << '\n'
            << "The module " << context.description << " returned an empty " << noun
            << " (contribution " << context.index << ")." << '\n'
        );
    }
    return result;
}

/******************************************************************************/
/** @brief This program's own toolchain-compatibility fingerprint (built from the host's predefined
 *  macros). Exposed for diagnostics and tests. */
const GenevaCompat &thisHostCompat();

/**
 * @brief Returns an empty string if @p module_compat is ABI-compatible with this program, otherwise
 * a diagnostic naming the first offending axis. This is the exact check the loader applies to a
 * module's manifest before touching any of its C++ contributions; exposed so tests can verify the
 * gate without a shared object.
 */
std::string moduleCompatMismatch(const GenevaCompat &module_compat);

/******************************************************************************/

} /* namespace Gem::Common */
