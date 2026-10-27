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

# A standalone multi-process exerciser (its own main(), no Catch2), formerly in courtier2/tests,
# merged in at Stage E. Launched by scripts/startCourtierJobs.sh as separate server + client
# processes. (The "2" in the name is dropped at the Stage-E identifier rename, E2.)
SET(LIBRARIES_TO_LINK ${GENEVA_LIBRARIES} ${Boost_LIBRARIES})
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK ${LIBRARIES_TO_LINK} ${MPI_LIBRARIES})
ENDIF()

ADD_EXECUTABLE(GCourtierNetworkedExerciser GCourtierNetworkedExerciser.cpp)
TARGET_LINK_LIBRARIES(GCourtierNetworkedExerciser ${LIBRARIES_TO_LINK})
INSTALL(TARGETS GCourtierNetworkedExerciser
		DESTINATION ${INSTALL_PREFIX_DATA}/tests/courtier/ManualTests)

GENEVA_AGGREGATE_TARGET( "tests-courtier-manual" )
