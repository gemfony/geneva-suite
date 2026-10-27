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

#include "geneva/GModuleLoader.hpp"

// Standard headers go here
#include <cstdint>
#include <mutex>
#include <sstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Boost headers go here
// Make Boost.DLL use std::filesystem / std::system_error (its `dll::fs` aliases) instead of
// boost::filesystem, so Geneva does not link libboost_filesystem for the one dll::fs::path we use.
#define BOOST_DLL_USE_STD_FS 1
#include <boost/dll/shared_library.hpp>

// Geneva headers go here
#include "common/GBuildFingerprint.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/GModuleManifest.hpp"
#include "common/GProviderT.hpp"
#include "geneva/oa/GFactoryStore.hpp" // oaFactoryStore() -- the OA-contribution registration target
#include "geneva/oa/GOAPlugin.hpp"     // GOAProviderPtr -- the OA contribution's void* holder type
#include "geneva/GMarshallerSetup.hpp" // marshallerProviderStore() -- the marshaller registration target
#include "geneva/GMarshallerPlugin.hpp" // GMarshallerProviderPtr -- the marshaller contribution's void* holder

namespace Gem::Geneva {

namespace {

/******************************************************************************/
/**
 * A process-lifetime store of loaded module handles. A loaded .so backs live objects AND their
 * GArchive type registrations, so it must never be unloaded while the program runs; the store
 * is therefore intentionally never cleared. Guarded by a mutex so concurrent loads are safe (loads are
 * rare -- once at startup -- so a plain mutex is ample).
 */
std::mutex g_module_mutex;

std::vector<boost::dll::shared_library> &keptModules_() {
    static std::vector<boost::dll::shared_library> libs;
    return libs;
}

/******************************************************************************/
/**
 * This host's own toolchain fingerprint, built from the compiling toolchain's predefined macros. Internal
 * linkage (a function-local static), so it is this translation unit's toolchain, never merged with a
 * module's copy under RTLD_GLOBAL.
 */
const GenevaCompat &hostCompat() {
    static const GenevaCompat c = GENEVA_BUILD_FINGERPRINT;
    return c;
}

const char *compilerFamilyName(std::uint32_t family) {
    switch(family) {
        case GENEVA_COMPILER_FAMILY_GNU: return "GNU";
        case GENEVA_COMPILER_FAMILY_CLANG: return "Clang";
        default: return "unknown-compiler";
    }
}

const char *stdlibFamilyName(std::uint32_t family) {
    switch(family) {
        case GENEVA_STDLIB_FAMILY_LIBSTDCXX: return "libstdc++";
        case GENEVA_STDLIB_FAMILY_LIBCXX: return "libc++";
        default: return "unknown-stdlib";
    }
}

/** @brief A one-line human description of a fingerprint, for the mismatch diagnostic. */
std::string describeCompat(const GenevaCompat &c) {
    std::ostringstream os;
    os << compilerFamilyName(c.compiler_family) << ' ' << c.compiler_major << '.' << c.compiler_minor
       << " / " << stdlibFamilyName(c.stdlib_family) << ' ' << c.stdlib_version << " / C++" << c.cxx_standard
       << " / cxx11abi=" << c.glibcxx_cxx11_abi << " / Boost " << (c.boost_version / 100000) << '.'
       << (c.boost_version / 100 % 1000) << '.' << (c.boost_version % 100) << " / Geneva "
       << c.geneva_version << " / abi_flags=0x" << std::hex << c.abi_flags;
    return os.str();
}

/**
 * @brief Returns an empty string if @p mod is ABI-compatible with @p host, otherwise a diagnostic naming
 * the first offending axis. Everything is matched exactly (Boost to major.minor); this is Track A's
 * exact-match policy, validated by PostgreSQL/nginx/kernel/Apache prior art.
 */
std::string compatMismatch(const GenevaCompat &host, const GenevaCompat &mod) {
    std::ostringstream os;
    auto axis = [&os](const char *name, unsigned long long modv, unsigned long long hostv) {
        os << "toolchain mismatch on " << name << ": plugin=" << modv << ", this Geneva=" << hostv;
    };
    if(mod.compiler_family != host.compiler_family) {
        os << "compiler family: plugin=" << compilerFamilyName(mod.compiler_family)
           << ", this Geneva=" << compilerFamilyName(host.compiler_family);
    } else if(mod.compiler_major != host.compiler_major) {
        axis("compiler major version", mod.compiler_major, host.compiler_major);
    } else if(mod.compiler_minor != host.compiler_minor) {
        axis("compiler minor version", mod.compiler_minor, host.compiler_minor);
    } else if(mod.stdlib_family != host.stdlib_family) {
        os << "standard-library family: plugin=" << stdlibFamilyName(mod.stdlib_family)
           << ", this Geneva=" << stdlibFamilyName(host.stdlib_family);
    } else if(mod.stdlib_version != host.stdlib_version) {
        axis("standard-library version", mod.stdlib_version, host.stdlib_version);
    } else if(mod.cxx_standard != host.cxx_standard) {
        axis("C++ standard", mod.cxx_standard, host.cxx_standard);
    } else if(mod.glibcxx_cxx11_abi != host.glibcxx_cxx11_abi) {
        axis("_GLIBCXX_USE_CXX11_ABI", mod.glibcxx_cxx11_abi, host.glibcxx_cxx11_abi);
    } else if((mod.boost_version / 100) != (host.boost_version / 100)) { // major.minor, ignore patch
        axis("Boost version", mod.boost_version, host.boost_version);
    } else if(mod.abi_flags != host.abi_flags) {
        axis("build-mode ABI flags (debug/sanitizer)", mod.abi_flags, host.abi_flags);
    } else if(mod.geneva_version != host.geneva_version) {
        axis("Geneva version", mod.geneva_version, host.geneva_version);
    } else {
        return {}; // compatible
    }
    return os.str();
}

/**
 * @brief dlopens @p path (RTLD_GLOBAL | RTLD_NOW) and keeps the handle resident for the process lifetime.
 * Caller must hold g_module_mutex. Returns a reference to the stored handle.
 */
boost::dll::shared_library &openAndKeep(const std::string &path_str) {
    namespace dll = boost::dll;
    dll::shared_library lib;
    try {
        // dll::fs::path is std::filesystem::path here (BOOST_DLL_USE_STD_FS); build it from the string.
        lib.load(dll::fs::path(path_str), dll::load_mode::rtld_global | dll::load_mode::rtld_now);
    }
    catch(const std::exception &e) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::openAndKeep(): Error!" << '\n'
            << "Could not load the module '" << path_str << "':" << '\n'
            << e.what() << '\n'
        );
    }
    keptModules_().push_back(std::move(lib));
    return keptModules_().back();
}

/**
 * @brief Validates a module's GenevaCompat against this host; throws with an axis-naming diagnostic on any
 * mismatch. Checks the struct-version/size envelope first (readable under any toolchain skew).
 */
void validateCompatOrThrow(const GenevaCompat &mod, const std::string &path_str) {
    if(mod.struct_version != GENEVA_COMPAT_STRUCT_VERSION ||
       mod.struct_size < sizeof(std::uint32_t) * 4) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::validateCompatOrThrow(): Error!" << '\n'
            << "The module '" << path_str << "' carries an incompatible GenevaCompat layout" << '\n'
            << "(struct_version=" << mod.struct_version << ", expected " << GENEVA_COMPAT_STRUCT_VERSION
            << "). Rebuild it against this Geneva." << '\n'
        );
    }
    const std::string mismatch = compatMismatch(hostCompat(), mod);
    if(not mismatch.empty()) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::validateCompatOrThrow(): Error!" << '\n'
            << "The module '" << path_str << "' is not ABI-compatible with this Geneva." << '\n'
            << mismatch << '\n'
            << "  plugin:      " << describeCompat(mod) << '\n'
            << "  this Geneva: " << describeCompat(hostCompat()) << '\n'
            << "Rebuild the module with the same toolchain as this Geneva." << '\n'
        );
    }
}

/**
 * @brief Validates the module-ABI stamp -- gate 2, run right after the toolchain gate and before any field
 * behind it is read. A mismatch means the module was built against a different version of
 * common/GModuleManifest.hpp: the struct layout, a contribution kind's number, or the C++ type a kind's
 * void* payload denotes may all differ, so every later field is untrustworthy.
 *
 * @param manifest The module's manifest (its compat gate has already passed)
 * @param path_str The module path, for the error text
 */
void validateAbiStampOrThrow(const GenevaModuleManifest &manifest, const std::string &path_str) {
    if(manifest.abi_version != GENEVA_MODULE_ABI_VERSION) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::validateAbiStampOrThrow(): Error!" << '\n'
            << "The module '" << path_str << "' was built against a different Geneva module ABI." << '\n'
            << "  module ABI version: " << manifest.abi_version << '\n'
            << "  this Geneva:        " << GENEVA_MODULE_ABI_VERSION << '\n'
            << "The manifest layout, the contribution-kind numbering or a kind's payload type differ, so"
            << " nothing beyond this point can be read safely. Rebuild the module against this Geneva."
            << '\n'
        );
    }
    if(manifest.manifest_size != static_cast<std::uint32_t>(sizeof(GenevaModuleManifest))) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::validateAbiStampOrThrow(): Error!" << '\n'
            << "The module '" << path_str << "' claims module ABI version " << manifest.abi_version
            << " but carries a manifest of " << manifest.manifest_size << " bytes, where this Geneva's is "
            << sizeof(GenevaModuleManifest) << "." << '\n'
            << "The ABI version was not bumped for a layout change (a Geneva bug) or the module was built"
            << " with mismatched headers. Rebuild the module against this Geneva." << '\n'
        );
    }
}

/** @brief The module, as the diagnostics name it: its path plus the self-description in its manifest.
 *  Only called once both gates have passed, so the two strings can be trusted.
 *  @param manifest The module's (fully gated) manifest
 *  @param path_str The module path
 *  @return A one-line human description, e.g. `'./libFoo.so' (module "Foo" 1.99.0-beta1)` */
std::string describeModule(const GenevaModuleManifest &manifest, const std::string &path_str) {
    std::ostringstream os;
    os << '\'' << path_str << '\'';
    if(manifest.module_name != nullptr) {
        os << " (module \"" << manifest.module_name << '"';
        if(manifest.module_version != nullptr) { os << ' ' << manifest.module_version; }
        os << ')';
    }
    return os.str();
}

/** @brief The shared open-and-validate prologue of openModule() and loadModule(): opens (and keeps)
 *  the shared object, requires the unified manifest entry point, and runs both gates -- the GenevaCompat
 *  toolchain fingerprint and the module-ABI stamp -- BEFORE any C++ contribution is touched. The caller
 *  must hold g_module_mutex.
 *  @param path_str The module path (as a string, for the error texts)
 *  @param caller The calling function's name, used in the error texts
 *  @return The module's (non-null, fully gated) manifest */
const GenevaModuleManifest *openModuleLocked(const std::string &path_str, std::string_view caller) {
    boost::dll::shared_library  const&lib = openAndKeep(path_str);

    if(not lib.has(Gem::Common::GENEVA_MODULE_MANIFEST_SYMBOL)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::" << caller << "(): Error!" << '\n'
            << "'" << path_str << "' is not a Geneva module: it does not export the '"
            << Gem::Common::GENEVA_MODULE_MANIFEST_SYMBOL << "' manifest entry point" << '\n'
            << "(it may predate the plugin scheme, or was built without a geneva_module_manifest()"
            << " entry point -- see Gem::Geneva::individualManifest() / oaManifest())." << '\n'
        );
    }
    const GenevaModuleManifest *manifest =
        lib.get<Gem::Common::geneva_module_manifest_fn>(Gem::Common::GENEVA_MODULE_MANIFEST_SYMBOL)();
    if(manifest == nullptr) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::" << caller << "(): Error!" << '\n'
            << "The module '" << path_str << "' returned a null manifest." << '\n'
        );
    }
    validateCompatOrThrow(manifest->compat, path_str);  // gate 1 -- before any C++ contribution is touched
    validateAbiStampOrThrow(*manifest, path_str);       // gate 2 -- before any field behind the stamp
    return manifest;
}

} // namespace

/******************************************************************************/

const GenevaCompat &thisHostCompat() {
    return hostCompat();
}

std::string moduleCompatMismatch(const GenevaCompat &moduleCompat) {
    return compatMismatch(hostCompat(), moduleCompat);
}

/******************************************************************************/

const GenevaModuleManifest *openModule(const std::filesystem::path &module_path) {
    const std::string path_str = module_path.string();
    std::scoped_lock const lock(g_module_mutex);

    return openModuleLocked(path_str, "openModule");
}

/******************************************************************************/

namespace {

/** @brief Moves one contribution's typed provider/factory out of the heap-allocated holder the
 *  module's make_factory() thunk hands back (the plain-C void* boundary), deleting the holder.
 *  Shared by every contribution kind; throws on a null or empty result.
 *  @tparam Ptr The typed smart-pointer holder the thunk allocates (e.g. GOAProviderPtr)
 *  @param contrib The manifest contribution whose factory thunk is invoked
 *  @param path_str The module path (for the error texts)
 *  @param noun What the contribution provides, for the error texts (e.g. "OA provider")
 *  @return The moved-out, non-empty smart pointer */
template <typename Ptr>
Ptr takeContribution(const GenevaContribution &contrib, const std::string &path_str, std::string_view noun) {
    void *raw = contrib.make_factory();
    if(raw == nullptr) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::loadModule(): Error!" << '\n'
            << "The module '" << path_str << "' produced a null " << noun << "." << '\n'
        );
    }
    // Adopt the heap-allocated holder into RAII immediately (Invariant 21), then move its payload out
    const std::unique_ptr<Ptr> holder(static_cast<Ptr *>(raw));
    Ptr result = std::move(*holder);
    if(not result) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::loadModule(): Error!" << '\n'
            << "The module '" << path_str << "' returned an empty " << noun << "." << '\n'
        );
    }
    return result;
}

/** @brief Registers one OA contribution's factory through registerOptimizationAlgorithm() -- the same seam
 *  a built-in algorithm's GInitializerT uses -- under the algorithm's own mnemonic. A mnemonic already
 *  held (a built-in or another module) is a hard error -- a module cannot shadow one. */
void registerOAContribution(const GenevaContribution &contrib, const std::string &path_str) {
    GOAProviderPtr const provider = takeContribution<GOAProviderPtr>(contrib, path_str, "OA provider");
    const std::string mnemonic = provider->getMnemonic();
    if(not registerOptimizationAlgorithm(provider)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::loadModule(): Error!" << '\n'
            << "The module '" << path_str << "' contributes an optimization algorithm with mnemonic '"
            << mnemonic << "'," << '\n'
            << "but that mnemonic is already registered (a built-in or another module owns it). A module"
            << " cannot shadow an existing algorithm; give the module's algorithm a distinct mnemonic." << '\n'
        );
    }
}

/** @brief Registers one MARSHALLER contribution's provider into marshallerProviderStore() under the
 *  marshaller's device target. A device target already held (a compiled-in marshaller or another
 *  module) is a hard error -- one problem per process means one marshaller per target. */
void registerMarshallerContribution(const GenevaContribution &contrib, const std::string &path_str) {
    GMarshallerProviderPtr const provider =
        takeContribution<GMarshallerProviderPtr>(contrib, path_str, "marshaller provider");
    const std::string device_target = provider->getMnemonic();
    if(not marshallerProviderStore()->setOnce(device_target, provider)) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::loadModule(): Error!" << '\n'
            << "The module '" << path_str << "' contributes a GPU marshaller for device target '"
            << device_target << "'," << '\n'
            << "but that target is already registered (a compiled-in marshaller or another module owns it)."
            << '\n'
            << "One problem per process means at most one marshaller per target." << '\n'
        );
    }
}

} // namespace

/******************************************************************************/

LoadedModule loadModule(const std::filesystem::path &module_path) {
    const std::string path_str = module_path.string();
    std::scoped_lock const lock(g_module_mutex);

    // Open + require-manifest + validate GenevaCompat, via the prologue shared with openModule();
    // then dispatch every contribution by kind.
    const GenevaModuleManifest *manifest = openModuleLocked(path_str, "loadModule");

    const std::string described = describeModule(*manifest, path_str);

    // A module that contributes nothing is a build accident (a manifest whose contribution list was never
    // filled in), not a legitimate no-op: say so rather than load it and let the caller fail later.
    if(manifest->contributions_count == 0 || manifest->contributions == nullptr) {
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In Gem::Geneva::loadModule(): Error!" << '\n'
            << "The module " << described << " contributes nothing: its manifest lists "
            << manifest->contributions_count << " contribution(s)"
            << (manifest->contributions == nullptr ? " and has no contribution array" : "") << "." << '\n'
            << "A module must carry at least one individual, optimization algorithm or marshaller -- see"
            << " Gem::Geneva::individualManifest() / oaManifest() / marshallerManifest()." << '\n'
        );
    }

    LoadedModule result;
    for(std::uint32_t i = 0; i < manifest->contributions_count; ++i) {
        const GenevaContribution &contrib = manifest->contributions[i];

        // A contribution without its factory thunk is a broken module, not something to skip silently: the
        // thing the manifest advertises would simply never appear, and the user would hunt for a missing
        // mnemonic with nothing to go on.
        if(contrib.make_factory == nullptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In Gem::Geneva::loadModule(): Error!" << '\n'
                << "Contribution " << i << " of the module " << described << " (kind " << contrib.kind
                << ", name '" << (contrib.name_or_mnemonic != nullptr ? contrib.name_or_mnemonic : "<null>")
                << "') has no factory entry point." << '\n'
                << "Its manifest entry was built by hand and left make_factory null; use the typed author"
                << " helpers (individualManifest() / oaManifest() / marshallerManifest()) instead." << '\n'
            );
        }

        switch(contrib.kind) {
            case GENEVA_CONTRIBUTION_INDIVIDUAL: {
                if(result.individual) {
                    throw geneva_exception(
                        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                        << "In Gem::Geneva::loadModule(): Error!" << '\n'
                        << "The module " << described << " contributes more than one individual;"
                        << " a module may contribute at most one (one problem per process)." << '\n'
                    );
                }
                result.individual =
                    takeContribution<GIndividualFactoryPtr>(contrib, path_str, "individual factory");
                break;
            }
            case GENEVA_CONTRIBUTION_OA: {
                registerOAContribution(contrib, path_str);
                ++result.oa_count;
                break;
            }
            case GENEVA_CONTRIBUTION_MARSHALLER: {
                registerMarshallerContribution(contrib, path_str);
                ++result.marshaller_count;
                break;
            }
            default:
                // Every other kind number -- the reserved-but-unwired ones and anything unknown -- is
                // refused rather than skipped. The ABI stamp above already guarantees the module and this
                // Geneva agree on what the numbers MEAN, so a kind arriving here is one this Geneva cannot
                // serve, and silently dropping it would leave the user's contribution missing without a word.
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In Gem::Geneva::loadModule(): Error!" << '\n'
                    << "Contribution " << i << " of the module " << described << " has kind "
                    << contrib.kind << " (name '"
                    << (contrib.name_or_mnemonic != nullptr ? contrib.name_or_mnemonic : "<null>")
                    << "'), which this Geneva cannot load." << '\n'
                    << "Supported kinds: " << GENEVA_CONTRIBUTION_INDIVIDUAL << " (individual), "
                    << GENEVA_CONTRIBUTION_OA << " (optimization algorithm), "
                    << GENEVA_CONTRIBUTION_MARSHALLER << " (GPU marshaller). Kinds "
                    << GENEVA_CONTRIBUTION_MONITOR << " (monitor) and " << GENEVA_CONTRIBUTION_CONSUMER
                    << " (consumer) are reserved and not yet wired." << '\n'
                );
        }
    }
    return result;
}

/******************************************************************************/

} /* namespace Gem::Geneva */
