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

/**
 * @file
 * @brief The one manifest a runtime-loadable Geneva module exports.
 *
 * A module (an individual, an optimization algorithm, a monitor or a consumer) exports a single C entry
 * point, @c geneva_module_manifest(), returning a @c GenevaModuleManifest. The manifest carries the
 * toolchain-compatibility fingerprint (`GenevaCompat`, validated FIRST -- see GBuildFingerprint.hpp), the
 * module-ABI stamp (validated SECOND, see below) and a list of typed contributions. It is the ONLY module
 * convention the loader accepts (the earlier two-symbol individual convention has been retired).
 *
 * Everything crossing the module boundary here is **plain C** (fixed-width integers, `const char*`, and a
 * `void*(*)()` factory thunk): the loader must read @c compat before it can trust any C++ type, so the
 * manifest itself cannot depend on C++ layout. Each contribution's @c make_factory returns a `void*` that
 * the loader reinterprets per @c kind.
 *
 * @par The two things a module and its host must agree on
 * 1. **The toolchain** -- `GenevaCompat`, matched exactly on every axis including @c GENEVA_VERSION.
 * 2. **This file's own contract** -- the layout of @c GenevaModuleManifest / @c GenevaContribution, the
 *    meaning of the @c GENEVA_CONTRIBUTION_* numbers, and, per kind, the C++ type the @c make_factory
 *    thunk's @c void* actually points at. `GenevaCompat` does NOT cover any of that: a Geneva version is
 *    not bumped per commit, so during development a module and a host can carry the same
 *    @c GENEVA_VERSION and still disagree about this header. @c GENEVA_MODULE_ABI_VERSION closes that gap
 *    -- it is a plain counter, stamped into every manifest and matched exactly by the loader, and it is
 *    what turns such a disagreement into a diagnostic instead of a misread pointer.
 *
 * @par When to bump GENEVA_MODULE_ABI_VERSION
 * Whenever an already-built module could misunderstand a manifest this header produces, i.e. on any
 * change to the layout of the two structs; to the numeric value or meaning of a contribution kind;
 * or to the C++ type a kind's @c void* payload denotes or any class reachable from it (e.g.
 * redefining @c GOAProviderPtr, or changing the interface of the consumer provider a
 * @c GConsumerProviderPtr points at, or a type its functions take, such as @c ConsumerSpec). Also when
 * state that a module and its host must share moves from the module into a library of the
 * collection: an older module would keep its own copy next to the library's (version 5: the MPI
 * module's environment moved into libgemfony-courtier-mpi). Adding a NEW kind number does not
 * require a bump -- an older loader already rejects a kind it does not know. Before 2.0 bumping is
 * free (every module is rebuilt anyway); afterwards a bump is a major-release event, so the counter
 * is expected to move rarely and only alongside a deliberate module-ABI decision.
 */

// Global checks, defines and includes needed for all of Geneva
#include "common/GBuildFingerprint.hpp" // GenevaCompat + GENEVA_BUILD_FINGERPRINT

// Standard headers go here
#include <cstddef>
#include <cstdint>

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

/******************************************************************************/
/*
 * Contribution kinds. A module may contribute several things (e.g. an OA plus its personality traits plus a
 * bundled monitor, or an individual plus its GPU marshaller). INDIVIDUAL, OA, MARSHALLER and CONSUMER are wired
 * by the loader today; MONITOR is reserved and added when that category is modularised.
 */
#define GENEVA_CONTRIBUTION_INDIVIDUAL 1u
#define GENEVA_CONTRIBUTION_OA         2u
#define GENEVA_CONTRIBUTION_MONITOR    3u
#define GENEVA_CONTRIBUTION_CONSUMER   4u
#define GENEVA_CONTRIBUTION_MARSHALLER 5u

/*
 * The module-ABI version: this header's own contract (struct layouts, kind numbers, per-kind payload
 * types), stamped into every manifest and matched EXACTLY by the loader. See the file comment for the
 * bump rule. A plain counter -- it is not derived from GENEVA_VERSION and does not follow it.
 */
// A macro, not an enum: it is part of the plain-C contract of this header, like the kinds above.
// NOLINTNEXTLINE(cppcoreguidelines-macro-to-enum,modernize-macro-to-enum)
#define GENEVA_MODULE_ABI_VERSION 5u

/* Two-level stringization, and the Geneva version as a "MAJOR.MINOR.PATCH[-PRERELEASE]" string literal
 * (a module's own version string when it is a Geneva-shipped module). GENEVA_VERSION_PRERELEASE
 * (GGlobalDefines.hpp) carries its own leading dash and is empty for a final release. */
#define GENEVA_MODULE_STRINGIZE_(x) #x
#define GENEVA_MODULE_STRINGIZE(x)  GENEVA_MODULE_STRINGIZE_(x)
#define GENEVA_VERSION_STRING                                                                      \
    GENEVA_MODULE_STRINGIZE(GENEVA_VERSION_MAJOR)                                                  \
    "." GENEVA_MODULE_STRINGIZE(GENEVA_VERSION_MINOR) "." GENEVA_MODULE_STRINGIZE(                 \
        GENEVA_VERSION_PATCH                                                                       \
    ) GENEVA_VERSION_PRERELEASE

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One thing a module provides: a typed factory keyed by name/mnemonic. Plain C.
 *
 * @c subject is the last member, so that a hand-written contribution that names only the first
 * three leaves it null. Only a consumer contribution has a subject: the name of the item type its
 * provider distributes, as @c typeid(item type).name() gives it (consumerManifest<>() writes it).
 * The loader's cast of the factory's @c void* is unchecked, so a host refuses a consumer for
 * another item type before it calls the factory. Every other kind leaves @c subject null.
 */
typedef struct GenevaContribution {
    std::uint32_t kind;           /* GENEVA_CONTRIBUTION_*                                     */
    const char *name_or_mnemonic; /* NUL-terminated; the mnemonic/name the host resolves by    */
    void *(*make_factory)(void);  /* returns the base factory ptr for `kind` (loader casts it)  */
    const char *subject;          /* consumer: typeid(item type).name(); NULL for other kinds  */
} GenevaContribution;

/**
 * @brief The single manifest a module exports via geneva_module_manifest(). Plain C.
 *
 * Field order is load-bearing and must not be rearranged without a GENEVA_MODULE_ABI_VERSION bump: the
 * loader reads @c compat first (it is self-describing via its own struct_version/struct_size, so it can be
 * read under any toolchain skew), then the stamp fields at their fixed offset right behind it
 * (the ABI version first: only a module of this version is known to have the two sizes behind
 * it), and only once BOTH gates pass does it touch anything further along.
 */
typedef struct GenevaModuleManifest {
    GenevaCompat compat;         /* gate 1: the toolchain, validated before any C++   */
    std::uint32_t abi_version;   /* gate 2: == GENEVA_MODULE_ABI_VERSION              */
    std::uint32_t manifest_size; /* sizeof(GenevaModuleManifest), for the diagnostic  */
    // NOLINTNEXTLINE(readability-identifier-naming) -- the plain-C fields are snake_case, as above
    std::uint32_t contribution_size;         /* sizeof(GenevaContribution), checked with gate 2   */
    const char *module_name;                 /* diagnostics + (later) checkpoint self-description */
    const char *module_version;              /* the module's own version string                   */
    const GenevaContribution *contributions; /* array of length contributions_count               */
    std::uint32_t contributions_count;
} GenevaModuleManifest;

#ifdef __cplusplus
} /* extern "C" */
#endif

/*
 * The leading gate fields of a manifest, as an initializer prefix. Every manifest -- the ones the typed
 * author helpers build and the hand-written multi-contribution ones -- starts with this, so no author ever
 * spells the stamp (and cannot get it wrong or leave it stale):
 *
 *   static const GenevaModuleManifest manifest{
 *       GENEVA_MODULE_ABI_STAMP, "MyModule", "1.0.0", contributions, 2u};
 */
#define GENEVA_MODULE_ABI_STAMP                                                                    \
    GENEVA_BUILD_FINGERPRINT, GENEVA_MODULE_ABI_VERSION,                                           \
        (std::uint32_t)sizeof(GenevaModuleManifest), (std::uint32_t)sizeof(GenevaContribution)

namespace Gem::Common {

/** @brief The (unmangled) name of the manifest entry point the loader resolves. */
inline constexpr const char *GENEVA_MODULE_MANIFEST_SYMBOL = "geneva_module_manifest";

/** @brief Signature of the manifest entry point (used by the loader's typed symbol lookup). */
using geneva_module_manifest_fn = const GenevaModuleManifest *();

/******************************************************************************/
/**
 * @brief A structural, compile-time fixed string usable as a non-type template parameter (C++20/23).
 *
 * Lets the manifest author helpers (Geneva's individualManifest() / oaManifest(), for example) take
 * the config path and module/contribution name as template arguments (string literals), so a
 * loadable module's manifest is an ordinary typed C++ construct rather than a preprocessor macro.
 * Shared by every kind's helper.
 */
template <std::size_t N>
struct GFixedString {
    char value[N]{};
    // NOLINTNEXTLINE(google-explicit-constructor) -- implicit from a string literal is the whole point.
    constexpr GFixedString(const char (&str)[N]) {
        for(std::size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }
    [[nodiscard]] constexpr const char *c_str() const noexcept {
        return value;
    }
};

} // namespace Gem::Common

/******************************************************************************/
/*
 * A loadable module publishes its manifest by hand-writing the fixed entry point (no macro):
 *
 *   extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
 *       return someManifest<...>();  // a typed helper of the library that defines the contribution
 *   }
 *
 * The extern "C" wrapper is irreducible (the loader resolves the fixed, unmangled symbol via dlsym); the
 * manifest itself is built by a typed helper of the library that defines the contribution kind (Geneva's
 * individualManifest<Factory, "config path", "Name">() for an individual, for example). A pointer return
 * keeps the extern "C" free of the class-return warning.
 */

/******************************************************************************/
