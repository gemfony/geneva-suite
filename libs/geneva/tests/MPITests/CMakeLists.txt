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

# Tests of Geneva's MPI module that need several ranks. The directory is only added when the MPI
# consumer is built (geneva/tests/CMakeLists.txt), which implies FIND_PACKAGE(MPI), so
# MPIEXEC_EXECUTABLE is a real, configure-time-detected dependency here.

# An MPI cascade on a base communicator whose rank 0 is not rank 0 of MPI_COMM_WORLD: the roles of
# the cascade and of Geneva's MPI module, loaded at run time, follow the base ranks; then one
# iteration with groups of two. Five ranks, each in a directory of its own.
ADD_EXECUTABLE ( GMPICascadeBaseCommTest GMPICascadeBaseCommTest.cpp )
TARGET_LINK_LIBRARIES ( GMPICascadeBaseCommTest
	GFunctionIndividual-obj
	${COURTIER_MPI_LIBNAME}
	${GENEVA_LIBRARIES}
	${Boost_LIBRARIES}
	MPI::MPI_CXX
)
ADD_DEPENDENCIES ( GMPICascadeBaseCommTest ${GENEVA_MPI_LIBNAME} )
SET ( CASCADE_WORKDIR ${CMAKE_CURRENT_BINARY_DIR}/cascade-base-comm )
FILE ( MAKE_DIRECTORY ${CASCADE_WORKDIR} )
GEM_ADD_MPI_TEST ( NAME GMPICascadeBaseCommTest TIMEOUT 60
	COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 5 ${MPIEXEC_PREFLAGS}
			${CMAKE_CURRENT_SOURCE_DIR}/runInRankDirectory.sh
			$<TARGET_FILE:GMPICascadeBaseCommTest> ${MPIEXEC_POSTFLAGS} --consumer mpi
	WORKING_DIRECTORY ${CASCADE_WORKDIR} )
SET_TESTS_PROPERTIES ( GMPICascadeBaseCommTest PROPERTIES LABELS "geneva" PROCESSORS 5 )
GEM_TEST_SIZE ( GMPICascadeBaseCommTest M )

# A bystander in a plain Go2 program: three ranks, of which the MPI consumer's communicator holds
# two. The third must be in client mode, and its clientRun() must return at once. Each rank runs in
# a directory of its own, because the program writes its configuration files where it starts. The
# program links courtier's MPI library, where it builds the environment, and not Geneva's MPI
# module, which libgemfony-geneva loads at run time: the roles show that both find the same
# environment.
ADD_EXECUTABLE ( GMPIBystanderTest GMPIBystanderTest.cpp )
TARGET_LINK_LIBRARIES ( GMPIBystanderTest
	GFunctionIndividual-obj
	${COURTIER_MPI_LIBNAME}
	${GENEVA_LIBRARIES}
	${Boost_LIBRARIES}
	MPI::MPI_CXX
)
ADD_DEPENDENCIES ( GMPIBystanderTest ${GENEVA_MPI_LIBNAME} )
SET ( BYSTANDER_WORKDIR ${CMAKE_CURRENT_BINARY_DIR}/bystander )
FILE ( MAKE_DIRECTORY ${BYSTANDER_WORKDIR} )
GEM_ADD_MPI_TEST ( NAME GMPIBystanderTest TIMEOUT 60
	COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS}
			${CMAKE_CURRENT_SOURCE_DIR}/runInRankDirectory.sh
			$<TARGET_FILE:GMPIBystanderTest> ${MPIEXEC_POSTFLAGS} --consumer mpi
	WORKING_DIRECTORY ${BYSTANDER_WORKDIR} )
SET_TESTS_PROPERTIES ( GMPIBystanderTest PROPERTIES LABELS "geneva" PROCESSORS 3 )
GEM_TEST_SIZE ( GMPIBystanderTest M )

# No rank enters MPI on the way out: a Go2 program asked for its help text, or for a refresh of its
# configuration files, with the MPI consumer named, must end with 0 under the launcher. A rank that
# initialized MPI and exited without finalizing it would make the launcher fail the job. Each rank
# runs in a directory of its own below a scratch directory, because both paths write configuration
# files.
IF ( GEM_BUILD_EXAMPLES )
	SET ( NOMPI_WORKDIR ${CMAKE_CURRENT_BINARY_DIR}/no-mpi-on-the-way-out )
	FILE ( MAKE_DIRECTORY ${NOMPI_WORKDIR} )
	FOREACH ( _case IN ITEMS Help UpdateConfigs )
		IF ( _case STREQUAL "Help" )
			SET ( _args --consumer mpi --help )
		ELSE ()
			SET ( _args --update-configs --consumer mpi )
		ENDIF ()
		GEM_ADD_MPI_TEST ( NAME GMPINoMpiOn${_case} TIMEOUT 60
			COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2 ${MPIEXEC_PREFLAGS}
					${CMAKE_CURRENT_SOURCE_DIR}/runInRankDirectory.sh
					$<TARGET_FILE:GSimpleOptimizer> ${MPIEXEC_POSTFLAGS} ${_args}
			WORKING_DIRECTORY ${NOMPI_WORKDIR} )
		SET_TESTS_PROPERTIES ( GMPINoMpiOn${_case} PROPERTIES LABELS "geneva" PROCESSORS 2 )
		GEM_TEST_SIZE ( GMPINoMpiOn${_case} S )
	ENDFOREACH ()

	# A program started with the MPI consumer as one rank (under the launcher with one process, or
	# without it) has no client rank to evaluate its work, and an MPI job keeps its ranks: it must
	# say so and end with a non-zero code within seconds, not wait for ever.
	SET ( ONERANK_WORKDIR ${CMAKE_CURRENT_BINARY_DIR}/one-rank )
	FILE ( MAKE_DIRECTORY ${ONERANK_WORKDIR} )
	GEM_ADD_MPI_TEST ( NAME GMPIOneRankRefused TIMEOUT 60
		COMMAND ${CMAKE_COMMAND}
			"-DCOMMAND=${MPIEXEC_EXECUTABLE};${MPIEXEC_NUMPROC_FLAG};1;${MPIEXEC_PREFLAGS};$<TARGET_FILE:GSimpleOptimizer>;${MPIEXEC_POSTFLAGS};--consumer;mpi"
			-DEXPECT_EXIT=nonzero
			"-DEXPECT=The MPI consumer has no client rank;at least two processes"
			-DLIMIT=20
			-P ${PROJECT_SOURCE_DIR}/libs/courtier/tests/CheckJobEnds.cmake
		WORKING_DIRECTORY ${ONERANK_WORKDIR} )
	SET_TESTS_PROPERTIES ( GMPIOneRankRefused PROPERTIES LABELS "geneva" )
	GEM_TEST_SIZE ( GMPIOneRankRefused S )

	# A batch job started again from its last checkpoint (checkpointRestart.sh says what it
	# requires): job 1 under the MPI consumer, with three ranks each in a directory of its own, ends
	# by its iteration limit; job 2, started with --cp_file on its final checkpoint and a higher
	# limit, continues the run.
	SET ( RESTART_WORKDIR ${CMAKE_CURRENT_BINARY_DIR}/checkpoint-restart )
	FILE ( MAKE_DIRECTORY ${RESTART_WORKDIR} )
	GEM_ADD_MPI_TEST ( NAME GMPICheckpointRestartLimit TIMEOUT 120
		COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/checkpointRestart.sh limit 3
			rank-0/config/GEvolutionaryAlgorithm.json rank-0/checkpoints --
			${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 3 ${MPIEXEC_PREFLAGS}
			${CMAKE_CURRENT_SOURCE_DIR}/runInRankDirectory.sh
			$<TARGET_FILE:GSimpleOptimizer> ${MPIEXEC_POSTFLAGS} --consumer mpi
		WORKING_DIRECTORY ${RESTART_WORKDIR} )
	SET_TESTS_PROPERTIES ( GMPICheckpointRestartLimit PROPERTIES LABELS "geneva" PROCESSORS 3 )
	GEM_TEST_SIZE ( GMPICheckpointRestartLimit M )
ENDIF ()
