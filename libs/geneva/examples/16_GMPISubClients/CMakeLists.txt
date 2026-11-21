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

# The individual is free of MPI: it is compiled without MPI's include path, and its source refuses
# to compile where an MPI header was included (directly, or through GParaboloidGroup.hpp), also
# where mpi.h is on the compiler's own search path.
ADD_LIBRARY(${EXECUTABLENAME}Individual OBJECT GParaboloidGroupIndividual.cpp)
TARGET_LINK_LIBRARIES(${EXECUTABLENAME}Individual PRIVATE ${GENEVA_LIBRARIES})

ADD_EXECUTABLE(${EXECUTABLENAME}
	GMPISubClients.cpp
	GParaboloidGroup.cpp
	$<TARGET_OBJECTS:${EXECUTABLENAME}Individual>
)

# The cascade lives in courtier's MPI library. The MPI consumer of the submitter and of the leads is
# Geneva's MPI module, which libgemfony-geneva loads at run time from its own directory: it is built
# with the example, not linked.
TARGET_LINK_LIBRARIES(${EXECUTABLENAME}
		${COURTIER_MPI_LIBNAME}
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		MPI::MPI_CXX)

# config/config-overrides.json: a small population, so that a run is short (Inv 22)
GEM_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME}/config )

# Run under the detected mpirun with 9 processes: the submitter and two groups of four (a lead and
# three members; sub_client_group_size is 4 in config/GMPICascade.json). One iteration of a small
# population; every evaluation sleeps 50 ms per parameter, shared out over the group. Every member
# must serve rounds (../../../courtier/tests/CheckJobEnds.cmake: exit code 0, the members' lines,
# none with 0 rounds, a time limit).
GEM_ADD_MPI_TEST(NAME ${EXECUTABLENAME} TIMEOUT 180
	COMMAND ${CMAKE_COMMAND}
		"-DCOMMAND=${MPIEXEC_EXECUTABLE};${MPIEXEC_NUMPROC_FLAG};9;${MPIEXEC_PREFLAGS};$<TARGET_FILE:${EXECUTABLENAME}>;${MPIEXEC_POSTFLAGS};--consumer;mpi;--eaMaxIterations;1"
		-DEXPECT_EXIT=zero
		"-DEXPECT=Optimization finished;member 1 served;member 2 served;member 3 served"
		"-DREFUSE=served 0 rounds"
		-DLIMIT=150
		-P ${PROJECT_SOURCE_DIR}/libs/courtier/tests/CheckJobEnds.cmake
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
#
# PROCESSORS 9 tells the `ctest -j` scheduler what this entry really costs: one CTest slot but nine
# live ranks. Without it the scheduler counts it as one and oversubscribes the machine.
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "geneva;example" PROCESSORS 9)
GEM_TEST_SIZE(${EXECUTABLENAME} L)

# A help request under the launcher ends with 0: no rank starts the cascade, and with it MPI,
# before Go2 prints the text and ends the process. Each rank runs in a directory of its own.
SET(_help_dir ${CMAKE_CURRENT_BINARY_DIR}/help)
FILE(MAKE_DIRECTORY ${_help_dir})
GEM_ADD_MPI_TEST(NAME ${EXECUTABLENAME}Help TIMEOUT 60
	COMMAND ${CMAKE_COMMAND}
		"-DCOMMAND=${MPIEXEC_EXECUTABLE};${MPIEXEC_NUMPROC_FLAG};3;${MPIEXEC_PREFLAGS};${PROJECT_SOURCE_DIR}/libs/geneva/tests/MPITests/runInRankDirectory.sh;$<TARGET_FILE:${EXECUTABLENAME}>;${MPIEXEC_POSTFLAGS};--consumer;mpi;--help"
		-DEXPECT_EXIT=zero
		-DEXPECT=Usage:
		-DLIMIT=30
		-P ${PROJECT_SOURCE_DIR}/libs/courtier/tests/CheckJobEnds.cmake
	WORKING_DIRECTORY ${_help_dir})
SET_TESTS_PROPERTIES(${EXECUTABLENAME}Help PROPERTIES LABELS "geneva;example" PROCESSORS 3)
GEM_TEST_SIZE(${EXECUTABLENAME}Help S)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME} )
