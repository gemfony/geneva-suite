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
# DOCUMENTATION-INVARIANTS.md): small, self-contained programs written to be QUOTED BY THE MANUAL.
# Each links only this library's own dependency closure and is registered as an S-size test, so a
# documentation program that stops compiling, or stops doing what the manual says, turns a gate red
# rather than rotting quietly. These are deliberately NOT in examples/: an example teaches a user,
# a docu-program backs a listing.

SET ( _COMMON_DOCPROGRAMS
	record_share_prices  # a typed stream with a time column, a few rows, the close
)

FOREACH ( _dp ${_COMMON_DOCPROGRAMS} )
	ADD_EXECUTABLE ( common_${_dp} ${_dp}.cpp )
	# common's own dependency closure only (common <- weft, in link order).
	TARGET_LINK_LIBRARIES ( common_${_dp} ${COMMON_LIBNAME} ${WEFT_LIBNAME} ${Boost_LIBRARIES} )
	# The program writes records/ into the working directory, so it runs in the build tree and
	# never in the source tree (Invariant 17).
	ADD_TEST ( NAME common_${_dp} COMMAND common_${_dp}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} )
	SET_TESTS_PROPERTIES ( common_${_dp} PROPERTIES LABELS "common;docprogram" TIMEOUT 120 )
	GEM_TEST_SIZE ( common_${_dp} S )
	INSTALL ( TARGETS common_${_dp}
		DESTINATION ${INSTALL_PREFIX_DATA}/docprograms/common )
ENDFOREACH ()

# The file of an earlier run is removed first, so a run neither backs it up nor leaves backups
# behind in the build tree.
ADD_TEST ( NAME common_docprogram_records_removed
	COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_CURRENT_BINARY_DIR}/records/quotes.csv )
SET_TESTS_PROPERTIES ( common_docprogram_records_removed PROPERTIES LABELS "common;docprogram"
	FIXTURES_SETUP common_docprogram_fresh )
GEM_TEST_SIZE ( common_docprogram_records_removed S )
SET_PROPERTY ( TEST common_record_share_prices APPEND PROPERTY FIXTURES_REQUIRED
	common_docprogram_fresh )

GEM_AGGREGATE_TARGET( "docprograms-common" )
