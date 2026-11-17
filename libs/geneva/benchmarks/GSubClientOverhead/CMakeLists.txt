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
# (GSpeedupMeasurement.hpp there), which itself needs no MPI. The groups of processes are those of
# courtier's MPI cascade (libgemfony-courtier-mpi).
SET ( EXECUTABLENAME GSubClientOverhead )

# The individual is free of MPI: it is compiled without MPI's include path, and its source refuses
# to compile where an MPI header was included (directly, or through GSleepGroup.hpp), also where
# mpi.h is on the compiler's own search path. A client with sub-clients evaluates it in its group
# through the lead's item processor (GSubClientOverhead.cpp).
ADD_LIBRARY(${EXECUTABLENAME}Individual OBJECT GSleepGroupIndividual.cpp)
TARGET_LINK_LIBRARIES(${EXECUTABLENAME}Individual PRIVATE ${GENEVA_LIBRARIES})

ADD_EXECUTABLE(${EXECUTABLENAME}
	GSubClientOverhead.cpp
	GSleepGroup.cpp
	$<TARGET_OBJECTS:${EXECUTABLENAME}Individual>
)
TARGET_INCLUDE_DIRECTORIES(${EXECUTABLENAME}
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../GParallelisationOverhead)

# The cascade lives in courtier's MPI library. The MPI consumer of the submitter and of the leads is
# Geneva's MPI module, which libgemfony-geneva loads at run time from its own directory: it is built
# with the benchmark, not linked.
TARGET_LINK_LIBRARIES(${EXECUTABLENAME}
		${COURTIER_MPI_LIBNAME}
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		MPI::MPI_CXX)
ADD_DEPENDENCIES(${EXECUTABLENAME} ${GENEVA_MPI_LIBNAME})

GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME}
	${INSTALL_PREFIX_DATA}/benchmarks/geneva/PerformanceTests/${EXECUTABLENAME}/config )

# Both modes with two clients of two sub-clients each and a few iterations: every evaluation must
# be done once, in a group of three, every process must end by itself, and both clients must use
# the prefetch depth passed on the command line (testSpeedupTools.py beside
# GParallelisationOverhead). The websocket run measures the serial reference the MPI run then
# uses. Another check starts jobs that do not form whole groups, in both layouts, and jobs whose
# submitter fails: they must end at once (each case is bounded at 60 s, the entry at 4 x 80 s).
# Python 3 and the MPI launcher are dependencies of the tests only.
FIND_PACKAGE(Python3 COMPONENTS Interpreter QUIET)
IF (Python3_Interpreter_FOUND AND MPIEXEC_EXECUTABLE)
	SET(SPEEDUP_TOOLS ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
		"MPIRUN=${MPIEXEC_EXECUTABLE} ${MPIEXEC_PREFLAGS} ${MPIEXEC_NUMPROC_FLAG}"
		${Python3_EXECUTABLE} -B
		${CMAKE_CURRENT_SOURCE_DIR}/../GParallelisationOverhead/testSpeedupTools.py)
	SET(SUBCLIENT_CHECK ${SPEEDUP_TOOLS} subclients $<TARGET_FILE:${EXECUTABLENAME}>)

	GENEVA_ADD_MPI_TEST(NAME ${EXECUTABLENAME}Websocket TIMEOUT 300
		COMMAND ${SUBCLIENT_CHECK} ${CMAKE_CURRENT_BINARY_DIR}/check-websocket beast)
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}Websocket PROPERTIES
		LABELS "benchmark" PROCESSORS 7
		FIXTURES_SETUP ${EXECUTABLENAME}Reference)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}Websocket M)

	GENEVA_ADD_MPI_TEST(NAME ${EXECUTABLENAME}MPI TIMEOUT 300
		COMMAND ${SUBCLIENT_CHECK} ${CMAKE_CURRENT_BINARY_DIR}/check-mpi mpi
			${CMAKE_CURRENT_BINARY_DIR}/check-websocket)
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}MPI PROPERTIES
		LABELS "benchmark" PROCESSORS 7
		FIXTURES_REQUIRED ${EXECUTABLENAME}Reference)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}MPI M)

	# The websocket check again, with both client jobs on one Open MPI job id: they must not share
	# Open MPI's shared memory, whose files are named after the job id (runWithClients.sh gives
	# every job a directory of its own for them). The check places the launchers on pids whose
	# job ids collide, by Open MPI's rule; so it is registered where the launcher is Open MPI's and
	# the pids can be read from /proc. While the second launcher is placed, up to 16 racers run
	# beside the 7 processes of the run: 23 processors. On a host whose pid_max is too small for the
	# placement the check ends with 77 and is skipped (testSpeedupTools.py says why).
	EXECUTE_PROCESS(COMMAND ${MPIEXEC_EXECUTABLE} --version
		OUTPUT_VARIABLE LAUNCHER_VERSION ERROR_QUIET TIMEOUT 30)
	IF (LAUNCHER_VERSION MATCHES "Open MPI" AND EXISTS /proc/sys/kernel/ns_last_pid)
		GENEVA_ADD_MPI_TEST(NAME ${EXECUTABLENAME}SameJobId TIMEOUT 300
			COMMAND ${SPEEDUP_TOOLS} samejobid $<TARGET_FILE:${EXECUTABLENAME}>
				${CMAKE_CURRENT_BINARY_DIR}/check-same-job-id
				${CMAKE_CURRENT_BINARY_DIR}/check-websocket)
		SET_TESTS_PROPERTIES(${EXECUTABLENAME}SameJobId PROPERTIES
			LABELS "benchmark" PROCESSORS 23 SKIP_RETURN_CODE 77
			FIXTURES_REQUIRED ${EXECUTABLENAME}Reference)
		GENEVA_TEST_SIZE(${EXECUTABLENAME}SameJobId M)
	ELSE ()
		MESSAGE(STATUS
			"${EXECUTABLENAME}SameJobId not registered: the MPI launcher is not Open MPI's, or "
			"there is no /proc/sys/kernel/ns_last_pid")
	ENDIF ()

	# runWithClients.sh, ended from outside while it measures (SIGINT, as Ctrl-C), ends the server,
	# the clients and their MPI jobs: nothing it started may remain, nor the geneva-sm.* directory
	# of their shared-memory files.
	GENEVA_ADD_MPI_TEST(NAME ${EXECUTABLENAME}Interrupted TIMEOUT 180
		COMMAND ${SPEEDUP_TOOLS} interrupt $<TARGET_FILE:${EXECUTABLENAME}>
			${CMAKE_CURRENT_BINARY_DIR}/check-interrupted
			${CMAKE_CURRENT_BINARY_DIR}/check-websocket)
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}Interrupted PROPERTIES
		LABELS "benchmark" PROCESSORS 7
		FIXTURES_REQUIRED ${EXECUTABLENAME}Reference)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}Interrupted M)

	GENEVA_ADD_MPI_TEST(NAME ${EXECUTABLENAME}Groups TIMEOUT 360
		COMMAND ${SPEEDUP_TOOLS} groups $<TARGET_FILE:${EXECUTABLENAME}>
			${CMAKE_CURRENT_BINARY_DIR}/check-groups)
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}Groups PROPERTIES LABELS "benchmark" PROCESSORS 6)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}Groups S)
ELSE ()
	MESSAGE(STATUS
		"The checks of ${EXECUTABLENAME} are not registered: no Python 3 or no MPI launcher")
ENDIF ()

INSTALL ( TARGETS ${EXECUTABLENAME}
	DESTINATION ${INSTALL_PREFIX_DATA}/benchmarks/geneva/PerformanceTests/${EXECUTABLENAME} )
