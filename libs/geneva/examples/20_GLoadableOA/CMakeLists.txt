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

# Example 20 -- a runtime-loadable OPTIMIZATION ALGORITHM. It has TWO artifacts: a loadable algorithm (the
# random-search OA, built as a shared object) and a GENERIC optimizer that loads it via --module and has no
# algorithm compiled in (only the individual is compiled in). It is the OA analogue of example 18's loadable
# individual.

# Headers, listed for CLion indexing.
SET(GLOADABLEOA_INCLUDES
	GRandomSearch.hpp
	GRandomSearch_PersonalityTraits.hpp
)
ADD_CUSTOM_TARGET(CLION_EXAMPLE21_HEADERS SOURCES ${GLOADABLEOA_INCLUDES})

SET(LIBRARIES_TO_LINK
		# The individual (the parabola) is compiled in via its object library (before the Geneva/Boost
		# libraries, the in-tree convention for an object library carrying archive registrations). The ALGORITHM is not
		# compiled in -- it comes from --module.
		GFunctionIndividual-obj
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})

IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK ${LIBRARIES_TO_LINK} ${MPI_LIBRARIES})
ENDIF()

# (1) The loadable OA module -> libGRandomSearch.so. It carries the algorithm + its personality traits + the
# archive tags + the manifest entry point. GENEVA_ADD_INDIVIDUAL_MODULE is kind-agnostic -- it builds
# a shared MODULE that links none of the Geneva libraries (it resolves their symbols from the HOST PROCESS at
# load time via RTLD_GLOBAL, needing only their headers, with pinned ABI flags) -- so it serves an OA module
# exactly as it serves an individual module.
GENEVA_ADD_INDIVIDUAL_MODULE(GRandomSearch
	GRandomSearch.cpp
	GRandomSearch_PersonalityTraits.cpp
	GRandomSearchPlugin.cpp)

# (2) The generic optimizer -> loads an algorithm at runtime. ENABLE_EXPORTS so a module loaded RTLD_GLOBAL
# resolves against one symbol namespace (single archive registry + single Geneva singletons).
ADD_EXECUTABLE(GLoadableOAOptimizer GLoadableOAOptimizer.cpp)
SET_TARGET_PROPERTIES(GLoadableOAOptimizer PROPERTIES ENABLE_EXPORTS TRUE)
TARGET_LINK_LIBRARIES(GLoadableOAOptimizer ${LIBRARIES_TO_LINK})

# Materialize the configuration from code defaults at build time. The loaded algorithm's own config
# (GRandomSearch.json) only exists once the module is loaded, so materialize with the just-built module (and
# depend on it so it exists first).
ADD_DEPENDENCIES(GLoadableOAOptimizer GRandomSearch)
GENEVA_MATERIALIZE_CONFIGS(GLoadableOAOptimizer
	${INSTALL_PREFIX_DATA}/examples/20_GLoadableOA/config
	--module $<TARGET_FILE:GRandomSearch>)

# Module-ABI smoke test: the generic optimizer dlopen()s the just-built OA module through the real loader path
# and optimizes with its "rsearch" mnemonic. This is also the empirical check that the module and process share
# ONE set of singletons / one serialization registry (the loaded algorithm submits through the process
# consumer). Registered unconditionally for the reason given in example 18's CMakeLists.txt: this trio is the
# only automated coverage of the loadable-module ABI, and the run costs well under a second.
ADD_TEST(
	NAME GLoadableOA
	COMMAND GLoadableOAOptimizer --module $<TARGET_FILE:GRandomSearch> --optimizationAlgorithms rsearch
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
SET_TESTS_PROPERTIES(GLoadableOA PROPERTIES LABELS "geneva;example" TIMEOUT 120)
GENEVA_TEST_SIZE(GLoadableOA S)

# The role check, through the same real path: this module is an ALGORITHM, so naming it as the optimization
# problem (--individual) must be refused by name rather than surface much later as a missing problem. What
# is asserted is the DIAGNOSTIC, not the exit status: the example installs no exception handler, so the
# refusal reaches the user through the terminate handler and the process aborts -- which ctest scores as a
# hard failure no matter what PASS_REGULAR_EXPRESSION says. Piping into grep makes the pipeline's status
# grep's, so the test passes exactly when the message is there.
ADD_TEST(
	NAME GLoadableOA_WrongRole
	COMMAND sh -c
		"$<TARGET_FILE:GLoadableOAOptimizer> --individual $<TARGET_FILE:GRandomSearch> 2>&1 | grep -q 'carries no individual'"
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
SET_TESTS_PROPERTIES(GLoadableOA_WrongRole PROPERTIES LABELS "geneva;example" TIMEOUT 120)
GENEVA_TEST_SIZE(GLoadableOA_WrongRole M)

INSTALL(TARGETS GLoadableOAOptimizer GRandomSearch
	DESTINATION ${INSTALL_PREFIX_DATA}/examples/20_GLoadableOA)
