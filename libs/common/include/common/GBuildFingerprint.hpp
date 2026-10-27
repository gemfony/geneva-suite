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
 * @brief The Geneva toolchain-compatibility fingerprint (`GenevaCompat`).
 *
 * A runtime-loadable Geneva module (an individual, an optimization algorithm, a consumer) is an in-process
 * C++ shared object that shares vtables, inline code and the GArchive type registry with the
 * host. Two such `.so`s are only ABI-compatible if they were built with a compatible toolchain: same
 * compiler family/version, same standard library, same `_GLIBCXX_USE_CXX11_ABI`, same Boost, same
 * build-mode ABI switches. The plugin loader historically compared only @c GENEVA_VERSION, which does NOT
 * capture any of that -- so a module built with a different toolchain but the same Geneva version passed
 * the gate and then crashed or silently corrupted the process.
 *
 * `GenevaCompat` closes that hole. It is a **plain-C struct** (fixed-width integers only) so that its
 * layout is readable under exactly the toolchain skew it exists to detect -- the loader must read it
 * BEFORE it can trust any C++ type from the module. It is filled entirely from predefined compiler/library
 * macros via the @c GENEVA_BUILD_FINGERPRINT initializer, never from hand-written literals, so no field can
 * silently go stale: the host and every module compile the same initializer under their own toolchain, and
 * a mismatch is detected structurally.
 *
 * The design mirrors long-proven prior art: PostgreSQL's @c PG_MODULE_MAGIC, nginx's module signature, the
 * Linux kernel's @c vermagic and Apache's Module Magic Number all gate module loading on discrete
 * ABI-relevant build knobs, matched exactly.
 *
 * @note The @c GENEVA_BUILD_FINGERPRINT macro is used to INITIALIZE a static instance at the point where a
 * module (or the host) defines its manifest. It must never be wrapped in a vague-linkage inline helper: an
 * @c inline function compiled into both the host and a module could be merged to a single definition under
 * @c RTLD_GLOBAL, which would make a module report the host's fingerprint instead of its own. Initialize a
 * @c static (internal-linkage) or module-owned object from the macro instead.
 */

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp" // GENEVA_VERSION, and (transitively) BOOST_VERSION

// Standard headers go here
#include <cstdint>

/******************************************************************************/
/*
 * Compat struct version. Bump ONLY when the layout of GenevaCompat itself changes; the leading
 * struct_version / struct_size fields let a loader read this much under any toolchain skew and tolerate a
 * module built against a later (larger) struct.
 */
#define GENEVA_COMPAT_STRUCT_VERSION 1u

/*
 * Enumerated axis values. Discrete integers (never free-form strings) so the loader compares fields, not
 * parses text.
 */
#define GENEVA_COMPILER_FAMILY_UNKNOWN 0u
#define GENEVA_COMPILER_FAMILY_GNU 1u
#define GENEVA_COMPILER_FAMILY_CLANG 2u

#define GENEVA_STDLIB_FAMILY_UNKNOWN 0u
#define GENEVA_STDLIB_FAMILY_LIBSTDCXX 1u
#define GENEVA_STDLIB_FAMILY_LIBCXX 2u

/*
 * abi_flags bitfield -- build-mode ABI switches. _GLIBCXX_DEBUG / _GLIBCXX_ASSERTIONS change standard
 * container layout outright; ASan / TSan instrumentation is not link-compatible with an uninstrumented
 * peer. UBSan stays link-compatible, so its bit is conservative and only ever set via the CMake -D path
 * (there is no predefined UBSan macro on GCC or Clang).
 */
#define GENEVA_ABI_FLAG_GLIBCXX_DEBUG 0x1u
#define GENEVA_ABI_FLAG_GLIBCXX_ASSERTIONS 0x2u
#define GENEVA_ABI_FLAG_ASAN 0x4u
#define GENEVA_ABI_FLAG_TSAN 0x8u
#define GENEVA_ABI_FLAG_UBSAN 0x10u

/******************************************************************************/
/*
 * The plain-C compatibility fingerprint. extern "C" so the tag type has C linkage and an identical layout
 * on both sides of a module boundary. Field order is frozen within GENEVA_COMPAT_STRUCT_VERSION.
 */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct GenevaCompat {
    std::uint32_t struct_version;    /* == GENEVA_COMPAT_STRUCT_VERSION                        */
    std::uint32_t struct_size;       /* sizeof(GenevaCompat), for forward tolerance            */
    std::uint32_t geneva_version;    /* GENEVA_VERSION baked in at build time                  */
    std::uint32_t compiler_family;   /* GENEVA_COMPILER_FAMILY_*                                */
    std::uint32_t compiler_major;
    std::uint32_t compiler_minor;
    std::uint32_t stdlib_family;     /* GENEVA_STDLIB_FAMILY_*                                  */
    std::uint32_t stdlib_version;    /* _GLIBCXX_RELEASE or _LIBCPP_VERSION                     */
    std::uint32_t cxx_standard;      /* 20, 23, ... (mapped from __cplusplus)                  */
    std::uint32_t glibcxx_cxx11_abi; /* _GLIBCXX_USE_CXX11_ABI (0/1); 0 when not libstdc++     */
    std::uint32_t boost_version;     /* BOOST_VERSION                                          */
    std::uint32_t abi_flags;         /* GENEVA_ABI_FLAG_* bitfield                              */
} GenevaCompat;

#ifdef __cplusplus
} /* extern "C" */
#endif

/******************************************************************************/
/*
 * Per-field derivation from predefined macros. Each expands to a plain unsigned integer literal at the
 * point of use, so GENEVA_BUILD_FINGERPRINT captures the fingerprint of whichever toolchain compiles the
 * translation unit that instantiates it.
 */

/* Compiler family + version. Check __clang__ FIRST: clang also defines __GNUC__. */
#if defined(__clang__)
#define GENEVA_FP_COMPILER_FAMILY GENEVA_COMPILER_FAMILY_CLANG
#define GENEVA_FP_COMPILER_MAJOR ((std::uint32_t)__clang_major__)
#define GENEVA_FP_COMPILER_MINOR ((std::uint32_t)__clang_minor__)
#elif defined(__GNUC__)
#define GENEVA_FP_COMPILER_FAMILY GENEVA_COMPILER_FAMILY_GNU
#define GENEVA_FP_COMPILER_MAJOR ((std::uint32_t)__GNUC__)
#define GENEVA_FP_COMPILER_MINOR ((std::uint32_t)__GNUC_MINOR__)
#else
#define GENEVA_FP_COMPILER_FAMILY GENEVA_COMPILER_FAMILY_UNKNOWN
#define GENEVA_FP_COMPILER_MAJOR 0u
#define GENEVA_FP_COMPILER_MINOR 0u
#endif

/* Standard library family + version. */
#if defined(_LIBCPP_VERSION)
#define GENEVA_FP_STDLIB_FAMILY GENEVA_STDLIB_FAMILY_LIBCXX
#define GENEVA_FP_STDLIB_VERSION ((std::uint32_t)_LIBCPP_VERSION)
#elif defined(__GLIBCXX__)
#define GENEVA_FP_STDLIB_FAMILY GENEVA_STDLIB_FAMILY_LIBSTDCXX
#if defined(_GLIBCXX_RELEASE)
#define GENEVA_FP_STDLIB_VERSION ((std::uint32_t)_GLIBCXX_RELEASE)
#else
#define GENEVA_FP_STDLIB_VERSION 0u
#endif
#else
#define GENEVA_FP_STDLIB_FAMILY GENEVA_STDLIB_FAMILY_UNKNOWN
#define GENEVA_FP_STDLIB_VERSION 0u
#endif

/* libstdc++ dual-ABI switch (irrelevant / absent under libc++). */
#if defined(_GLIBCXX_USE_CXX11_ABI)
#define GENEVA_FP_GLIBCXX_CXX11_ABI ((std::uint32_t)_GLIBCXX_USE_CXX11_ABI)
#else
#define GENEVA_FP_GLIBCXX_CXX11_ABI 0u
#endif

/* C++ standard, mapped from __cplusplus to a plain year-based number (20, 23, ...). */
#if __cplusplus >= 202302L
#define GENEVA_FP_CXX_STANDARD 23u
#elif __cplusplus >= 202002L
#define GENEVA_FP_CXX_STANDARD 20u
#elif __cplusplus >= 201703L
#define GENEVA_FP_CXX_STANDARD 17u
#else
#define GENEVA_FP_CXX_STANDARD 0u
#endif

/* Build-mode ABI switches. */
#if defined(_GLIBCXX_DEBUG)
#define GENEVA_FP_FLAG_GLIBCXX_DEBUG GENEVA_ABI_FLAG_GLIBCXX_DEBUG
#else
#define GENEVA_FP_FLAG_GLIBCXX_DEBUG 0u
#endif

#if defined(_GLIBCXX_ASSERTIONS)
#define GENEVA_FP_FLAG_GLIBCXX_ASSERTIONS GENEVA_ABI_FLAG_GLIBCXX_ASSERTIONS
#else
#define GENEVA_FP_FLAG_GLIBCXX_ASSERTIONS 0u
#endif

/* ASan: __SANITIZE_ADDRESS__ on GCC; __has_feature(address_sanitizer) on Clang. */
#if defined(__SANITIZE_ADDRESS__)
#define GENEVA_FP_FLAG_ASAN GENEVA_ABI_FLAG_ASAN
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GENEVA_FP_FLAG_ASAN GENEVA_ABI_FLAG_ASAN
#else
#define GENEVA_FP_FLAG_ASAN 0u
#endif
#else
#define GENEVA_FP_FLAG_ASAN 0u
#endif

/* TSan: __SANITIZE_THREAD__ on GCC; __has_feature(thread_sanitizer) on Clang. */
#if defined(__SANITIZE_THREAD__)
#define GENEVA_FP_FLAG_TSAN GENEVA_ABI_FLAG_TSAN
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define GENEVA_FP_FLAG_TSAN GENEVA_ABI_FLAG_TSAN
#else
#define GENEVA_FP_FLAG_TSAN 0u
#endif
#else
#define GENEVA_FP_FLAG_TSAN 0u
#endif

/*
 * UBSan has no predefined macro on GCC or Clang. CMake injects -DGENEVA_ABI_UBSAN_ACTIVE=1 when the
 * undefined-behaviour sanitizer is enabled; default off. Conservative -- plain UBSan is link-compatible
 * with uninstrumented code, so this bit is informational.
 */
#if defined(GENEVA_ABI_UBSAN_ACTIVE) && (GENEVA_ABI_UBSAN_ACTIVE + 0)
#define GENEVA_FP_FLAG_UBSAN GENEVA_ABI_FLAG_UBSAN
#else
#define GENEVA_FP_FLAG_UBSAN 0u
#endif

#define GENEVA_FP_ABI_FLAGS                                                                               \
    (GENEVA_FP_FLAG_GLIBCXX_DEBUG | GENEVA_FP_FLAG_GLIBCXX_ASSERTIONS | GENEVA_FP_FLAG_ASAN |             \
     GENEVA_FP_FLAG_TSAN | GENEVA_FP_FLAG_UBSAN)

/******************************************************************************/
/**
 * @brief Aggregate initializer for a GenevaCompat, filled from the compiling toolchain's predefined macros.
 *
 * Use it to initialize a static instance, e.g. @code static const GenevaCompat c = GENEVA_BUILD_FINGERPRINT;
 * @endcode  Designated initializers keep the mapping field->value readable and order-checked by the compiler
 * (Geneva builds at C++23). Never wrap this in a vague-linkage inline (see the file note).
 */
#define GENEVA_BUILD_FINGERPRINT                                                                          \
    {                                                                                                    \
        .struct_version = GENEVA_COMPAT_STRUCT_VERSION,                                                   \
        .struct_size = (std::uint32_t)sizeof(GenevaCompat),                                               \
        .geneva_version = (std::uint32_t)GENEVA_VERSION,                                                  \
        .compiler_family = GENEVA_FP_COMPILER_FAMILY,                                                     \
        .compiler_major = GENEVA_FP_COMPILER_MAJOR,                                                       \
        .compiler_minor = GENEVA_FP_COMPILER_MINOR,                                                       \
        .stdlib_family = GENEVA_FP_STDLIB_FAMILY,                                                         \
        .stdlib_version = GENEVA_FP_STDLIB_VERSION,                                                       \
        .cxx_standard = GENEVA_FP_CXX_STANDARD,                                                           \
        .glibcxx_cxx11_abi = GENEVA_FP_GLIBCXX_CXX11_ABI,                                                 \
        .boost_version = (std::uint32_t)BOOST_VERSION,                                                    \
        .abi_flags = GENEVA_FP_ABI_FLAGS,                                                                 \
    }

/******************************************************************************/
