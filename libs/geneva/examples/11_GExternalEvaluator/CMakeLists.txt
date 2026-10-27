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

SET ( EXECUTABLENAME GExternalEvaluator )

SET ( ${EXECUTABLENAME}_SRCS
	GExternalEvaluator.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		# GExternalEvaluatorIndividual is no longer in the geneva library; this example compiles it in via
		# its object library (the alternative to loading it as a module).
		GExternalEvaluatorIndividual-obj)

# Add MPI Libraries in case building with MPI-consumer
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK
			${LIBRARIES_TO_LINK}
			${MPI_LIBRARIES})
ENDIF()

TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK})


GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME}/config )
ADD_SUBDIRECTORY (evaluator)

# This example delegates every evaluation to an EXTERNAL program -- the shipped evaluator/evaluator.py,
# started through its "#!/usr/bin/env python3" shebang. A Python 3 interpreter is therefore a genuine
# external dependency of the test (not of the build), so the test is registered only where configure
# time finds one; everything else about it is unconditional (Inv 9). The run itself halts on the stall
# criterion after a couple of seconds.
FIND_PACKAGE ( Python3 COMPONENTS Interpreter QUIET )
IF ( Python3_Interpreter_FOUND )
	ADD_TEST(NAME ${EXECUTABLENAME} COMMAND ${EXECUTABLENAME}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
	SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "geneva;example" TIMEOUT 300)
	GENEVA_TEST_SIZE(${EXECUTABLENAME} M)
ELSE ()
	MESSAGE ( STATUS
		"Example 11 (GExternalEvaluator): no Python 3 interpreter found -- its end-to-end test is not registered" )
ENDIF ()

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME} )
