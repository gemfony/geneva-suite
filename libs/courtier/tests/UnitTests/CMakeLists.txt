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

# courtier's own closure only (CourtierProgramsLinkOnlyCourtier): the work items of the test support,
# courtier (and through it common and weft), Boost and Catch2
SET(LIBRARIES_TO_LINK
		courtier-test-support
		${COURTIER_LIBNAME}
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
# run() returns only once no evaluation of the client runs any more, also when it is asked to close
# while one runs
geneva_courtier_test(GCourtierClientRunEndTests M  GCourtierClientRunEndTests.cpp GInProcessTestSupport.hpp)
# The TCP relay that stands between a client and its server in the tests of a lost connection
geneva_courtier_test(GTcpRelayTests            S  GTcpRelayTests.cpp GTcpRelay.hpp)
# A result that a client carries from one server process to another is dropped there; the first
# server runs in a process of its own (the helper below), because a server tells the items of
# another process from its own by a salt drawn once per process
ADD_EXECUTABLE(GCourtierHelperServer GCourtierHelperServer.cpp)
TARGET_LINK_LIBRARIES(GCourtierHelperServer courtier-test-support ${COURTIER_LIBNAME} ${Boost_LIBRARIES})
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
# Catch2 test. Every entry runs the items without a time limit (the default): an item that is lost
# makes the entry hang until ctest ends it.
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	ADD_EXECUTABLE(GCourtierMpiExerciser GCourtierMpiExerciser.cpp)
	# The exercisers build the MPI consumer and its client directly, so they -- and only they -- link
	# courtier's MPI library and MPI.
	TARGET_LINK_LIBRARIES(GCourtierMpiExerciser ${LIBRARIES_TO_LINK} ${COURTIER_MPI_LIBNAME} MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)

	# Helper: one entry of the exerciser
	#
	#     geneva_courtier_mpi_scenario(<entry-name> <ranks> <scenario> [TIMEOUT <seconds>]
	#                                  [<further arguments>...])
	#
	# The entry runs under GENEVA_ADD_MPI_TEST, bounded at 120 s unless a TIMEOUT is given.
	#
	# PROCESSORS tells the `ctest -j` scheduler what the entry really costs: one CTest slot but
	# <ranks> live processes. Without it the scheduler counts it as one and oversubscribes the
	# machine.
	FUNCTION(geneva_courtier_mpi_scenario entry ranks scenario)
		CMAKE_PARSE_ARGUMENTS(PARSE_ARGV 3 arg "" "TIMEOUT" "")
		IF(NOT arg_TIMEOUT)
			SET(arg_TIMEOUT 120)
		ENDIF()
		GENEVA_ADD_MPI_TEST(NAME ${entry} TIMEOUT ${arg_TIMEOUT}
				COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${ranks}
						${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiExerciser> --scenario=${scenario}
						${arg_UNPARSED_ARGUMENTS} ${MPIEXEC_POSTFLAGS})
		SET_TESTS_PROPERTIES(${entry} PROPERTIES LABELS "courtier" PROCESSORS ${ranks})
		GENEVA_TEST_SIZE(${entry} M)
	ENDFUNCTION()

	# Every item makes the full master->client->master round trip.
	geneva_courtier_mpi_scenario(GCourtierMpiTest 3 clean)
	# An evaluation that throws must not end the worker rank: the item comes back flagged and the
	# submission policy deals with it.
	geneva_courtier_mpi_scenario(GCourtierMpiThrowingItemTest 3 throwing)
	# A master consumer that is destroyed without ever starting its server tells its clients, which
	# wait for its greeting without a limit: everyone ends at once.
	geneva_courtier_mpi_scenario(GCourtierMpiNeverStartedTest 3 never-started TIMEOUT 30)
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
	# client still there is told and ends, within a bound.
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
	GENEVA_ADD_MPI_TEST(NAME GCourtierMpiCallsTest TIMEOUT 60
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiCallsExerciser> ${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiCallsTest PROPERTIES LABELS "courtier" PROCESSORS 2)
	GENEVA_TEST_SIZE(GCourtierMpiCallsTest M)
	# Sends that are given up and never received do not keep MPI from finalizing: both ranks end.
	GENEVA_ADD_MPI_TEST(NAME GCourtierMpiGivenUpSendsTest TIMEOUT 20
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiCallsExerciser> ${MPIEXEC_POSTFLAGS}
					--never-received)
	SET_TESTS_PROPERTIES(GCourtierMpiGivenUpSendsTest PROPERTIES LABELS "courtier" PROCESSORS 2)
	GENEVA_TEST_SIZE(GCourtierMpiGivenUpSendsTest M)

	# A process that ends on an error nobody handles ends the whole MPI job, at once and with the
	# exit code 1, while the other ranks wait for it (the fatal report, common/GFatalReport.hpp;
	# ../CheckJobEnds.cmake: exit code, message, a bound of 30 s): an error in a configuration, the
	# same while main() holds the MPI environment (an unwinding would finalize MPI and wait for the
	# other ranks), and an exception out of a thread other than main's. A std::terminate() with no
	# exception in flight, an internal fault, ends the job with the line and an abort instead.
	ADD_EXECUTABLE(GCourtierMpiFatalEndExerciser GCourtierMpiFatalEndExerciser.cpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiFatalEndExerciser ${LIBRARIES_TO_LINK} ${COURTIER_MPI_LIBNAME}
		${Boost_LIBRARIES} MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiFatalEndExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	SET(_dir ${CMAKE_CURRENT_BINARY_DIR}/fatal-end)
	FILE(MAKE_DIRECTORY ${_dir})
	FOREACH(_scenario config holder thread fault)
		SET(_exit 1)
		SET(_end "The program ends with the exit code 1")
		IF(_scenario STREQUAL "thread")
			SET(_expect "Error: ;a thread of rank 1 failed (a standard exception)")
		ELSEIF(_scenario STREQUAL "fault") # no exception in flight: an internal fault aborts
			SET(_expect "Error: ;std::terminate() was called without an exception")
			SET(_exit nonzero)
			SET(_end "The program aborts")
		ELSE()
			SET(_expect "Error: ;has the wrong form: false")
		ENDIF()
		SET(_name GCourtierMpiFatalEndTest)
		IF(NOT _scenario STREQUAL "config")
			STRING(APPEND _name "_${_scenario}")
		ENDIF()
		GENEVA_ADD_MPI_TEST(NAME ${_name} TIMEOUT 60
				COMMAND ${CMAKE_COMMAND}
					"-DCOMMAND=${MPIEXEC_EXECUTABLE};${MPIEXEC_NUMPROC_FLAG};3;${MPIEXEC_PREFLAGS};$<TARGET_FILE:GCourtierMpiFatalEndExerciser>;${MPIEXEC_POSTFLAGS};${_scenario}"
					-DEXPECT_EXIT=${_exit}
					"-DEXPECT=${_expect};${_end}"
					"-DREFUSE=terminate called;FAIL"
					-DLIMIT=30
					-P ${CMAKE_CURRENT_SOURCE_DIR}/../CheckJobEnds.cmake
				WORKING_DIRECTORY ${_dir})
		SET_TESTS_PROPERTIES(${_name} PROPERTIES LABELS "courtier" PROCESSORS 3)
		GENEVA_TEST_SIZE(${_name} M)
	ENDFOREACH()

	# Signs of life over real MPI (a keep-alive of 1 s; ../CheckJobEnds.cmake: a non-zero end, what
	# the job must say, a bound of 30 s): a client rank that stops itself (SIGSTOP) in the middle of
	# a run is found silent, the submitter finishes the run with the other client, and the job is
	# ended where MPI would be finalized (MPI_Finalize would wait for the stopped rank); a submitter
	# that stops itself is found silent by its clients, which end the job the same way.
	SET(_dir ${CMAKE_CURRENT_BINARY_DIR}/silent-end)
	FILE(MAKE_DIRECTORY ${_dir})
	FOREACH(_scenario client-stops submitter-stops)
		IF(_scenario STREQUAL "client-stops")
			SET(_expect "OK: 120/120 items processed")
		ELSE()
			# The first client that ends the job may end the other before it says so
			SET(_expect "found its submitter silent")
		ENDIF()
		GENEVA_ADD_MPI_TEST(NAME GCourtierMpiSilent_${_scenario} TIMEOUT 60
				COMMAND ${CMAKE_COMMAND}
					"-DCOMMAND=${MPIEXEC_EXECUTABLE};${MPIEXEC_NUMPROC_FLAG};3;${MPIEXEC_PREFLAGS};$<TARGET_FILE:GCourtierMpiExerciser>;--scenario=${_scenario};${MPIEXEC_POSTFLAGS}"
					-DEXPECT_EXIT=nonzero
					"-DEXPECT=${_expect};MPI_Finalize would wait for them: the job is ended"
					-DLIMIT=30
					-P ${CMAKE_CURRENT_SOURCE_DIR}/../CheckJobEnds.cmake
				WORKING_DIRECTORY ${_dir})
		SET_TESTS_PROPERTIES(GCourtierMpiSilent_${_scenario} PROPERTIES LABELS "courtier" PROCESSORS 3)
		GENEVA_TEST_SIZE(GCourtierMpiSilent_${_scenario} M)
	ENDFOREACH()

	# The MPI transport's ends over real MPI: a client rank is told when the submitter's end for it
	# ends without a close of its role (the end of the listener's work, a session that cannot be
	# made). A client that is not told waits for ever: the time limit ends the entry then.
	ADD_EXECUTABLE(GCourtierMpiPeerExerciser GCourtierMpiPeerExerciser.cpp GPeerTestRole.hpp
			GInProcessTestSupport.hpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiPeerExerciser ${LIBRARIES_TO_LINK} ${COURTIER_MPI_LIBNAME} MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiPeerExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	FOREACH(scenario end-of-work session-fails)
		GENEVA_ADD_MPI_TEST(NAME GCourtierMpiPeer_${scenario} TIMEOUT 60
				COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
						${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiPeerExerciser> --scenario=${scenario}
						${MPIEXEC_POSTFLAGS})
		SET_TESTS_PROPERTIES(GCourtierMpiPeer_${scenario} PROPERTIES LABELS "courtier" PROCESSORS 3)
		GENEVA_TEST_SIZE(GCourtierMpiPeer_${scenario} M)
	ENDFOREACH()

	# Large-payload variant: each work item far exceeds the former fixed 20 KB MPI message cap,
	# so this covers the dynamically-sized (MPI_Iprobe + MPI_Get_count) receive path.
	ADD_EXECUTABLE(GCourtierMpiLargePayloadExerciser GCourtierMpiLargePayloadExerciser.cpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiLargePayloadExerciser ${LIBRARIES_TO_LINK} ${COURTIER_MPI_LIBNAME}
			MPI::MPI_CXX)
	INSTALL(TARGETS GCourtierMpiLargePayloadExerciser
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)
	GENEVA_ADD_MPI_TEST(NAME GCourtierMpiLargePayloadTest TIMEOUT 120
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3
					${MPIEXEC_PREFLAGS} $<TARGET_FILE:GCourtierMpiLargePayloadExerciser> ${MPIEXEC_POSTFLAGS})
	SET_TESTS_PROPERTIES(GCourtierMpiLargePayloadTest PROPERTIES LABELS "courtier" PROCESSORS 3)
	GENEVA_TEST_SIZE(GCourtierMpiLargePayloadTest M)

	# The MPI cascade (courtier/mpi/GMPICascade.hpp) under mpirun, with real MPI. The program links
	# courtier's MPI library and MPI only -- nothing of Geneva (GCourtierMpiCascadeTestNoGeneva, in
	# ../CMakeLists.txt) -- and exports its symbols, so that its watch of MPI calls
	# (GMPICallWatch.cpp) also sees the calls made inside courtier's MPI library.
	ADD_EXECUTABLE(GCourtierMpiCascadeTest GCourtierMpiCascadeTest.cpp GMPICallWatch.cpp GMPICallWatch.hpp)
	TARGET_LINK_LIBRARIES(GCourtierMpiCascadeTest courtier-test-support ${COURTIER_MPI_LIBNAME} ${Boost_LIBRARIES}
		MPI::MPI_CXX)
	SET_TARGET_PROPERTIES(GCourtierMpiCascadeTest PROPERTIES ENABLE_EXPORTS TRUE)
	INSTALL(TARGETS GCourtierMpiCascadeTest
			DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/UnitTests)

	# Helper: an entry that runs one scenario of GCourtierMpiCascadeTest and judges how the job ends
	# (../CheckJobEnds.cmake: exit code, texts, a bound of 60 s)
	#
	#     geneva_courtier_mpi_cascade_test(<entry> <ranks> <size> <zero|nonzero|code> <scenario>
	#                                      [NO_LAUNCHER] [ARGS <arguments>] [EXPECT <texts>]
	#                                      [REFUSE <texts>])
	#
	# Each entry runs in a directory of its own below the build directory. Every entry refuses
	# "FAIL:" (the driver's rule) and the texts of REFUSE, and expects those of EXPECT; one that must
	# end with 0 under mpirun also expects the watch's verdict "OK: rank <r>:" of every rank. NO_LAUNCHER
	# starts the program directly, as one process.
	FUNCTION(geneva_courtier_mpi_cascade_test entry ranks size expect_exit scenario)
		CMAKE_PARSE_ARGUMENTS(PARSE_ARGV 5 arg "NO_LAUNCHER" "" "ARGS;EXPECT;REFUSE")
		SET(_dir ${CMAKE_CURRENT_BINARY_DIR}/cascade/${entry})
		FILE(MAKE_DIRECTORY ${_dir})
		SET(_program $<TARGET_FILE:GCourtierMpiCascadeTest> --scenario=${scenario} ${arg_ARGS})
		SET(_expect ${arg_EXPECT})
		IF(arg_NO_LAUNCHER)
			SET(_command ${_program})
		ELSE()
			SET(_command ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${ranks} ${MPIEXEC_PREFLAGS}
				${_program} ${MPIEXEC_POSTFLAGS})
			IF(expect_exit STREQUAL "zero")
				MATH(EXPR _last "${ranks} - 1")
				FOREACH(_rank RANGE ${_last})
					LIST(APPEND _expect "OK: rank ${_rank}:")
				ENDFOREACH()
			ENDIF()
		ENDIF()
		# Also the entry without the launcher (it makes no MPI call): one way to register them all
		GENEVA_ADD_MPI_TEST(NAME ${entry} TIMEOUT 90
				COMMAND ${CMAKE_COMMAND}
					"-DCOMMAND=${_command}"
					-DEXPECT_EXIT=${expect_exit}
					"-DEXPECT=${_expect}"
					"-DREFUSE=${arg_REFUSE}"
					-DLIMIT=60
					-P ${CMAKE_CURRENT_SOURCE_DIR}/../CheckJobEnds.cmake
				WORKING_DIRECTORY ${_dir})
		SET_TESTS_PROPERTIES(${entry} PROPERTIES LABELS "courtier" PROCESSORS ${ranks})
		GENEVA_TEST_SIZE(${entry} ${size})
	ENDFUNCTION()

	# The roles, groups and rounds of the two layouts; a member's second serve() returns at once
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeLayoutA 7 M zero layout-a)
	# A last group with fewer processes than the group size (1 + 3 + 2)
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeLayoutAShortGroup 6 M zero layout-a)
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeLayoutB 3 M zero layout-b)
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeLayoutBTwoGroups 6 M zero layout-b)
	# A group of one is a group: its lead announces every round
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeOneA 2 M zero one-a)
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeOneB 1 M zero one-b)
	# Only rank 0 provides the configuration
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeRankZeroSize 7 M zero rank-zero-size)
	# setUp() and tearDown() of lead and members run once, collectively, around the rounds
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeLifecycle 4 M zero lifecycle)
	# A member that leaves without serve() ends the job
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeMemberNeverServes 4 M nonzero member-never-serves
		EXPECT "without serve()" "The job is ended (MPI_Abort)")
	# A base communicator whose rank 0 is not rank 0 of MPI_COMM_WORLD, on MPI the program initialized
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeBaseComm 5 M zero base-comm)
	# Rounds from two threads run one after the other
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeTwoThreads 3 M zero two-threads)
	# release() waits for a running round; a round() after it throws
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeReleaseDuringRound 3 M zero release-during-round)
	# --update-configs: no MPI call, the file is written
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeRefresh 1 M zero refresh NO_LAUNCHER
		ARGS --update-configs)
	# A wrong role is refused with the layout and the remedy
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeRequireRoleA 4 M zero require-role-a)
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeRequireRoleB 3 M zero require-role-b)
	# An idle member does not wait busily
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeIdleMember 3 M zero idle-member)
	# A configuration that is refused, or that rank 0 cannot provide: every rank throws, none hangs
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeGroupSizeZero 3 M zero group-size-zero)
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadePausesRefused 3 M zero pauses-refused)
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeConfigThrows 3 M zero config-throws)
	# The group size and the poll pauses from the cascade's configuration file reach every rank
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeConfigFile 3 M zero config-file)
	# A lead whose role was not confirmed ends the job at its first round() or lead(), also when the
	# round runs in a client's item processor, after it has released its members
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeRoleNotConfirmed 3 M nonzero role-not-confirmed
		EXPECT "member 1 left its rounds" "member 2 left its rounds" "round(): the role of a lead was not confirmed with requireRole()" "The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeRoleNotConfirmedLead 3 M nonzero role-not-confirmed-lead
		EXPECT "member 1 left its rounds" "member 2 left its rounds" "lead(): the role of a lead was not confirmed with requireRole()" "The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeWebsocketNotConfirmed 3 M nonzero websocket-not-confirmed
		EXPECT "member 1 left its rounds" "member 2 left its rounds" "round(): the role of a lead was not confirmed with requireRole()" "The job is ended (MPI_Abort)")
	# A lead that never confirms its role and never uses the cascade ends the job in the destructor
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeNeverUsed 3 M nonzero never-used
		EXPECT "requireRole()" "The job is ended (MPI_Abort)")
	# An exception out of a member's half, the lead's half or a lifecycle function ends the job
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeServeRoundThrows 3 M nonzero serve-round-throws
		EXPECT "an exception left serveRound(): injected exception in serveRound()" "The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeLeadHalfThrows 3 M nonzero lead-half-throws
		EXPECT "an exception left the lead's half of a round: injected exception in the lead's half"
		"The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeSetUpThrows 3 M nonzero set-up-throws
		EXPECT "an exception left setUp(): injected exception in setUp()" "The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeTearDownThrows 3 M nonzero tear-down-throws
		EXPECT "an exception left tearDown(): injected exception in tearDown()" "The job is ended (MPI_Abort)")
	# An MPI error of a started collective cannot be survived: it ends the job
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeIbcastStartFails 3 M nonzero ibcast-start-fails
		EXPECT "the broadcast of the configuration failed with the MPI error code" "The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeTestallFails 3 M nonzero testall-fails
		EXPECT "the broadcast of the configuration failed with the MPI error code" "The job is ended (MPI_Abort)")
	# lead() after a round, after release() or a second time is refused
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeLeadAfterRound 3 M zero lead-after-round)
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeLeadTwice 3 M zero lead-twice)
	# release() or round() from inside a lead's half ends the job with a message, not a deadlock
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeReleaseInsideRound 3 M nonzero release-inside-round
		EXPECT "release() was called from inside the lead's half of a round" "The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeRoundInsideRound 3 M nonzero round-inside-round
		EXPECT "round() was called from inside the lead's half of a round" "The job is ended (MPI_Abort)")
	# A rank with an MPI environment of its own refuses; the others end the job instead of waiting
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeEnvironmentExists 3 M nonzero environment-exists
		EXPECT "could not take part in the cascade" "The job is ended (MPI_Abort)")
	# A submitter that never confirms its role ends the job in its destructor
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeSubmitterNeverConfirms 4 M nonzero submitter-never-confirms
		EXPECT "the role of the submitter was never confirmed" "The job is ended (MPI_Abort)")
	# A submitter that throws before its MPI consumer exists, while its leads wait for the greeting,
	# ends the job in its destructor; one that throws while its consumer exists does not: the
	# consumer's end tells the leads, and the job ends by itself
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeSubmitterThrows 4 M nonzero submitter-throws
		EXPECT "an exception leaves the submitter before an MPI consumer was built"
		"The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeSubmitterThrowsAfterConsumer 4 M zero
		submitter-throws-after-consumer EXPECT "the submitter's exception reached the program"
		REFUSE "MPI_Abort")
	# A submitter that catches its failure and ends the job with abortJob() and a code: the job
	# ends with that code (a catch that returned instead would leave the leads waiting); a second
	# call, with code 43, made while the first is held before its MPI_Abort, waits for the first:
	# its reason is never logged and the job ends with 42; abortJob() is refused on leads and
	# members; without MPI it ends the process with the code
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeSubmitterAborts 4 M 42 submitter-aborts
		EXPECT "abortJob(): the submitter failed: injected failure of the submitter"
			"The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeSubmitterAbortsTwice 4 M 42
		submitter-aborts-twice
		EXPECT "abortJob(): the submitter failed: injected failure of the submitter"
			"The job is ended (MPI_Abort)"
		REFUSE "a second call of abortJob()")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeAbortWithoutMpi 1 M 42 abort-without-mpi
		NO_LAUNCHER ARGS --update-configs
		EXPECT "abortJob(): the submitter failed without MPI"
			"the process is ended with the exit code 42")
	# ... and from inside setUp(), tearDown() or a member's serveRound()
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeRoundInsideSetUp 3 M nonzero round-inside-set-up
		EXPECT "round() was called from inside setUp()" "The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeReleaseInsideTearDown 3 M nonzero release-inside-tear-down
		EXPECT "release() was called from inside tearDown()" "The job is ended (MPI_Abort)")
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeServeInsideServeRound 3 M nonzero serve-inside-serve-round
		EXPECT "serve() was called from inside serveRound()" "The job is ended (MPI_Abort)")
	# A configuration callable that asks for the MPI role is refused on every rank, not a deadlock
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeCallableAsksRole 3 M zero callable-asks-role)
	# The cascade behind a websocket client with two evaluations at a time
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeWebsocket 3 M zero websocket)
	# The cascade behind courtier's MPI consumer (SUBMITTER_AND_GROUPS end to end): the leads are MPI
	# clients of base rank 0 with the consumer's defaults, two groups of three
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeMpiConsumer 7 M zero mpi-consumer)
	# ... and a lead that never confirms its role ends the whole job at its first round
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeMpiConsumerNotConfirmed 4 M nonzero mpi-consumer-not-confirmed
		EXPECT "member 2 left its rounds" "member 3 left its rounds" "round(): the role of a lead was not confirmed with requireRole()" "The job is ended (MPI_Abort)")
	# The time of an empty round: printed for a caption, not judged
	geneva_courtier_mpi_cascade_test(GCourtierMpiCascadeEmptyRound 3 M zero empty-round
		EXPECT "empty round: median")
ENDIF()

GENEVA_AGGREGATE_TARGET( "tests-courtier-unit" )
