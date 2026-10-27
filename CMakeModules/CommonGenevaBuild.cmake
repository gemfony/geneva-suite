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

#
# This module is needed to setup the Geneva build system, and may also be used
# to compile independent Geneva applications.
#

################################################################################

CMAKE_MINIMUM_REQUIRED(VERSION 3.27 FATAL_ERROR)

# Include guard
IF(NOT COMMON_GENEVA_BUILD_INCLUDED)
	SET(COMMON_GENEVA_BUILD_INCLUDED TRUE)

	################################################################################
	# Set some default values for user-settings. Generally, these should be provided
	# through a custom genevaConfig.gcfg, in conjunction with the prepareBuild.sh script.

	IF( NOT DEFINED GENEVA_BUILD_TYPE )
		SET( GENEVA_BUILD_TYPE "Release" )
	ENDIF()

	IF( NOT DEFINED GENEVA_BUILD_TESTS )
		SET( GENEVA_BUILD_TESTS TRUE )
	ENDIF()

	IF( NOT DEFINED CMAKE_VERBOSE_MAKEFILE )
		SET( CMAKE_VERBOSE_MAKEFILE FALSE )
	ENDIF()

	###############################################################################
	# Include host and compiler identification functions

	INCLUDE(IdentifySystemParameters)

	################################################################################
	# Validate the build type chosen by the user. This macro also sets
	# the variable CMAKE_BUILD_TYPE accordingly.

	VALIDATE_BUILD_TYPE()

	################################################################################
	# Identify the operating system

	FIND_HOST_OS (
			"GENEVA_OS_NAME"
			"GENEVA_OS_VERSION"
	)

	###############################################################################
	# Identify unsupported setups as early as possible

	FLAG_UNSUPPORTED_SETUPS(
			${GENEVA_OS_NAME}
			${GENEVA_OS_VERSION}
			${GENEVA_BUILD_TYPE}
	)

	################################################################################
	# Set the C++ standard to be used

	# Geneva requires at least the C++23 Standard. The user may force a NEWER value
	# at his own risk by setting CMAKE_CXX_STANDARD; anything below 23 is rejected.
	IF( NOT DEFINED CMAKE_CXX_STANDARD )
		SET( CMAKE_CXX_STANDARD "23" )
	ELSEIF( CMAKE_CXX_STANDARD LESS 23 )
		MESSAGE(FATAL_ERROR
			"Geneva requires C++23 or newer, but CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD} was requested.")
	ENDIF()

	SET(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_CXX_EXTENSIONS OFF)

	# The CUDA device dialect is set centrally here too, and DELIBERATELY trails the host C++ standard:
	# nvcc (CUDA 13.3) does not implement a C++23 device dialect yet, and Geneva's device code (.cu) is
	# intentionally kept at C++20. Setting it centrally keeps every CUDA target off the inherited C++23
	# (which nvcc would reject) without any per-target restatement. Only relevant when CUDA is enabled;
	# harmless as an unused variable otherwise. Raise this in lockstep once nvcc gains C++23 device support.
	IF( NOT DEFINED CMAKE_CUDA_STANDARD )
		SET( CMAKE_CUDA_STANDARD "20" )
	ENDIF()
	SET(CMAKE_CUDA_STANDARD_REQUIRED ON)

	################################################################################
	# Set the compiler and linker flags

	SET_COMPILER_FLAGS (
			${GENEVA_OS_NAME}
			${GENEVA_OS_VERSION}
			${GENEVA_BUILD_TYPE}
	)

	SET_LINKER_FLAGS (
			${GENEVA_OS_NAME}
			${GENEVA_OS_VERSION}
			${GENEVA_BUILD_TYPE}
	)

	################################################################################
	# Set other necessary build flags

	GET_BUILD_FLAGS (
			${GENEVA_OS_NAME}
			${GENEVA_OS_VERSION}
			${GENEVA_BUILD_TYPE}
			"PLATFORM_NEEDS_LIBRARY_LINKING"
	)

	################################################################################
	# Geneva only supports shared libraries

	################################################################################
	# Set the preprocessor definition for enabling testing code

	IF ( GENEVA_BUILD_TESTS )
		ADD_DEFINITIONS("-DGEM_TESTING")
	ENDIF ()

	################################################################################
	# Define the required Boost environment

	SET (Boost_USE_MULTITHREAD ON)
	SET (Boost_ADDITIONAL_VERSIONS
			"1.90.0"
			"1.90"
	)

	SET (Boost_USE_STATIC_LIBS OFF)

	# The minimum Boost version required for building Geneva and Geneva applications
	SET (GENEVA_MIN_BOOST_VERSION 1.91)

	# These are the libraries required for any Geneva build
	SET (
			GENEVA_BOOST_LIBS
			atomic
			json
			regex
			program_options
	)

	# Use Boost's own BoostConfig.cmake (available since Boost 1.70) rather than
	# CMake's legacy FindBoost module, which was removed in CMake 4.x (CMP0167).
	if(POLICY CMP0167)
		cmake_policy(SET CMP0167 NEW)
	endif()
	if(POLICY CMP0153)
		cmake_policy(SET CMP0153 NEW)
	endif()

	# Search for the required libraries
	MESSAGE("Searching for Boost...\n")
	FIND_PACKAGE(
			Boost
			${GENEVA_MIN_BOOST_VERSION} REQUIRED
			COMPONENTS ${GENEVA_BOOST_LIBS}
	)
	MESSAGE("")

	IF (GENEVA_BUILD_TESTS)
		MESSAGE("Searching for Catch2...\n")
		FIND_PACKAGE(Catch2 3 REQUIRED)
		MESSAGE("")
	ENDIF()

	INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIRS})

	# Optionally Search for MPI
	IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
		MESSAGE("Searching for MPI...\n")
		FIND_PACKAGE(MPI REQUIRED)
		MESSAGE("")
		IF(MPI_FOUND)
			INCLUDE_DIRECTORIES(${MPI_INCLUDE_PATH})
		ENDIF()
	ENDIF()

	################################################################################
	# The names of the Geneva libraries

	SET ( WEFT_LIBNAME              "gemfony-weft" )
	SET ( COMMON_LIBNAME            "gemfony-common" )
	SET ( DIETRICH_LIBNAME          "gemfony-dietrich" )
	SET ( HAP_LIBNAME               "gemfony-hap" )
	SET ( COURTIER_LIBNAME          "gemfony-courtier" )
	# The GPU consumer is folded INTO gemfony-courtier as an opt-in add-on (GENEVA_BUILD_WITH_GPU_CONSUMER),
	# exactly like the MPI consumer: when enabled, courtier dynamically links the CUDA/OpenCL backends; when
	# disabled, no GPU code is compiled. There is no separate GPU library.
	SET ( GENEVA_LIBNAME            "gemfony-geneva" )
	# The geneva-individuals library was dissolved into the geneva library: the sample
	# individuals now live in Gem::Geneva::Individuals under geneva/{include,src}/individuals/,
	# compiled into gemfony-geneva. There is no separate individuals library (and no
	# GENEVA_INDIVIDUAL_LIBNAME variable — it was removed as an unused leftover of the fold-in).

	# The order of the entries is important, as it translates to the linking
	# order in TARGET_LINK_LIBRARIES() later...
	# Dietrich (plotting) is a leaf peer on top of common, used by geneva; it links
	# after hap so geneva -> dietrich -> common resolves left-to-right.
	# Weft (the standalone serialization engine) is the deepest layer: common depends
	# on it, so it links LAST, after common.
	SET (
			GENEVA_LIBNAMES
			${GENEVA_LIBNAME}
			${COURTIER_LIBNAME}
			${HAP_LIBNAME}
			${DIETRICH_LIBNAME}
			${COMMON_LIBNAME}
			${WEFT_LIBNAME}
	)

	# This variable contains the library names.
	# The function TARGET_LINK_LIBRARIES() can use either variant.
	SET ( GENEVA_LIBRARIES ${GENEVA_LIBNAMES} )

	################################################################################
	# Add a custom target to run a "make clean" and remove temporaries,
	# so the configuration process may start fresh.

	IF (NOT TARGET "clean-cmake")
		ADD_CUSTOM_TARGET(
				"clean-cmake"
				COMMAND ${CMAKE_BUILD_TOOL} clean 2>&1 > /dev/null
				COMMAND ${CMAKE_COMMAND} -DOLD_CMAKE=${OLD_CMAKE} -P ${PROJECT_SOURCE_DIR}/CMakeModules/CleanCmakeTemporaries.cmake
		)
	ENDIF ()

	################################################################################
	# Set the installation locations

	IF (INSTALL_PREFIX_INCLUDES AND INSTALL_PREFIX_LIBS
			AND INSTALL_PREFIX_DOCS AND INSTALL_PREFIX_DATA)
		# All are set, we ignore CMAKE_INSTALL_PREFIX and install
		# each kind of files in its own location
		SET( INFO_INSTALL_PREFIX                       "\n\t\t(libs)\t\t${INSTALL_PREFIX_LIBS}" )
		SET( INFO_INSTALL_PREFIX "${INFO_INSTALL_PREFIX}\n\t\t(headers)\t${INSTALL_PREFIX_INCLUDES}" )
		SET( INFO_INSTALL_PREFIX "${INFO_INSTALL_PREFIX}\n\t\t(docs)\t\t${INSTALL_PREFIX_DOCS}" )
		SET( INFO_INSTALL_PREFIX "${INFO_INSTALL_PREFIX}\n\t\t(other files)\t${INSTALL_PREFIX_DATA}" )
	ELSEIF (NOT INSTALL_PREFIX_INCLUDES AND NOT INSTALL_PREFIX_LIBS
			AND NOT INSTALL_PREFIX_DOCS AND NOT INSTALL_PREFIX_DATA)
		# All unset, Geneva is installed as a standalone tree in CMAKE_INSTALL_PREFIX
		IF (NOT INSTALL_PREFIX_ROOT)
			IF (CMAKE_INSTALL_PREFIX)
				SET( INSTALL_PREFIX_ROOT ${CMAKE_INSTALL_PREFIX} )
			ELSE ()
				# If no value was set, use relative paths
				SET( INSTALL_PREFIX_ROOT "." )
			ENDIF ()
		ENDIF ()

		SET( INSTALL_PREFIX_INCLUDES "${INSTALL_PREFIX_ROOT}/include" )
		SET( INSTALL_PREFIX_LIBS     "${INSTALL_PREFIX_ROOT}/lib" )
		SET( INSTALL_PREFIX_DOCS     "${INSTALL_PREFIX_ROOT}" )
		SET( INSTALL_PREFIX_DATA     "${INSTALL_PREFIX_ROOT}" )
		SET( INFO_INSTALL_PREFIX     "${INSTALL_PREFIX_ROOT}" )
	ELSE ()
		# Inconsistent settings
		MESSAGE (FATAL_ERROR "Please set either all four or none of the"
				" installation prefix values INSTALL_PREFIX_INCLUDES,"
				" INSTALL_PREFIX_LIBS, INSTALL_PREFIX_DOCS, and INSTALL_PREFIX_DATA .")
	ENDIF ()

	################################################################################
	# Install RPATH so an INSTALLED host executable (and every runtime-loadable module) finds the Geneva
	# shared libraries -- and the module directory -- without LD_LIBRARY_PATH. Nothing set an RPATH before,
	# so an installed binary relied on the loader's default search path; once individuals / algorithms /
	# consumers ship as modules alongside the libs this must be self-locating. The Geneva libraries always
	# install into <prefix>/lib (INSTALL_PREFIX_LIBS); executables land at various depths under the prefix
	# (<prefix>/..., <prefix>/examples/<name>/, ...), so the RPATH lists $ORIGIN-relative entries covering
	# those depths (relocatable) plus the absolute lib dir as a backstop. USE_LINK_PATH also records the
	# link-time dependency dirs (e.g. Boost). A user-supplied CMAKE_INSTALL_RPATH is respected.
	IF (NOT CMAKE_INSTALL_RPATH)
		SET (CMAKE_INSTALL_RPATH
			"$ORIGIN/../lib;$ORIGIN/../../lib;$ORIGIN/../../../lib;${INSTALL_PREFIX_LIBS}")
	ENDIF ()
	SET (CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

	################################################################################
	# Print a summary of the build settings before continuing with the main script

	MESSAGE ("========================================")
	MESSAGE ("")
	MESSAGE ("Building:")
	MESSAGE ("\tthe Geneva library collection")
	MESSAGE ("\tdynamically linked")
	IF (GENEVA_BUILD_TESTS)
		MESSAGE ("\tincluding testing code")
	ELSE ()
		MESSAGE ("\twithout testing code")
	ENDIF ()
	# Don't print the useless build type on multi-config generators
	IF(NOT CMAKE_CONFIGURATION_TYPES)
		MESSAGE ("\tin ${CMAKE_BUILD_TYPE} mode")
		IF ("${GENEVA_BUILD_TYPE}" STREQUAL "Sanitize")
			MESSAGE ("\twith -fsanitize=thread (if available)")
		ENDIF ()
	ENDIF ()

	MESSAGE ("\twith Boost include location:\t ${Boost_INCLUDE_DIRS}")
	MESSAGE ("\twith Boost library location:\t ${Boost_LIBRARY_DIRS}")

	IF(GENEVA_BUILD_WITH_OPENCL_EXAMPLES)
		MESSAGE ("\twith OpenCL include location:\t ${OpenCL_INCLUDE_DIRS}")
		MESSAGE ("\twith OpenCL library location:\t ${OpenCL_LIBRARIES}")
		MESSAGE ("\twith with highest supported OpenCL version ${OpenCL_VERSION_STRING}")
	ENDIF()

	MESSAGE ("\tusing compiler:\t\t\t ${CMAKE_CXX_COMPILER_ID} v${CMAKE_CXX_COMPILER_VERSION}")
	# Don't try to access the build type on multi-config generators
	IF(NOT CMAKE_CONFIGURATION_TYPES)
		STRING (TOUPPER ${CMAKE_BUILD_TYPE} B_MODE)
		SET (CMAKE_CXX_FLAGS_ALL "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${B_MODE}}")
		STRING (STRIP "${CMAKE_CXX_FLAGS_ALL}" CMAKE_CXX_FLAGS_STRIPPED)
		STRING (REGEX REPLACE "[ \t]+" "\n\t\t\t\t\t " CMAKE_CXX_FLAGS_SEP ${CMAKE_CXX_FLAGS_STRIPPED})
		MESSAGE ("\twith C++ compiler flags:\t ${CMAKE_CXX_FLAGS_SEP}")
	ENDIF ()

	MESSAGE("\tUsing C++ standard ${CMAKE_CXX_STANDARD}")

	# Don't print linker options if empty
	STRING (STRIP "${CMAKE_EXE_LINKER_FLAGS}" CMAKE_L_FLAGS_STRIPPED)
	IF(NOT "${CMAKE_L_FLAGS_STRIPPED}" STREQUAL "")
		STRING (REGEX REPLACE "[ \t]+" "\n\t\t\t\t\t " CMAKE_L_FLAGS_SEP ${CMAKE_L_FLAGS_STRIPPED})
		MESSAGE ("\twith extra linker flags:\t ${CMAKE_L_FLAGS_SEP}")
	ENDIF ()
	IF( CMAKE_VERBOSE_MAKEFILE )
		MESSAGE ("\tproducing verbose CMake output")
	ELSE()
		MESSAGE ("\tproducing sparse CMake output")
	ENDIF()
	MESSAGE ("\tfor operating system:\t\t ${GENEVA_OS_NAME}")
	MESSAGE ("\tto install into prefix:\t\t ${INFO_INSTALL_PREFIX}")
	MESSAGE ("")
	MESSAGE ("========================================\n")

	###############################################################################
	# Drift-proof aggregate-target helper.
	#
	# GENEVA_AGGREGATE_TARGET(<name>) creates a custom target depending on EVERY
	# buildsystem target defined in the current directory and all of its
	# subdirectories (recursively). Call it AFTER the ADD_SUBDIRECTORY() calls of
	# a CMakeLists. This replaces hand-maintained DEPENDS lists -- which have
	# silently drifted in the past (commented-out entries, missing sub-aggregates,
	# new tests/benchmarks/examples never added) -- so that anything added under
	# the directory is picked up automatically.

	FUNCTION(_GENEVA_COLLECT_TARGETS_RECURSIVE _out_var _dir)
		GET_PROPERTY(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
		GET_PROPERTY(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
		SET(_acc "")
		# Skip targets that opt out of the aggregate (GENEVA_EXCLUDE_FROM_AGGREGATE): opt-in maintenance
		# targets (e.g. config-reference, which regenerates a version-controlled tree) must not run as part
		# of a normal build.
		FOREACH(_t ${_targets})
			GET_TARGET_PROPERTY(_excl ${_t} GENEVA_EXCLUDE_FROM_AGGREGATE)
			IF(NOT _excl)
				LIST(APPEND _acc ${_t})
			ENDIF()
		ENDFOREACH()
		FOREACH(_sub ${_subdirs})
			_GENEVA_COLLECT_TARGETS_RECURSIVE(_child "${_sub}")
			LIST(APPEND _acc ${_child})
		ENDFOREACH()
		SET(${_out_var} "${_acc}" PARENT_SCOPE)
	ENDFUNCTION()

	FUNCTION(GENEVA_AGGREGATE_TARGET _name)
		_GENEVA_COLLECT_TARGETS_RECURSIVE(_collected "${CMAKE_CURRENT_SOURCE_DIR}")
		IF(_collected)
			LIST(REMOVE_DUPLICATES _collected)
		ENDIF()
		ADD_CUSTOM_TARGET("${_name}" DEPENDS ${_collected}
			COMMENT "Building all auto-collected targets for \"${_name}\".")
	ENDFUNCTION()

	###############################################################################
	# Build-time config materialization (GENEVA_MATERIALIZE_CONFIGS). The function body lives in the
	# shared, installable GenevaConfigMaterialization.cmake so the SAME mechanism is available to an
	# out-of-tree project that consumes an installed Geneva via find_package(Geneva) (the installed
	# GenevaConfig.cmake includes the same file).
	INCLUDE(${CMAKE_CURRENT_LIST_DIR}/GenevaConfigMaterialization.cmake)

	###############################################################################
	# The test-size label helper (GENEVA_TEST_SIZE). In-tree only: it is about how THIS suite is
	# scheduled, not about anything an installed Geneva exposes.
	INCLUDE(${CMAKE_CURRENT_LIST_DIR}/GenevaTestSize.cmake)

	###############################################################################
	# Runtime-loadable / compiled-in individual packaging helpers
	# (GENEVA_ADD_INDIVIDUAL_MODULE / GENEVA_DECLARE_INDIVIDUAL). The function bodies live in the shared,
	# installable GenevaIndividualModule.cmake so the SAME helpers are available to an out-of-tree project
	# that consumes an installed Geneva via find_package(Geneva). Here (the in-tree build) we point them at
	# the in-tree source include layout; the installed GenevaConfig.cmake points them at the install prefix.
	SET(GENEVA_INDIVIDUAL_INCLUDE_DIRS
		${PROJECT_SOURCE_DIR}/libs/weft/include
		${PROJECT_SOURCE_DIR}/libs/common/include
		${PROJECT_SOURCE_DIR}/libs/hap/include
		${PROJECT_SOURCE_DIR}/libs/courtier/include
		${PROJECT_SOURCE_DIR}/libs/dietrich/include
		${PROJECT_SOURCE_DIR}/libs/geneva/include)
	SET(GENEVA_INDIVIDUAL_CXX_STANDARD ${CMAKE_CXX_STANDARD})

	# ABI-affecting compile options a loadable individual module MUST match: it links none of the Geneva
	# libraries, so it inherits none of their build flags and would otherwise pick up the consumer's toolchain
	# defaults. The axis that bites is _GLIBCXX_ASSERTIONS (part of the module-compat fingerprint's abi_flags):
	# on this stdlib it is toggled by the OPTIMISATION / _FORTIFY_SOURCE level (on at -O0, off at -O2+), NOT by
	# NDEBUG. So propagate Geneva's effective build-type compile flags (e.g. "-O3 -DNDEBUG" for Release) to the
	# module, which makes its _GLIBCXX_ASSERTIONS state -- and hence abi_flags -- match this Geneva regardless
	# of the consumer's own build type. (A sanitizer / _GLIBCXX_DEBUG build would be carried the same way, since
	# those flags are in the build-type flags too.)
	SET(_gi_bt "${CMAKE_BUILD_TYPE}")
	IF(NOT _gi_bt AND DEFINED GENEVA_BUILD_TYPE)
		SET(_gi_bt "${GENEVA_BUILD_TYPE}")
	ENDIF()
	STRING(TOUPPER "${_gi_bt}" _gi_bt_u)
	SEPARATE_ARGUMENTS(GENEVA_INDIVIDUAL_ABI_OPTIONS UNIX_COMMAND
		"${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_gi_bt_u}}")

	INCLUDE(GenevaIndividualModule)

	###############################################################################
	# End of the include-guard

ENDIF(NOT COMMON_GENEVA_BUILD_INCLUDED)

################################################################################
# Done
