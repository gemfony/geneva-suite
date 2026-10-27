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

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		Catch2::Catch2WithMain)

IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK
			${LIBRARIES_TO_LINK}
			${MPI_LIBRARIES})
ENDIF()

# Helper: declare a Courtier Catch2 test executable
#
#     geneva_courtier_test(<exe-name> <size> <source-files...>)
#
# register it with CTest under the "courtier" label plus its size label, install it, and add
# it to the tests-courtier-unit aggregate.
#
# <size> is S, M, L or XL, measured -- see the "Running the test suite" section of INSTALL. It
# is a required argument on purpose: a new test cannot be added without saying what it costs,
# because the size is what decides whether a gate runs it.
FUNCTION(geneva_courtier_test name size)
	ADD_EXECUTABLE(${name} ${ARGN})
	TARGET_LINK_LIBRARIES(${name} ${LIBRARIES_TO_LINK})
	ADD_TEST(${name} ${name})
	SET_TESTS_PROPERTIES(${name} PROPERTIES LABELS "courtier")
	GENEVA_TEST_SIZE(${name} ${size})
	INSTALL(TARGETS ${name} DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
ENDFUNCTION()

# Submission-path unit tests (formerly courtier2/tests/UnitTests, merged in at Stage E). The test
# executable / file names still carry the "2" until the Stage-E identifier rename (E2); the CTest
# labels and install paths are already "courtier".
#
# None of these needs a RESOURCE_LOCK under `ctest -j`, including the two that really put a server on
# the loopback interface: GCourtierAsioTests and GCourtierWebsocketTests construct their consumer with
# port 0 and ask the bound socket for the port it actually got (getPort()), so the kernel hands every
# concurrent instance a distinct ephemeral port. Keep it that way -- a test that hard-codes a port
# number is the thing that would need serializing.
geneva_courtier_test(GWireBlobRegistryTests  S  GWireBlobRegistryTests.cpp)
geneva_courtier_test(GProcessableIdentityTests S  GProcessableIdentityTests.cpp)
geneva_courtier_test(GCommandContainerSerializationTests S  GCommandContainerSerializationTests.cpp)
geneva_courtier_test(GProcessingStatusTests    S  GProcessingStatusTests.cpp)
geneva_courtier_test(GCourtierPolicyTests      S  GCourtierPolicyTests.cpp)
geneva_courtier_test(GCourtierTimeoutTests     M  GCourtierTimeoutTests.cpp)
geneva_courtier_test(GCourtierConcurrencyTests S  GCourtierConcurrencyTests.cpp)
geneva_courtier_test(GCourtierAsioTests        M  GCourtierAsioTests.cpp)
geneva_courtier_test(GCourtierWebsocketTests   M  GCourtierWebsocketTests.cpp)

# The fatal reconciliation paths exit the process cleanly with a non-zero status (via LOGEXIT, no
# core dump), so they are exercised as subprocesses: the "ok" control must succeed (exit 0); each
# fatal scenario must exit non-zero, which CTest's WILL_FAIL asserts directly.
ADD_EXECUTABLE(GCourtierFatalExerciser GCourtierFatalExerciser.cpp)
TARGET_LINK_LIBRARIES(GCourtierFatalExerciser ${LIBRARIES_TO_LINK})
INSTALL(TARGETS GCourtierFatalExerciser
		DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)

ADD_TEST(NAME GCourtierFatal_ok COMMAND GCourtierFatalExerciser --scenario=ok)
SET_TESTS_PROPERTIES(GCourtierFatal_ok PROPERTIES LABELS "courtier")
GENEVA_TEST_SIZE(GCourtierFatal_ok S)

FOREACH(scenario fatal-unfixable fatal-none-fatal fatal-none-clone)
	ADD_TEST(NAME GCourtierFatal_${scenario}
			COMMAND GCourtierFatalExerciser --scenario=${scenario})
	SET_TESTS_PROPERTIES(GCourtierFatal_${scenario} PROPERTIES LABELS "courtier" WILL_FAIL TRUE)
	GENEVA_TEST_SIZE(GCourtierFatal_${scenario} S)
ENDFOREACH()

# The MPI consumer fixes its rank layout at launch, so it is exercised as a multi-process program
# under mpirun (1 master + 2 workers) rather than as an in-process Catch2 test.
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	ADD_EXECUTABLE(GCourtierMpiExerciser GCourtierMpiExerciser.cpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiExerciser ${LIBRARIES_TO_LINK})
	INSTALL(TARGETS GCourtierMpiExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	ADD_TEST(NAME GCourtierMpiTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> ${MPIEXEC_POSTFLAGS})
	# PROCESSORS tells the `ctest -j` scheduler what this entry really costs: one CTest slot but three
	# live processes. Without it the scheduler counts it as one and oversubscribes the machine.
	SET_TESTS_PROPERTIES(GCourtierMpiTest PROPERTIES LABELS "courtier" PROCESSORS 3)
	GENEVA_TEST_SIZE(GCourtierMpiTest M)

	# Large-payload variant: each work item far exceeds the former fixed 20 KB MPI message cap,
	# so this covers the dynamically-sized (MPI_Iprobe + MPI_Get_count) receive path.
	ADD_EXECUTABLE(GCourtierMpiLargePayloadExerciser GCourtierMpiLargePayloadExerciser.cpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiLargePayloadExerciser ${LIBRARIES_TO_LINK})
	INSTALL(TARGETS GCourtierMpiLargePayloadExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	ADD_TEST(NAME GCourtierMpiLargePayloadTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiLargePayloadExerciser> ${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiLargePayloadTest PROPERTIES LABELS "courtier" PROCESSORS 3)
	GENEVA_TEST_SIZE(GCourtierMpiLargePayloadTest M)
ENDIF()

GENEVA_AGGREGATE_TARGET( "tests-courtier-unit" )
