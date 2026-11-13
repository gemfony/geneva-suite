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

SET ( EXECUTABLENAME GCommandContainerSerialization )

SET ( ${EXECUTABLENAME}_SRCS
    GCommandContainerSerialization.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		# GFunctionIndividual is no longer in the geneva library; compiled in via its object library (before the
		# Geneva/Boost libraries, the in-tree convention for an object library carrying archive registrations).
		GFunctionIndividual-obj
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})


TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK})

GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/tests/geneva/ManualTests/${EXECUTABLENAME}/config )

ADD_TEST(${EXECUTABLENAME} ${EXECUTABLENAME})
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "geneva")
GENEVA_TEST_SIZE(${EXECUTABLENAME} M)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/tests/geneva/ManualTests/${EXECUTABLENAME} )
