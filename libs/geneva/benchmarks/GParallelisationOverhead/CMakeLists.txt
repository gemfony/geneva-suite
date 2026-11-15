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

SET ( EXECUTABLENAME GParallelisationOverhead )

SET ( ${EXECUTABLENAME}_SRCS
	GParallelisationOverhead.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		# GDelayIndividual is no longer in the geneva library; this benchmark compiles it in via its object library.
		GDelayIndividual-obj)


TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK})

GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/tests/geneva/PerformanceTests/${EXECUTABLENAME}/config )

# A throughput-scale run: it sweeps the number of parallel workers against a deliberately delayed
# evaluation, so it takes minutes, not seconds -- 1144 s measured on the development host. Registered
# like every other test; what keeps it out of the development loop is its measured size (XL), not a
# flag.
#
# The TIMEOUT is ~3x that measurement. The former 2500 s was only 2.2x, which is not enough margin for
# a wall-clock measurement of PARALLEL throughput: a slower or busier machine would have tripped the
# clock and reported a defect that is not there.
# Two runs: the serial reference first (it stores its line in the working directory), then the
# measurement, which reads it.
ADD_TEST(NAME ${EXECUTABLENAME}Reference COMMAND ${EXECUTABLENAME} --reference)
SET_TESTS_PROPERTIES(${EXECUTABLENAME}Reference PROPERTIES
		LABELS "benchmark" TIMEOUT 3600 FIXTURES_SETUP ${EXECUTABLENAME}ReferenceLine)
GENEVA_TEST_SIZE(${EXECUTABLENAME}Reference XL)
ADD_TEST(${EXECUTABLENAME} ${EXECUTABLENAME})
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES
		LABELS "benchmark" TIMEOUT 3600 FIXTURES_REQUIRED ${EXECUTABLENAME}ReferenceLine)
GENEVA_TEST_SIZE(${EXECUTABLENAME} XL)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/benchmarks/geneva/PerformanceTests/${EXECUTABLENAME} )

# Draws the speed-up tables of several runs (for example of two consumers) into one figure. It reads
# result tables only and needs nothing of Geneva but the plotting library.
ADD_EXECUTABLE(GSpeedupFigure GSpeedupFigure.cpp)
TARGET_LINK_LIBRARIES(GSpeedupFigure ${DIETRICH_LIBNAME} ${COMMON_LIBNAME} ${Boost_LIBRARIES})
INSTALL ( TARGETS GSpeedupFigure DESTINATION ${INSTALL_PREFIX_DATA}/benchmarks/geneva/PerformanceTests/${EXECUTABLENAME} )

# The tools around this benchmark are checked with short runs (they are not measurements): the
# figure of prepared tables, whose means must be those of compareSpeedups.py; a run of
# runWithClients.sh with two websocket clients, and one with the MPI consumer that shares its serial
# reference; and the refusal of a reference too short to give its line an error. A Python 3
# interpreter and an MPI launcher are dependencies of these tests only, so a test is registered
# where configure finds what it needs.
FIND_PACKAGE(Python3 COMPONENTS Interpreter QUIET)
IF (Python3_Interpreter_FOUND)
	SET(SPEEDUP_TOOLS_CHECK ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
		${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/testSpeedupTools.py)

	ADD_TEST(NAME GSpeedupFigureMeans
		COMMAND ${SPEEDUP_TOOLS_CHECK} figure $<TARGET_FILE:GSpeedupFigure>
			${CMAKE_CURRENT_BINARY_DIR}/check-figure)
	SET_TESTS_PROPERTIES(GSpeedupFigureMeans PROPERTIES LABELS "benchmark" TIMEOUT 60)
	GENEVA_TEST_SIZE(GSpeedupFigureMeans S)

	ADD_TEST(NAME ${EXECUTABLENAME}RunWithClients
		COMMAND ${SPEEDUP_TOOLS_CHECK} run $<TARGET_FILE:${EXECUTABLENAME}>
			${CMAKE_CURRENT_BINARY_DIR}/check-run-beast beast)
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}RunWithClients PROPERTIES
		LABELS "benchmark" TIMEOUT 300 PROCESSORS 3
		FIXTURES_SETUP ${EXECUTABLENAME}SharedReference)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}RunWithClients M)

	ADD_TEST(NAME ${EXECUTABLENAME}ShortReferenceRefused
		COMMAND ${SPEEDUP_TOOLS_CHECK} refusal $<TARGET_FILE:${EXECUTABLENAME}>
			${CMAKE_CURRENT_BINARY_DIR}/check-refusal)
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}ShortReferenceRefused PROPERTIES
		LABELS "benchmark" TIMEOUT 300 PROCESSORS 3)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}ShortReferenceRefused M)

	IF (GENEVA_BUILD_WITH_MPI_CONSUMER AND MPIEXEC_EXECUTABLE)
		ADD_TEST(NAME ${EXECUTABLENAME}RunWithMPIClients
			COMMAND ${CMAKE_COMMAND} -E env
				"MPIRUN=${MPIEXEC_EXECUTABLE} ${MPIEXEC_PREFLAGS} ${MPIEXEC_NUMPROC_FLAG}"
				${SPEEDUP_TOOLS_CHECK} run $<TARGET_FILE:${EXECUTABLENAME}>
				${CMAKE_CURRENT_BINARY_DIR}/check-run-mpi mpi
				${CMAKE_CURRENT_BINARY_DIR}/check-run-beast)
		SET_TESTS_PROPERTIES(${EXECUTABLENAME}RunWithMPIClients PROPERTIES
			LABELS "benchmark" TIMEOUT 300 PROCESSORS 3
			FIXTURES_REQUIRED ${EXECUTABLENAME}SharedReference)
		GENEVA_TEST_SIZE(${EXECUTABLENAME}RunWithMPIClients M)
	ELSE ()
		MESSAGE(STATUS "${EXECUTABLENAME}RunWithMPIClients not registered: no MPI consumer or launcher")
	ENDIF ()
ELSE ()
	MESSAGE(STATUS "The checks of the speed-up tools are not registered: no Python 3 interpreter found")
ENDIF ()
