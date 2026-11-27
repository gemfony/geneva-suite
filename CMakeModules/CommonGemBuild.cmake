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
# This module sets up the build of the Geneva collection itself. It is not
# installed: a project outside the tree uses find_package(Geneva).
#

################################################################################

CMAKE_MINIMUM_REQUIRED(VERSION 4.0 FATAL_ERROR)

# Include guard
IF(NOT COMMON_GEM_BUILD_INCLUDED)
	SET(COMMON_GEM_BUILD_INCLUDED TRUE)

	################################################################################
	# Set some default values for user-settings. Generally, these should be provided
	# through a custom genevaConfig.gcfg, in conjunction with the prepareBuild.sh script.

	IF( NOT DEFINED GEM_BUILD_TYPE )
		SET( GEM_BUILD_TYPE "Release" )
	ENDIF()

	IF( NOT DEFINED GEM_BUILD_TESTS )
		SET( GEM_BUILD_TESTS TRUE )
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
			"GEM_OS_NAME"
			"GEM_OS_VERSION"
	)

	###############################################################################
	# Identify unsupported setups as early as possible

	FLAG_UNSUPPORTED_SETUPS(
			${GEM_OS_NAME}
			${GEM_OS_VERSION}
			${GEM_BUILD_TYPE}
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
			${GEM_OS_NAME}
			${GEM_OS_VERSION}
			${GEM_BUILD_TYPE}
	)

	SET_LINKER_FLAGS (
			${GEM_OS_NAME}
			${GEM_OS_VERSION}
			${GEM_BUILD_TYPE}
	)

	################################################################################
	# Set other necessary build flags

	GET_BUILD_FLAGS (
			${GEM_OS_NAME}
			${GEM_OS_VERSION}
			${GEM_BUILD_TYPE}
			"PLATFORM_NEEDS_LIBRARY_LINKING"
	)

	################################################################################
	# Geneva only supports shared libraries

	################################################################################
	# Define the required Boost environment

	SET (Boost_USE_MULTITHREAD ON)
	SET (Boost_ADDITIONAL_VERSIONS
			"1.90.0"
			"1.90"
	)

	SET (Boost_USE_STATIC_LIBS OFF)

	# The minimum Boost version required for building Geneva and Geneva applications. It is
	# written only as GEM_MIN_BOOST_VERSION_NUMBER in GGlobalDefines.hpp (BOOST_VERSION
	# encoding, e.g. 109100) and is read from there as "major.minor" (e.g. 1.91).
	SET (_GEM_BOOST_HEADER "${PROJECT_SOURCE_DIR}/libs/common/include/common/GGlobalDefines.hpp")
	FILE (STRINGS "${_GEM_BOOST_HEADER}" _GEM_BOOST_LINE
		REGEX "^#define[ \t]+GEM_MIN_BOOST_VERSION_NUMBER[ \t]+[0-9]+00[ \t]*$")
	IF (NOT _GEM_BOOST_LINE MATCHES "GEM_MIN_BOOST_VERSION_NUMBER[ \t]+([0-9]+)")
		MESSAGE (FATAL_ERROR "Cannot read GEM_MIN_BOOST_VERSION_NUMBER (a BOOST_VERSION-encoded number with patch level 00, such as 109100) from ${_GEM_BOOST_HEADER}")
	ENDIF ()
	MATH (EXPR _GEM_BOOST_MAJOR "${CMAKE_MATCH_1} / 100000")
	MATH (EXPR _GEM_BOOST_MINOR "${CMAKE_MATCH_1} / 100 % 1000")
	SET (GEM_MIN_BOOST_VERSION ${_GEM_BOOST_MAJOR}.${_GEM_BOOST_MINOR})

	# These are the libraries required for any Geneva build
	SET (
			GEM_BOOST_LIBS
			json
			program_options
	)

	# Search for the required libraries
	MESSAGE("Searching for Boost...\n")
	FIND_PACKAGE(
			Boost
			${GEM_MIN_BOOST_VERSION} REQUIRED
			COMPONENTS ${GEM_BOOST_LIBS}
	)
	MESSAGE("")

	# Boost.Process, which only common uses, privately (runExternalCommand). It is found apart from
	# GEM_BOOST_LIBS, as Boost's own BoostConfig.cmake finds a component, so that Boost_LIBRARIES --
	# which Geneva's programs link -- does not carry it and its dependencies (container, context,
	# date_time, exception); an installed Geneva's users need no Boost.Process package.
	GET_FILENAME_COMPONENT(_GEM_BOOST_CMAKEDIR "${Boost_DIR}" DIRECTORY)
	FIND_PACKAGE(boost_process ${Boost_VERSION} EXACT CONFIG REQUIRED HINTS "${_GEM_BOOST_CMAKEDIR}")

	IF (GEM_BUILD_TESTS)
		MESSAGE("Searching for Catch2...\n")
		FIND_PACKAGE(Catch2 3 REQUIRED)
		MESSAGE("")
	ENDIF()

	INCLUDE_DIRECTORIES(${Boost_INCLUDE_DIRS})

	# The MQTT transport (GEM_BUILD_WITH_MQTT_CONSUMER: auto|on|off, declared in the top-level
	# CMakeLists.txt) needs Boost.MQTT5, which not every Boost has. Resolved here into
	# GEM_MQTT_CONSUMER_ENABLED and the compile definition GEM_BUILD_WITH_MQTT_CONSUMER.
	SET(GEM_MQTT_CONSUMER_ENABLED FALSE)
	IF(NOT DEFINED GEM_BUILD_WITH_MQTT_CONSUMER)
		SET(GEM_BUILD_WITH_MQTT_CONSUMER "auto")
	ENDIF()
	STRING(TOLOWER "${GEM_BUILD_WITH_MQTT_CONSUMER}" _gem_mqtt)
	IF(NOT _gem_mqtt MATCHES "^(off|0|false|no)$")
		SET(_gem_mqtt_found FALSE)
		FOREACH(_gem_dir IN LISTS Boost_INCLUDE_DIRS)
			IF(EXISTS "${_gem_dir}/boost/mqtt5/mqtt_client.hpp")
				SET(_gem_mqtt_found TRUE)
			ENDIF()
		ENDFOREACH()
		IF(_gem_mqtt_found)
			SET(GEM_MQTT_CONSUMER_ENABLED TRUE)
			ADD_COMPILE_DEFINITIONS(GEM_BUILD_WITH_MQTT_CONSUMER)
			MESSAGE(STATUS "MQTT consumer (GEM_BUILD_WITH_MQTT_CONSUMER=${GEM_BUILD_WITH_MQTT_CONSUMER}): built (Boost.MQTT5 found)")
		ELSEIF(_gem_mqtt MATCHES "^(on|1|true|yes)$")
			MESSAGE(FATAL_ERROR "GEM_BUILD_WITH_MQTT_CONSUMER=on, but the Boost in use has no Boost.MQTT5 (boost/mqtt5/mqtt_client.hpp)")
		ELSE()
			MESSAGE(STATUS "MQTT consumer (GEM_BUILD_WITH_MQTT_CONSUMER=auto): not built, the Boost in use has no Boost.MQTT5")
		ENDIF()
	ELSE()
		MESSAGE(STATUS "MQTT consumer (GEM_BUILD_WITH_MQTT_CONSUMER=off): not built")
	ENDIF()

	# Optionally Search for MPI
	IF(GEM_BUILD_WITH_MPI_CONSUMER)
		MESSAGE("Searching for MPI...\n")
		FIND_PACKAGE(MPI REQUIRED)
		MESSAGE("")
	ENDIF()

	################################################################################
	# The names of the Geneva libraries

	SET ( WEFT_LIBNAME              "gemfony-weft" )
	SET ( COMMON_LIBNAME            "gemfony-common" )
	SET ( DIETRICH_LIBNAME          "gemfony-dietrich" )
	SET ( HAP_LIBNAME               "gemfony-hap" )
	SET ( COURTIER_LIBNAME          "gemfony-courtier" )
	# Courtier's MPI library: built only with the MPI consumer; holds the process's one MPI
	# environment (libs/courtier/src/mpi). Nothing that does not use MPI links it.
	SET ( COURTIER_MPI_LIBNAME      "gemfony-courtier-mpi" )
	# The GPU consumer is folded INTO gemfony-courtier as an opt-in add-on (GEM_BUILD_WITH_GPU_CONSUMER):
	# when enabled, courtier carries the CUDA backend and loads the CUDA driver at run time; when disabled,
	# no GPU code is compiled. There is no separate GPU library. The MPI consumer, by contrast, lives in a
	# module of its own (GENEVA_MPI_LIBNAME below), because MPI is a library not every machine has.
	SET ( GENEVA_LIBNAME            "gemfony-geneva" )
	# Geneva's MPI module: built only with the MPI consumer, and loaded at run time (libs/geneva/src)
	SET ( GENEVA_MPI_LIBNAME        "gemfony-geneva-mpi" )
	# The geneva-individuals library was dissolved into the geneva library: the sample
	# individuals live in Gem::Geneva::Individuals under geneva/{include,src}/individuals/ and are
	# packaged next to it (object libraries and loadable modules, libs/geneva/src/CMakeLists.txt),
	# not compiled into gemfony-geneva. There is no separate individuals library (and no
	# GENEVA_INDIVIDUAL_LIBNAME variable — it was removed as an unused leftover of the fold-in).

	# The order of the entries is important, as it translates to the linking
	# order in TARGET_LINK_LIBRARIES() later: every library comes before the ones it
	# builds on. geneva builds on courtier, hap and dietrich, three peers that each
	# build on common and so on weft; common builds on weft (the standalone
	# serialization engine, the deepest layer), so weft links LAST, after common.
	SET (
			GEM_LIBNAMES
			${GENEVA_LIBNAME}
			${COURTIER_LIBNAME}
			${HAP_LIBNAME}
			${DIETRICH_LIBNAME}
			${COMMON_LIBNAME}
			${WEFT_LIBNAME}
	)

	# This variable contains the library names.
	# The function TARGET_LINK_LIBRARIES() can use either variant.
	SET ( GENEVA_LIBRARIES ${GEM_LIBNAMES} )

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
		# each kind of files in its own location. These are absolute paths, and the exported targets
		# name them: such an installation is not relocatable (see the branch below for one that is).
		SET( INFO_INSTALL_PREFIX                       "\n\t\t(libs)\t\t${INSTALL_PREFIX_LIBS}" )
		SET( INFO_INSTALL_PREFIX "${INFO_INSTALL_PREFIX}\n\t\t(headers)\t${INSTALL_PREFIX_INCLUDES}" )
		SET( INFO_INSTALL_PREFIX "${INFO_INSTALL_PREFIX}\n\t\t(docs)\t\t${INSTALL_PREFIX_DOCS}" )
		SET( INFO_INSTALL_PREFIX "${INFO_INSTALL_PREFIX}\n\t\t(other files)\t${INSTALL_PREFIX_DATA}" )
	ELSEIF (NOT INSTALL_PREFIX_INCLUDES AND NOT INSTALL_PREFIX_LIBS
			AND NOT INSTALL_PREFIX_DOCS AND NOT INSTALL_PREFIX_DATA)
		# All unset, Geneva is installed as a standalone tree in CMAKE_INSTALL_PREFIX. The
		# destinations are RELATIVE to it, so that the installation can be moved: CMake then
		# writes the exported targets (GenevaTargets*.cmake) and the config package relative to
		# their own location, and a tree installed elsewhere -- by "cmake --install --prefix",
		# or staged with DESTDIR -- is as usable as one at the configured prefix. (With absolute
		# destinations the exported targets name the configured prefix, and only an installation
		# at exactly that place can be built against.)
		IF (INSTALL_PREFIX_ROOT)
			# A root given explicitly (or the default of the top-level CMakeLists.txt when
			# CMAKE_INSTALL_PREFIX is empty) is the prefix the relative destinations refer to.
			SET( CMAKE_INSTALL_PREFIX "${INSTALL_PREFIX_ROOT}" )
		ENDIF ()

		SET( INSTALL_PREFIX_INCLUDES "include" )
		SET( INSTALL_PREFIX_LIBS     "lib" )
		SET( INSTALL_PREFIX_DOCS     "." )
		SET( INSTALL_PREFIX_DATA     "." )
		SET( INFO_INSTALL_PREFIX     "${CMAKE_INSTALL_PREFIX}" )
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
	# (<prefix>/..., <prefix>/examples/<name>/, down to <prefix>/benchmarks/<lib>/<group>/<name>/ and
	# <prefix>/tests/<lib>/<group>/<name>/), so the RPATH lists $ORIGIN-relative entries covering those
	# depths (relocatable) plus the absolute lib dir as a backstop. GenevaInstalledPackageProbes checks
	# every installed file with ldd after the installation has been moved, so a program installed
	# deeper fails it. USE_LINK_PATH also records the link-time dependency dirs (e.g. Boost). A
	# user-supplied CMAKE_INSTALL_RPATH is respected.
	IF (NOT CMAKE_INSTALL_RPATH)
		# The backstop names the library directory at the configured prefix; a relative destination
		# is made absolute against CMAKE_INSTALL_PREFIX. After a move the $ORIGIN entries, which
		# come first, find the libraries.
		IF (IS_ABSOLUTE "${INSTALL_PREFIX_LIBS}")
			SET (_gem_installed_libdir "${INSTALL_PREFIX_LIBS}")
		ELSE ()
			SET (_gem_installed_libdir "${CMAKE_INSTALL_PREFIX}/${INSTALL_PREFIX_LIBS}")
		ENDIF ()
		SET (CMAKE_INSTALL_RPATH
			"$ORIGIN/../lib;$ORIGIN/../../lib;$ORIGIN/../../../lib;$ORIGIN/../../../../lib;${_gem_installed_libdir}")
	ENDIF ()
	SET (CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

	################################################################################
	# Print a summary of the build settings before continuing with the main script

	MESSAGE ("========================================")
	MESSAGE ("")
	MESSAGE ("Building:")
	MESSAGE ("\tthe Geneva library collection")
	MESSAGE ("\tdynamically linked")
	IF (GEM_BUILD_TESTS)
		MESSAGE ("\tincluding testing code")
	ELSE ()
		MESSAGE ("\twithout testing code")
	ENDIF ()
	# Don't print the useless build type on multi-config generators
	IF(NOT CMAKE_CONFIGURATION_TYPES)
		MESSAGE ("\tin ${CMAKE_BUILD_TYPE} mode")
		IF ("${GEM_BUILD_TYPE}" STREQUAL "Sanitize")
			MESSAGE ("\twith -fsanitize=thread (if available)")
		ENDIF ()
	ENDIF ()

	MESSAGE ("\twith Boost include location:\t ${Boost_INCLUDE_DIRS}")
	MESSAGE ("\twith Boost library location:\t ${Boost_LIBRARY_DIRS}")

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
	MESSAGE ("\tfor operating system:\t\t ${GEM_OS_NAME}")
	MESSAGE ("\tto install into prefix:\t\t ${INFO_INSTALL_PREFIX}")
	MESSAGE ("")
	MESSAGE ("========================================\n")

	###############################################################################
	# Drift-proof aggregate-target helper.
	#
	# GEM_AGGREGATE_TARGET(<name>) creates a custom target depending on EVERY
	# buildsystem target defined in the current directory and all of its
	# subdirectories (recursively). Call it AFTER the ADD_SUBDIRECTORY() calls of
	# a CMakeLists. This replaces hand-maintained DEPENDS lists -- which have
	# silently drifted in the past (commented-out entries, missing sub-aggregates,
	# new tests/benchmarks/examples never added) -- so that anything added under
	# the directory is picked up automatically.

	FUNCTION(_GEM_COLLECT_TARGETS_RECURSIVE _out_var _dir)
		GET_PROPERTY(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
		GET_PROPERTY(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
		SET(_acc "")
		# Skip targets that opt out of the aggregate (GEM_EXCLUDE_FROM_AGGREGATE): opt-in maintenance
		# targets (e.g. config-reference, which regenerates a version-controlled tree) must not run as part
		# of a normal build.
		FOREACH(_t ${_targets})
			GET_TARGET_PROPERTY(_excl ${_t} GEM_EXCLUDE_FROM_AGGREGATE)
			IF(NOT _excl)
				LIST(APPEND _acc ${_t})
			ENDIF()
		ENDFOREACH()
		FOREACH(_sub ${_subdirs})
			_GEM_COLLECT_TARGETS_RECURSIVE(_child "${_sub}")
			LIST(APPEND _acc ${_child})
		ENDFOREACH()
		SET(${_out_var} "${_acc}" PARENT_SCOPE)
	ENDFUNCTION()

	FUNCTION(GEM_AGGREGATE_TARGET _name)
		_GEM_COLLECT_TARGETS_RECURSIVE(_collected "${CMAKE_CURRENT_SOURCE_DIR}")
		IF(_collected)
			LIST(REMOVE_DUPLICATES _collected)
		ENDIF()
		ADD_CUSTOM_TARGET("${_name}" DEPENDS ${_collected}
			COMMENT "Building all auto-collected targets for \"${_name}\".")
	ENDFUNCTION()

	###############################################################################
	# Build-time config materialization (GEM_MATERIALIZE_CONFIGS). The function body lives in the
	# shared, installable GemConfigMaterialization.cmake so the SAME mechanism is available to an
	# out-of-tree project that consumes an installed Geneva via find_package(Geneva) (the installed
	# GenevaConfig.cmake includes the same file).
	INCLUDE(${CMAKE_CURRENT_LIST_DIR}/GemConfigMaterialization.cmake)

	###############################################################################
	# The stamp of a user's program (GEM_STAMP_PROGRAM), shared with the installed package like
	# the materialization above. The in-tree examples and benchmarks are not stamped: they are
	# Geneva's own tree, whose commit the HELLO names already.
	INCLUDE(${CMAKE_CURRENT_LIST_DIR}/GemProgramStamp.cmake)

	###############################################################################
	# The test-size label helper (GEM_TEST_SIZE). In-tree only: it is about how THIS suite is
	# scheduled, not about anything an installed Geneva exposes.
	INCLUDE(${CMAKE_CURRENT_LIST_DIR}/GemTestSize.cmake)

	###############################################################################
	# The registration of a test that starts MPI jobs (GEM_ADD_MPI_TEST): a directory of its own
	# for Open MPI's shared-memory files. In-tree only, like the size labels.
	INCLUDE(${CMAKE_CURRENT_LIST_DIR}/GemMpiTest.cmake)

	###############################################################################
	# The check that a library's programs link only the library's own closure
	# (gem_check_link_closure). In-tree only, like the size labels.
	INCLUDE(${CMAKE_CURRENT_LIST_DIR}/GemLinkClosure.cmake)

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
	IF(NOT _gi_bt AND DEFINED GEM_BUILD_TYPE)
		SET(_gi_bt "${GEM_BUILD_TYPE}")
	ENDIF()
	STRING(TOUPPER "${_gi_bt}" _gi_bt_u)
	SEPARATE_ARGUMENTS(GENEVA_INDIVIDUAL_ABI_OPTIONS UNIX_COMMAND
		"${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_gi_bt_u}}")

	INCLUDE(GenevaIndividualModule)

	###############################################################################
	# End of the include-guard

ENDIF(NOT COMMON_GEM_BUILD_INCLUDED)

################################################################################
# Done
