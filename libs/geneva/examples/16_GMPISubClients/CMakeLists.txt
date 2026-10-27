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

SET ( EXECUTABLENAME GMPISubClients )

SET ( ${EXECUTABLENAME}_SRCS
	GMPISubClients.cpp
	GMPISubClientParaboloidIndividualMultiD.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})

# Add MPI Libraries in case building with MPI-consumer
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK
			${LIBRARIES_TO_LINK}
			${MPI_LIBRARIES})
ENDIF()

TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK}
		)

GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME}/config )

# Run as a multi-process program under the detected launcher, like example 15 and the courtier MPI
# tests. The rank count is dictated by the example's own topology: sub_client_group_size is 4, so 9
# ranks = 1 master + two groups of (1 geneva client + 3 sub-clients). The consumer must be named
# explicitly -- without it the example builds the default local consumer and every rank throws.
#
# --eaMaxIterations 1 is the budget. This individual deliberately SIMULATES an expensive evaluation
# (a 50 ms sleep per parameter, spread over the sub-client group), so a full run costs minutes; one
# generation already exercises the whole sub-client path -- scatter, per-rank work, gather, shutdown
# barrier -- which is what the example demonstrates (Inv 22).
ADD_TEST(NAME ${EXECUTABLENAME}
	COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 9
			${MPIEXEC_PREFLAGS} $<TARGET_FILE:${EXECUTABLENAME}> ${MPIEXEC_POSTFLAGS}
			--consumer mpi --eaMaxIterations 1
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
#
# PROCESSORS 9 tells the `ctest -j` scheduler what this entry really costs: one CTest slot but nine
# live ranks. Without it the scheduler counts it as one and oversubscribes the machine.
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "geneva;example" TIMEOUT 600 PROCESSORS 9)
GENEVA_TEST_SIZE(${EXECUTABLENAME} L)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME} )
