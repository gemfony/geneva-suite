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

# One ctest entry per test case: the GPU switch is process-wide and a source that left the GPU does not
# return to it, so every case needs a fresh process. Cases that need a GPU source skip themselves
# (Catch2 exit code 4) in builds without the CUDA backend or without a device.

SET ( EXECUTABLENAME HapGpuFallbackTests )

ADD_EXECUTABLE(${EXECUTABLENAME}
	HapGpuFallbackTests.cpp
)

TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${HAP_LIBNAME}
		${COMMON_LIBNAME}
		${Boost_LIBRARIES}
		Catch2::Catch2WithMain)

FOREACH(_case gpu-off-at-start gpu-off-later gpu-failure)
	ADD_TEST(NAME ${EXECUTABLENAME}_${_case} COMMAND ${EXECUTABLENAME} "[${_case}]")
	SET_TESTS_PROPERTIES(${EXECUTABLENAME}_${_case} PROPERTIES LABELS "hap" SKIP_RETURN_CODE 4)
	GENEVA_TEST_SIZE(${EXECUTABLENAME}_${_case} S)
ENDFOREACH()

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/tests/hap/HapGpuFallbackTests )

GENEVA_AGGREGATE_TARGET( "tests-hap-gpufallback" )
