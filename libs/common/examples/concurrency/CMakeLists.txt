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

# A runnable tour of the Gem::Common::Concurrency shared primitives -- living documentation of the
# intended call patterns. Extended as each primitive lands.

SET ( EXECUTABLENAME GConcurrencyPrimitives )

SET ( ${EXECUTABLENAME}_SRCS
	GConcurrencyPrimitives.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})

IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK
			${LIBRARIES_TO_LINK}
			${MPI_LIBRARIES})
ENDIF()

TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK})

# A tour of the shared concurrency primitives -- each demo uses a handful of items and threads, so the
# whole walkthrough returns in well under a second.
ADD_TEST(NAME ${EXECUTABLENAME} COMMAND ${EXECUTABLENAME}
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "common;example" TIMEOUT 120)
GENEVA_TEST_SIZE(${EXECUTABLENAME} S)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME} )
