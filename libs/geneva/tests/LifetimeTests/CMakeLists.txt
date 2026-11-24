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

# The start and the orderly end of a Geneva program, once, in a process of its own: a short run on
# courtier's launcher, then the end of the process in the order GenevaInitializer's class comment
# states. The program checks the end itself, after the static destructors of the libraries, and
# ends with 1 if the random-number factory was not finalized and destroyed or a reserved thread was
# not returned. It runs in a directory of its own, because it writes its configuration files where
# it starts.
ADD_EXECUTABLE ( GLifetimeOrderTest GLifetimeOrderTest.cpp )
TARGET_LINK_LIBRARIES ( GLifetimeOrderTest
	GFunctionIndividual-obj
	${GENEVA_LIBRARIES}
	${Boost_LIBRARIES}
)
SET ( LIFETIME_ORDER_WORKDIR ${CMAKE_CURRENT_BINARY_DIR}/lifetime-order )
FILE ( MAKE_DIRECTORY ${LIFETIME_ORDER_WORKDIR} )
ADD_TEST ( NAME GLifetimeOrderTest
	COMMAND ${CMAKE_COMMAND}
		-DCOMMAND=$<TARGET_FILE:GLifetimeOrderTest>
		-DEXPECT_EXIT=zero
		"-DEXPECT=OK: the factory was finalized and destroyed"
		-DLIMIT=60
		-P ${PROJECT_SOURCE_DIR}/libs/courtier/tests/CheckJobEnds.cmake
	WORKING_DIRECTORY ${LIFETIME_ORDER_WORKDIR} )
SET_TESTS_PROPERTIES ( GLifetimeOrderTest PROPERTIES LABELS "geneva" )
GEM_TEST_SIZE ( GLifetimeOrderTest S )
