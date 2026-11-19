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
IF ( GEM_BUILD_WITH_MPI_CONSUMER )
	ADD_SUBDIRECTORY ( MPITests )
	ADD_SUBDIRECTORY ( OutOfTree )
ENDIF ()

# With the CUDA backend of the GPU consumer built in: no library of geneva's closure (geneva,
# courtier, hap, dietrich, common, weft -- ldd lists them all) may need the CUDA driver library at
# load time, so that a CUDA build still starts on machines without an NVIDIA driver.
IF ( GEM_GPU_CONSUMER_ENABLED AND CMAKE_CUDA_COMPILER )
	ADD_TEST ( NAME GenevaNoDriverLink
		COMMAND ${CMAKE_COMMAND}
			"-DLIBS=$<TARGET_FILE:${GENEVA_LIBNAME}>"
			-P ${PROJECT_SOURCE_DIR}/libs/courtier/tests/CheckNoDriverLink.cmake )
	SET_TESTS_PROPERTIES ( GenevaNoDriverLink PROPERTIES LABELS "geneva" )
	GEM_TEST_SIZE ( GenevaNoDriverLink S )
ENDIF ()

# With the MPI consumer built: no Geneva library, and no ordinary Geneva program, may need the MPI
# library or courtier's MPI library at load time -- MPI lives in the module libgemfony-geneva-mpi,
# loaded at run time, and in libgemfony-courtier-mpi, which only the module and programs that use
# MPI link, so that a Geneva program still starts on a machine without MPI. GenevaStandardTests
# stands for the programs. ldd lists the whole closure, so hap, dietrich, common and weft are
# covered with geneva.
IF ( GEM_BUILD_WITH_MPI_CONSUMER )
	ADD_TEST ( NAME GenevaNoMpiLink
		COMMAND ${CMAKE_COMMAND}
			"-DLIBS=$<TARGET_FILE:${COURTIER_LIBNAME}>;$<TARGET_FILE:${GENEVA_LIBNAME}>;$<TARGET_FILE:GenevaStandardTests>"
			"-DPATTERN=libmpi\\.so|libgemfony-courtier-mpi"
			-P ${PROJECT_SOURCE_DIR}/libs/courtier/tests/CheckNoDriverLink.cmake )
	SET_TESTS_PROPERTIES ( GenevaNoMpiLink PROPERTIES LABELS "geneva" )
	GEM_TEST_SIZE ( GenevaNoMpiLink S )
ENDIF ()

GEM_AGGREGATE_TARGET( "tests-geneva" )
