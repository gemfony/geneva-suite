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

# Documentation programs (suite Invariant 27; D2 in the manual repository's
# DOCUMENTATION-INVARIANTS.md): small, self-contained programs written to be QUOTED BY
# THE MANUAL -- and, here, to PRODUCE ITS FIGURES -- where the real library code is too
# heavyweight to excerpt. Each is registered as an S-size test, so a documentation
# program that stops compiling, or stops doing what the manual says, turns a gate red
# rather than rotting quietly. The manual's snippet markers point in here (see
# source/snippets.map in the manual repository), and its `make figures` target runs the
# binaries built here. These are deliberately NOT in examples/: an example teaches a
# user, a docu-program backs a listing.

SET ( _GENEVA_DOCPROGRAMS
	noisy_parabola_scan  # the scanned landscape + the search drawn on top of it (2 figures)
)

SET ( _GENEVA_DOCPROGRAM_LIBS
	# GFunctionIndividual lives in its own object library, linked ahead of the Geneva
	# libraries (the in-tree convention for an object library carrying archive registrations).
	GFunctionIndividual-obj
	${GENEVA_LIBRARIES}
	${Boost_LIBRARIES} )

IF ( GENEVA_BUILD_WITH_MPI_CONSUMER )
	SET ( _GENEVA_DOCPROGRAM_LIBS ${_GENEVA_DOCPROGRAM_LIBS} ${MPI_LIBRARIES} )
ENDIF ()

FOREACH ( _dp ${_GENEVA_DOCPROGRAMS} )
	ADD_EXECUTABLE ( geneva_${_dp} ${_dp}.cpp )
	TARGET_LINK_LIBRARIES ( geneva_${_dp} ${_GENEVA_DOCPROGRAM_LIBS} )
	# The program writes its plot files to the working directory, so it runs in the build
	# tree and never in the source tree (Invariant 17).
	ADD_TEST ( NAME geneva_${_dp} COMMAND geneva_${_dp}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} )
	SET_TESTS_PROPERTIES ( geneva_${_dp} PROPERTIES LABELS "geneva;docprogram" TIMEOUT 120 )
	GENEVA_TEST_SIZE ( geneva_${_dp} S )
	INSTALL ( TARGETS geneva_${_dp}
		DESTINATION ${INSTALL_PREFIX_DATA}/docprograms/geneva )
ENDFOREACH ()

GENEVA_AGGREGATE_TARGET( "docprograms-geneva" )
