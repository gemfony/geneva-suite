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
 * @brief The Geneva toolchain-compatibility fingerprint (`GemCompat`).
 *
 * A runtime-loadable module (in Geneva: an individual, an optimization algorithm, a consumer) is an
 * in-process C++ shared object that shares vtables, inline code and the GArchive type registry with
 * the host. Two such `.so`s are only ABI-compatible if they were built with a compatible toolchain:
 * same compiler family/version, same standard library, same `_GLIBCXX_USE_CXX11_ABI`, same Boost,
 * same build-mode ABI switches. The plugin loader historically compared only @c GEM_VERSION,
 * which does NOT capture any of that -- so a module built with a different toolchain but the same
 * Geneva version passed the gate and then crashed or silently corrupted the process.
 *
 * `GemCompat` closes that hole. It is a **plain-C struct** (fixed-width integers only) so that its
 * layout is readable under exactly the toolchain skew it exists to detect -- the loader must read it
 * BEFORE it can trust any C++ type from the module. It is filled entirely from predefined compiler/library
 * macros via the @c GEM_BUILD_FINGERPRINT initializer, never from hand-written literals, so no field can
 * silently go stale: the host and every module compile the same initializer under their own toolchain, and
 * a mismatch is detected structurally.
 *
 * The design mirrors long-proven prior art: PostgreSQL's @c PG_MODULE_MAGIC, nginx's module signature, the
 * Linux kernel's @c vermagic and Apache's Module Magic Number all gate module loading on discrete
 * ABI-relevant build knobs, matched exactly.
 *
 * @note The @c GEM_BUILD_FINGERPRINT macro is used to INITIALIZE a static instance at the point where a
 * module (or the host) defines its manifest. It must never be wrapped in a vague-linkage inline helper: an
 * @c inline function compiled into both the host and a module could be merged to a single definition under
 * @c RTLD_GLOBAL, which would make a module report the host's fingerprint instead of its own. Initialize a
 * @c static (internal-linkage) or module-owned object from the macro instead.
 */

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp" // GEM_VERSION, and (transitively) BOOST_VERSION

// Standard headers go here
#include <cstdint>

/******************************************************************************/
/*
 * Compat struct version. Bump ONLY when the layout of GemCompat itself changes; the leading
 * struct_version / struct_size fields let a loader read this much under any toolchain skew and tolerate a
 * module built against a later (larger) struct.
 */
#define GEM_COMPAT_STRUCT_VERSION 1u

/*
 * Enumerated axis values. Discrete integers (never free-form strings) so the loader compares fields, not
 * parses text.
 */
#define GEM_COMPILER_FAMILY_UNKNOWN 0u
#define GEM_COMPILER_FAMILY_GNU     1u
#define GEM_COMPILER_FAMILY_CLANG   2u

#define GEM_STDLIB_FAMILY_UNKNOWN   0u
#define GEM_STDLIB_FAMILY_LIBSTDCXX 1u
#define GEM_STDLIB_FAMILY_LIBCXX    2u

/*
 * abi_flags bitfield -- build-mode ABI switches. _GLIBCXX_DEBUG / _GLIBCXX_ASSERTIONS change standard
 * container layout outright; ASan / TSan instrumentation is not link-compatible with an uninstrumented
 * peer. UBSan stays link-compatible, so its bit is conservative and only ever set via the CMake -D path
 * (there is no predefined UBSan macro on GCC or Clang).
 */
#define GEM_ABI_FLAG_GLIBCXX_DEBUG      0x1u
#define GEM_ABI_FLAG_GLIBCXX_ASSERTIONS 0x2u
#define GEM_ABI_FLAG_ASAN               0x4u
#define GEM_ABI_FLAG_TSAN               0x8u
#define GEM_ABI_FLAG_UBSAN              0x10u

/******************************************************************************/
/*
 * The plain-C compatibility fingerprint. extern "C" so the tag type has C linkage and an identical layout
 * on both sides of a module boundary. Field order is frozen within GEM_COMPAT_STRUCT_VERSION.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct GemCompat {
    std::uint32_t struct_version;  /* == GEM_COMPAT_STRUCT_VERSION                         */
    std::uint32_t struct_size;     /* sizeof(GemCompat), for forward tolerance             */
    std::uint32_t gem_version;     /* GEM_VERSION baked in at build time                   */
    std::uint32_t compiler_family; /* GEM_COMPILER_FAMILY_*                                */
    std::uint32_t compiler_major;
    std::uint32_t compiler_minor;
    std::uint32_t stdlib_family;     /* GEM_STDLIB_FAMILY_*                                     */
    std::uint32_t stdlib_version;    /* _GLIBCXX_RELEASE or _LIBCPP_VERSION                     */
    std::uint32_t cxx_standard;      /* 20, 23, ... (mapped from __cplusplus)                   */
    std::uint32_t glibcxx_cxx11_abi; /* _GLIBCXX_USE_CXX11_ABI (0/1); 0 when not libstdc++      */
    std::uint32_t boost_version;     /* BOOST_VERSION                                           */
    std::uint32_t abi_flags;         /* GEM_ABI_FLAG_* bitfield                                 */
} GemCompat;

#ifdef __cplusplus
} /* extern "C" */
#endif

/******************************************************************************/
/*
 * Per-field derivation from predefined macros. Each expands to a plain unsigned integer literal at the
 * point of use, so GEM_BUILD_FINGERPRINT captures the fingerprint of whichever toolchain compiles the
 * translation unit that instantiates it.
 */

/* Compiler family + version. Check __clang__ FIRST: clang also defines __GNUC__. */
#if defined(__clang__)
#define GEM_FP_COMPILER_FAMILY GEM_COMPILER_FAMILY_CLANG
#define GEM_FP_COMPILER_MAJOR  ((std::uint32_t)__clang_major__)
#define GEM_FP_COMPILER_MINOR  ((std::uint32_t)__clang_minor__)
#elif defined(__GNUC__)
#define GEM_FP_COMPILER_FAMILY GEM_COMPILER_FAMILY_GNU
#define GEM_FP_COMPILER_MAJOR  ((std::uint32_t)__GNUC__)
#define GEM_FP_COMPILER_MINOR  ((std::uint32_t)__GNUC_MINOR__)
#else
#define GEM_FP_COMPILER_FAMILY GEM_COMPILER_FAMILY_UNKNOWN
#define GEM_FP_COMPILER_MAJOR  0u
#define GEM_FP_COMPILER_MINOR  0u
#endif

/* Standard library family + version. */
#if defined(_LIBCPP_VERSION)
#define GEM_FP_STDLIB_FAMILY  GEM_STDLIB_FAMILY_LIBCXX
#define GEM_FP_STDLIB_VERSION ((std::uint32_t)_LIBCPP_VERSION)
#elif defined(__GLIBCXX__)
#define GEM_FP_STDLIB_FAMILY GEM_STDLIB_FAMILY_LIBSTDCXX
#if defined(_GLIBCXX_RELEASE)
#define GEM_FP_STDLIB_VERSION ((std::uint32_t)_GLIBCXX_RELEASE)
#else
#define GEM_FP_STDLIB_VERSION 0u
#endif
#else
#define GEM_FP_STDLIB_FAMILY  GEM_STDLIB_FAMILY_UNKNOWN
#define GEM_FP_STDLIB_VERSION 0u
#endif

/* libstdc++ dual-ABI switch (irrelevant / absent under libc++). */
#if defined(_GLIBCXX_USE_CXX11_ABI)
#define GEM_FP_GLIBCXX_CXX11_ABI ((std::uint32_t)_GLIBCXX_USE_CXX11_ABI)
#else
#define GEM_FP_GLIBCXX_CXX11_ABI 0u
#endif

/* C++ standard, mapped from __cplusplus to a plain year-based number (20, 23, ...). */
#if __cplusplus >= 202302L
#define GEM_FP_CXX_STANDARD 23u
#elif __cplusplus >= 202002L
#define GEM_FP_CXX_STANDARD 20u
#elif __cplusplus >= 201703L
#define GEM_FP_CXX_STANDARD 17u
#else
#define GEM_FP_CXX_STANDARD 0u
#endif

/* Build-mode ABI switches. */
#if defined(_GLIBCXX_DEBUG)
#define GEM_FP_FLAG_GLIBCXX_DEBUG GEM_ABI_FLAG_GLIBCXX_DEBUG
#else
#define GEM_FP_FLAG_GLIBCXX_DEBUG 0u
#endif

#if defined(_GLIBCXX_ASSERTIONS)
#define GEM_FP_FLAG_GLIBCXX_ASSERTIONS GEM_ABI_FLAG_GLIBCXX_ASSERTIONS
#else
#define GEM_FP_FLAG_GLIBCXX_ASSERTIONS 0u
#endif

/* ASan: __SANITIZE_ADDRESS__ on GCC; __has_feature(address_sanitizer) on Clang. */
#if defined(__SANITIZE_ADDRESS__)
#define GEM_FP_FLAG_ASAN GEM_ABI_FLAG_ASAN
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GEM_FP_FLAG_ASAN GEM_ABI_FLAG_ASAN
#else
#define GEM_FP_FLAG_ASAN 0u
#endif
#else
#define GEM_FP_FLAG_ASAN 0u
#endif

/* TSan: __SANITIZE_THREAD__ on GCC; __has_feature(thread_sanitizer) on Clang. */
#if defined(__SANITIZE_THREAD__)
#define GEM_FP_FLAG_TSAN GEM_ABI_FLAG_TSAN
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define GEM_FP_FLAG_TSAN GEM_ABI_FLAG_TSAN
#else
#define GEM_FP_FLAG_TSAN 0u
#endif
#else
#define GEM_FP_FLAG_TSAN 0u
#endif

/*
 * UBSan has no predefined macro on GCC or Clang. CMake injects -DGEM_ABI_UBSAN_ACTIVE=1 when the
 * undefined-behaviour sanitizer is enabled; default off. Conservative -- plain UBSan is link-compatible
 * with uninstrumented code, so this bit is informational.
 */
#if defined(GEM_ABI_UBSAN_ACTIVE) && (GEM_ABI_UBSAN_ACTIVE + 0)
#define GEM_FP_FLAG_UBSAN GEM_ABI_FLAG_UBSAN
#else
#define GEM_FP_FLAG_UBSAN 0u
#endif

#define GEM_FP_ABI_FLAGS                                                                           \
    (GEM_FP_FLAG_GLIBCXX_DEBUG | GEM_FP_FLAG_GLIBCXX_ASSERTIONS | GEM_FP_FLAG_ASAN |               \
     GEM_FP_FLAG_TSAN | GEM_FP_FLAG_UBSAN)

/******************************************************************************/
/**
 * @brief Aggregate initializer for a GemCompat, filled from the compiling toolchain's predefined macros.
 *
 * Use it to initialize a static instance, e.g. @code static const GemCompat c = GEM_BUILD_FINGERPRINT;
 * @endcode  Designated initializers keep the mapping field->value readable and order-checked by the compiler
 * (Geneva builds at C++23). Never wrap this in a vague-linkage inline (see the file note).
 */
#define GEM_BUILD_FINGERPRINT                                                                      \
    {                                                                                              \
        .struct_version = GEM_COMPAT_STRUCT_VERSION,                                               \
        .struct_size = (std::uint32_t)sizeof(GemCompat),                                           \
        .gem_version = (std::uint32_t)GEM_VERSION,                                                 \
        .compiler_family = GEM_FP_COMPILER_FAMILY,                                                 \
        .compiler_major = GEM_FP_COMPILER_MAJOR,                                                   \
        .compiler_minor = GEM_FP_COMPILER_MINOR,                                                   \
        .stdlib_family = GEM_FP_STDLIB_FAMILY,                                                     \
        .stdlib_version = GEM_FP_STDLIB_VERSION,                                                   \
        .cxx_standard = GEM_FP_CXX_STANDARD,                                                       \
        .glibcxx_cxx11_abi = GEM_FP_GLIBCXX_CXX11_ABI,                                             \
        .boost_version = (std::uint32_t)BOOST_VERSION,                                             \
        .abi_flags = GEM_FP_ABI_FLAGS,                                                             \
    }

/******************************************************************************/
