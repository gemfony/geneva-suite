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
#
# An optional "SPEC <test spec>" before the sources makes the entry run only the Catch2 test cases
# the spec selects (see geneva_courtier_test_entry() for a second entry of the same executable).
FUNCTION(geneva_courtier_test name size)
	CMAKE_PARSE_ARGUMENTS(PARSE_ARGV 2 arg "" "SPEC" "")
	ADD_EXECUTABLE(${name} ${arg_UNPARSED_ARGUMENTS})
	TARGET_LINK_LIBRARIES(${name} ${LIBRARIES_TO_LINK})
	ADD_TEST(${name} ${name} ${arg_SPEC})
	SET_TESTS_PROPERTIES(${name} PROPERTIES LABELS "courtier")
	GENEVA_TEST_SIZE(${name} ${size})
	INSTALL(TARGETS ${name} DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
ENDFUNCTION()

# Helper: a further CTest entry of an executable declared with geneva_courtier_test(), running the
# test cases <spec> selects
#
#     geneva_courtier_test_entry(<entry-name> <exe-name> <size> <spec>)
FUNCTION(geneva_courtier_test_entry entry exe size spec)
	ADD_TEST(${entry} ${exe} ${spec})
	SET_TESTS_PROPERTIES(${entry} PROPERTIES LABELS "courtier")
	GENEVA_TEST_SIZE(${entry} ${size})
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
geneva_courtier_test(GServerSessionLogicTests  S  GServerSessionLogicTests.cpp)
geneva_courtier_test(GProcessingStatusTests    S  GProcessingStatusTests.cpp)
geneva_courtier_test(GCourtierPolicyTests      S  GCourtierPolicyTests.cpp)
geneva_courtier_test(GCourtierConsumerConfigTests S  GCourtierConsumerConfigTests.cpp)
geneva_courtier_test(GCourtierTimeoutTests     M  GCourtierTimeoutTests.cpp)
geneva_courtier_test(GCourtierConcurrencyTests S  GCourtierConcurrencyTests.cpp)
geneva_courtier_test(GCourtierClientTests      M  GCourtierClientTests.cpp)
geneva_courtier_test(GCourtierAsioTests        M  GCourtierAsioTests.cpp)
geneva_courtier_test(GCourtierWebsocketTests   M  GCourtierWebsocketTests.cpp)
geneva_courtier_test(GCourtierWebsocketPeerTests M  GCourtierWebsocketPeerTests.cpp)
# The rules of GPeerI.hpp on the in-process pair of GInProcessPeer.hpp (a header of this
# directory, used by later tests of the roles as their transport)
geneva_courtier_test(GCourtierPeerRulesTests   S  GCourtierPeerRulesTests.cpp GInProcessPeer.hpp
	GInProcessTestSupport.hpp)
geneva_courtier_test(GCourtierPeerSessionTests S  GCourtierPeerSessionTests.cpp GInProcessPeer.hpp
	GInProcessTestSupport.hpp)
geneva_courtier_test(GCourtierPeerConsumerTests S  GCourtierPeerConsumerTests.cpp GInProcessPeer.hpp
	GInProcessTestSupport.hpp)
geneva_courtier_test(GCourtierLifecycleTests   M  GCourtierLifecycleTests.cpp)
geneva_courtier_test(GCourtierWebsocketLivenessTests L  SPEC "~[slowcodec]" GCourtierWebsocketLivenessTests.cpp)
# A connection survives a message that takes one end three ping intervals to encode or decode: six
# cases of 4 to 7 s, in an entry of their own so that neither entry grows beyond L
geneva_courtier_test_entry(GCourtierWebsocketCodecLivenessTests GCourtierWebsocketLivenessTests L "[slowcodec]")

# The GPU consumer's own logic, exercised against a stand-in backend: no device is needed, but the
# consumer's backend factory is, and that is compiled into the library only with the GPU consumer.
IF(GENEVA_GPU_CONSUMER_ENABLED)
	geneva_courtier_test(GCourtierGpuConsumerTests S  GCourtierGpuConsumerTests.cpp)
ENDIF()

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

FOREACH(scenario fatal-unfixable fatal-none-fatal fatal-none-clone fatal-oversized-batch)
	ADD_TEST(NAME GCourtierFatal_${scenario}
			COMMAND GCourtierFatalExerciser --scenario=${scenario})
	SET_TESTS_PROPERTIES(GCourtierFatal_${scenario} PROPERTIES LABELS "courtier" WILL_FAIL TRUE)
	GENEVA_TEST_SIZE(GCourtierFatal_${scenario} S)
ENDFOREACH()

# The MPI consumer fixes its rank layout at launch, so it is exercised as a multi-process program
# under mpirun (1 master + 2 workers) rather than as an in-process Catch2 test.
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	ADD_EXECUTABLE(GCourtierMpiExerciser GCourtierMpiExerciser.cpp)
	# The exercisers use the header-only MPI consumer directly, so they -- and only they -- link MPI.
	TARGET_LINK_LIBRARIES(GCourtierMpiExerciser ${LIBRARIES_TO_LINK} MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	ADD_TEST(NAME GCourtierMpiTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> ${MPIEXEC_POSTFLAGS})
	# PROCESSORS tells the `ctest -j` scheduler what this entry really costs: one CTest slot but three
	# live processes. Without it the scheduler counts it as one and oversubscribes the machine.
	SET_TESTS_PROPERTIES(GCourtierMpiTest PROPERTIES LABELS "courtier" PROCESSORS 3)
	GENEVA_TEST_SIZE(GCourtierMpiTest M)

	# An evaluation that throws must not end the worker rank: the item comes back flagged and the
	# submission policy deals with it.
	ADD_TEST(NAME GCourtierMpiThrowingItemTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=throwing
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiThrowingItemTest PROPERTIES LABELS "courtier" PROCESSORS 3)
	GENEVA_TEST_SIZE(GCourtierMpiThrowingItemTest M)

	# A master consumer that is destroyed without ever starting its server (a single rank suffices).
	ADD_TEST(NAME GCourtierMpiNeverStartedTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 1
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=never-started
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiNeverStartedTest PROPERTIES LABELS "courtier")
	GENEVA_TEST_SIZE(GCourtierMpiNeverStartedTest M)

	# A program that initialized MPI itself, below MPI_THREAD_MULTIPLE, is refused. An MPI library
	# that grants the full level whatever is asked for cannot be checked; the entry is then skipped.
	ADD_TEST(NAME GCourtierMpiThreadLevelTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 1
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=low-thread-level
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiThreadLevelTest PROPERTIES LABELS "courtier" SKIP_RETURN_CODE 77)
	GENEVA_TEST_SIZE(GCourtierMpiThreadLevelTest M)

	# A cleanup interval of 0 ms is refused: the master's cleanup thread would occupy a core.
	ADD_TEST(NAME GCourtierMpiCleanIntervalTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 1
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=zero-clean-interval
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiCleanIntervalTest PROPERTIES LABELS "courtier")
	GENEVA_TEST_SIZE(GCourtierMpiCleanIntervalTest M)

	# A master session that has not sent anything can be asked whether its send has completed.
	ADD_TEST(NAME GCourtierMpiIdleSessionTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 1
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=idle-session
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiIdleSessionTest PROPERTIES LABELS "courtier")
	GENEVA_TEST_SIZE(GCourtierMpiIdleSessionTest M)

	# A message a worker cannot decode must not end the worker rank: it asks again, and the item
	# comes back to the master through the lease.
	ADD_TEST(NAME GCourtierMpiUndecodableTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=undecodable
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiUndecodableTest PROPERTIES LABELS "courtier" PROCESSORS 3)
	GENEVA_TEST_SIZE(GCourtierMpiUndecodableTest M)

	# A client must be refused the consumer, and the submitter the client.
	ADD_TEST(NAME GCourtierMpiRefusalTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=refusal
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiRefusalTest PROPERTIES LABELS "courtier" PROCESSORS 2 TIMEOUT 60)
	GENEVA_TEST_SIZE(GCourtierMpiRefusalTest M)

	# A time limit set on the MPI client has no effect: the client says so and serves until the
	# master tells it to stop, so every item is processed.
	ADD_TEST(NAME GCourtierMpiClientLimitsTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=client-limits
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiClientLimitsTest PROPERTIES
			LABELS "courtier" PROCESSORS 2 TIMEOUT 60)
	GENEVA_TEST_SIZE(GCourtierMpiClientLimitsTest M)

	# A client given an item processor processes every item through it: no item comes back with
	# the result of its own process().
	ADD_TEST(NAME GCourtierMpiItemProcessorTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=item-processor
					${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiItemProcessorTest PROPERTIES
			LABELS "courtier" PROCESSORS 3 TIMEOUT 60)
	GENEVA_TEST_SIZE(GCourtierMpiItemProcessorTest M)

	# Large-payload variant: each work item far exceeds the former fixed 20 KB MPI message cap,
	# so this covers the dynamically-sized (MPI_Iprobe + MPI_Get_count) receive path.
	ADD_EXECUTABLE(GCourtierMpiLargePayloadExerciser GCourtierMpiLargePayloadExerciser.cpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiLargePayloadExerciser ${LIBRARIES_TO_LINK} MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiLargePayloadExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	ADD_TEST(NAME GCourtierMpiLargePayloadTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiLargePayloadExerciser> ${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiLargePayloadTest PROPERTIES LABELS "courtier" PROCESSORS 3)
	GENEVA_TEST_SIZE(GCourtierMpiLargePayloadTest M)
ENDIF()

GENEVA_AGGREGATE_TARGET( "tests-courtier-unit" )
