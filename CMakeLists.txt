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

CMAKE_MINIMUM_REQUIRED(VERSION 4.0 FATAL_ERROR)

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
# Overrides GEM_USE_CUDA_RNG, CUDA_NVCC, and CUDA_ROOT.  Useful when a
# system CUDA installation is broken or incompatible (e.g. CUDA 13.1 + glibc 2.39).
option(GEM_SKIP_CUDA "Disable all CUDA compilation" OFF)

# By default Geneva only enables CUDA-dependent code when the build host's GPU driver actually
# supports the required CUDA version (see GEM_MIN_CUDA_VERSION below) -- a 13.3 toolkit paired with
# an older driver compiles but fails at runtime in the NVRTC/PTX JIT, so that combination is
# auto-disabled. Set GEM_FORCE_CUDA=ON to bypass the DEVICE check and build CUDA code whenever a
# sufficiently new toolkit is present, regardless of the build host's driver -- intended for build
# farms / CI where the machine that builds Geneva is not the machine that runs it. The toolkit-version
# requirement is NOT bypassed. (gcfg: FORCECUDA.)
option(GEM_FORCE_CUDA "Enable CUDA code on a new-enough toolkit even if the build host's driver cannot be confirmed to support it" OFF)

# Tri-state opt-in for the Hap CUDA RNG backend: auto|on|off (legacy 0/1 still accepted).
#   auto -> use CUDA when a toolkit is present (and GEM_SKIP_CUDA is OFF), else the SIMD/CPU engine
#   on   -> require CUDA (FATAL_ERROR if no toolkit / SKIPALLCUDA)
#   off  -> never use CUDA for the RNG
# Resolved below into the normal boolean GEM_USE_CUDA_RNG_ENABLED used downstream.
set(GEM_USE_CUDA_RNG "auto" CACHE STRING "Use CUDA for RNG in Hap: auto|on|off")
set_property(CACHE GEM_USE_CUDA_RNG PROPERTY STRINGS auto on off)

# Tri-state opt-in for the GPU consumer (GGPUConsumerT) built INTO the courtier library, exposing
# the "gpu" consumer mnemonic: auto|on|off (legacy 0/1 still accepted). Like the MPI consumer this
# is an opt-in add-on: when enabled the courtier library dynamically links the CUDA (NVRTC + driver
# API) device backend when present; when disabled no GPU code is compiled and "gpu" is not a
# selectable consumer (example 14 and the GPU benchmark are skipped too). A CPU backend is always
# available when the consumer is built. Independent of GEM_USE_CUDA_RNG (a separate Hap feature);
# honours GEM_SKIP_CUDA, which forces the whole consumer off.
#   auto -> build the consumer when a CUDA toolkit is present (and GEM_SKIP_CUDA is OFF), else off
#   on   -> require CUDA (FATAL_ERROR if no toolkit / SKIPALLCUDA)
#   off  -> never build the GPU consumer
# Resolved below into the normal boolean GEM_GPU_CONSUMER_ENABLED used downstream.
set(GEM_BUILD_WITH_GPU_CONSUMER "auto" CACHE STRING "Build the GPU consumer (GGPUConsumerT) into courtier and expose the 'gpu' mnemonic: auto|on|off")
set_property(CACHE GEM_BUILD_WITH_GPU_CONSUMER PROPERTY STRINGS auto on off)

# Whether to build the MQTT transport (GMqttConsumerT, the "mqtt" consumer mnemonic) into courtier.
# It needs Boost.MQTT5, a header-only part of Boost that not every Boost has:
#   auto -> build it when the Boost found has boost/mqtt5/mqtt_client.hpp
#   on   -> require it (FATAL_ERROR if the header is missing)
#   off  -> never build it
# Resolved in CMakeModules/CommonGemBuild.cmake, once Boost is found, into the boolean
# GEM_MQTT_CONSUMER_ENABLED used downstream.
set(GEM_BUILD_WITH_MQTT_CONSUMER "auto" CACHE STRING "Build the MQTT transport into courtier and expose the 'mqtt' mnemonic: auto|on|off")
set_property(CACHE GEM_BUILD_WITH_MQTT_CONSUMER PROPERTY STRINGS auto on off)

# Build example 14 (the Mona-Lisa problem) with FLOAT parameters + a float GPU kernel. This is the
# DEFAULT (ON): FP32 is ~14x faster than FP64 at the 1000-triangle workload on consumer GPUs. Turn
# OFF for the double variant (e.g. to compare FP32 vs FP64). Selects the scalar type at compile time
# (gimage_fp_t) and the matching default GPU-consumer config / device kernel. Settable from
# genevaConfig.gcfg via GIMAGE_USE_FLOAT. Declared here rather than in the example, which only a CUDA
# build adds: a build without CUDA would otherwise warn that the variable prepareBuild.sh passes is
# unused.
OPTION ( GIMAGE_USE_FLOAT "Build example 14 (Mona-Lisa) with float parameters + a float GPU kernel instead of double" ON )

# Selects the source the default Gem::Hap::GRandom proxy draws from (compile-time only -- exactly
# one backend exists in a given build):
#   queue      -> the central factory package queue (the default; producers may bulk-fill via cuRAND/SIMD)
#   local      -> a per-proxy engine seeded by the factory (CPU-only, contention-free fallback)
#   staged     -> chunks from a shared, bulk-filled staging pool into a per-proxy double buffer
#   quarantine -> read in place from N rotating, bulk-filled pools (benign-race, aligned-64-bit)
# Every other source remains usable explicitly via GRandomT<randomSource::...>; this only
# picks what the GRandom alias maps to.
set(HAP_RANDOM_SOURCE "queue" CACHE STRING "Source for the default Gem::Hap::GRandom proxy: queue|local|staged|quarantine")
set_property(CACHE HAP_RANDOM_SOURCE PROPERTY STRINGS queue local staged quarantine)
if(NOT HAP_RANDOM_SOURCE MATCHES "^(queue|local|staged|quarantine)$")
	message(FATAL_ERROR "HAP_RANDOM_SOURCE must be one of queue|local|staged|quarantine (got '${HAP_RANDOM_SOURCE}').")
endif()
# The compile definition that makes <hap/GRandomT.hpp> pick the source; none for queue, the header's
# default. Every consumer of hap must see the same one (else GRandom differs between the library
# and the program): hap's PUBLIC compile definition (libs/hap/src/CMakeLists.txt), and the one of
# Geneva::hap in the installed FindGeneva.cmake (CMakeModules/CMakeLists.txt).
set(GEM_HAP_PUBLIC_DEFINE "")
if(NOT HAP_RANDOM_SOURCE STREQUAL "queue")
	string(TOUPPER "HAP_DEFAULT_SOURCE_${HAP_RANDOM_SOURCE}=1" GEM_HAP_PUBLIC_DEFINE)
endif()

# Coverage: ON instruments all libraries, tests and programs with clang's source-based coverage;
# the target "coverage" then runs the tests without the XL ones and reports the line coverage of
# each library (CMakeModules/RunCoverage.cmake). Costs run time; leave OFF for normal builds. CUDA
# is switched off: its programs link through the CUDA host compiler, which lacks clang's profile
# runtime. Set here, before the CUDA detection below.
option(GEM_BUILD_WITH_COVERAGE "Instrument all libraries and tests for clang source-based coverage" OFF)
IF (GEM_BUILD_WITH_COVERAGE)
	IF (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		MESSAGE(FATAL_ERROR "GEM_BUILD_WITH_COVERAGE needs clang (source-based coverage, llvm-cov); "
			"the C++ compiler is ${CMAKE_CXX_COMPILER_ID}. Choose COMPILER=\"clang\" in genevaConfig.gcfg.")
	ENDIF ()
	# The LLVM tools of the compiler's release: first from the directory clang really lives in.
	GET_FILENAME_COMPONENT(_gem_clang_dir "${CMAKE_CXX_COMPILER}" REALPATH)
	GET_FILENAME_COMPONENT(_gem_clang_dir "${_gem_clang_dir}" DIRECTORY)
	FIND_PROGRAM(GEM_LLVM_PROFDATA llvm-profdata HINTS "${_gem_clang_dir}")
	FIND_PROGRAM(GEM_LLVM_COV llvm-cov HINTS "${_gem_clang_dir}")
	IF (NOT GEM_LLVM_PROFDATA OR NOT GEM_LLVM_COV)
		MESSAGE(FATAL_ERROR "GEM_BUILD_WITH_COVERAGE needs llvm-profdata and llvm-cov of clang "
			"${CMAKE_CXX_COMPILER_VERSION} (Debian/Ubuntu: the package llvm-<major version>).")
	ENDIF ()
	SET(GEM_SKIP_CUDA ON)
	STRING(APPEND CMAKE_CXX_FLAGS " -fprofile-instr-generate -fcoverage-mapping")
	FOREACH (_gem_kind EXE SHARED MODULE)
		STRING(APPEND CMAKE_${_gem_kind}_LINKER_FLAGS " -fprofile-instr-generate")
	ENDFOREACH ()
	MESSAGE(STATUS "Coverage ENABLED: all C++ code instrumented, CUDA off, tools ${GEM_LLVM_COV}")

	# The programs and libraries of the build, whose coverage mappings llvm-cov reads: written when
	# every directory has been processed.
	FUNCTION(_gem_write_coverage_objects)
		SET(_dirs "${PROJECT_SOURCE_DIR}")
		SET(_files "")
		WHILE (_dirs)
			LIST(POP_FRONT _dirs _dir)
			GET_PROPERTY(_sub DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
			LIST(APPEND _dirs ${_sub})
			GET_PROPERTY(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
			FOREACH (_t IN LISTS _targets)
				GET_TARGET_PROPERTY(_type ${_t} TYPE)
				IF (_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
					LIST(APPEND _files "$<TARGET_FILE:${_t}>")
				ENDIF ()
			ENDFOREACH ()
		ENDWHILE ()
		LIST(JOIN _files "\n" _content)
		FILE(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/CMakeFiles/gem-coverage-objects.txt"
			CONTENT "${_content}\n")
	ENDFUNCTION()
	CMAKE_LANGUAGE(DEFER CALL _gem_write_coverage_objects)

	ADD_CUSTOM_TARGET(coverage
		COMMAND ${CMAKE_COMMAND}
			-DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DBINARY_DIR=${PROJECT_BINARY_DIR}
			-DOBJECTS_FILE=${PROJECT_BINARY_DIR}/CMakeFiles/gem-coverage-objects.txt
			-DCTEST=${CMAKE_CTEST_COMMAND}
			-DLLVM_PROFDATA=${GEM_LLVM_PROFDATA} -DLLVM_COV=${GEM_LLVM_COV}
			-P ${PROJECT_SOURCE_DIR}/CMakeModules/RunCoverage.cmake
		WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
		COMMENT "Running the tests without XL and measuring the line coverage of each library"
		USES_TERMINAL
		VERBATIM
	)
ENDIF ()

# Probe for an optional CUDA toolchain.  If nvcc is found, CUDA examples and
# benchmarks are added to the build automatically; if not, the rest of the
# build proceeds as pure C++.  GEM_USE_CUDA_RNG remains a separate opt-in
# flag (see genevaConfig.gcfg) because enabling it makes the Hap library
# depend on the CUDA runtime, reducing portability of programs linked with it.

# Detect the maximum CUDA version supported by the build host's installed GPU DRIVER (NOT the toolkit).
# Sets <out_var> to "MAJOR.MINOR" (e.g. 13.3) in the parent scope, or "" if it cannot be determined
# (no driver / no GPU / query failed). Tries nvidia-smi first (fast, no compile); if that yields
# nothing, falls back to a cuDriverGetVersion() probe via try_run (authoritative, and the durable path
# once nvidia-smi's deprecated "CUDA Version" field is removed in CUDA 14). Must be called AFTER
# find_package(CUDAToolkit) so the CUDA::cuda_driver target exists for the fallback.
function(_gem_detect_driver_cuda_version out_var)
    set(_ver "")

    find_program(_gem_nvidia_smi nvidia-smi)
    if(_gem_nvidia_smi)
        execute_process(
            COMMAND "${_gem_nvidia_smi}" -q
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
        set(_probe "${CMAKE_BINARY_DIR}/CMakeFiles/gem_cuda_driver_probe.cpp")
        file(WRITE "${_probe}"
            "#include <cuda.h>\n#include <cstdio>\n"
            "int main(){int v=0; if(cuInit(0)!=CUDA_SUCCESS) return 2;"
            " if(cuDriverGetVersion(&v)!=CUDA_SUCCESS) return 3;"
            " std::printf(\"%d.%d\\n\", v/1000, (v%1000)/10); return 0;}\n")
        try_run(_run_res _comp_res
            "${CMAKE_BINARY_DIR}/CMakeFiles/gem_cuda_driver_probe"
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

if(NOT GEM_SKIP_CUDA)
    # If only CMAKE_CUDA_COMPILER was supplied (e.g. by prepareBuild.sh), derive
    # CUDAToolkit_ROOT from it so find_package(CUDAToolkit) locates the matching
    # headers and libraries.  Assumes the standard NVIDIA layout: <root>/bin/nvcc.
    if(DEFINED CMAKE_CUDA_COMPILER AND NOT DEFINED CUDAToolkit_ROOT)
        get_filename_component(_gem_cuda_bindir "${CMAKE_CUDA_COMPILER}" DIRECTORY)
        get_filename_component(_gem_cuda_root   "${_gem_cuda_bindir}"  DIRECTORY)
        set(CUDAToolkit_ROOT "${_gem_cuda_root}"
            CACHE PATH "CUDA toolkit root (derived from CMAKE_CUDA_COMPILER)")
        unset(_gem_cuda_bindir)
        unset(_gem_cuda_root)
    endif()

    # The minimum supported CUDA version (toolkit AND device driver). 13.3 is the first CUDA Geneva
    # targets: its host front end consumes the C++23 code base, and earlier versions are no longer
    # validated against. Bump this in lock-step with the documented requirement (README, docs,
    # scripts/genevaConfig.gcfg). Overridable via -DGEM_MIN_CUDA_VERSION for advanced users/tests.
    if(NOT DEFINED GEM_MIN_CUDA_VERSION)
        set(GEM_MIN_CUDA_VERSION 13.3)
    endif()

    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        # A CUDA toolkit is present. Before enabling the language, decide whether CUDA-dependent code is
        # actually BUILDABLE and RUNNABLE on this host: (1) the toolkit must be >= GEM_MIN_CUDA_VERSION,
        # and (2) unless GEM_FORCE_CUDA is set, the build host's GPU driver must support that CUDA
        # version too (a new toolkit on an older driver compiles fine but fails at runtime in the
        # NVRTC/PTX JIT with CUDA_ERROR_UNSUPPORTED_PTX_VERSION). If either check fails we AUTO-DISABLE
        # CUDA -- the rest of Geneva still builds -- and say so loudly, so the outcome is never silent.
        find_package(CUDAToolkit REQUIRED)

        set(_gem_cuda_ok TRUE)
        set(_gem_cuda_disable_reason "")

        # (1) Toolkit-version gate.
        if(CUDAToolkit_VERSION VERSION_LESS GEM_MIN_CUDA_VERSION)
            set(_gem_cuda_ok FALSE)
            set(_gem_cuda_disable_reason
                "the CUDA toolkit is ${CUDAToolkit_VERSION} (< required ${GEM_MIN_CUDA_VERSION})")
        endif()

        # (2) Device/driver-capability gate (bypassed by GEM_FORCE_CUDA).
        set(_gem_driver_cuda "")
        if(_gem_cuda_ok AND NOT GEM_FORCE_CUDA)
            _gem_detect_driver_cuda_version(_gem_driver_cuda)
            if(NOT _gem_driver_cuda)
                set(_gem_cuda_ok FALSE)
                set(_gem_cuda_disable_reason
                    "no CUDA-capable GPU/driver could be detected on the build host")
            elseif(_gem_driver_cuda VERSION_LESS GEM_MIN_CUDA_VERSION)
                set(_gem_cuda_ok FALSE)
                set(_gem_cuda_disable_reason
                    "the GPU driver supports only CUDA ${_gem_driver_cuda} (< required ${GEM_MIN_CUDA_VERSION})")
            endif()
        endif()

        if(_gem_cuda_ok)
            enable_language(CUDA)
            if(GEM_FORCE_CUDA)
                message(STATUS "CUDA ${CUDAToolkit_VERSION} enabled (GEM_FORCE_CUDA=ON — build-host driver check bypassed)")
            else()
                message(STATUS "CUDA ${CUDAToolkit_VERSION} enabled — toolkit and GPU driver (supports CUDA ${_gem_driver_cuda}) are both >= ${GEM_MIN_CUDA_VERSION}")
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
                    set(GEM_CXX_RUNTIME_LIB_DIR "${_cxx_lib_dir}" CACHE INTERNAL "CXX runtime lib dir for CUDA link fix")
                endif()
                unset(_cxx_libstdcxx_path)
                unset(_cxx_lib_dir)
            endif()
        else()
            # AUTO-DISABLE: behave exactly as if no CUDA toolkit were present, so every downstream gate
            # (CUDA RNG, GPU consumer, CUDA examples/benchmark) uniformly turns off. We deliberately do
            # NOT enable_language(CUDA); clearing CMAKE_CUDA_COMPILER makes _gem_cuda_available FALSE.
            set(GEM_CUDA_AUTODISABLED TRUE)
            set(GEM_CUDA_DISABLE_REASON "${_gem_cuda_disable_reason}")
            unset(CMAKE_CUDA_COMPILER)
            unset(CMAKE_CUDA_COMPILER CACHE)
            message(WARNING
                "CUDA-dependent code DISABLED: ${_gem_cuda_disable_reason}.\n"
                "  All non-CUDA parts of Geneva still build; the CUDA examples/benchmark, the CUDA RNG "
                "backend and the GPU consumer ('gpu' mnemonic) are skipped.\n"
                "  To enable CUDA, install a toolkit AND GPU driver that both support CUDA "
                ">= ${GEM_MIN_CUDA_VERSION}. If the build host's driver differs from the deploy "
                "machine (build farm / CI), set -DGEM_FORCE_CUDA=ON (gcfg FORCECUDA=1) to build CUDA "
                "code anyway.")
        endif()
    else()
        message(STATUS "CUDA not found — building without CUDA examples and benchmarks")
    endif()
else()
    message(STATUS "CUDA disabled (GEM_SKIP_CUDA=ON) — skipping all CUDA compilation")
endif()

################################################################################
# Resolve the two tri-state CUDA opt-ins (GEM_USE_CUDA_RNG, GEM_BUILD_WITH_GPU_CONSUMER)
# into the NORMAL (non-cache) booleans the hap/courtier/geneva subdirectories gate on. Done here,
# once, right after the shared CUDA detection above and before any ADD_SUBDIRECTORY, so the two
# features stay independent yet share a single source of truth (the detection + SKIPALLCUDA).
#
#   _gem_cuda_available -> a usable CUDA toolkit was detected and not skipped
#   value -> ON  (legacy 1/TRUE/YES/ON) : require CUDA, FATAL_ERROR if unavailable
#   value -> AUTO                       : enable iff a toolkit is available, else fall back
#   value -> OFF (legacy 0/FALSE/NO/empty) : disabled
#   anything else                       : FATAL_ERROR (must be auto|on|off)
set(_gem_cuda_available FALSE)
if((NOT GEM_SKIP_CUDA) AND CMAKE_CUDA_COMPILER)
    set(_gem_cuda_available TRUE)
endif()

# Resolve one tri-state cache STRING (<in_var>) into a normal boolean (<out_var>), naming the
# feature (<label>) and its fallback (<fallback>) in the messages.
function(_gem_resolve_cuda_tristate in_var out_var label fallback)
    string(TOUPPER "${${in_var}}" _v)
    if(_v MATCHES "^(1|TRUE|YES|ON)$")
        set(_v "ON")
    elseif(_v STREQUAL "" OR _v MATCHES "^(0|FALSE|NO|OFF)$")
        set(_v "OFF")
    endif()

    if(GEM_SKIP_CUDA)
        if(_v STREQUAL "ON")
            message(WARNING "${label}=on but GEM_SKIP_CUDA=ON overrides it — ${label} disabled")
        endif()
        set(${out_var} FALSE PARENT_SCOPE)
    elseif(_v STREQUAL "ON")
        if(_gem_cuda_available)
            set(${out_var} TRUE PARENT_SCOPE)
        elseif(GEM_CUDA_AUTODISABLED)
            message(FATAL_ERROR
                "${label}=on, but CUDA was auto-disabled: ${GEM_CUDA_DISABLE_REASON}. "
                "Fix the toolkit/driver to CUDA >= ${GEM_MIN_CUDA_VERSION}, set GEM_FORCE_CUDA=ON, "
                "or set ${label}=auto|off.")
        else()
            message(FATAL_ERROR "${label}=on but no CUDA toolkit found (or SKIPALLCUDA on)")
        endif()
    elseif(_v STREQUAL "AUTO")
        if(_gem_cuda_available)
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

_gem_resolve_cuda_tristate(GEM_USE_CUDA_RNG          GEM_USE_CUDA_RNG_ENABLED "GEM_USE_CUDA_RNG"          "the SIMD/CPU RNG engine")
_gem_resolve_cuda_tristate(GEM_BUILD_WITH_GPU_CONSUMER GEM_GPU_CONSUMER_ENABLED "GEM_BUILD_WITH_GPU_CONSUMER" "no GPU consumer")

# Unified, no-surprise configure summary for the CUDA/GPU features.
if(_gem_cuda_available)
    set(_gem_cuda_summary "found ${CMAKE_CUDA_COMPILER_VERSION}")
elseif(GEM_SKIP_CUDA)
    set(_gem_cuda_summary "skipped (SKIPALLCUDA)")
elseif(GEM_CUDA_AUTODISABLED)
    set(_gem_cuda_summary "auto-disabled (${GEM_CUDA_DISABLE_REASON})")
else()
    set(_gem_cuda_summary "not found")
endif()
if(GEM_SKIP_CUDA)
    set(_gem_skip_summary "on")
else()
    set(_gem_skip_summary "off")
endif()
if(GEM_USE_CUDA_RNG_ENABLED)
    set(_gem_rng_summary "CUDA")
else()
    set(_gem_rng_summary "SIMD-or-CPU")
endif()
if(GEM_GPU_CONSUMER_ENABLED)
    set(_gem_gpu_summary "CUDA")
else()
    set(_gem_gpu_summary "off")
endif()
message(STATUS "Geneva GPU/CUDA: SKIPALLCUDA=${_gem_skip_summary}  CUDA-toolkit=${_gem_cuda_summary}")
message(STATUS "  Hap-RNG(USECUDARNG=${GEM_USE_CUDA_RNG}) -> ${_gem_rng_summary}   GPU-consumer(BUILDGPUCONSUMER=${GEM_BUILD_WITH_GPU_CONSUMER}) -> ${_gem_gpu_summary}")

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
        set(GEM_CXX_RUNTIME_LIB_DIR "${_cxx_lib_dir}" CACHE INTERNAL "CXX runtime lib dir for CUDA link fix")
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
# assigned with GEM_TEST_SIZE (CMakeModules/GemTestSize.cmake).
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

###############################################################################
# The library/package version. Uneven minor numbers denote development versions
# while even minor numbers stand for production releases. The version is written
# only in libs/common/include/common/GGlobalDefines.hpp and is read from there.
#
# The pre-release tag (e.g. "beta1", the header's GEM_VERSION_PRERELEASE without
# its dash) rides only in the human-facing strings (full version, install prefix,
# Doxygen, CPack) -- GEM_VERSION stays pure x.y.z, as required by the library
# VERSION property and WRITE_BASIC_PACKAGE_VERSION_FILE.

SET(_GEM_VERSION_HEADER "${PROJECT_SOURCE_DIR}/libs/common/include/common/GGlobalDefines.hpp")
FILE(STRINGS "${_GEM_VERSION_HEADER}" _GEM_VERSION_LINES
	REGEX "^#define[ \t]+GEM_VERSION_(MAJOR|MINOR|PATCH|PRERELEASE)[ \t]")
FOREACH(_comp MAJOR MINOR PATCH)
	IF(NOT _GEM_VERSION_LINES MATCHES "#define[ \t]+GEM_VERSION_${_comp}[ \t]+([0-9]+)")
		MESSAGE(FATAL_ERROR "Cannot read GEM_VERSION_${_comp} (a plain decimal) from ${_GEM_VERSION_HEADER}")
	ENDIF()
	SET(VERSION_${_comp} "${CMAKE_MATCH_1}")
ENDFOREACH()
IF(NOT _GEM_VERSION_LINES MATCHES "#define[ \t]+GEM_VERSION_PRERELEASE[ \t]+\"(-[^\"]+)?\"")
	MESSAGE(FATAL_ERROR "Cannot read GEM_VERSION_PRERELEASE (\"\" or a tag with a leading dash, such as \"-beta1\") from ${_GEM_VERSION_HEADER}")
ENDIF()
STRING(REGEX REPLACE "^-" "" VERSION_SUFFIX "${CMAKE_MATCH_1}")

SET(GEM_VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})
IF(VERSION_SUFFIX STREQUAL "")
	SET(GEM_VERSION_FULL ${GEM_VERSION})
ELSE()
	SET(GEM_VERSION_FULL ${GEM_VERSION}-${VERSION_SUFFIX})
ENDIF()

################################################################################
# Different subcomponents of the Geneva binary tree can be installed in
# customized locations, by setting the "INSTALL_PREFIX_*" variables:
#   INSTALL_PREFIX_INCLUDES -> installation folder for the header files
#   INSTALL_PREFIX_LIBS     -> installation folder for the libraries
#   INSTALL_PREFIX_DOCS     -> installation folder for the documentation files
#   INSTALL_PREFIX_DATA     -> installation folder for other files like
#                              examples, tests and other scripts
# If none of these variables is set, the whole Geneva binary tree is installed
# (as expected) in "CMAKE_INSTALL_PREFIX", with destinations relative to it, so
# that the installation can be moved (see the 'CommonGemBuild' module). The
# default value for this last variable is set here.
# Generally, this can be provided through a custom genevaConfig.gcfg, in
# conjunction with the prepareBuild.sh script.

IF( NOT CMAKE_INSTALL_PREFIX )
	SET( INSTALL_PREFIX_ROOT "/opt/geneva-${GEM_VERSION_FULL}" )
ENDIF()

################################################################################
# Make sure we first check our local modules directory

SET(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/CMakeModules")

################################################################################
# Set preprocessor define to use in source code if building with MPI consumer
IF (GEM_BUILD_WITH_MPI_CONSUMER)
	ADD_COMPILE_DEFINITIONS(GEM_BUILD_WITH_MPI_CONSUMER)
ENDIF ()

# Set preprocessor define to use in source code if building with the GPU consumer.
# Whether a consumer exists is decided by its module alone (libs/geneva/src/CMakeLists.txt); the
# definition gates the tests that need the GPU consumer. The CUDA backend selection inside courtier
# uses the separate GPUGEN_HAVE_CUDA feature macro (see libs/courtier/src/CMakeLists.txt).
IF (GEM_GPU_CONSUMER_ENABLED)
	ADD_COMPILE_DEFINITIONS(GEM_BUILD_WITH_GPU_CONSUMER)
ENDIF ()

###############################################################################
# Include the shared functionality module for any Geneva build

INCLUDE(CommonGemBuild)

###############################################################################
# Sanitizer support (ThreadSanitizer / AddressSanitizer / UBSan).
#
# OFF by default (GEM_SANITIZER=none). When enabled it instruments the whole
# C++ build (compile + every link kind, including the shared libraries). CUDA
# and MPI are deliberately EXCLUDED: nvcc cannot compile device code with
# -fsanitize, and MPI internals flood ThreadSanitizer. The caller (prepareBuild.sh)
# forces GEM_SKIP_CUDA=ON and GEM_BUILD_WITH_MPI_CONSUMER=OFF when a
# sanitizer is selected; we hard-fail here if that contract is violated.
SET(GEM_SANITIZER "none" CACHE STRING
    "Build with a sanitizer: none | thread | address | undefined (default: none)")
SET_PROPERTY(CACHE GEM_SANITIZER PROPERTY STRINGS none thread address undefined)

IF(NOT GEM_SANITIZER STREQUAL "none")
    IF(NOT GEM_SANITIZER MATCHES "^(thread|address|undefined)$")
        MESSAGE(FATAL_ERROR "GEM_SANITIZER must be none|thread|address|undefined, got '${GEM_SANITIZER}'")
    ENDIF()
    IF(NOT GEM_SKIP_CUDA)
        MESSAGE(FATAL_ERROR
            "GEM_SANITIZER=${GEM_SANITIZER} requires GEM_SKIP_CUDA=ON "
            "(CUDA device code cannot be built with -fsanitize).")
    ENDIF()
    IF(GEM_BUILD_WITH_MPI_CONSUMER)
        MESSAGE(FATAL_ERROR
            "GEM_SANITIZER=${GEM_SANITIZER} requires GEM_BUILD_WITH_MPI_CONSUMER=OFF "
            "(MPI is excluded from sanitizing).")
    ENDIF()
    SET(_gem_san "-fsanitize=${GEM_SANITIZER} -g -fno-omit-frame-pointer")
    STRING(APPEND CMAKE_CXX_FLAGS           " ${_gem_san}")
    STRING(APPEND CMAKE_C_FLAGS             " ${_gem_san}")
    STRING(APPEND CMAKE_EXE_LINKER_FLAGS    " -fsanitize=${GEM_SANITIZER}")
    STRING(APPEND CMAKE_SHARED_LINKER_FLAGS " -fsanitize=${GEM_SANITIZER}")
    STRING(APPEND CMAKE_MODULE_LINKER_FLAGS " -fsanitize=${GEM_SANITIZER}")
    # UBSan has no predefined compiler macro, so the GemCompat build fingerprint
    # (common/GBuildFingerprint.hpp) cannot detect it on its own; feed it the bit here so a UBSan module
    # and an uninstrumented host are told apart. Conservative -- plain UBSan stays link-compatible.
    IF(GEM_SANITIZER STREQUAL "undefined")
        ADD_COMPILE_DEFINITIONS(GEM_ABI_UBSAN_ACTIVE=1)
    ENDIF()
    MESSAGE(STATUS "Sanitizer ENABLED: -fsanitize=${GEM_SANITIZER} (CUDA + MPI excluded)")
ELSE()
    MESSAGE(STATUS "Sanitizer: none (default)")
ENDIF()

################################################################################
# The commit of the sources (CMakeModules/GemCommit.cmake): taken here once, so that a tree that
# does not say which commit it is does not configure, and again at every build, before the common
# library is compiled (libs/common/src/CMakeLists.txt), so that a commit made after the configure is
# seen. The one source it writes defines Gem::Common::gemCommit().
SET(GEM_COMMIT_SOURCE ${CMAKE_BINARY_DIR}/libs/common/src/GGemCommit.cpp)
EXECUTE_PROCESS(
	COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${GEM_COMMIT_SOURCE}
		-P ${PROJECT_SOURCE_DIR}/CMakeModules/GemCommit.cmake
	RESULT_VARIABLE _gem_commit_rc
	ERROR_VARIABLE _gem_commit_error
)
IF(NOT _gem_commit_rc EQUAL 0)
	MESSAGE(FATAL_ERROR "${_gem_commit_error}")
ENDIF()

################################################################################
# Include directories, libraries

INCLUDE_DIRECTORIES (
	${PROJECT_SOURCE_DIR}/libs/weft/include
	${PROJECT_SOURCE_DIR}/libs/common/include
	${PROJECT_SOURCE_DIR}/libs/dietrich/include
	${PROJECT_SOURCE_DIR}/libs/hap/include
	${PROJECT_SOURCE_DIR}/libs/courtier/include
	${PROJECT_SOURCE_DIR}/libs/geneva/include
)

# Custom targets for building single libraries only
ADD_CUSTOM_TARGET( "common"             DEPENDS ${COMMON_LIBNAME}            COMMENT "Building only the Common library." )
ADD_CUSTOM_TARGET( "dietrich"           DEPENDS ${DIETRICH_LIBNAME}          COMMENT "Building only the Dietrich plotting library." )
ADD_CUSTOM_TARGET( "hap"                DEPENDS ${HAP_LIBNAME}               COMMENT "Building only the Hap library." )
ADD_CUSTOM_TARGET( "courtier"           DEPENDS ${COURTIER_LIBNAME}          COMMENT "Building only the Courtier library." )
ADD_CUSTOM_TARGET( "geneva"             DEPENDS ${GENEVA_LIBNAME}            COMMENT "Building only the Geneva library (now includes the former geneva-individuals sources)." )

# Custom target for building all the libraries but no examples nor tests
ADD_CUSTOM_TARGET( "core"               DEPENDS ${GEM_LIBNAMES}           COMMENT "Building all the Geneva libraries.")

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
IF (GEM_BUILD_TESTS)
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

# Self-contained per-library subtrees under libs/; the legacy split-by-kind
# include/, src/, tests/, examples/, benchmarks/ trees no longer exist. The
# layers: weft (the standalone serialization engine) at the bottom, common on
# weft, dietrich, hap and courtier each on common and so on weft (three peers),
# geneva on all of them; a library may include everything below it, also
# transitively. A library is added after the ones it builds on, so that their
# targets exist when it configures.
ADD_SUBDIRECTORY (libs/weft)
ADD_SUBDIRECTORY (libs/common)
ADD_SUBDIRECTORY (libs/dietrich)
ADD_SUBDIRECTORY (libs/hap)
ADD_SUBDIRECTORY (libs/courtier)
ADD_SUBDIRECTORY (libs/geneva)

# Geneva's headers are C++23, so every program that includes them is compiled as C++23 too: each
# library carries the standard as a PUBLIC compile feature, which travels with its imported target
# (Geneva::*) into a downstream project. The global CMAKE_CXX_STANDARD reaches only the targets of
# this build, never a consumer of the installed package.
FOREACH ( _gem_lib IN LISTS GEM_LIBNAMES )
	TARGET_COMPILE_FEATURES ( ${_gem_lib} PUBLIC cxx_std_${CMAKE_CXX_STANDARD} )
ENDFOREACH ()

################################################################################
# Installed CMake config-file package.
#
# This is the modern, preferred way for downstream projects to consume an
# installed Geneva: find_package(Geneva CONFIG REQUIRED) then link the imported
# targets Geneva::weft / Geneva::common / Geneva::dietrich / Geneva::hap /
# Geneva::courtier / Geneva::geneva.
#
# The exported targets carry their include directories ($<INSTALL_INTERFACE>),
# their inter-library link dependencies (geneva -> courtier, hap, dietrich;
# each of these -> common -> weft),
# the C++23 requirement and the Boost dependency transitively. The config file
# re-finds Boost so consumers do not have to.
#
# The cmake-config directory is placed next to the installed libraries
# (<libdir>/cmake/Geneva), honouring the INSTALL_PREFIX_LIBS indirection.

INCLUDE(CMakePackageConfigHelpers)

# Where the config package lands: alongside the installed libraries.
SET(GEM_INSTALL_CMAKEDIR "${INSTALL_PREFIX_LIBS}/cmake/Geneva")

# Export the imported targets accumulated via EXPORT GenevaTargets in the
# per-library install(TARGETS ...) calls, under the Geneva:: namespace.
INSTALL(
	EXPORT GenevaTargets
	NAMESPACE Geneva::
	DESTINATION ${GEM_INSTALL_CMAKEDIR}
	FILE GenevaTargets.cmake
)

# Record whether this install was built with the MPI consumer: Geneva::courtier-mpi (and the MPI
# module) then carry MPI::MPI_CXX in their interface, and the config package must find MPI for it.
IF (GEM_BUILD_WITH_MPI_CONSUMER)
	SET(GEM_INSTALLED_WITH_MPI TRUE)
ELSE ()
	SET(GEM_INSTALLED_WITH_MPI FALSE)
ENDIF ()

# Record whether this install was built with the GPU consumer: its backends are then compiled into
# Geneva::courtier. The GPU headers are installed in any case (Inv 19), so a program can tell only
# from this variable whether using them will link.
IF (GEM_GPU_CONSUMER_ENABLED)
	SET(GEM_INSTALLED_WITH_GPU TRUE)
ELSE ()
	SET(GEM_INSTALLED_WITH_GPU FALSE)
ENDIF ()

# Generate GenevaConfig.cmake from the template. PATH_VARS / INSTALL_DESTINATION
# allow the file to be relocatable.
CONFIGURE_PACKAGE_CONFIG_FILE(
	${PROJECT_SOURCE_DIR}/CMakeModules/GenevaConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/GenevaConfig.cmake
	INSTALL_DESTINATION ${GEM_INSTALL_CMAKEDIR}
)

# Version compatibility policy: SameMinorVersion. Geneva's minor version encodes
# release vs. development series (even = production, odd = development) and the
# ABI of the shared libraries may change between minor releases, so a consumer
# that requested e.g. 2.0 must not silently be satisfied by 2.2.
WRITE_BASIC_PACKAGE_VERSION_FILE(
	${CMAKE_CURRENT_BINARY_DIR}/GenevaConfigVersion.cmake
	VERSION ${GEM_VERSION}
	COMPATIBILITY SameMinorVersion
)

INSTALL(
	FILES
	${CMAKE_CURRENT_BINARY_DIR}/GenevaConfig.cmake
	${CMAKE_CURRENT_BINARY_DIR}/GenevaConfigVersion.cmake
	# The individual-packaging helpers, so an out-of-tree consumer's find_package(Geneva) exposes
	# GENEVA_ADD_INDIVIDUAL_MODULE / GENEVA_DECLARE_INDIVIDUAL (GenevaConfig.cmake includes this file).
	${PROJECT_SOURCE_DIR}/CMakeModules/GenevaIndividualModule.cmake
	# The config-materialization helper (GEM_MATERIALIZE_CONFIGS) and the quiet-run script it drives,
	# so an out-of-tree consumer materializes its configs from code defaults the same way the in-tree
	# examples do (GenevaConfig.cmake includes the former; the latter must sit next to it).
	${PROJECT_SOURCE_DIR}/CMakeModules/GemConfigMaterialization.cmake
	${PROJECT_SOURCE_DIR}/CMakeModules/GemRunQuiet.cmake
	# The stamp of a program (GEM_STAMP_PROGRAM; GenevaConfig.cmake includes it) and the script
	# that takes the commit of a tree for it, which must sit next to it.
	${PROJECT_SOURCE_DIR}/CMakeModules/GemProgramStamp.cmake
	${PROJECT_SOURCE_DIR}/CMakeModules/GemCommit.cmake
	DESTINATION ${GEM_INSTALL_CMAKEDIR}
)

################################################################################
# The installation serves programs outside the tree: the build is installed into a fresh
# directory, the installation is moved, and the quickstart, the MPI module probe and a project
# that uses FindGeneva.cmake are built against the moved installation, at the same time, and
# run, without LD_LIBRARY_PATH (CMakeModules/CheckInstalledPackage.cmake says how). Not registered in a
# sanitizer build: a program built without -fsanitize cannot run against instrumented libraries.
IF (GEM_BUILD_TESTS)
	IF (GEM_SANITIZER STREQUAL "none")
		# git makes the quickstart's copy a work tree of its own, whose commit its programs carry
		FIND_PACKAGE(Git QUIET)
		ADD_TEST(
			NAME GenevaInstalledPackageProbes
			COMMAND ${CMAKE_COMMAND}
				-DBUILD_DIR=${CMAKE_BINARY_DIR}
				-DSOURCE_DIR=${PROJECT_SOURCE_DIR}
				-DCXX_COMPILER=${CMAKE_CXX_COMPILER}
				-DCXX_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
				-DBUILD_TYPE=${CMAKE_BUILD_TYPE}
				-DBOOST_DIR=${Boost_DIR}
				-DCATCH2_DIR=${Catch2_DIR}
				-DWITH_MPI=${GEM_BUILD_WITH_MPI_CONSUMER}
				-DWITH_GPU=${GEM_INSTALLED_WITH_GPU}
				-DHAP_DEFINE=${GEM_HAP_PUBLIC_DEFINE}
				-DGIT=${GIT_EXECUTABLE}
				-DJOBS=4
				-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckInstalledPackage.cmake
		)
		# The timeout guards against a hang only; how long the test may take is said by its size
		# label. The test takes about 10 s on an idle host (gcc Release and clang Debug alike), up
		# to 25 s under the load of a full "ctest -j16", and once reached 52 s on a host loaded by
		# other compilations; 180 s leave room for such load without letting a hang wait long.
		SET_TESTS_PROPERTIES(GenevaInstalledPackageProbes PROPERTIES LABELS "install" TIMEOUT 180)
		GEM_TEST_SIZE(GenevaInstalledPackageProbes L)
	ELSE ()
		MESSAGE(STATUS "GenevaInstalledPackageProbes not registered: a sanitizer build "
			"(GEM_SANITIZER=${GEM_SANITIZER}) cannot serve uninstrumented programs")
	ENDIF ()
ENDIF ()

################################################################################
# Conventions of the tree that a test can check, without building anything.
IF (GEM_BUILD_TESTS)
	# Every library and program includes only the libraries of its layer (courtier nothing of hap).
	ADD_TEST(
		NAME LibraryLayersRespected
		COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/libs
			-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckLibraryLayers.cmake
	)
	SET_TESTS_PROPERTIES(LibraryLayersRespected PROPERTIES LABELS "conventions")
	GEM_TEST_SIZE(LibraryLayersRespected S)

	# The build driver reports a failed configuration by its exit status.
	ADD_TEST(
		NAME PrepareBuildFailsWhenCMakeFails
		COMMAND ${CMAKE_COMMAND} -DSCRIPT=${PROJECT_SOURCE_DIR}/scripts/prepareBuild.sh
			-DWORK_DIR=${PROJECT_BINARY_DIR}/prepareBuildStatus
			-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckPrepareBuildStatus.cmake
	)
	SET_TESTS_PROPERTIES(PrepareBuildFailsWhenCMakeFails PROPERTIES LABELS "conventions")
	GEM_TEST_SIZE(PrepareBuildFailsWhenCMakeFails S)

	# Every Catch2 test case can be selected by its full name (no , [ ] " or \ in a name).
	ADD_TEST(
		NAME TestCaseNamesSelectable
		COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/libs
			-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckTestCaseNames.cmake
	)
	SET_TESTS_PROPERTIES(TestCaseNamesSelectable PROPERTIES LABELS "conventions")
	GEM_TEST_SIZE(TestCaseNamesSelectable S)

	# A NOLINT of cppcoreguidelines-special-member-functions sits on the class's name line, where
	# clang-tidy reports the check (elsewhere it suppresses nothing).
	ADD_TEST(
		NAME SpecialMemberNolintEffective
		COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/libs
			-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckSpecialMemberNolint.cmake
	)
	SET_TESTS_PROPERTIES(SpecialMemberNolintEffective PROPERTIES LABELS "conventions")
	GEM_TEST_SIZE(SpecialMemberNolintEffective S)

	# Doxygen reports no warning about the libraries' documentation beyond the few that
	# CMakeModules/DoxygenWarningsAllowed.txt lists with their reason. Needs Doxygen, like the doc
	# targets.
	IF (DOXYGEN_FOUND)
		ADD_TEST(
			NAME DoxygenWarningsOnlyAllowed
			COMMAND ${CMAKE_COMMAND}
				-DDOXYGEN=${DOXYGEN_EXECUTABLE}
				-DBUILD_DIR=${CMAKE_BINARY_DIR}
				-DSOURCE_DIR=${PROJECT_SOURCE_DIR}
				-DALLOWED=${PROJECT_SOURCE_DIR}/CMakeModules/DoxygenWarningsAllowed.txt
				-DLIBS=weft,common,hap,dietrich,courtier,geneva
				-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckDoxygenWarnings.cmake
		)
		SET_TESTS_PROPERTIES(DoxygenWarningsOnlyAllowed PROPERTIES LABELS "conventions")
		GEM_TEST_SIZE(DoxygenWarningsOnlyAllowed M)
	ELSE ()
		MESSAGE(STATUS "DoxygenWarningsOnlyAllowed not registered: Doxygen not found")
	ENDIF ()

	# The rule of CheckFigureTitles.cmake on two figures of one pad with a canvas label: a series
	# with a blank title (the plotting library's " " for an unlabelled series) shows no second
	# title, a labelled one does.
	SET(_titles_dir ${CMAKE_BINARY_DIR}/CheckFigureTitles)
	FOREACH(_case blank labelled)
		IF(_case STREQUAL "blank")
			SET(_series_title " ")
		ELSE()
			SET(_series_title "a series")
		ENDIF()
		FILE(WRITE ${_titles_dir}/${_case}.C
			"TPaveLabel* canvasTitle = new TPaveLabel(0.2,0.95,0.8,0.99, \"A figure\");\n"
			"graphPad->cd(1);\n"
			"graph_0->SetTitle(\"${_series_title}\");\n")
		ADD_TEST(NAME CheckFigureTitlesRule_${_case}
			COMMAND ${CMAKE_COMMAND} -DFIGURES=${_titles_dir}/${_case}.C
				-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckFigureTitles.cmake)
		GEM_TEST_SIZE(CheckFigureTitlesRule_${_case} S)
	ENDFOREACH()
	SET_TESTS_PROPERTIES(CheckFigureTitlesRule_labelled PROPERTIES
		PASS_REGULAR_EXPRESSION "one pad, two titles: the canvas label \"A figure\"")

	# Where a build takes the commit of the sources from (GemCommit.cmake), on made-up source
	# trees, and how a program is stamped with it. Need git, to make the work trees among them.
	FIND_PACKAGE(Git QUIET)
	IF (GIT_FOUND)
		ADD_TEST(
			NAME GemCommitSources
			COMMAND ${CMAKE_COMMAND} -DSCRIPT=${PROJECT_SOURCE_DIR}/CMakeModules/GemCommit.cmake
				-DGIT=${GIT_EXECUTABLE} -DWORK=${CMAKE_BINARY_DIR}/GemCommitSources
				-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckGemCommit.cmake
		)
		SET_TESTS_PROPERTIES(GemCommitSources PROPERTIES LABELS "conventions" TIMEOUT 60)
		GEM_TEST_SIZE(GemCommitSources S)
		# How GEM_STAMP_PROGRAM (GemProgramStamp.cmake) stamps a user's program, in made-up
		# projects (configured, not compiled).
		ADD_TEST(
			NAME GemProgramStamp
			COMMAND ${CMAKE_COMMAND} -DMODULE=${PROJECT_SOURCE_DIR}/CMakeModules/GemProgramStamp.cmake
				-DGIT=${GIT_EXECUTABLE} -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
				-DWORK=${CMAKE_BINARY_DIR}/GemProgramStamp
				-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckGemProgramStamp.cmake
		)
		SET_TESTS_PROPERTIES(GemProgramStamp PROPERTIES LABELS "conventions" TIMEOUT 120)
		GEM_TEST_SIZE(GemProgramStamp M)
	ELSE ()
		MESSAGE(STATUS "GemCommitSources and GemProgramStamp not registered: git not found")
	ENDIF ()

	# The runner of the tests that start MPI jobs (GemMpiTest.cmake) gives two runs at the same
	# time two directories for Open MPI's shared-memory files and removes each, also after a
	# failure and at its bound; and every entry that names the MPI launcher runs under it.
	ADD_TEST(
		NAME GemMpiTestDirectories
		COMMAND ${CMAKE_COMMAND} -DRUNNER=${GEM_MPI_TEST_RUNNER}
			-DWORK=${CMAKE_BINARY_DIR}/GemMpiTestDirectories
			-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckMpiTestDirectories.cmake
	)
	SET_TESTS_PROPERTIES(GemMpiTestDirectories PROPERTIES LABELS "conventions" TIMEOUT 60)
	GEM_TEST_SIZE(GemMpiTestDirectories S)
	IF (MPIEXEC_EXECUTABLE)
		ADD_TEST(
			NAME GemMpiTestsRegistered
			COMMAND ${CMAKE_COMMAND} -DCTEST=${CMAKE_CTEST_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR}
				-DMPIEXEC=${MPIEXEC_EXECUTABLE} -DRUNNER=${GEM_MPI_TEST_RUNNER}
				-DMARGIN=${GEM_MPI_TEST_MARGIN}
				-P ${PROJECT_SOURCE_DIR}/CMakeModules/CheckMpiTestRegistrations.cmake
		)
		SET_TESTS_PROPERTIES(GemMpiTestsRegistered PROPERTIES LABELS "conventions" TIMEOUT 60)
		GEM_TEST_SIZE(GemMpiTestsRegistered S)
	ENDIF ()

	# The release harness's own unit tests (ReleaseTests/tests). A Python 3 interpreter is an
	# external dependency of the test, not of the build, so it is registered only where configure
	# finds one. -B and PYTHONDONTWRITEBYTECODE: no __pycache__ is written into the source tree.
	FIND_PACKAGE(Python3 COMPONENTS Interpreter QUIET)
	IF (Python3_Interpreter_FOUND)
		ADD_TEST(
			NAME ReleaseHarnessUnitTests
			COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
				${Python3_EXECUTABLE} -B -m unittest discover
				-s ${PROJECT_SOURCE_DIR}/ReleaseTests/tests
			WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		)
		SET_TESTS_PROPERTIES(ReleaseHarnessUnitTests PROPERTIES LABELS "harness")
		GEM_TEST_SIZE(ReleaseHarnessUnitTests S)
	ELSE ()
		MESSAGE(STATUS "ReleaseHarnessUnitTests not registered: no Python 3 interpreter found")
	ENDIF ()
ENDIF ()

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
	weft-all        # serialization lib + tests + examples
	common-all      # lib + tests + examples + docprograms + benchmarks for the common library
	dietrich-all    # plotting lib + tests (unit + validity gates)
	hap-all         # lib + tests + examples + docprograms + benchmarks
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
# target so `make doc` produces all six lib bundles with cross-references
# wired via Doxygen TAGFILES (the per-library CMakeLists declares the
# upstream tag-file dependencies, so ordering is automatic).
#
IF (DOXYGEN_FOUND)
	ADD_CUSTOM_TARGET (doc
		DEPENDS doc-weft doc-common doc-hap doc-dietrich doc-courtier doc-geneva
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
SET(CPACK_PACKAGE_VERSION ${GEM_VERSION_FULL}) # carries the pre-release tag, if any
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
	"libboost-dev (>= ${GEM_MIN_BOOST_VERSION}.0),
	libboost-atomic-dev (>= ${GEM_MIN_BOOST_VERSION}.0),
	libboost-json-dev (>= ${GEM_MIN_BOOST_VERSION}.0),
	libboost-program-options-dev (>= ${GEM_MIN_BOOST_VERSION}.0),
	libboost-regex-dev (>= ${GEM_MIN_BOOST_VERSION}.0),
	libcatch2-dev (>= 3.0)"
)

# RPM specific variables
SET(CPACK_RPM_PACKAGE_DESCRIPTION ${CPACK_AUX_PACKAGE_DESCRIPTION})
SET(CPACK_RPM_PACKAGE_LICENSE "Apache License 2.0")
SET(CPACK_RPM_PACKAGE_GROUP "Sciences/Libraries")
SET(CPACK_RPM_PACKAGE_REQUIRES "boost-devel >= ${GEM_MIN_BOOST_VERSION}")

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
