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
	# courtier's own dependency closure only (courtier <- hap <- common <- weft, in link order) --
	# a docu-program of this library must not need the libraries that sit above it.
	TARGET_LINK_LIBRARIES ( courtier_${_dp}
		${COURTIER_LIBNAME} ${HAP_LIBNAME} ${COMMON_LIBNAME} ${WEFT_LIBNAME} ${Boost_LIBRARIES} )
	ADD_TEST ( NAME courtier_${_dp} COMMAND courtier_${_dp} )
	SET_TESTS_PROPERTIES ( courtier_${_dp} PROPERTIES LABELS "courtier;docprogram" TIMEOUT 120 )
	GENEVA_TEST_SIZE ( courtier_${_dp} S )
	INSTALL ( TARGETS courtier_${_dp}
		DESTINATION ${INSTALL_PREFIX_DATA}/docprograms/courtier )
ENDFOREACH ()

GENEVA_AGGREGATE_TARGET( "docprograms-courtier" )
