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

################################################################################
# Helper: declare a Common-library Catch2 test executable. Each call
#
#     geneva_common_test(<exe-name> <size> <source-files...>)
#
# creates an executable, links it against the Geneva libraries + Boost +
# Catch2::Catch2WithMain (+ MPI when GENEVA_BUILD_WITH_MPI_CONSUMER), registers
# it with CTest under the "common" label plus its size label, installs it, and
# appends it to the `tests-common-unit` aggregate target. Used by every test
# below.
#
# <size> is S, M, L or XL, measured -- see the "Running the test suite" section
# of INSTALL. It is a required argument on purpose: a new test cannot be added
# without saying what it costs, because the size is what decides whether a gate
# runs it.

SET(_GENEVA_COMMON_TEST_TARGETS "")

FUNCTION(geneva_common_test name size)
	ADD_EXECUTABLE(${name} ${ARGN})

	SET(_libs ${GENEVA_LIBRARIES} ${Boost_LIBRARIES} Catch2::Catch2WithMain)
	IF (GENEVA_BUILD_WITH_MPI_CONSUMER)
		LIST(APPEND _libs ${MPI_LIBRARIES})
	ENDIF ()
	TARGET_LINK_LIBRARIES(${name} ${_libs})

	ADD_TEST(${name} ${name})
	SET_TESTS_PROPERTIES(${name} PROPERTIES LABELS "common")
	GENEVA_TEST_SIZE(${name} ${size})
	INSTALL(TARGETS ${name}
		DESTINATION ${INSTALL_PREFIX_DATA}/tests/geneva/UnitTests)

	# Append to the parent-scope aggregate list.
	SET(_GENEVA_COMMON_TEST_TARGETS "${_GENEVA_COMMON_TEST_TARGETS};${name}"
		PARENT_SCOPE)
ENDFUNCTION()

################################################################################
# Test executables — one per production file, alphabetical.

geneva_common_test(GAgingStoreTTests              S  GAgingStoreTTests.cpp)
geneva_common_test(GArchiveIntegrationTests       S  GArchiveIntegrationTests.cpp)
geneva_common_test(GArchivePolymorphicTests       S  GArchivePolymorphicTests.cpp)
geneva_common_test(GBlockingMPMCQueueTTests       S  GBlockingMPMCQueueTTests.cpp)
geneva_common_test(GCommonEnumsTests              S  GCommonEnumsTests.cpp)
geneva_common_test(GCompletionLatchTTests         S  GCompletionLatchTTests.cpp)
geneva_common_test(GContentAddressedStoreTTests   S  GContentAddressedStoreTTests.cpp)
geneva_common_test(GSPSCStagingRingTTests         S  GSPSCStagingRingTTests.cpp)
geneva_common_test(GThreadBudgetTests             S  GThreadBudgetTests.cpp)
geneva_common_test(GThreadSafeKeyedStoreTTests    S  GThreadSafeKeyedStoreTTests.cpp)
geneva_common_test(GThreadSafeSetTTests           S  GThreadSafeSetTTests.cpp)
geneva_common_test(GCommonHelperFunctionsTests    S  GCommonHelperFunctionsTests.cpp)
geneva_common_test(GCommonHelperFunctionsTTests   S  GCommonHelperFunctionsTTests.cpp)
geneva_common_test(GCommonInterfaceTTests         S  GCommonInterfaceTTests.cpp)
geneva_common_test(GCommonMathHelperFunctionsTTests
                                                  S  GCommonMathHelperFunctionsTTests.cpp)
geneva_common_test(GContainerTTests               S  GContainerTTests.cpp)
geneva_common_test(GDefaultValueTTests            S  GDefaultValueTTests.cpp)
geneva_common_test(GErrorStreamerTests            S  GErrorStreamerTests.cpp)
geneva_common_test(GExceptionsTests               S  GExceptionsTests.cpp)
geneva_common_test(GExpectationChecksTTests       S  GExpectationChecksTTests.cpp)
geneva_common_test(GFactoryTTests                 S  GFactoryTTests.cpp)
geneva_common_test(GFixedSizePriorityQueueTTests  S  GFixedSizePriorityQueueTTests.cpp)
geneva_common_test(GGlobalOptionsTTests           S  GGlobalOptionsTTests.cpp)
geneva_common_test(GJsonIOTests                   S  GJsonIOTests.cpp)
geneva_common_test(GLoadOnceCellTTests            S  GLoadOnceCellTTests.cpp)
geneva_common_test(GLoggerTests                   S  GLoggerTests.cpp)
geneva_common_test(GMPMCQueueConformanceTests     M  GMPMCQueueConformanceTests.cpp)
geneva_common_test(GMPMCQueueShutdownStressTests  L  GMPMCQueueShutdownStressTests.cpp)
geneva_common_test(GParserBuilderTests            S  GParserBuilderTests.cpp)
geneva_common_test(GProviderStoreTTests           S  GProviderStoreTTests.cpp)
geneva_common_test(GSelfTestableTests             S  GSelfTestableTests.cpp)
geneva_common_test(GSerializableFunctionObjectTTests
                                                  S  GSerializableFunctionObjectTTests.cpp)
geneva_common_test(GSerializationHelperFunctionsTTests
                                                  S  GSerializationHelperFunctionsTTests.cpp)
geneva_common_test(GSerializeTupleTTests          S  GSerializeTupleTTests.cpp)
geneva_common_test(GSingletonTTests               S  GSingletonTTests.cpp)
geneva_common_test(GThreadGroupTests              S  GThreadGroupTests.cpp)
geneva_common_test(GThreadPoolTests               S  GThreadPoolTests.cpp)
geneva_common_test(GTupleIOTests                  S  GTupleIOTests.cpp)
geneva_common_test(GTypeToStringTTests            S  GTypeToStringTTests.cpp)
geneva_common_test(GTypeTraitsTTests              S  GTypeTraitsTTests.cpp)
geneva_common_test(GUnitTestFrameworkTTests       S  GUnitTestFrameworkTTests.cpp)

################################################################################
# Aggregate `tests-common-unit` target builds every Common test exe at once.

ADD_CUSTOM_TARGET( "tests-common-unit"
	DEPENDS ${_GENEVA_COMMON_TEST_TARGETS}
	COMMENT "Building the unit tests for the \"common\" library."
)
