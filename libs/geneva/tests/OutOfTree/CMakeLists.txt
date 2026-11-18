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

# The out-of-tree probe (MpiModuleProbe/), built and run in the normal build with what a program
# outside the tree gets and nothing else: the usage requirements of the targets it links, the same
# ones its own CMakeLists.txt names as Geneva::courtier-mpi and Geneva::geneva.
# The collection's own directory-level settings are taken away first: its compile definitions
# (GENEVA_BUILD_WITH_MPI_CONSUMER among them, which no exported target carries) and its include
# directories (Boost's and MPI's, which a program outside gets only through the targets). A header
# that compiles only with one of them breaks this build, as it would break a user's. The probe
# itself is configured standalone against an installation by the release harness and the container
# image build; this directory is added only when the MPI consumer is built.

SET_PROPERTY ( DIRECTORY PROPERTY COMPILE_DEFINITIONS "" )
SET_PROPERTY ( DIRECTORY PROPERTY INCLUDE_DIRECTORIES "" )

ADD_EXECUTABLE ( GenevaMpiModuleProbe MpiModuleProbe/GenevaMpiModuleProbe.cpp )
TARGET_LINK_LIBRARIES ( GenevaMpiModuleProbe
	PRIVATE ${COURTIER_MPI_LIBNAME} ${GENEVA_LIBNAME} )
ADD_DEPENDENCIES ( GenevaMpiModuleProbe ${GENEVA_MPI_LIBNAME} ) # loaded at run time, not linked

# The probe loads the "mpi" consumer from the module next to libgemfony-geneva and exits with 0 if
# that worked. It initializes no MPI, so it needs no launcher.
ADD_TEST ( NAME GenevaMpiModuleProbe
	COMMAND $<TARGET_FILE:GenevaMpiModuleProbe>
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} )
SET_TESTS_PROPERTIES ( GenevaMpiModuleProbe PROPERTIES LABELS "geneva" )
GENEVA_TEST_SIZE ( GenevaMpiModuleProbe S )
