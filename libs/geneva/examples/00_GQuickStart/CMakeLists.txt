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

SET ( EXECUTABLENAME GQuickStart )

SET ( ${EXECUTABLENAME}_SRCS
	GQuickStart.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES})


TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK}
		)

# Materialize this example's configuration from the code defaults at build time, instead of copying a
# hand-maintained set of shipped config files.
GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME}/config )

# End-to-end test on the materialized default configuration: the stall criterion halts the run after a
# few dozen iterations, and the program fails unless it got close to the known minimum (Inv 22). About one
# run in fifteen stalls early in a shallow region, so the test passes if one of three runs gets close;
# the program's own tolerance stays as it is. GQuickStart and GenevaMpiModuleUnavailable share this
# working directory and its config/ tree, hence the resource lock.
ADD_TEST(NAME ${EXECUTABLENAME}
	COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:${EXECUTABLENAME}> -DATTEMPTS=3
		-P ${PROJECT_SOURCE_DIR}/CMakeModules/GenevaRunBestOf.cmake
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "geneva;example" TIMEOUT 120
	RESOURCE_LOCK GQuickStart_workdir)
GENEVA_TEST_SIZE(${EXECUTABLENAME} S)

# On a machine without the MPI library, the MPI module cannot be loaded and "mpi" is no consumer. Asked
# for it, a Go2 program must say why -- the module is installed, but a library it needs is missing --
# not merely that the mnemonic is unknown. An empty libmpi.so.40 first on the library path stands in
# for the missing library.
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(_no_mpi_dir ${CMAKE_CURRENT_BINARY_DIR}/no-mpi)
	FILE(MAKE_DIRECTORY ${_no_mpi_dir})
	FILE(WRITE ${_no_mpi_dir}/libmpi.so.40 "")
	ADD_TEST(NAME GenevaMpiModuleUnavailable
		COMMAND ${CMAKE_COMMAND} -E env LD_LIBRARY_PATH=${_no_mpi_dir} $<TARGET_FILE:${EXECUTABLENAME}> --consumer mpi
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
	SET_TESTS_PROPERTIES(GenevaMpiModuleUnavailable PROPERTIES LABELS "geneva"
		PASS_REGULAR_EXPRESSION "could not be loaded on this machine:.*libgemfony-geneva-mpi"
		RESOURCE_LOCK GQuickStart_workdir)
	GENEVA_TEST_SIZE(GenevaMpiModuleUnavailable S)
ENDIF()

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME} )
