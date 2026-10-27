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

SET ( EXECUTABLENAME GNeuralNetwork )

SET ( ${EXECUTABLENAME}_SRCS
	GNeuralNetwork.cpp
)

ADD_EXECUTABLE(${EXECUTABLENAME}
	${${EXECUTABLENAME}_SRCS}
)

SET(LIBRARIES_TO_LINK
		${GENEVA_LIBRARIES}
		${Boost_LIBRARIES}
		# GNeuralNetworkIndividual is no longer in the geneva library; this example compiles it in via its
		# object library (the alternative to loading it as a module).
		GNeuralNetworkIndividual-obj)

# Add MPI Libraries in case building with MPI-consumer
IF(GENEVA_BUILD_WITH_MPI_CONSUMER)
	SET(LIBRARIES_TO_LINK
			${LIBRARIES_TO_LINK}
			${MPI_LIBRARIES})
ENDIF()

TARGET_LINK_LIBRARIES (${EXECUTABLENAME}
		${LIBRARIES_TO_LINK}
		)


GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME}/config )

# The four sample training sets are MATERIALIZED FROM CODE at build time -- the same model
# GENEVA_MATERIALIZE_CONFIGS applies to configuration -- instead of being shipped as data files. This
# example writes them itself (--trainingDataType selects the distribution and the run also emits the
# matching ROOT visualization macro "<name>.dat.C"), so a checked-in copy is a hand-synced duplicate of
# what the code already produces: the shipped set was written by Boost.Serialization and became
# unreadable the moment serialization moved to GArchive, with the example aborting on start.
# Generation costs a fraction of a second and cannot go stale.
SET ( _nn_data_dir ${CMAKE_CURRENT_BINARY_DIR}/DataSets )
SET ( _nn_data_sets 2000 )
# Exist at configure time so the INSTALL(DIRECTORY) rule below is valid; the POST_BUILD steps
# (re)populate it from code.
FILE ( MAKE_DIRECTORY ${_nn_data_dir} )
ADD_CUSTOM_COMMAND ( TARGET ${EXECUTABLENAME} POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E rm -rf ${_nn_data_dir}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${_nn_data_dir}
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMENT "Generating ${EXECUTABLENAME} sample training data from code"
	VERBATIM )
# The names are listed in the order of the trainingDataType enumeration (HYPERCUBE=1 ... SINUS=4); each
# run is routed through GenevaRunQuiet.cmake, so its output is replayed only if it fails.
SET ( _nn_type 1 )
FOREACH ( _nn_name hyper_cube hyper_sphere axis_centric sinus )
	ADD_CUSTOM_COMMAND ( TARGET ${EXECUTABLENAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND}
			"-DRQ_CMD=$<TARGET_FILE:${EXECUTABLENAME}>|--trainingDataFile|./DataSets/${_nn_name}.dat|--trainingDataType|${_nn_type}|--nDataSets|${_nn_data_sets}"
			"-DRQ_WD=${CMAKE_CURRENT_BINARY_DIR}"
			"-DRQ_LOG=${CMAKE_CURRENT_BINARY_DIR}/dataset-${_nn_name}.log"
			"-DRQ_DESC=Training-data generation for ${_nn_name}"
			-P ${GENEVA_RUN_QUIET_SCRIPT}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		VERBATIM )
	MATH ( EXPR _nn_type "${_nn_type} + 1" )
ENDFOREACH ()
INSTALL ( DIRECTORY ${_nn_data_dir}/ DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME}/DataSets )

# The default run trains against ./DataSets/hyper_sphere.dat (materialized above) and halts on the
# stall criterion after a couple of seconds.
ADD_TEST ( NAME ${EXECUTABLENAME} COMMAND ${EXECUTABLENAME}
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} )
SET_TESTS_PROPERTIES ( ${EXECUTABLENAME} PROPERTIES LABELS "geneva;example" TIMEOUT 300 )
GENEVA_TEST_SIZE(${EXECUTABLENAME} M)

INSTALL ( TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME} )
