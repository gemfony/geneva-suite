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
# The Geneva quickstart -- a self-contained downstream project.
#
# This directory is not part of the in-tree Geneva build and does not belong to
# the numbered examples: it is the canonical template showing how an external
# project consumes an *installed* Geneva. Copy it out of the tree and it still
# builds. To build it you first need to install Geneva (see INSTALL), then point
# this project at that installation:
#
#   mkdir build && cd build
#   cmake -DCMAKE_PREFIX_PATH=<geneva-install-prefix> ..
#   make
#
# Geneva is located via find_package(Geneva). The preferred mechanism is the
# installed CMake config package (find_package(Geneva CONFIG)). If that is not
# available (an older install without the config package) the build falls back
# to the FindGeneva.cmake module of the installation. In both cases the install prefix is
# supplied through CMAKE_PREFIX_PATH or the GENEVA_ROOT variable, e.g.
#
#   cmake -DGENEVA_ROOT=<geneva-install-prefix> ..
#
################################################################################

CMAKE_MINIMUM_REQUIRED(VERSION 4.0)

PROJECT(GStarter CXX)

# Allow GENEVA_ROOT to be used interchangeably with CMAKE_PREFIX_PATH so users
# can point at the installation either way.
IF (DEFINED GENEVA_ROOT)
	LIST(APPEND CMAKE_PREFIX_PATH "${GENEVA_ROOT}")
ENDIF ()

# The installation carries FindGeneva.cmake in its CMakeModules directory, for
# the MODULE-mode fallback below.
FOREACH (_geneva_prefix IN LISTS CMAKE_PREFIX_PATH)
	LIST(APPEND CMAKE_MODULE_PATH "${_geneva_prefix}/CMakeModules")
ENDFOREACH ()

# Prefer the installed CMake config package (Geneva::* imported targets). Fall
# back to the FindGeneva.cmake module if no config package is present.
FIND_PACKAGE(Geneva CONFIG QUIET)
IF (NOT Geneva_FOUND)
	MESSAGE(STATUS "Geneva config package not found, falling back to FindGeneva.cmake (MODULE mode)")
	FIND_PACKAGE(Geneva MODULE REQUIRED)
ENDIF ()

################################################################################
# The GStarter executable.
#
# Linking Geneva::geneva transitively pulls in courtier/hap/common, the Geneva
# include directories, Boost, the C++23 requirement and (when the install was
# built with it) the MPI consumer dependencies.

# Package the GStarter individual BOTH ways from one source (the DA1b "build both" model), using the helper
# the installed Geneva CMake package exports -- find_package(Geneva) above brought GENEVA_DECLARE_INDIVIDUAL
# into scope, so this standalone/out-of-tree example authors its loadable module with the exact same one-line
# declaration the in-tree examples use. It produces a compiled-in object library (GStarterIndividual-obj) and
# a runtime-loadable module (libGStarterIndividual.so). The individual's serialization export lives in
# GStarterIndividual.cpp (SOURCES, in both targets); the module's fixed entry point is
# GStarterIndividualPlugin.cpp (PLUGIN, module only).
#! [quickstart-cmake-declare]
GENEVA_DECLARE_INDIVIDUAL(GStarterIndividual
	MODE    both
	SOURCES GStarterIndividual.cpp
	PLUGIN  GStarterIndividualPlugin.cpp
	CONFIG  ./config/GStarterIndividual.json)

# (1) The COMPILED-IN variant: the classic bespoke GStarter driver with the individual
# linked in. The object library is listed BEFORE Geneva::geneva, the in-tree convention
# for an object library carrying archive registrations (see the link-order note in
# GENEVA_DECLARE_INDIVIDUAL).
ADD_EXECUTABLE(GStarter GStarter.cpp)
TARGET_LINK_LIBRARIES(GStarter PRIVATE GStarterIndividual-obj Geneva::geneva)
TARGET_COMPILE_FEATURES(GStarter PRIVATE cxx_std_23)
INSTALL(TARGETS GStarter DESTINATION bin)

# (2) The LOADABLE variant's runner: a generic optimizer with NO problem compiled in,
# which loads the just-built module at runtime via --individual (the GLoadableIndividual
# example's pattern). ENABLE_EXPORTS so a plugin loaded RTLD_GLOBAL resolves against one
# symbol namespace (single archive registry + Geneva singletons).
ADD_EXECUTABLE(GStarterGenericOptimizer GStarterGenericOptimizer.cpp)
SET_TARGET_PROPERTIES(GStarterGenericOptimizer PROPERTIES ENABLE_EXPORTS TRUE)
TARGET_LINK_LIBRARIES(GStarterGenericOptimizer PRIVATE Geneva::geneva)
TARGET_COMPILE_FEATURES(GStarterGenericOptimizer PRIVATE cxx_std_23)
ADD_DEPENDENCIES(GStarterGenericOptimizer GStarterIndividual)
INSTALL(TARGETS GStarterGenericOptimizer GStarterIndividual DESTINATION bin)
#! [quickstart-cmake-declare]

################################################################################
# Stamp both programs with the commit of this project's sources (when they are inside a git work
# tree, at its top or in a subdirectory; "-dirty" when a tracked file has changed). A server names it in the greeting it sends
# each client, and a client built from other sources ends instead of computing with them. Add
# IDENTITY <text> to give a text of your own instead.

GEM_STAMP_PROGRAM(GStarter)
GEM_STAMP_PROGRAM(GStarterGenericOptimizer)

################################################################################
# Materialize the configuration files from code defaults at build time, so the
# example can be run directly from the build tree (GStarter expects
# ./config/*.json). find_package(Geneva) above exported the same
# GEM_MATERIALIZE_CONFIGS mechanism the in-tree examples use: after GStarter
# is built it runs once with --update-configs, which (re)creates every config
# file the example reads under <build>/config from the code's registered
# defaults. No hand-maintained config files are shipped.

GEM_MATERIALIZE_CONFIGS(GStarter bin/config)

################################################################################
# Optional unit test.
# The unit test runs the standard tests of the installed collection's harness
# (common/testing/GStandardTests.hpp, found through Geneva::geneva's include
# directories) on GStarterIndividual, plus the individual's own test cases. It
# needs Catch2 v3 and nothing else; without Catch2 it is skipped with a notice.

FIND_PACKAGE(Catch2 3 QUIET)

# Unconditional, so that a build in which the test is skipped still answers
# "ctest" with an empty test set rather than "no test configuration file found".
ENABLE_TESTING()

IF (Catch2_FOUND)
	SET(TESTNAME GStarterIndividualUnitTests)

	ADD_EXECUTABLE(${TESTNAME} Tests/UnitTests/GStarterIndividualUnitTests.cpp)

	# The object library before Geneva::geneva, as for GStarter above.
	TARGET_LINK_LIBRARIES(${TESTNAME} PRIVATE
		GStarterIndividual-obj
		Geneva::geneva
		Catch2::Catch2WithMain
	)

	TARGET_COMPILE_FEATURES(${TESTNAME} PRIVATE cxx_std_23)

	ADD_TEST(NAME ${TESTNAME} COMMAND ${TESTNAME})
ELSE ()
	MESSAGE(STATUS "Skipping GStarterIndividualUnitTests: Catch2 v3 was not found")
ENDIF ()
