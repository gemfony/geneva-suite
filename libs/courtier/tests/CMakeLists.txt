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

ADD_SUBDIRECTORY ( UnitTests )
ADD_SUBDIRECTORY ( ManualTests )

# With the CUDA backend of the GPU consumer built in: no Geneva library may need the CUDA driver
# library at load time, so that a CUDA build still starts on machines without an NVIDIA driver.
IF ( GENEVA_GPU_CONSUMER_ENABLED AND CMAKE_CUDA_COMPILER )
	ADD_TEST ( NAME CourtierNoDriverLink
		COMMAND ${CMAKE_COMMAND}
			"-DLIBS=$<TARGET_FILE:${COURTIER_LIBNAME}>;$<TARGET_FILE:${HAP_LIBNAME}>;$<TARGET_FILE:${GENEVA_LIBNAME}>"
			-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoDriverLink.cmake )
	SET_TESTS_PROPERTIES ( CourtierNoDriverLink PROPERTIES LABELS "courtier" )
	GENEVA_TEST_SIZE ( CourtierNoDriverLink S )
ENDIF ()

GENEVA_AGGREGATE_TARGET( "tests-courtier" )
