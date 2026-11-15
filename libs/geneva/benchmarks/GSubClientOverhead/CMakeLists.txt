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

# The speed-up of clients that share their evaluations with MPI sub-clients; built where the MPI
# consumer is (benchmarks/CMakeLists.txt). It shares the measurement with GParallelisationOverhead
# (GSpeedupMeasurement.hpp there), which itself needs no MPI.
SET ( EXECUTABLENAME GSubClientOverhead )

ADD_EXECUTABLE(${EXECUTABLENAME}
	GSubClientOverhead.cpp
	GSleepGroup.cpp
	GSleepGroupIndividual.cpp
)
TARGET_INCLUDE_DIRECTORIES(${EXECUTABLENAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../GParallelisationOverhead)

# GMPISubClientOptimizer lives in Geneva's MPI library, the MPI environment in courtier's.
TARGET_LINK_LIBRARIES(${EXECUTABLENAME}
		${GENEVA_MPI_LIBNAME}
		${COURTIER_MPI_LIBNAME}
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		MPI::MPI_CXX)

GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/benchmarks/geneva/PerformanceTests/${EXECUTABLENAME}/config )

# Both modes with two clients of two sub-clients each and a few iterations: every evaluation must
# be done once, in a group of three, and every process must end by itself (testSpeedupTools.py
# beside GParallelisationOverhead). The websocket run measures the serial reference the MPI run
# then uses. Python 3 and the MPI launcher are dependencies of the tests only.
FIND_PACKAGE(Python3 COMPONENTS Interpreter QUIET)
IF (Python3_Interpreter_FOUND AND MPIEXEC_EXECUTABLE)
	SET(SUBCLIENT_CHECK ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
		"MPIRUN=${MPIEXEC_EXECUTABLE} ${MPIEXEC_PREFLAGS} ${MPIEXEC_NUMPROC_FLAG}"
		${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/../GParallelisationOverhead/testSpeedupTools.py
		subclients $<TARGET_FILE:${EXECUTABLENAME}>)

	ADD_TEST(NAME ${EXECUTABLENAME}Websocket
		COMMAND ${SUBCLIENT_CHECK} ${CMAKE_CURRENT_BINARY_DIR}/check-websocket beast)
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}Websocket PROPERTIES
		LABELS "benchmark" TIMEOUT 300 PROCESSORS 7
		FIXTURES_SETUP ${EXECUTABLENAME}Reference)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}Websocket M)

	ADD_TEST(NAME ${EXECUTABLENAME}MPI
		COMMAND ${SUBCLIENT_CHECK} ${CMAKE_CURRENT_BINARY_DIR}/check-mpi mpi
			${CMAKE_CURRENT_BINARY_DIR}/check-websocket)
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}MPI PROPERTIES
		LABELS "benchmark" TIMEOUT 300 PROCESSORS 7
		FIXTURES_REQUIRED ${EXECUTABLENAME}Reference)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}MPI M)
ELSE ()
	MESSAGE(STATUS "The checks of ${EXECUTABLENAME} are not registered: no Python 3 or no MPI launcher")
ENDIF ()

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/benchmarks/geneva/PerformanceTests/${EXECUTABLENAME} )
