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
#     gem_common_test(<exe-name> <size> <source-files...>)
#
# creates an executable, links it against the common library + Boost +
# Catch2::Catch2WithMain (nothing of the libraries above common:
# CommonProgramsLinkOnlyCommon), registers it with CTest under the "common" label plus its size label, installs it, and
# appends it to the `tests-common-unit` aggregate target. Used by every test
# below.
#
# <size> is S, M, L or XL, measured -- see the "Running the test suite" section
# of INSTALL. It is a required argument on purpose: a new test cannot be added
# without saying what it costs, because the size is what decides whether a gate
# runs it.

SET(_GEM_COMMON_TEST_TARGETS "")

FUNCTION(gem_common_test name size)
	ADD_EXECUTABLE(${name} ${ARGN})

	SET(_libs ${COMMON_LIBNAME} ${Boost_LIBRARIES} Catch2::Catch2WithMain)
	TARGET_LINK_LIBRARIES(${name} ${_libs})

	ADD_TEST(${name} ${name})
	SET_TESTS_PROPERTIES(${name} PROPERTIES LABELS "common")
	GEM_TEST_SIZE(${name} ${size})
	INSTALL(TARGETS ${name}
		DESTINATION ${INSTALL_PREFIX_DATA}/tests/geneva/UnitTests)

	# Append to the parent-scope aggregate list.
	SET(_GEM_COMMON_TEST_TARGETS "${_GEM_COMMON_TEST_TARGETS};${name}"
		PARENT_SCOPE)
ENDFUNCTION()

################################################################################
# Test executables — one per production file, alphabetical.

gem_common_test(GAgingStoreTTests              S  GAgingStoreTTests.cpp)
gem_common_test(GArchiveIntegrationTests       S  GArchiveIntegrationTests.cpp)
gem_common_test(GArchivePolymorphicTests       S  GArchivePolymorphicTests.cpp)
gem_common_test(GBlockingMPMCQueueTTests       S  GBlockingMPMCQueueTTests.cpp)
gem_common_test(GCommonEnumsTests              S  GCommonEnumsTests.cpp)
gem_common_test(GCompletionLatchTTests         S  GCompletionLatchTTests.cpp)
gem_common_test(GContentAddressedStoreTTests   S  GContentAddressedStoreTTests.cpp)
gem_common_test(GSPSCStagingRingTTests         S  GSPSCStagingRingTTests.cpp)
gem_common_test(GThreadBudgetTests             S  GThreadBudgetTests.cpp)
gem_common_test(GThreadSafeKeyedStoreTTests    S  GThreadSafeKeyedStoreTTests.cpp)
gem_common_test(GThreadSafeSetTTests           S  GThreadSafeSetTTests.cpp)
gem_common_test(GCommonHelperFunctionsTests    S  GCommonHelperFunctionsTests.cpp)
gem_common_test(GCommonHelperFunctionsTTests   S  GCommonHelperFunctionsTTests.cpp)
gem_common_test(GCommonInterfaceTTests         S  GCommonInterfaceTTests.cpp)
gem_common_test(GCommonMathHelperFunctionsTTests
                                                  S  GCommonMathHelperFunctionsTTests.cpp)
gem_common_test(GContainerTTests               S  GContainerTTests.cpp)
gem_common_test(GDefaultValueTTests            S  GDefaultValueTTests.cpp)
gem_common_test(GErrorStreamerTests            S  GErrorStreamerTests.cpp)
gem_common_test(GExceptionsTests               S  GExceptionsTests.cpp)
gem_common_test(GExpectationChecksTTests       S  GExpectationChecksTTests.cpp)
gem_common_test(GFactoryTTests                 S  GFactoryTTests.cpp)
gem_common_test(GFatalReportTests              S  GFatalReportTests.cpp)
gem_common_test(GFixedSizePriorityQueueTTests  S  GFixedSizePriorityQueueTTests.cpp)
gem_common_test(GGemCommitTests             S  GGemCommitTests.cpp)
gem_common_test(GGlobalDefinesTests            S  GGlobalDefinesTests.cpp)
gem_common_test(GGlobalOptionsTTests           S  GGlobalOptionsTTests.cpp)
gem_common_test(GJsonIOTests                   S  GJsonIOTests.cpp)
gem_common_test(GLoadOnceCellTTests            S  GLoadOnceCellTTests.cpp)
gem_common_test(GLoggerTests                   S  GLoggerTests.cpp)
gem_common_test(GMPMCQueueConformanceTests     M  GMPMCQueueConformanceTests.cpp)
gem_common_test(GMPMCQueueShutdownStressTests  L  GMPMCQueueShutdownStressTests.cpp)
gem_common_test(GModuleLoaderTests             S  GModuleLoaderTests.cpp)
gem_common_test(GParserBuilderTests            S  GParserBuilderTests.cpp)
gem_common_test(GProviderStoreTTests           S  GProviderStoreTTests.cpp)
gem_common_test(GRecorderTests                 S  GRecorderTests.cpp)
gem_common_test(GSelfTestableTests             S  GSelfTestableTests.cpp)
gem_common_test(GSerializableFunctionObjectTTests
                                                  S  GSerializableFunctionObjectTTests.cpp)
gem_common_test(GSerializationHelperFunctionsTTests
                                                  S  GSerializationHelperFunctionsTTests.cpp)
gem_common_test(GSerializeTupleTTests          S  GSerializeTupleTTests.cpp)
gem_common_test(GSingletonTTests               S  GSingletonTTests.cpp)
gem_common_test(GStandardTestsTests            S  GStandardTestsTests.cpp)
gem_common_test(GThreadGroupTests              S  GThreadGroupTests.cpp)
gem_common_test(GThreadPoolTests               S  GThreadPoolTests.cpp)
gem_common_test(GTupleIOTests                  S  GTupleIOTests.cpp)
gem_common_test(GTypeToStringTTests            S  GTypeToStringTTests.cpp)
gem_common_test(GTypeTraitsTTests              S  GTypeTraitsTTests.cpp)
gem_common_test(GUnitTestFrameworkTTests       S  GUnitTestFrameworkTTests.cpp)

# A real module for GModuleLoaderTests, built with the recipe of a genuine one (it links no
# library of the collection). Its path reaches the test as a compile definition.
GENEVA_ADD_INDIVIDUAL_MODULE(GTwoContributionsModule modules/GTwoContributionsModule.cpp)
ADD_DEPENDENCIES(GModuleLoaderTests GTwoContributionsModule)
TARGET_COMPILE_DEFINITIONS(GModuleLoaderTests PRIVATE
	"GEM_TEST_MODULE_TWO_CONTRIBUTIONS=\"$<TARGET_FILE:GTwoContributionsModule>\""
	# A shared library that is no module: common's own.
	"GEM_TEST_NOT_A_MODULE=\"$<TARGET_FILE:${COMMON_LIBNAME}>\"")

################################################################################
# Aggregate `tests-common-unit` target builds every Common test exe at once.

ADD_CUSTOM_TARGET( "tests-common-unit"
	DEPENDS ${_GEM_COMMON_TEST_TARGETS}
	COMMENT "Building the unit tests for the \"common\" library."
)
