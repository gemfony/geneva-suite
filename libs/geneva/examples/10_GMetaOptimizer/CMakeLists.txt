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

SET ( EXECUTABLENAME GMetaOptimizer )

SET ( ${EXECUTABLENAME}_SRCS
	GMetaOptimizer.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		# The meta-optimizer and its umbrella individual are no longer in the geneva library; compiled in via
		# their object libraries (the meta individual's instantiation needs GFunctionIndividual's symbols too).
		GMetaOptimizerIndividualT-obj
		GFunctionIndividual-obj)

# Add MPI Libraries in case building with MPI-consumer
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK
			${LIBRARIES_TO_LINK}
			${MPI_LIBRARIES})
ENDIF()

TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK})


GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME}/config )

# A meta-optimization is an optimization whose every EVALUATION is itself a full optimization run, so
# its cost is the product of three budgets. The code defaults multiply out to a run of well over ten
# minutes, which is a convergence-scale run, not a development one (Inv 22). config-overrides.json
# therefore ships the demo with one sub-run per evaluation, a small child population and a short
# sub-EA -- the whole meta loop is still exercised, in seconds instead of minutes. Unlike example 14 the
# budget belongs in the configuration rather than on the test's command line: none of these three
# knobs is reachable from the command line (the meta level is driven by a directly constructed
# GMetaEvolutionaryAlgorithm, and the sub-EA comes from its own factory), and a user who wants a
# convergence-scale meta-optimization raises them in the config, which is what it is for.
ADD_TEST(NAME ${EXECUTABLENAME} COMMAND ${EXECUTABLENAME}
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "geneva;example" TIMEOUT 300)
GENEVA_TEST_SIZE(${EXECUTABLENAME} L)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME} )
