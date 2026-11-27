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

#include "common/GModuleLoader.hpp"

// Standard headers go here
#include <cstdint>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Boost headers go here
// Make Boost.DLL use std::filesystem / std::system_error (its `dll::fs` aliases) instead of
// boost::filesystem, so the library does not link libboost_filesystem for the one dll::fs::path we
// use.
#define BOOST_DLL_USE_STD_FS 1 // NOLINT(cppcoreguidelines-macro-usage): Boost.DLL reads a macro
#include <boost/dll/shared_library.hpp>

// Geneva headers go here
#include "common/GBuildFingerprint.hpp"
#include "common/GChecks.hpp"

namespace Gem::Common {

namespace {

/******************************************************************************/
/**
 * A process-lifetime store of loaded module handles. A loaded .so backs live objects AND their
 * GArchive type registrations, so it must never be unloaded while the program runs; the store is
 * therefore intentionally never cleared. Guarded by a mutex so concurrent loads are safe (loads are
 * rare -- once at startup -- so a plain mutex is ample).
 *
 * Nor is it ever destroyed: a static vector would be torn down at exit and unload every module
 * while objects built from module code -- providers in the provider stores, the process consumer
 * and its threads -- are still alive in other statics, which would then run code that is no longer
 * mapped. Hence a deliberately leaked store; the operating system releases the modules with the
 * process.
 */
std::mutex moduleMutex; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

std::vector<boost::dll::shared_library> &keptModules() {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    static auto *libs = new std::vector<boost::dll::shared_library>();
    return *libs;
}

/******************************************************************************/
/**
 * This host's own toolchain fingerprint, built from the compiling toolchain's predefined macros.
 * Internal linkage (a function-local static), so it is this translation unit's toolchain, never
 * merged with a module's copy under RTLD_GLOBAL.
 */
const GemCompat &hostCompat() {
    static const GemCompat COMPAT = GEM_BUILD_FINGERPRINT;
    return COMPAT;
}

const char *compilerFamilyName(std::uint32_t family) {
    switch(family) {
    case GEM_COMPILER_FAMILY_GNU:
        return "GNU";
    case GEM_COMPILER_FAMILY_CLANG:
        return "Clang";
    default:
        return "unknown-compiler";
    }
}

const char *stdlibFamilyName(std::uint32_t family) {
    switch(family) {
    case GEM_STDLIB_FAMILY_LIBSTDCXX:
        return "libstdc++";
    case GEM_STDLIB_FAMILY_LIBCXX:
        return "libc++";
    default:
        return "unknown-stdlib";
    }
}

/** @brief A one-line human description of a fingerprint, for the mismatch diagnostic. */
std::string describeCompat(const GemCompat &c) {
    std::ostringstream os;
    os << compilerFamilyName(c.compiler_family) << ' ' << c.compiler_major << '.'
       << c.compiler_minor << " / " << stdlibFamilyName(c.stdlib_family) << ' ' << c.stdlib_version
       << " / C++" << c.cxx_standard << " / cxx11abi=" << c.glibcxx_cxx11_abi << " / Boost "
       << (c.boost_version / 100000) << '.' << (c.boost_version / 100 % 1000) << '.'
       << (c.boost_version % 100) << " / Geneva " << c.gem_version << " / abi_flags=0x" << std::hex
       << c.abi_flags;
    return os.str();
}

/**
 * @brief Returns an empty string if @p mod is ABI-compatible with @p host, otherwise a diagnostic
 * naming the first offending axis. Everything is matched exactly (Boost to major.minor); this is
 * Track A's exact-match policy, validated by PostgreSQL/nginx/kernel/Apache prior art.
 */
std::string compatMismatch(const GemCompat &host, const GemCompat &mod) {
    std::ostringstream os;
    auto axis = [&os](const char *name, unsigned long long modv, unsigned long long hostv) {
        os << "toolchain mismatch on " << name << ": plugin=" << modv << ", this program=" << hostv;
    };
    if(mod.compiler_family != host.compiler_family) {
        os << "compiler family: plugin=" << compilerFamilyName(mod.compiler_family)
           << ", this program=" << compilerFamilyName(host.compiler_family);
    }
    else if(mod.compiler_major != host.compiler_major) {
        axis("compiler major version", mod.compiler_major, host.compiler_major);
    }
    else if(mod.compiler_minor != host.compiler_minor) {
        axis("compiler minor version", mod.compiler_minor, host.compiler_minor);
    }
    else if(mod.stdlib_family != host.stdlib_family) {
        os << "standard-library family: plugin=" << stdlibFamilyName(mod.stdlib_family)
           << ", this program=" << stdlibFamilyName(host.stdlib_family);
    }
    else if(mod.stdlib_version != host.stdlib_version) {
        axis("standard-library version", mod.stdlib_version, host.stdlib_version);
    }
    else if(mod.cxx_standard != host.cxx_standard) {
        axis("C++ standard", mod.cxx_standard, host.cxx_standard);
    }
    else if(mod.glibcxx_cxx11_abi != host.glibcxx_cxx11_abi) {
        axis("_GLIBCXX_USE_CXX11_ABI", mod.glibcxx_cxx11_abi, host.glibcxx_cxx11_abi);
    }
    else if((mod.boost_version / 100) != (host.boost_version / 100)) { // major.minor, ignore patch
        axis("Boost version", mod.boost_version, host.boost_version);
    }
    else if(mod.abi_flags != host.abi_flags) {
        axis("build-mode ABI flags (debug/sanitizer)", mod.abi_flags, host.abi_flags);
    }
    else if(mod.gem_version != host.gem_version) {
        axis("Geneva version", mod.gem_version, host.gem_version);
    }
    else {
        return {}; // compatible
    }
    return os.str();
}

/**
 * @brief dlopens @p path (RTLD_GLOBAL | RTLD_NOW) and keeps the handle resident for the process
 * lifetime. Caller must hold moduleMutex. Returns a reference to the stored handle.
 */
boost::dll::shared_library &openAndKeep(const std::string &path_str) {
    boost::dll::shared_library lib;
    try {
        // dll::fs::path is std::filesystem::path here (BOOST_DLL_USE_STD_FS); build it from the
        // string.
        lib.load(
            boost::dll::fs::path(path_str),
            boost::dll::load_mode::rtld_global | boost::dll::load_mode::rtld_now
        );
    }
    catch(const std::exception &e) {
        throw gem_exception(
            g_error_streamer() << "Could not load the module '" << path_str << "':" << '\n'
                               << e.what() << '\n'
        );
    }
    keptModules().push_back(std::move(lib));
    return keptModules().back();
}

/**
 * @brief Validates a module's GemCompat against this host; throws with an axis-naming diagnostic
 * on any mismatch. Checks the struct-version/size envelope first (readable under any toolchain
 * skew).
 */
void validateCompatOrThrow(const GemCompat &mod, const std::string &path_str) {
    if(mod.struct_version != GEM_COMPAT_STRUCT_VERSION ||
       mod.struct_size < sizeof(std::uint32_t) * 4) {
        throw gem_exception(
            g_error_streamer() << "The module '" << path_str
                               << "' carries an incompatible GemCompat layout" << '\n'
                               << "(struct_version=" << mod.struct_version << ", expected "
                               << GEM_COMPAT_STRUCT_VERSION
                               << "). Rebuild it against the Geneva libraries this program uses."
                               << '\n'
        );
    }
    const std::string mismatch = compatMismatch(hostCompat(), mod);
    if(not mismatch.empty()) {
        throw gem_exception(
            g_error_streamer()
            << "The module '" << path_str << "' is not ABI-compatible with this program." << '\n'
            << mismatch << '\n'
            << "  plugin:       " << describeCompat(mod) << '\n'
            << "  this program: " << describeCompat(hostCompat()) << '\n'
            << "Rebuild the module with the same toolchain as the Geneva libraries this program"
            << " uses." << '\n'
        );
    }
}

/**
 * @brief Validates the module-ABI stamp -- gate 2, run right after the toolchain gate and before
 * any field behind it is read. A mismatch means the module was built against a different version of
 * common/GModuleManifest.hpp: the struct layout, a contribution kind's number, or the C++ type a
 * kind's void* payload denotes may all differ, so every later field is untrustworthy.
 *
 * @param manifest The module's manifest (its compat gate has already passed)
 * @param path_str The module path, for the error text
 */
void validateAbiStampOrThrow(const GemModuleManifest &manifest, const std::string &path_str) {
    if(manifest.abi_version != GEM_MODULE_ABI_VERSION) {
        throw gem_exception(
            g_error_streamer()
            << "The module '" << path_str << "' was built against a different Geneva module ABI."
            << '\n'
            << "  module ABI version: " << manifest.abi_version << '\n'
            << "  this program:       " << GEM_MODULE_ABI_VERSION << '\n'
            << "The manifest layout, the contribution-kind numbering or a kind's payload type"
            << " differ, so nothing beyond this point can be read safely. Rebuild the module"
            << " against the Geneva libraries this program uses." << '\n'
        );
    }
    if(manifest.manifest_size != static_cast<std::uint32_t>(sizeof(GemModuleManifest))) {
        throw gem_exception(
            g_error_streamer()
            << "The module '" << path_str << "' claims module ABI version " << manifest.abi_version
            << " but carries a manifest of " << manifest.manifest_size
            << " bytes, where this program's is " << sizeof(GemModuleManifest) << "." << '\n'
            << "The ABI version was not bumped for a layout change (a Geneva bug) or the module"
            << " was built with mismatched headers. Rebuild the module against the Geneva libraries"
            << " this program uses." << '\n'
        );
    }
    if(manifest.contribution_size != static_cast<std::uint32_t>(sizeof(GemContribution))) {
        throw gem_exception(
            g_error_streamer()
            << "The module '" << path_str << "' claims module ABI version " << manifest.abi_version
            << " but carries contributions of " << manifest.contribution_size
            << " bytes, where this program's are " << sizeof(GemContribution) << "." << '\n'
            << "The ABI version was not bumped for a layout change (a Geneva bug) or the module"
            << " was built with mismatched headers. Rebuild the module against the Geneva libraries"
            << " this program uses." << '\n'
        );
    }
}

/** @brief The module, as the diagnostics name it: its path plus the self-description in its
 *  manifest. Only called once both gates have passed, so the two strings can be trusted.
 *  @param manifest The module's (fully gated) manifest
 *  @param path_str The module path
 *  @return A one-line human description, e.g. `'./libFoo.so' (module "Foo" 1.99.0-beta1)` */
std::string describeModule(const GemModuleManifest &manifest, const std::string &path_str) {
    std::ostringstream os;
    os << '\'' << path_str << '\'';
    if(manifest.module_name != nullptr) {
        os << " (module \"" << manifest.module_name << '"';
        if(manifest.module_version != nullptr) {
            os << ' ' << manifest.module_version;
        }
        os << ')';
    }
    return os.str();
}

/** @brief The shared open-and-validate prologue of openModule() and loadModule(): opens (and keeps)
 *  the shared object, requires the unified manifest entry point, and runs both gates -- the
 *  GemCompat toolchain fingerprint and the module-ABI stamp -- BEFORE any C++ contribution is
 *  touched. The caller must hold moduleMutex.
 *  @param path_str The module path (as a string, for the error texts)
 *  @param caller The calling function's name, used in the error texts
 *  @return The module's (non-null, fully gated) manifest */
const GemModuleManifest *openModuleLocked(const std::string &path_str, std::string_view caller) {
    boost::dll::shared_library const &lib = openAndKeep(path_str);

    if(not lib.has(GEM_MODULE_MANIFEST_SYMBOL)) {
        throw gem_exception(
            g_error_streamer()
            << "In Gem::Common::" << caller << "(): Error!" << '\n'
            << "'" << path_str << "' is not a Geneva module: it does not export the '"
            << GEM_MODULE_MANIFEST_SYMBOL << "' manifest entry point" << '\n'
            << "(it may predate the module scheme, or was built without a gem_module_manifest()"
            << " entry point -- see common/GModuleManifest.hpp)." << '\n'
        );
    }
    const GemModuleManifest *manifest =
        lib.get<gem_module_manifest_fn>(GEM_MODULE_MANIFEST_SYMBOL)();
    if(manifest == nullptr) {
        throw gem_exception(
            g_error_streamer() << "In Gem::Common::" << caller << "(): Error!" << '\n'
                               << "The module '" << path_str << "' returned a null manifest."
                               << '\n'
        );
    }
    // Gate 1 -- before any C++ contribution is touched; gate 2 -- before any field behind the stamp
    validateCompatOrThrow(manifest->compat, path_str);
    validateAbiStampOrThrow(*manifest, path_str);
    return manifest;
}

/** @brief The kinds a caller accepts, as the messages list them: "\"<kind>\" (<noun>)", joined
 *  with ", ", in the order the handlers were given.
 *  @param handlers The caller's handlers
 *  @return The list, e.g. "\"mylib.solver\" (solver), \"mylib.reader\" (reader)" */
std::string describeKinds(std::span<const GContributionHandler> handlers) {
    std::ostringstream os;
    bool first = true;
    for(const GContributionHandler &handler : handlers) {
        if(not first) {
            os << ", ";
        }
        os << '"' << handler.kind << "\" (" << handler.noun << ")";
        first = false;
    }
    return os.str();
}

/** @brief Whether @p kind has the form "<library>.<kind>": a prefix, a dot and a name.
 *  @param kind A kind
 *  @return true if it has that form */
bool isPrefixedKind(std::string_view kind) {
    const std::size_t dot = kind.find('.');
    return dot != std::string_view::npos && dot != 0 && dot + 1 < kind.size();
}

/** @brief Refuses a list of handlers the loader cannot work with: none at all, one whose kind has
 *  no library prefix, one without a take function, or two for the same kind. Runs before the
 *  module is opened, because an opened module stays in the process.
 *  @param handlers The caller's handlers */
void validateHandlersOrThrow(std::span<const GContributionHandler> handlers) {
    checkThat(
        not handlers.empty(),
        "No contribution handler was given: a program states at the call which kinds of",
        " contribution it accepts."
    );
    std::set<std::string> kinds;
    for(const GContributionHandler &handler : handlers) {
        checkThat(
            isPrefixedKind(handler.kind),
            "The handler for the kind \"",
            handler.kind,
            "\" (",
            handler.noun,
            ") does not name it with the prefix of its library (\"<library>.<kind>\")."
        );
        checkThat(
            handler.take != nullptr,
            "The handler for kind \"",
            handler.kind,
            "\" (",
            handler.noun,
            ") has no take function."
        );
        checkThat(
            kinds.insert(handler.kind).second,
            "More than one handler was given for kind \"",
            handler.kind,
            "\"."
        );
    }
}

/** @brief The handler a caller gave for @p kind, or nullptr if it gave none.
 *  @param handlers The caller's handlers
 *  @param kind The kind of a contribution (not null)
 *  @return The handler, or nullptr */
const GContributionHandler *
handlerFor(std::span<const GContributionHandler> handlers, std::string_view kind) {
    for(const GContributionHandler &handler : handlers) {
        if(handler.kind == kind) {
            return &handler;
        }
    }
    return nullptr;
}

/** @brief A contribution's name for the messages; "<null>" if the manifest left it out.
 *  @param contribution The contribution
 *  @return Its name or mnemonic */
std::string_view contributionName(const GemContribution &contribution) {
    return contribution.name_or_mnemonic != nullptr ? contribution.name_or_mnemonic : "<null>";
}

} // namespace

/******************************************************************************/

const GemCompat &thisHostCompat() {
    return hostCompat();
}

std::string moduleCompatMismatch(const GemCompat &module_compat) {
    return compatMismatch(hostCompat(), module_compat);
}

/******************************************************************************/

const GemModuleManifest *openModule(const std::filesystem::path &module_path) {
    const std::string path_str = module_path.string();
    std::scoped_lock const lock(moduleMutex);

    return openModuleLocked(path_str, "openModule");
}

/******************************************************************************/

void loadModule(
    const std::filesystem::path &module_path,
    std::span<const GContributionHandler> handlers
) {
    // A mistake of the program is found before the module is opened: an opened module stays in the
    // process for good.
    validateHandlersOrThrow(handlers);

    const std::string path_str = module_path.string();
    std::scoped_lock const lock(moduleMutex);

    // Open + require-manifest + validate GemCompat and the ABI stamp, via the prologue shared
    // with openModule(); then hand every contribution to the handler of its kind.
    const GemModuleManifest *manifest = openModuleLocked(path_str, "loadModule");

    const std::string described = describeModule(*manifest, path_str);

    // A module that contributes nothing is a build accident (a manifest whose contribution list was
    // never filled in), not a legitimate no-op: say so rather than load it and let the caller fail
    // later.
    if(manifest->contributions_count == 0 || manifest->contributions == nullptr) {
        throw gem_exception(
            g_error_streamer()
            << "The module " << described << " contributes nothing: its manifest lists "
            << manifest->contributions_count << " contribution(s)"
            << (manifest->contributions == nullptr ? " and has no contribution array" : "") << "."
            << '\n'
            << "A module carries at least one contribution. This program accepts: "
            << describeKinds(handlers) << "." << '\n'
        );
    }

    // The contributions as a range; the manifest is a plain-C array of contributions_count entries.
    const std::span<const GemContribution> contributions(
        manifest->contributions,
        manifest->contributions_count
    );

    // First pass: every contribution is checked before anything of the module is called, so a
    // module with one defect that can be seen from the outside hands over nothing.
    for(std::size_t i = 0; i < contributions.size(); ++i) {
        const GemContribution &contribution = contributions[i];

        // A contribution without a kind cannot be given to anyone
        if(contribution.kind == nullptr) {
            throw gem_exception(
                g_error_streamer()
                << "Contribution " << i << " of the module " << described << " (name '"
                << contributionName(contribution) << "') names no kind." << '\n'
                << "This program accepts: " << describeKinds(handlers) << "." << '\n'
            );
        }

        // A contribution without its factory thunk is a broken module, not something to skip
        // silently: the thing the manifest advertises would simply never appear, and the user would
        // hunt for a missing mnemonic with nothing to go on.
        if(contribution.make_factory == nullptr) {
            throw gem_exception(
                g_error_streamer()
                << "Contribution " << i << " of the module " << described << " (kind \""
                << contribution.kind << "\", name '" << contributionName(contribution)
                << "') has no factory entry point." << '\n'
                << "Its manifest entry left make_factory null; the typed author helpers always"
                << " set it." << '\n'
            );
        }

        // A kind the caller gave no handler for is refused rather than skipped: dropping it would
        // leave the contribution missing without a word.
        const GContributionHandler *handler = handlerFor(handlers, contribution.kind);
        if(handler == nullptr) {
            throw gem_exception(
                g_error_streamer()
                << "Contribution " << i << " of the module " << described << " has the kind \""
                << contribution.kind << "\" (name '" << contributionName(contribution)
                << "'), which this program does not accept." << '\n'
                << "Supported kinds: " << describeKinds(handlers) << "." << '\n'
            );
        }

        if(handler->check) {
            handler->check(
                contribution,
                GContributionContext{.path = module_path, .description = described, .index = i}
            );
        }
    }

    // Second pass: every contribution is handed over.
    for(std::size_t i = 0; i < contributions.size(); ++i) {
        const GemContribution &contribution = contributions[i];
        handlerFor(handlers, contribution.kind)
            ->take(
                contribution,
                GContributionContext{.path = module_path, .description = described, .index = i}
            );
    }
}

/******************************************************************************/

} /* namespace Gem::Common */
