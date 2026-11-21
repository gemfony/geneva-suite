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

# Documentation programs (D2 in the manual repository's DOCUMENTATION-INVARIANTS.md):
# small, self-contained programs written to be QUOTED BY THE MANUAL where the real
# library code is too heavyweight to excerpt. Each links only this library's own
# dependency closure and is registered as an S-size test, so a documentation program
# that stops compiling, or stops doing what the manual says, turns a gate red rather
# than rotting quietly. The manual's snippet markers point in here (see
# source/snippets.map in the manual repository). These are deliberately NOT in
# examples/: an example teaches a user, a docu-program backs a listing.

SET ( _COURTIER_DOCPROGRAMS
	minimal_consumer  # the consumer contract: implement dispatch_(), submit through processBatch()
)

FOREACH ( _dp ${_COURTIER_DOCPROGRAMS} )
	ADD_EXECUTABLE ( courtier_${_dp} ${_dp}.cpp )
	# courtier's own dependency closure only (courtier <- common <- weft, in link order) --
	# a docu-program of this library must not need the libraries that sit above it or beside it.
	# Its work item comes from courtier's test support (../tests/support), which links courtier only.
	TARGET_LINK_LIBRARIES ( courtier_${_dp} courtier-test-support
		${COURTIER_LIBNAME} ${COMMON_LIBNAME} ${WEFT_LIBNAME} ${Boost_LIBRARIES} )
	ADD_TEST ( NAME courtier_${_dp} COMMAND courtier_${_dp} )
	SET_TESTS_PROPERTIES ( courtier_${_dp} PROPERTIES LABELS "courtier;docprogram" TIMEOUT 120 )
	GEM_TEST_SIZE ( courtier_${_dp} S )
	INSTALL ( TARGETS courtier_${_dp}
		DESTINATION ${INSTALL_PREFIX_DATA}/docprograms/courtier )
ENDFOREACH ()

# grid_minimum: a program without Geneva on courtier's launcher, with a consumer module of its own
# per consumer (grid_minimum_<consumer>.cpp), built for its item type and lying next to it as
# libcourtier_grid_minimum_<consumer>.so. A module links nothing it can resolve in the program
# (linked with exported symbols); the MPI module links courtier's MPI library, which holds the
# process's MPI environment. Run on every consumer: stc alone; beast and mqtt as a server and two
# clients, processes of their own (runServerAndClients.sh; mqtt on the tests' broker); mpi under
# mpirun with three ranks.
ADD_EXECUTABLE ( courtier_grid_minimum grid_minimum.cpp GGridSlice.hpp )
TARGET_LINK_LIBRARIES ( courtier_grid_minimum ${COURTIER_LIBNAME} ${COMMON_LIBNAME} ${WEFT_LIBNAME}
	${Boost_LIBRARIES} )
SET_TARGET_PROPERTIES ( courtier_grid_minimum PROPERTIES ENABLE_EXPORTS ON )
SET ( _GRID_MINIMUM_CONSUMERS stc beast )
IF ( GEM_MQTT_CONSUMER_ENABLED )
	LIST ( APPEND _GRID_MINIMUM_CONSUMERS mqtt )
ENDIF ()
IF ( GEM_BUILD_WITH_MPI_CONSUMER )
	LIST ( APPEND _GRID_MINIMUM_CONSUMERS mpi )
ENDIF ()
FOREACH ( _consumer ${_GRID_MINIMUM_CONSUMERS} )
	SET ( _module courtier_grid_minimum_${_consumer} )
	ADD_LIBRARY ( ${_module} MODULE grid_minimum_${_consumer}.cpp )
	SET_TARGET_PROPERTIES ( ${_module} PROPERTIES PREFIX "lib" )
	TARGET_INCLUDE_DIRECTORIES ( ${_module} PRIVATE
		$<TARGET_PROPERTY:${COURTIER_LIBNAME},INTERFACE_INCLUDE_DIRECTORIES> )
	TARGET_COMPILE_OPTIONS ( ${_module} PRIVATE -UGEM_TESTING )
	ADD_DEPENDENCIES ( courtier_grid_minimum ${_module} )
	INSTALL ( TARGETS ${_module} DESTINATION ${INSTALL_PREFIX_DATA}/docprograms/courtier )
ENDFOREACH ()
IF ( GEM_BUILD_WITH_MPI_CONSUMER )
	TARGET_LINK_LIBRARIES ( courtier_grid_minimum_mpi ${COURTIER_MPI_LIBNAME} )
ENDIF ()
INSTALL ( TARGETS courtier_grid_minimum DESTINATION ${INSTALL_PREFIX_DATA}/docprograms/courtier )

ADD_TEST ( NAME courtier_grid_minimum COMMAND courtier_grid_minimum )
ADD_TEST ( NAME courtier_grid_minimum_beast
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/runServerAndClients.sh $<TARGET_FILE:courtier_grid_minimum>
	beast 2 - )
SET ( _GRID_MINIMUM_TESTS courtier_grid_minimum courtier_grid_minimum_beast )
IF ( GEM_MQTT_CONSUMER_ENABLED AND GEM_MOSQUITTO_EXECUTABLE )
	ADD_TEST ( NAME courtier_grid_minimum_mqtt
		COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/runServerAndClients.sh $<TARGET_FILE:courtier_grid_minimum>
		mqtt 2 ${GEM_MQTT_TEST_BROKER_DIR} )
	SET_TESTS_PROPERTIES ( courtier_grid_minimum_mqtt PROPERTIES FIXTURES_REQUIRED courtier_mqtt_broker )
	LIST ( APPEND _GRID_MINIMUM_TESTS courtier_grid_minimum_mqtt )
ENDIF ()
FOREACH ( _test ${_GRID_MINIMUM_TESTS} )
	SET_TESTS_PROPERTIES ( ${_test} PROPERTIES TIMEOUT 300 )
ENDFOREACH ()
SET_TESTS_PROPERTIES ( courtier_grid_minimum_beast PROPERTIES PROCESSORS 3 )
IF ( TEST courtier_grid_minimum_mqtt )
	SET_TESTS_PROPERTIES ( courtier_grid_minimum_mqtt PROPERTIES PROCESSORS 3 )
ENDIF ()
IF ( GEM_BUILD_WITH_MPI_CONSUMER AND MPIEXEC_EXECUTABLE )
	GEM_ADD_MPI_TEST ( NAME courtier_grid_minimum_mpi TIMEOUT 120
		COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS}
		$<TARGET_FILE:courtier_grid_minimum> --consumer mpi ${MPIEXEC_POSTFLAGS} )
	SET_TESTS_PROPERTIES ( courtier_grid_minimum_mpi PROPERTIES PROCESSORS 3 )
	LIST ( APPEND _GRID_MINIMUM_TESTS courtier_grid_minimum_mpi )
ENDIF ()
FOREACH ( _test ${_GRID_MINIMUM_TESTS} )
	SET_TESTS_PROPERTIES ( ${_test} PROPERTIES LABELS "courtier;docprogram" )
	GEM_TEST_SIZE ( ${_test} S )
ENDFOREACH ()

GEM_AGGREGATE_TARGET( "docprograms-courtier" )
