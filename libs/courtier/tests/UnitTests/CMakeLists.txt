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
# Every entry ends after this many seconds at most, as the tree's other tests do with their TIMEOUT
# (ctest's default is 1500 s): a test that hangs instead of failing is ended in reasonable time.
# Generous, for the sanitizer builds and a loaded host.
SET(GENEVA_COURTIER_TEST_TIMEOUT 600)

FUNCTION(geneva_courtier_test name size)
	CMAKE_PARSE_ARGUMENTS(PARSE_ARGV 2 arg "" "SPEC" "")
	ADD_EXECUTABLE(${name} ${arg_UNPARSED_ARGUMENTS})
	TARGET_LINK_LIBRARIES(${name} ${LIBRARIES_TO_LINK})
	ADD_TEST(${name} ${name} ${arg_SPEC})
	SET_TESTS_PROPERTIES(${name} PROPERTIES LABELS "courtier" TIMEOUT ${GENEVA_COURTIER_TEST_TIMEOUT})
	GENEVA_TEST_SIZE(${name} ${size})
	INSTALL(TARGETS ${name} DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
ENDFUNCTION()

# Helper: a further CTest entry of an executable declared with geneva_courtier_test(), running the
# test cases <spec> selects
#
#     geneva_courtier_test_entry(<entry-name> <exe-name> <size> <spec>)
FUNCTION(geneva_courtier_test_entry entry exe size spec)
	ADD_TEST(${entry} ${exe} ${spec})
	SET_TESTS_PROPERTIES(${entry} PROPERTIES LABELS "courtier" TIMEOUT ${GENEVA_COURTIER_TEST_TIMEOUT})
	GENEVA_TEST_SIZE(${entry} ${size})
ENDFUNCTION()

# Submission-path unit tests (formerly courtier2/tests/UnitTests, merged in at Stage E). The test
# executable / file names still carry the "2" until the Stage-E identifier rename (E2); the CTest
# labels and install paths are already "courtier".
#
# None of these needs a RESOURCE_LOCK under `ctest -j`, including those that really put a server on
# the loopback interface: GCourtierWebsocketTests and its siblings construct their consumer with
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
geneva_courtier_test(GCourtierWebsocketTests   M  GCourtierWebsocketTests.cpp GTcpRelay.hpp)
geneva_courtier_test(GCourtierWebsocketPeerTests M  GCourtierWebsocketPeerTests.cpp)
geneva_courtier_test(GCourtierWebsocketTransportTests S  GCourtierWebsocketTransportTests.cpp)
# The end of a run on the websocket transport (T7.1 to T7.4): every client is told, within a bound
geneva_courtier_test(GCourtierWebsocketStopTests M  GCourtierWebsocketStopTests.cpp)
# The rules of GPeerI.hpp on the in-process pair of GInProcessPeer.hpp (a header of this
# directory, used by later tests of the roles as their transport) and on the MPI transport's ends
# over the in-memory stand-in for MPI (GMPIFakeCalls.hpp): one set of rule tests for both
geneva_courtier_test(GCourtierPeerRulesTests   S  GCourtierPeerRulesTests.cpp GInProcessPeer.hpp
	GInProcessTestSupport.hpp GMPIFakeCalls.hpp GPeerTestRole.hpp)
geneva_courtier_test(GCourtierPeerSessionTests S  GCourtierPeerSessionTests.cpp GInProcessPeer.hpp
	GInProcessTestSupport.hpp)
geneva_courtier_test(GCourtierPeerConsumerTests S  GCourtierPeerConsumerTests.cpp GInProcessPeer.hpp
	GInProcessTestSupport.hpp)
geneva_courtier_test(GCourtierPeerClientTests  M  GCourtierPeerClientTests.cpp GInProcessPeer.hpp
	GInProcessTestSupport.hpp)
geneva_courtier_test(GCourtierLifecycleTests   M  GCourtierLifecycleTests.cpp)
# The TCP relay that stands between a client and its server in the tests of a lost connection
geneva_courtier_test(GTcpRelayTests            S  GTcpRelayTests.cpp GTcpRelay.hpp)
# A result that a client carries from one server process to another is dropped there; the first
# server runs in a process of its own (the helper below), because a server tells the items of
# another process from its own by a salt drawn once per process
ADD_EXECUTABLE(GCourtierHelperServer GCourtierHelperServer.cpp)
TARGET_LINK_LIBRARIES(GCourtierHelperServer ${GENEVA_LIBRARIES} ${Boost_LIBRARIES})
geneva_courtier_test(GCourtierForeignResultTests M GCourtierForeignResultTests.cpp GTcpRelay.hpp)
TARGET_COMPILE_DEFINITIONS(GCourtierForeignResultTests
	PRIVATE GCOURTIER_HELPER_SERVER="$<TARGET_FILE:GCourtierHelperServer>")
ADD_DEPENDENCIES(GCourtierForeignResultTests GCourtierHelperServer)
# The MPI poll on an in-memory stand-in for MPI (GMPIFakeCalls.hpp): no mpirun, and so also in the
# build with ThreadSanitizer, which has no MPI
geneva_courtier_test(GCourtierMpiPollCoreTests M  GCourtierMpiPollCoreTests.cpp GMPIFakeCalls.hpp
	GInProcessTestSupport.hpp)
# The MPI transport's ends, listener and connector on the same stand-in
geneva_courtier_test(GCourtierMpiPeerTests     M  GCourtierMpiPeerTests.cpp GMPIFakeCalls.hpp
	GInProcessTestSupport.hpp GPeerTestRole.hpp)
geneva_courtier_test(GCourtierWebsocketLivenessTests L  SPEC "~[slowcodec]" GCourtierWebsocketLivenessTests.cpp
	GTcpRelay.hpp)
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
# under mpirun (1 master + 2 workers, unless an entry says otherwise) rather than as an in-process
# Catch2 test. Every entry runs the items with the time lease off (wait_indefinitely): an item that is
# lost makes the entry hang until ctest ends it.
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	ADD_EXECUTABLE(GCourtierMpiExerciser GCourtierMpiExerciser.cpp)
	# The exercisers build the MPI consumer and its client directly, so they -- and only they -- link
	# courtier's MPI library and MPI.
	TARGET_LINK_LIBRARIES(GCourtierMpiExerciser ${LIBRARIES_TO_LINK} ${COURTIER_MPI_LIBNAME} MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)

	# Helper: one entry of the exerciser
	#
	#     geneva_courtier_mpi_scenario(<entry-name> <ranks> <scenario> [<further arguments>...])
	#
	# PROCESSORS tells the `ctest -j` scheduler what the entry really costs: one CTest slot but
	# <ranks> live processes. Without it the scheduler counts it as one and oversubscribes the
	# machine.
	FUNCTION(geneva_courtier_mpi_scenario entry ranks scenario)
		ADD_TEST(NAME ${entry}
				COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${ranks}
						${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=${scenario}
						${ARGN} ${MPIEXEC_POSTFLAGS})
		SET_TESTS_PROPERTIES(${entry} PROPERTIES LABELS "courtier" PROCESSORS ${ranks} TIMEOUT 120)
		GENEVA_TEST_SIZE(${entry} M)
	ENDFUNCTION()

	# Every item makes the full master->client->master round trip.
	geneva_courtier_mpi_scenario(GCourtierMpiTest 3 clean)
	# An evaluation that throws must not end the worker rank: the item comes back flagged and the
	# submission policy deals with it.
	geneva_courtier_mpi_scenario(GCourtierMpiThrowingItemTest 3 throwing)
	# A master consumer that is destroyed without ever starting its server tells its clients, which
	# wait for its greeting without a limit: everyone ends at once.
	geneva_courtier_mpi_scenario(GCourtierMpiNeverStartedTest 3 never-started)
	SET_TESTS_PROPERTIES(GCourtierMpiNeverStartedTest PROPERTIES TIMEOUT 30)
	# A program that initialized MPI itself, below MPI_THREAD_MULTIPLE, is refused. An MPI library
	# that grants the full level whatever is asked for cannot be checked; the entry is then skipped.
	geneva_courtier_mpi_scenario(GCourtierMpiThreadLevelTest 1 low-thread-level)
	SET_TESTS_PROPERTIES(GCourtierMpiThreadLevelTest PROPERTIES SKIP_RETURN_CODE 77)
	# A message a worker cannot decode must not end the worker rank: it says so, and the master puts
	# back what it held.
	geneva_courtier_mpi_scenario(GCourtierMpiUndecodableTest 3 undecodable)
	# A client must be refused the consumer, and the submitter the client.
	geneva_courtier_mpi_scenario(GCourtierMpiRefusalTest 2 refusal)
	# A client with an item limit processes exactly that many items, sends its last result and
	# leaves; the master goes on with the other client and puts nothing back.
	geneva_courtier_mpi_scenario(GCourtierMpiClientLimitsTest 3 client-limits)
	# A client with a time limit ends by itself while the master goes on with the other client;
	# what it held is handed out again.
	geneva_courtier_mpi_scenario(GCourtierMpiClientTimeLimitTest 3 client-time-limit)
	# A client given an item processor processes every item through it: no item comes back with
	# the result of its own process().
	geneva_courtier_mpi_scenario(GCourtierMpiItemProcessorTest 3 item-processor)
	# Many batches of changing size from three submitters, with idle phases: nothing is put back,
	# dropped or late, every client waits at the master at the end, and the stop is quick.
	geneva_courtier_mpi_scenario(GCourtierMpiSoakTest 5 soak)
	# The end of a run: the master stops while every client waits at it, while every client
	# evaluates, before the last client has started, and after a client has left. Each time every
	# client still there is told and confirms, within a bound.
	geneva_courtier_mpi_scenario(GCourtierMpiStopParkedTest 3 stop-parked)
	geneva_courtier_mpi_scenario(GCourtierMpiStopEvaluatingTest 3 stop-evaluating)
	geneva_courtier_mpi_scenario(GCourtierMpiStopLateRankTest 3 stop-late-rank --lateMs=3000)
	geneva_courtier_mpi_scenario(GCourtierMpiStopClientLeftTest 3 stop-client-left --leaveAfter=20)

	# The binding of the MPI transport's calls to MPI (GMPICalls, courtier's MPI library) on two ranks:
	# messages of three tags and lengths arrive in order, every send completes, a send is given up.
	ADD_EXECUTABLE(GCourtierMpiCallsExerciser GCourtierMpiCallsExerciser.cpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiCallsExerciser ${LIBRARIES_TO_LINK} ${COURTIER_MPI_LIBNAME} MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiCallsExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	ADD_TEST(NAME GCourtierMpiCallsTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiCallsExerciser> ${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiCallsTest PROPERTIES LABELS "courtier" PROCESSORS 2 TIMEOUT 60)
	GENEVA_TEST_SIZE(GCourtierMpiCallsTest M)
	# Sends that are given up and never received do not keep MPI from finalizing: both ranks end.
	ADD_TEST(NAME GCourtierMpiGivenUpSendsTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiCallsExerciser> ${MPIEXEC_POSTFLAGS}
					--never-received)
	SET_TESTS_PROPERTIES(GCourtierMpiGivenUpSendsTest PROPERTIES LABELS "courtier" PROCESSORS 2 TIMEOUT 20)
	GENEVA_TEST_SIZE(GCourtierMpiGivenUpSendsTest M)

	# The MPI transport's ends over real MPI: a client rank is told when the submitter's end for it
	# ends without a close of its role (the end of the listener's work, a session that cannot be
	# made). A client that is not told waits for ever: the time limit ends the entry then.
	ADD_EXECUTABLE(GCourtierMpiPeerExerciser GCourtierMpiPeerExerciser.cpp GPeerTestRole.hpp
			GInProcessTestSupport.hpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiPeerExerciser ${LIBRARIES_TO_LINK} ${COURTIER_MPI_LIBNAME} MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiPeerExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	FOREACH(scenario end-of-work session-fails)
		ADD_TEST(NAME GCourtierMpiPeer_${scenario}
				COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
						${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiPeerExerciser> --scenario=${scenario}
						${MPIEXEC_POSTFLAGS})
		SET_TESTS_PROPERTIES(GCourtierMpiPeer_${scenario} PROPERTIES
				LABELS "courtier" PROCESSORS 3 TIMEOUT 60)
		GENEVA_TEST_SIZE(GCourtierMpiPeer_${scenario} M)
	ENDFOREACH()

	# Large-payload variant: each work item far exceeds the former fixed 20 KB MPI message cap,
	# so this covers the dynamically-sized (MPI_Iprobe + MPI_Get_count) receive path.
	ADD_EXECUTABLE(GCourtierMpiLargePayloadExerciser GCourtierMpiLargePayloadExerciser.cpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiLargePayloadExerciser ${LIBRARIES_TO_LINK} ${COURTIER_MPI_LIBNAME}
			MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiLargePayloadExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	ADD_TEST(NAME GCourtierMpiLargePayloadTest
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiLargePayloadExerciser> ${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiLargePayloadTest PROPERTIES LABELS "courtier" PROCESSORS 3)
	GENEVA_TEST_SIZE(GCourtierMpiLargePayloadTest M)
ENDIF()

GENEVA_AGGREGATE_TARGET( "tests-courtier-unit" )
