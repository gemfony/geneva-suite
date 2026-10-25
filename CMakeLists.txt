################################################################################
#
# This file is part of the Geneva library collection. The following license
# applies to this file:
#
# ------------------------------------------------------------------------------
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ------------------------------------------------------------------------------
#
# Note that other files in the Geneva library collection may use a different
# license. Please see the licensing information in each file.
#
################################################################################
#
# See the NOTICE file in the top-level directory of the Geneva library
# collection for a list of contributors and copyright information.
#
################################################################################

CMAKE_MINIMUM_REQUIRED(VERSION 3.27 FATAL_ERROR)

MESSAGE ("")

# C is enabled alongside CXX so the C compiler that the build scripts pass (CMAKE_C_COMPILER) is
# actually consumed by the project -- otherwise CMake warns that it was "not used". Geneva itself
# has no C sources, but several find-modules (Threads, ZLIB, MPI, ...) probe with the C toolchain.
PROJECT (Geneva_Library_Collection LANGUAGES C CXX)

# Emit compile_commands.json so clang-tidy / IWYU / clangd / IDE code-insight
# can pick up the exact per-TU flags without an extra cmake invocation. Free
# at build time (just writes one JSON next to the Makefiles).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set ON to skip all CUDA compilation regardless of whether nvcc is present.
# Overrides GENEVA_USE_CUDA_RNG, CUDA_NVCC, and CUDA_ROOT.  Useful when a
# system CUDA installation is broken or incompatible (e.g. CUDA 13.1 + glibc 2.39).
option(GENEVA_SKIP_CUDA "Disable all CUDA compilation" OFF)

# By default Geneva only enables CUDA-dependent code when the build host's GPU driver actually
# supports the required CUDA version (see GENEVA_MIN_CUDA_VERSION below) -- a 13.3 toolkit paired with
# an older driver compiles but fails at runtime in the NVRTC/PTX JIT, so that combination is
# auto-disabled. Set GENEVA_FORCE_CUDA=ON to bypass the DEVICE check and build CUDA code whenever a
# sufficiently new toolkit is present, regardless of the build host's driver -- intended for build
# farms / CI where the machine that builds Geneva is not the machine that runs it. The toolkit-version
# requirement is NOT bypassed. (gcfg: FORCECUDA.)
option(GENEVA_FORCE_CUDA "Enable CUDA code on a new-enough toolkit even if the build host's driver cannot be confirmed to support it" OFF)

# Tri-state opt-in for the Hap CUDA RNG backend: auto|on|off (legacy 0/1 still accepted).
#   auto -> use CUDA when a toolkit is present (and GENEVA_SKIP_CUDA is OFF), else the SIMD/CPU engine
#   on   -> require CUDA (FATAL_ERROR if no toolkit / SKIPALLCUDA)
#   off  -> never use CUDA for the RNG
# Resolved below into the normal boolean GENEVA_USE_CUDA_RNG_ENABLED used downstream.
set(GENEVA_USE_CUDA_RNG "auto" CACHE STRING "Use CUDA for RNG in Hap: auto|on|off")
set_property(CACHE GENEVA_USE_CUDA_RNG PROPERTY STRINGS auto on off)

# Tri-state opt-in for the GPU consumer (GGPUConsumerT) built INTO the courtier library, exposing
# the "gpu" consumer mnemonic: auto|on|off (legacy 0/1 still accepted). Like the MPI consumer this
# is an opt-in add-on: when enabled the courtier library dynamically links the CUDA (NVRTC + driver
# API) device backend when present; when disabled no GPU code is compiled and "gpu" is not a
# selectable consumer (example 14 and the GPU benchmark are skipped too). A CPU backend is always
# available when the consumer is built. Independent of GENEVA_USE_CUDA_RNG (a separate Hap feature);
# honours GENEVA_SKIP_CUDA, which forces the whole consumer off.
#   auto -> build the consumer when a CUDA toolkit is present (and GENEVA_SKIP_CUDA is OFF), else off
#   on   -> require CUDA (FATAL_ERROR if no toolkit / SKIPALLCUDA)
#   off  -> never build the GPU consumer
# Resolved below into the normal boolean GENEVA_GPU_CONSUMER_ENABLED used downstream.
set(GENEVA_BUILD_WITH_GPU_CONSUMER "auto" CACHE STRING "Build the GPU consumer (GGPUConsumerT) into courtier and expose the 'gpu' mnemonic: auto|on|off")
set_property(CACHE GENEVA_BUILD_WITH_GPU_CONSUMER PROPERTY STRINGS auto on off)

# Selects the source the default Gem::Hap::GRandom proxy draws from (compile-time only -- exactly
# one backend exists in a given build):
#   queue      -> the central factory package queue (the default; producers may bulk-fill via cuRAND/SIMD)
#   local      -> a per-proxy engine seeded by the factory (CPU-only, contention-free fallback)
#   staged     -> chunks from a shared, bulk-filled staging pool into a per-proxy double buffer
#   quarantine -> read in place from N rotating, bulk-filled pools (benign-race, aligned-64-bit)
# Every other source remains usable explicitly via GRandomT<randomSource::...>; this only
# picks what the GRandom alias maps to. Consumed in projects/hap/src/CMakeLists.txt.
set(HAP_RANDOM_SOURCE "queue" CACHE STRING "Source for the default Gem::Hap::GRandom proxy: queue|local|staged|quarantine")
set_property(CACHE HAP_RANDOM_SOURCE PROPERTY STRINGS queue local staged quarantine)
if(NOT HAP_RANDOM_SOURCE MATCHES "^(queue|local|staged|quarantine)$")
	message(FATAL_ERROR "HAP_RANDOM_SOURCE must be one of queue|local|staged|quarantine (got '${HAP_RANDOM_SOURCE}').")
endif()

# Set ON to instrument the Common library and its tests with coverage flags
# (--coverage on GCC, -fprofile-instr-generate -fcoverage-mapping on Clang).
# Used together with the `coverage` make target to produce gcovr reports.
# Costs a noticeable amount of runtime; leave OFF for normal builds.
option(GENEVA_BUILD_WITH_COVERAGE "Instrument Common library + tests for code coverage" OFF)

# Probe for an optional CUDA toolchain.  If nvcc is found, CUDA examples and
# benchmarks are added to the build automatically; if not, the rest of the
# build proceeds as pure C++.  GENEVA_USE_CUDA_RNG remains a separate opt-in
# flag (see genevaConfig.gcfg) because enabling it makes the Hap library
# depend on the CUDA runtime, reducing portability of programs linked with it.

# Detect the maximum CUDA version supported by the build host's installed GPU DRIVER (NOT the toolkit).
# Sets <out_var> to "MAJOR.MINOR" (e.g. 13.3) in the parent scope, or "" if it cannot be determined
# (no driver / no GPU / query failed). Tries nvidia-smi first (fast, no compile); if that yields
# nothing, falls back to a cuDriverGetVersion() probe via try_run (authoritative, and the durable path
# once nvidia-smi's deprecated "CUDA Version" field is removed in CUDA 14). Must be called AFTER
# find_package(CUDAToolkit) so the CUDA::cuda_driver target exists for the fallback.
function(_geneva_detect_driver_cuda_version out_var)
    set(_ver "")

    find_program(_geneva_nvidia_smi nvidia-smi)
    if(_geneva_nvidia_smi)
        execute_process(
            COMMAND "${_geneva_nvidia_smi}" -q
            OUTPUT_VARIABLE _smi_out
            ERROR_QUIET
            RESULT_VARIABLE _smi_res)
        if(_smi_res EQUAL 0)
            string(REGEX MATCH "CUDA Version[ \t]*:[ \t]*([0-9]+\\.[0-9]+)" _m "${_smi_out}")
            if(CMAKE_MATCH_1)
                set(_ver "${CMAKE_MATCH_1}")
            endif()
        endif()
    endif()

    if((NOT _ver) AND TARGET CUDA::cuda_driver)
        set(_probe "${CMAKE_BINARY_DIR}/CMakeFiles/geneva_cuda_driver_probe.cpp")
        file(WRITE "${_probe}"
            "#include <cuda.h>\n#include <cstdio>\n"
            "int main(){int v=0; if(cuInit(0)!=CUDA_SUCCESS) return 2;"
            " if(cuDriverGetVersion(&v)!=CUDA_SUCCESS) return 3;"
            " std::printf(\"%d.%d\\n\", v/1000, (v%1000)/10); return 0;}\n")
        try_run(_run_res _comp_res
            "${CMAKE_BINARY_DIR}/CMakeFiles/geneva_cuda_driver_probe"
            "${_probe}"
            LINK_LIBRARIES CUDA::cuda_driver
            RUN_OUTPUT_VARIABLE _run_out)
        if(_comp_res AND (_run_res EQUAL 0))
            string(STRIP "${_run_out}" _run_out)
            if(_run_out MATCHES "^[0-9]+\\.[0-9]+$")
                set(_ver "${_run_out}")
            endif()
        endif()
    endif()

    set(${out_var} "${_ver}" PARENT_SCOPE)
endfunction()

if(NOT GENEVA_SKIP_CUDA)
    # If only CMAKE_CUDA_COMPILER was supplied (e.g. by prepareBuild.sh), derive
    # CUDAToolkit_ROOT from it so find_package(CUDAToolkit) locates the matching
    # headers and libraries.  Assumes the standard NVIDIA layout: <root>/bin/nvcc.
    if(DEFINED CMAKE_CUDA_COMPILER AND NOT DEFINED CUDAToolkit_ROOT)
        get_filename_component(_geneva_cuda_bindir "${CMAKE_CUDA_COMPILER}" DIRECTORY)
        get_filename_component(_geneva_cuda_root   "${_geneva_cuda_bindir}"  DIRECTORY)
        set(CUDAToolkit_ROOT "${_geneva_cuda_root}"
            CACHE PATH "CUDA toolkit root (derived from CMAKE_CUDA_COMPILER)")
        unset(_geneva_cuda_bindir)
        unset(_geneva_cuda_root)
    endif()

    # The minimum supported CUDA version (toolkit AND device driver). 13.3 is the first CUDA Geneva
    # targets: its host front end consumes the C++23 code base, and earlier versions are no longer
    # validated against. Bump this in lock-step with the documented requirement (README, docs,
    # scripts/genevaConfig.gcfg). Overridable via -DGENEVA_MIN_CUDA_VERSION for advanced users/tests.
    if(NOT DEFINED GENEVA_MIN_CUDA_VERSION)
        set(GENEVA_MIN_CUDA_VERSION 13.3)
    endif()

    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        # A CUDA toolkit is present. Before enabling the language, decide whether CUDA-dependent code is
        # actually BUILDABLE and RUNNABLE on this host: (1) the toolkit must be >= GENEVA_MIN_CUDA_VERSION,
        # and (2) unless GENEVA_FORCE_CUDA is set, the build host's GPU driver must support that CUDA
        # version too (a new toolkit on an older driver compiles fine but fails at runtime in the
        # NVRTC/PTX JIT with CUDA_ERROR_UNSUPPORTED_PTX_VERSION). If either check fails we AUTO-DISABLE
        # CUDA -- the rest of Geneva still builds -- and say so loudly, so the outcome is never silent.
        find_package(CUDAToolkit REQUIRED)

        set(_geneva_cuda_ok TRUE)
        set(_geneva_cuda_disable_reason "")

        # (1) Toolkit-version gate.
        if(CUDAToolkit_VERSION VERSION_LESS GENEVA_MIN_CUDA_VERSION)
            set(_geneva_cuda_ok FALSE)
            set(_geneva_cuda_disable_reason
                "the CUDA toolkit is ${CUDAToolkit_VERSION} (< required ${GENEVA_MIN_CUDA_VERSION})")
        endif()

        # (2) Device/driver-capability gate (bypassed by GENEVA_FORCE_CUDA).
        set(_geneva_driver_cuda "")
        if(_geneva_cuda_ok AND NOT GENEVA_FORCE_CUDA)
            _geneva_detect_driver_cuda_version(_geneva_driver_cuda)
            if(NOT _geneva_driver_cuda)
                set(_geneva_cuda_ok FALSE)
                set(_geneva_cuda_disable_reason
                    "no CUDA-capable GPU/driver could be detected on the build host")
            elseif(_geneva_driver_cuda VERSION_LESS GENEVA_MIN_CUDA_VERSION)
                set(_geneva_cuda_ok FALSE)
                set(_geneva_cuda_disable_reason
                    "the GPU driver supports only CUDA ${_geneva_driver_cuda} (< required ${GENEVA_MIN_CUDA_VERSION})")
            endif()
        endif()

        if(_geneva_cuda_ok)
            enable_language(CUDA)
            if(GENEVA_FORCE_CUDA)
                message(STATUS "CUDA ${CUDAToolkit_VERSION} enabled (GENEVA_FORCE_CUDA=ON — build-host driver check bypassed)")
            else()
                message(STATUS "CUDA ${CUDAToolkit_VERSION} enabled — toolkit and GPU driver (supports CUDA ${_geneva_driver_cuda}) are both >= ${GENEVA_MIN_CUDA_VERSION}")
            endif()
            message(STATUS "  Toolkit root: ${CUDAToolkit_ROOT}")

            # Suppress known Boost-internal false positives from NVCC.
            # NVCC does not honour -isystem suppression the way GCC does, so these
            # diagnostics fire on every Boost TU that pulls in mpl, mp11, fusion,
            # or the Boost.Archive serialization headers:
            #   68    – integer conversion sign change   (boost/mpl/print.hpp)
            #   186   – unsigned comparison with zero    (boost/mp11/detail/mp_count.hpp)
            #   191   – meaningless cast qualifier       (boost/archive/detail/iserializer.hpp)
            #   20012 – __host__/__device__ annotation ignored on defaulted function
            #           (boost/fusion/container/vector/vector.hpp, boost/fusion/tuple/tuple.hpp,
            #            boost/fusion/support/unused.hpp)
            add_compile_options($<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=68>
                                $<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=186>
                                $<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=191>
                                $<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=20012>)

            # Detect the CXX compiler's libstdc++.so location and cache it.
            # CUDA targets link it by full path to prevent the CUDA toolkit's bundled
            # (older) GCC from injecting an incompatible libstdc++ into the link step,
            # which would cause "undefined reference to __cxa_*@CXXABI_X.Y" failures.
            if(CMAKE_CXX_COMPILER)
                execute_process(
                    COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++.so
                    OUTPUT_VARIABLE _cxx_libstdcxx_path
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                if(_cxx_libstdcxx_path AND NOT _cxx_libstdcxx_path STREQUAL "libstdc++.so")
                    get_filename_component(_cxx_lib_dir "${_cxx_libstdcxx_path}" DIRECTORY)
                    set(GENEVA_CXX_RUNTIME_LIB_DIR "${_cxx_lib_dir}" CACHE INTERNAL "CXX runtime lib dir for CUDA link fix")
                endif()
                unset(_cxx_libstdcxx_path)
                unset(_cxx_lib_dir)
            endif()
        else()
            # AUTO-DISABLE: behave exactly as if no CUDA toolkit were present, so every downstream gate
            # (CUDA RNG, GPU consumer, CUDA examples/benchmark) uniformly turns off. We deliberately do
            # NOT enable_language(CUDA); clearing CMAKE_CUDA_COMPILER makes _geneva_cuda_available FALSE.
            set(GENEVA_CUDA_AUTODISABLED TRUE)
            set(GENEVA_CUDA_DISABLE_REASON "${_geneva_cuda_disable_reason}")
            unset(CMAKE_CUDA_COMPILER)
            unset(CMAKE_CUDA_COMPILER CACHE)
            message(WARNING
                "CUDA-dependent code DISABLED: ${_geneva_cuda_disable_reason}.\n"
                "  All non-CUDA parts of Geneva still build; the CUDA examples/benchmark, the CUDA RNG "
                "backend and the GPU consumer ('gpu' mnemonic) are skipped.\n"
                "  To enable CUDA, install a toolkit AND GPU driver that both support CUDA "
                ">= ${GENEVA_MIN_CUDA_VERSION}. If the build host's driver differs from the deploy "
                "machine (build farm / CI), set -DGENEVA_FORCE_CUDA=ON (gcfg FORCECUDA=1) to build CUDA "
                "code anyway.")
        endif()
    else()
        message(STATUS "CUDA not found — building without CUDA examples and benchmarks")
    endif()
else()
    message(STATUS "CUDA disabled (GENEVA_SKIP_CUDA=ON) — skipping all CUDA compilation")
endif()

################################################################################
# Resolve the two tri-state CUDA opt-ins (GENEVA_USE_CUDA_RNG, GENEVA_BUILD_WITH_GPU_CONSUMER)
# into the NORMAL (non-cache) booleans the hap/courtier/geneva subdirectories gate on. Done here,
# once, right after the shared CUDA detection above and before any ADD_SUBDIRECTORY, so the two
# features stay independent yet share a single source of truth (the detection + SKIPALLCUDA).
#
#   _geneva_cuda_available -> a usable CUDA toolkit was detected and not skipped
#   value -> ON  (legacy 1/TRUE/YES/ON) : require CUDA, FATAL_ERROR if unavailable
#   value -> AUTO                       : enable iff a toolkit is available, else fall back
#   value -> OFF (legacy 0/FALSE/NO/empty) : disabled
#   anything else                       : FATAL_ERROR (must be auto|on|off)
set(_geneva_cuda_available FALSE)
if((NOT GENEVA_SKIP_CUDA) AND CMAKE_CUDA_COMPILER)
    set(_geneva_cuda_available TRUE)
endif()

# Resolve one tri-state cache STRING (<in_var>) into a normal boolean (<out_var>), naming the
# feature (<label>) and its fallback (<fallback>) in the messages.
function(_geneva_resolve_cuda_tristate in_var out_var label fallback)
    string(TOUPPER "${${in_var}}" _v)
    if(_v MATCHES "^(1|TRUE|YES|ON)$")
        set(_v "ON")
    elseif(_v STREQUAL "" OR _v MATCHES "^(0|FALSE|NO|OFF)$")
        set(_v "OFF")
    endif()

    if(GENEVA_SKIP_CUDA)
        if(_v STREQUAL "ON")
            message(WARNING "${label}=on but GENEVA_SKIP_CUDA=ON overrides it — ${label} disabled")
        endif()
        set(${out_var} FALSE PARENT_SCOPE)
    elseif(_v STREQUAL "ON")
        if(_geneva_cuda_available)
            set(${out_var} TRUE PARENT_SCOPE)
        elseif(GENEVA_CUDA_AUTODISABLED)
            message(FATAL_ERROR
                "${label}=on, but CUDA was auto-disabled: ${GENEVA_CUDA_DISABLE_REASON}. "
                "Fix the toolkit/driver to CUDA >= ${GENEVA_MIN_CUDA_VERSION}, set GENEVA_FORCE_CUDA=ON, "
                "or set ${label}=auto|off.")
        else()
            message(FATAL_ERROR "${label}=on but no CUDA toolkit found (or SKIPALLCUDA on)")
        endif()
    elseif(_v STREQUAL "AUTO")
        if(_geneva_cuda_available)
            set(${out_var} TRUE PARENT_SCOPE)
        else()
            message(STATUS "${label}=auto: no CUDA toolkit — using ${fallback}")
            set(${out_var} FALSE PARENT_SCOPE)
        endif()
    elseif(_v STREQUAL "OFF")
        set(${out_var} FALSE PARENT_SCOPE)
    else()
        message(FATAL_ERROR "${label} must be auto|on|off, got '${${in_var}}'")
    endif()
endfunction()

_geneva_resolve_cuda_tristate(GENEVA_USE_CUDA_RNG          GENEVA_USE_CUDA_RNG_ENABLED "GENEVA_USE_CUDA_RNG"          "the SIMD/CPU RNG engine")
_geneva_resolve_cuda_tristate(GENEVA_BUILD_WITH_GPU_CONSUMER GENEVA_GPU_CONSUMER_ENABLED "GENEVA_BUILD_WITH_GPU_CONSUMER" "no GPU consumer")

# Unified, no-surprise configure summary for the CUDA/GPU features.
if(_geneva_cuda_available)
    set(_geneva_cuda_summary "found ${CMAKE_CUDA_COMPILER_VERSION}")
elseif(GENEVA_SKIP_CUDA)
    set(_geneva_cuda_summary "skipped (SKIPALLCUDA)")
elseif(GENEVA_CUDA_AUTODISABLED)
    set(_geneva_cuda_summary "auto-disabled (${GENEVA_CUDA_DISABLE_REASON})")
else()
    set(_geneva_cuda_summary "not found")
endif()
if(GENEVA_SKIP_CUDA)
    set(_geneva_skip_summary "on")
else()
    set(_geneva_skip_summary "off")
endif()
if(GENEVA_USE_CUDA_RNG_ENABLED)
    set(_geneva_rng_summary "CUDA")
else()
    set(_geneva_rng_summary "SIMD-or-CPU")
endif()
if(GENEVA_GPU_CONSUMER_ENABLED)
    set(_geneva_gpu_summary "CUDA")
else()
    set(_geneva_gpu_summary "off")
endif()
message(STATUS "Geneva GPU/CUDA: SKIPALLCUDA=${_geneva_skip_summary}  CUDA-toolkit=${_geneva_cuda_summary}")
message(STATUS "  Hap-RNG(USECUDARNG=${GENEVA_USE_CUDA_RNG}) -> ${_geneva_rng_summary}   GPU-consumer(BUILDGPUCONSUMER=${GENEVA_BUILD_WITH_GPU_CONSUMER}) -> ${_geneva_gpu_summary}")

# Detect the CXX compiler's libstdc++.so location and cache it.
# The GCUDAWorker example uses this to link explicitly against the right
# libstdc++.so version when the CUDA toolkit's bundled host compiler (an
# older GCC) would otherwise inject an older libstdc++ into the link path,
# causing "undefined reference to __cxa_*@CXXABI_X.Y" link failures.
if(CMAKE_CUDA_COMPILER_LOADED AND CMAKE_CXX_COMPILER)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libstdc++.so
        OUTPUT_VARIABLE _cxx_libstdcxx_path
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_cxx_libstdcxx_path AND NOT _cxx_libstdcxx_path STREQUAL "libstdc++.so")
        get_filename_component(_cxx_lib_dir "${_cxx_libstdcxx_path}" DIRECTORY)
        set(GENEVA_CXX_RUNTIME_LIB_DIR "${_cxx_lib_dir}" CACHE INTERNAL "CXX runtime lib dir for CUDA link fix")
    endif()
    unset(_cxx_libstdcxx_path)
    unset(_cxx_lib_dir)
endif()

################################################################################
# Test sizes.
#
# EVERY test is registered -- there is no flag that hides one (a disabled test is a
# red that was hidden rather than diagnosed). What decides whether a given run
# includes a test is its measured SIZE, carried as a CTest label: S (< 1 s),
# M (1-10 s), L (10-60 s), XL (> 60 s). Every registered test carries exactly one,
# assigned with GENEVA_TEST_SIZE (CMakeModules/GenevaTestSize.cmake).
#
# The other labels -- the owning library, `example`, `benchmark` -- are descriptive
# and never decide anything. A benchmark is a program whose purpose is a
# convergence- or throughput-scale measurement; that it is currently also XL is a
# fact about its configuration, not about its name.
#
# The size rule, how to measure one, and the commands that select by size are
# documented in one place, the "Running the test suite" section of INSTALL.
# Conditional registration remains reserved for a real, configure-time-detected
# dependency (an MPI launcher, a Python interpreter, a GPU consumer build), never
# for parking a failure.

################################################################################
# Different subcomponents of the Geneva binary tree can be installed in
# customized locations, by setting the "INSTALL_PREFIX_*" variables:
#   INSTALL_PREFIX_INCLUDES -> installation folder for the header files
#   INSTALL_PREFIX_LIBS     -> installation folder for the libraries
#   INSTALL_PREFIX_DOCS     -> installation folder for the documentation files
#   INSTALL_PREFIX_DATA     -> installation folder for other files like
#                              examples, tests and other scripts
# If none of these variables is set, the whole Geneva binary tree is installed
# (as expected) in "CMAKE_INSTALL_PREFIX". The default value for this last
# variable is set here, and further handled in the 'CommonGenevaBuild' module.
# Generally, this can be provided through a custom genevaConfig.gcfg, in
# conjunction with the prepareBuild.sh script.

IF( NOT CMAKE_INSTALL_PREFIX )
	SET( INSTALL_PREFIX_ROOT "/opt/geneva-${GENEVA_VERSION_FULL}" )
ENDIF()

################################################################################
# Make sure we first check our local modules directory

SET(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/CMakeModules")

################################################################################
# Set preprocessor define to use in source code if building with MPI consumer
IF (GENEVA_BUILD_WITH_MPI_CONSUMER)
	ADD_COMPILE_DEFINITIONS(GENEVA_BUILD_WITH_MPI_CONSUMER)
ENDIF ()

# Set preprocessor define to use in source code if building with the GPU consumer.
# This gates the "gpu" mnemonic in GConsumerSetup just as GENEVA_BUILD_WITH_MPI_CONSUMER
# gates "mpi". The CUDA/OpenCL backend selection inside courtier uses the separate
# GPUGEN_HAVE_CUDA / GPUGEN_HAVE_OPENCL feature macros (see projects/courtier/src/CMakeLists.txt).
IF (GENEVA_GPU_CONSUMER_ENABLED)
	ADD_COMPILE_DEFINITIONS(GENEVA_BUILD_WITH_GPU_CONSUMER)
ENDIF ()

###############################################################################
# Include the shared functionality module for any Geneva build

INCLUDE(CommonGenevaBuild)

###############################################################################
# Sanitizer support (ThreadSanitizer / AddressSanitizer / UBSan).
#
# OFF by default (GENEVA_SANITIZER=none). When enabled it instruments the whole
# C++ build (compile + every link kind, including the shared libraries). CUDA
# and MPI are deliberately EXCLUDED: nvcc cannot compile device code with
# -fsanitize, and MPI internals flood ThreadSanitizer. The caller (prepareBuild.sh)
# forces GENEVA_SKIP_CUDA=ON and GENEVA_BUILD_WITH_MPI_CONSUMER=OFF when a
# sanitizer is selected; we hard-fail here if that contract is violated.
# See prompts/2026-05-30-sanitizer-rollout-plan.md.
SET(GENEVA_SANITIZER "none" CACHE STRING
    "Build with a sanitizer: none | thread | address | undefined (default: none)")
SET_PROPERTY(CACHE GENEVA_SANITIZER PROPERTY STRINGS none thread address undefined)

IF(NOT GENEVA_SANITIZER STREQUAL "none")
    IF(NOT GENEVA_SANITIZER MATCHES "^(thread|address|undefined)$")
        MESSAGE(FATAL_ERROR "GENEVA_SANITIZER must be none|thread|address|undefined, got '${GENEVA_SANITIZER}'")
    ENDIF()
    IF(NOT GENEVA_SKIP_CUDA)
        MESSAGE(FATAL_ERROR
            "GENEVA_SANITIZER=${GENEVA_SANITIZER} requires GENEVA_SKIP_CUDA=ON "
            "(CUDA device code cannot be built with -fsanitize).")
    ENDIF()
    IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
        MESSAGE(FATAL_ERROR
            "GENEVA_SANITIZER=${GENEVA_SANITIZER} requires GENEVA_BUILD_WITH_MPI_CONSUMER=OFF "
            "(MPI is excluded from sanitizing).")
    ENDIF()
    SET(_geneva_san "-fsanitize=${GENEVA_SANITIZER} -g -fno-omit-frame-pointer")
    STRING(APPEND CMAKE_CXX_FLAGS           " ${_geneva_san}")
    STRING(APPEND CMAKE_C_FLAGS             " ${_geneva_san}")
    STRING(APPEND CMAKE_EXE_LINKER_FLAGS    " -fsanitize=${GENEVA_SANITIZER}")
    STRING(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=${GENEVA_SANITIZER}")
    STRING(APPEND CMAKE_MODULE_LINKER_FLAGS " -fsanitize=${GENEVA_SANITIZER}")
    # UBSan has no predefined compiler macro, so the GenevaCompat build fingerprint
    # (common/GBuildFingerprint.hpp) cannot detect it on its own; feed it the bit here so a UBSan module
    # and an uninstrumented host are told apart. Conservative -- plain UBSan stays link-compatible.
    IF(GENEVA_SANITIZER STREQUAL "undefined")
        ADD_COMPILE_DEFINITIONS(GENEVA_ABI_UBSAN_ACTIVE=1)
    ENDIF()
    MESSAGE(STATUS "Sanitizer ENABLED: -fsanitize=${GENEVA_SANITIZER} (CUDA + MPI excluded)")
ELSE()
    MESSAGE(STATUS "Sanitizer: none (default)")
ENDIF()

###############################################################################
# Set the library/package version. Uneven minor numbers denote development
# versions while even minor numbers stand for production releases.
#
# IMPORTANT: When bumping the version here, also update the two macros
#   GENEVA_VERSION components (+ GENEVA_VERSION_PRERELEASE)  and  MIN_BOOST_VERSION
# in projects/common/include/common/GGlobalDefines.hpp accordingly.

SET(VERSION_MAJOR "1")
SET(VERSION_MINOR "99")
SET(VERSION_PATCH "0")
# Pre-release tag (e.g. "beta1"); set to "" for a final release. It rides only in the
# human-facing strings (full version, install prefix, Doxygen, CPack) -- the numeric
# GENEVA_VERSION below stays pure x.y.z, as required by the library VERSION property and
# WRITE_BASIC_PACKAGE_VERSION_FILE.
SET(VERSION_SUFFIX "beta1")

SET(GENEVA_VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})
IF(VERSION_SUFFIX STREQUAL "")
	SET(GENEVA_VERSION_FULL ${GENEVA_VERSION})
ELSE()
	SET(GENEVA_VERSION_FULL ${GENEVA_VERSION}-${VERSION_SUFFIX})
ENDIF()

################################################################################
# Include directories, libraries

INCLUDE_DIRECTORIES (
	${PROJECT_SOURCE_DIR}/projects/weft/include
	${PROJECT_SOURCE_DIR}/projects/common/include
	${PROJECT_SOURCE_DIR}/projects/dietrich/include
	${PROJECT_SOURCE_DIR}/projects/hap/include
	${PROJECT_SOURCE_DIR}/projects/courtier/include
	${PROJECT_SOURCE_DIR}/projects/geneva/include
)

# Custom targets for building single libraries only
ADD_CUSTOM_TARGET( "common"             DEPENDS ${COMMON_LIBNAME}            COMMENT "Building only the Common library." )
ADD_CUSTOM_TARGET( "dietrich"           DEPENDS ${DIETRICH_LIBNAME}          COMMENT "Building only the Dietrich plotting library." )
ADD_CUSTOM_TARGET( "hap"                DEPENDS ${HAP_LIBNAME}               COMMENT "Building only the Hap library." )
ADD_CUSTOM_TARGET( "courtier"           DEPENDS ${COURTIER_LIBNAME}          COMMENT "Building only the Courtier library." )
ADD_CUSTOM_TARGET( "geneva"             DEPENDS ${GENEVA_LIBNAME}            COMMENT "Building only the Geneva library (now includes the former geneva-individuals sources)." )

# Custom target for building all the libraries but no examples nor tests
ADD_CUSTOM_TARGET( "core"               DEPENDS ${GENEVA_LIBNAMES}           COMMENT "Building all the Geneva libraries.")

################################################################################
# Coverage target (Common library only). Only meaningful when configured with
# -DGENEVA_BUILD_WITH_COVERAGE=ON. Builds the Common library and its tests,
# runs them under CTest (label "common"), and produces both an HTML report
# under <build>/coverage/common/ and a text summary on the terminal. Fails
# if the line-coverage drops below GENEVA_COVERAGE_LINE_THRESHOLD (default
# 90; override at configure time to relax during early test development).
SET(GENEVA_COVERAGE_LINE_THRESHOLD 90 CACHE STRING
    "Minimum acceptable line-coverage percentage for the Common library")
SET(GENEVA_COVERAGE_BRANCH_THRESHOLD 80 CACHE STRING
    "Minimum acceptable branch-coverage percentage for the Common library")
IF (GENEVA_BUILD_WITH_COVERAGE)
    FIND_PROGRAM(GCOVR_EXECUTABLE NAMES gcovr)
    IF (NOT GCOVR_EXECUTABLE)
        MESSAGE(WARNING "GENEVA_BUILD_WITH_COVERAGE=ON but gcovr was not "
                "found in PATH; the `coverage` make target will not work.")
    ENDIF ()
    # Filter: only the Common library's headers and sources count. (The plotting
    # code now lives in the separate dietrich library and is covered there.)
    SET(_GENEVA_COV_FILTER
        "--filter=${PROJECT_SOURCE_DIR}/projects/common/include/common/.*"
        "--filter=${PROJECT_SOURCE_DIR}/projects/common/src/.*")
    ADD_CUSTOM_TARGET( "coverage"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_BINARY_DIR}/coverage/common"
        COMMAND ${CMAKE_CTEST_COMMAND} -L common --output-on-failure
        # --gcov-ignore-errors: needed because gcovr scans every .gcno in
        # the build tree and stumbles over CUDA / system-header artefacts
        # that don't resolve a working directory. Those files are outside
        # the filter anyway; this just silences the noise.
        COMMAND ${GCOVR_EXECUTABLE}
                --root "${PROJECT_SOURCE_DIR}"
                ${_GENEVA_COV_FILTER}
                --gcov-ignore-errors=no_working_dir_found
                --print-summary
                --txt "${CMAKE_BINARY_DIR}/coverage/common/summary.txt"
                --html-details "${CMAKE_BINARY_DIR}/coverage/common/index.html"
                --fail-under-line ${GENEVA_COVERAGE_LINE_THRESHOLD}
                --fail-under-branch ${GENEVA_COVERAGE_BRANCH_THRESHOLD}
                "${CMAKE_BINARY_DIR}"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Building, running, and measuring Common-library coverage."
        VERBATIM
    )
ENDIF ()

################################################################################
# Add sub-directories

# Accessory files just get copied
ADD_SUBDIRECTORY (CMakeModules)
ADD_SUBDIRECTORY (licenses)
ADD_SUBDIRECTORY (scripts)

# Enable CTest before any subdirectory's add_test() runs: common/ (and the
# upcoming per-library subtrees) own their own tests/ subdirectories, so
# enable_testing() must be in scope before those add_subdirectory calls or
# their add_test() invocations are silently dropped.
IF (GENEVA_BUILD_TESTS)
	INCLUDE(CTest)
ENDIF ()

# Find Doxygen ONCE here so the per-library <lib>/doc/CMakeLists.txt files
# can short-circuit when it is absent. DOXYGEN_FOUND propagates to all
# subdirectories.
FIND_PACKAGE (Doxygen)
IF (DOXYGEN_FOUND)
	MESSAGE (STATUS "Doxygen found (${DOXYGEN_VERSION}); per-library 'doc-<lib>' targets and the aggregated 'doc' target are available.")
ELSE ()
	MESSAGE (STATUS "Doxygen not found; the per-library doc targets and 'make doc' are disabled.")
ENDIF ()

# Self-contained per-library subtrees. All four libraries now live under
# their own top-level directory; the legacy split-by-kind include/, src/,
# tests/, examples/, benchmarks/ trees no longer exist. Order matches the
# dependency chain (weft <- common <- hap <- courtier <- geneva) so every
# library's targets exist when its dependents configure. Weft (the standalone
# serialization engine) is the deepest layer, added before common.
ADD_SUBDIRECTORY (projects/weft)
ADD_SUBDIRECTORY (projects/common)
ADD_SUBDIRECTORY (projects/dietrich)
ADD_SUBDIRECTORY (projects/hap)
ADD_SUBDIRECTORY (projects/courtier)
ADD_SUBDIRECTORY (projects/geneva)

################################################################################
# Installed CMake config-file package.
#
# This is the modern, preferred way for downstream projects to consume an
# installed Geneva: find_package(Geneva CONFIG REQUIRED) then link the imported
# targets Geneva::common / Geneva::hap / Geneva::courtier / Geneva::geneva.
#
# The exported targets carry their include directories ($<INSTALL_INTERFACE>),
# their inter-library link dependencies (geneva -> courtier -> hap -> common),
# the C++23 requirement and the Boost dependency transitively. The config file
# re-finds Boost so consumers do not have to.
#
# The cmake-config directory is placed next to the installed libraries
# (<libdir>/cmake/Geneva), honouring the INSTALL_PREFIX_LIBS indirection.

INCLUDE(CMakePackageConfigHelpers)

# Where the config package lands: alongside the installed libraries.
SET(GENEVA_INSTALL_CMAKEDIR "${INSTALL_PREFIX_LIBS}/cmake/Geneva")

# Export the imported targets accumulated via EXPORT GenevaTargets in the
# per-library install(TARGETS ...) calls, under the Geneva:: namespace.
INSTALL(
	EXPORT GenevaTargets
	NAMESPACE Geneva::
	DESTINATION ${GENEVA_INSTALL_CMAKEDIR}
	FILE GenevaTargets.cmake
)

# Record whether this install was built with testing support, so the config
# package can report the build flavor to consumers as GENEVA_TESTING: a
# testing-enabled libgemfony-geneva references Catch2 symbols and must re-find
# Catch2, and only a testing-enabled install carries working self-test hook
# bodies for a consumer to drive (see GenevaConfig.cmake.in).
IF (GENEVA_BUILD_TESTS)
	SET(GENEVA_INSTALLED_WITH_TESTING TRUE)
ELSE ()
	SET(GENEVA_INSTALLED_WITH_TESTING FALSE)
ENDIF ()

# Generate GenevaConfig.cmake from the template. PATH_VARS / INSTALL_DESTINATION
# allow the file to be relocatable.
CONFIGURE_PACKAGE_CONFIG_FILE(
	${PROJECT_SOURCE_DIR}/CMakeModules/GenevaConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/GenevaConfig.cmake
	INSTALL_DESTINATION ${GENEVA_INSTALL_CMAKEDIR}
)

# Version compatibility policy: SameMinorVersion. Geneva's minor version encodes
# release vs. development series (even = production, odd = development) and the
# ABI of the shared libraries may change between minor releases, so a consumer
# that requested e.g. 2.0 must not silently be satisfied by 2.2.
WRITE_BASIC_PACKAGE_VERSION_FILE(
	${CMAKE_CURRENT_BINARY_DIR}/GenevaConfigVersion.cmake
	VERSION ${GENEVA_VERSION}
	COMPATIBILITY SameMinorVersion
)

INSTALL(
	FILES
	${CMAKE_CURRENT_BINARY_DIR}/GenevaConfig.cmake
	${CMAKE_CURRENT_BINARY_DIR}/GenevaConfigVersion.cmake
	# The individual-packaging helpers, so an out-of-tree consumer's find_package(Geneva) exposes
	# GENEVA_ADD_INDIVIDUAL_MODULE / GENEVA_DECLARE_INDIVIDUAL (GenevaConfig.cmake includes this file).
	${PROJECT_SOURCE_DIR}/CMakeModules/GenevaIndividualModule.cmake
	# The config-materialization helper (GENEVA_MATERIALIZE_CONFIGS) and the quiet-run script it drives,
	# so an out-of-tree consumer materializes its configs from code defaults the same way the in-tree
	# examples do (GenevaConfig.cmake includes the former; the latter must sit next to it).
	${PROJECT_SOURCE_DIR}/CMakeModules/GenevaConfigMaterialization.cmake
	${PROJECT_SOURCE_DIR}/CMakeModules/GenevaRunQuiet.cmake
	DESTINATION ${GENEVA_INSTALL_CMAKEDIR}
)

# Tests / examples / benchmarks are gated and dispatched inside each library's
# own CMakeLists.txt (common/, hap/, courtier/, geneva/) now that the legacy
# top-level tests/ examples/ benchmarks/ trees have been removed. CTest was
# enabled earlier (above ADD_SUBDIRECTORY (common)) so per-library add_test()
# calls register.

################################################################################
# Custom target for all compilable items in the distribution
ADD_CUSTOM_TARGET(
	"gemfony-build-all" DEPENDS
	core
	common-all      # lib + tests + examples + benchmarks for the common library
	dietrich-all    # plotting lib + tests (unit + validity gates)
	hap-all         # lib + tests + examples
	courtier-all    # lib + tests + examples-stub + docprograms + benchmarks
	geneva-all      # lib + tests + examples + benchmarks (the largest subtree)
	COMMENT "Build the entire Gemfony library collection"
)

################################################################################
# Custom target for all compilable items in the distribution
ADD_CUSTOM_TARGET(
	"install_gemfony-build-all"
	"${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target install
	DEPENDS gemfony-build-all
	COMMENT "Installing gemfony-build-all"
)

################################################################################
# Additional information files to be copied

SET ( INFORMATIONFILES
	docs/CHANGES-2.0.md
	CHANGES
	INSTALL
	LICENSE
	NOTICE
	README
)

INSTALL ( FILES ${INFORMATIONFILES} DESTINATION ${INSTALL_PREFIX_DOCS} )

################################################################################
# Aggregated reference documentation: depends on every per-library doc-<lib>
# target so `make doc` produces all four lib bundles with cross-references
# wired via Doxygen TAGFILES (the per-library CMakeLists declares the
# upstream tag-file dependencies, so ordering is automatic).
#
IF (DOXYGEN_FOUND)
	ADD_CUSTOM_TARGET (doc
		DEPENDS doc-common doc-hap doc-courtier doc-geneva
		COMMENT "Generating aggregated Doxygen documentation for all Geneva libraries."
	)
ENDIF ()

################################################################################
# Code for package generation with CPack. This is experimental and the packages
# don't satisfy the Linux FHS yet. Use with care.
#
# RPM or DEB packages can be created using umask 022 && fakeroot make package.

INCLUDE(InstallRequiredSystemLibraries)
SET(CPACK_PACKAGE_NAME "geneva-opt")
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "The Geneva optimization library")

# The package description. Indentation is important!
SET(CPACK_AUX_PACKAGE_DESCRIPTION
 "This is the Geneva optimization library. As the name suggests --it stands for
 'Grid-enabled evolutionary algorithms'--, Geneva aims to provide the necessary
 tools to perform parametric optimization in parallel on devices ranging from
 multi-processor machines over clusters all the way to Grids and Cloud installations.
 Geneva does not want to be the fastest library of evolutionary algorithms on
 single-processor machines, but is instead targeted at large scale problems,
 where the evaluation of a single candidate solution will typically take longer
 than a few seconds."
)

SET(CPACK_PACKAGE_VENDOR "Gemfony scientific")
SET(CPACK_PACKAGE_CONTACT "Gemfony scientific <contact (AT) gemfony (DOT) eu>")
SET(CPACK_PACKAGE_VERSION_MAJOR ${VERSION_MAJOR})
SET(CPACK_PACKAGE_VERSION_MINOR ${VERSION_MINOR})
SET(CPACK_PACKAGE_VERSION_PATCH ${VERSION_PATCH})
SET(CPACK_PACKAGE_VERSION ${GENEVA_VERSION_FULL}) # carries the pre-release tag, if any
SET(CPACK_PACKAGING_INSTALL_PREFIX "/opt/geneva")

# DEB specific variables. Indentation is important in the description!
SET(
	CPACK_DEBIAN_PACKAGE_DESCRIPTION
	"${CPACK_PACKAGE_DESCRIPTION_SUMMARY}\n ${CPACK_AUX_PACKAGE_DESCRIPTION}"
)
SET(CPACK_DEBIAN_PACKAGE_SECTION "Science")
SET(CPACK_DEBIAN_PACKAGE_PRIORITY "Extra")
SET(
	CPACK_DEBIAN_PACKAGE_DEPENDS
	"libboost-dev (>= ${GENEVA_MIN_BOOST_VERSION}.0),
	libboost-program-options-dev (>= ${GENEVA_MIN_BOOST_VERSION}.0),
	libboost-regex-dev (>= ${GENEVA_MIN_BOOST_VERSION}.0),
	libboost-system-dev (>= ${GENEVA_MIN_BOOST_VERSION}.0),
	libcatch2-dev (>= 3.0)"
)

# RPM specific variables
SET(CPACK_RPM_PACKAGE_DESCRIPTION ${CPACK_AUX_PACKAGE_DESCRIPTION})
SET(CPACK_RPM_PACKAGE_LICENSE "Apache License 2.0")
SET(CPACK_RPM_PACKAGE_GROUP "Sciences/Libraries")
SET(CPACK_RPM_PACKAGE_REQUIRES "boost-devel >= ${GENEVA_MIN_BOOST_VERSION}")

# Detect the distribution to autos-elect the package generator...
EXECUTE_PROCESS(COMMAND lsb_release -is
	OUTPUT_VARIABLE CPACK_AUX_DISTRO
)
IF(CPACK_AUX_DISTRO MATCHES "^Debian" OR CPACK_AUX_DISTRO MATCHES "^Ubuntu")
	SET(CPACK_GENERATOR "DEB")
ELSE(CPACK_AUX_DISTRO MATCHES "^RedHat" OR CPACK_AUX_DISTRO MATCHES "^CentOS" OR
     CPACK_AUX_DISTRO MATCHES "^Scientific" OR CPACK_AUX_DISTRO MATCHES "^SUSE" )
	SET(CPACK_GENERATOR "RPM")
ENDIF()

INCLUDE(CPack)

################################################################################
# Done
