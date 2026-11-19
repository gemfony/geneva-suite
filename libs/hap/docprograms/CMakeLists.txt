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

# Documentation programs (suite Invariant 27; D2 in the manual repository's
# DOCUMENTATION-INVARIANTS.md): small, self-contained programs written to back the manual --
# here, to PRODUCE ITS FIGURES. Each links only this library's own dependency closure and is
# registered as an S-size test, so a documentation program that stops compiling, or stops doing
# what the manual says, turns a gate red rather than rotting quietly. These are deliberately NOT
# in examples/: an example teaches a user, a docu-program backs a listing or a figure.

SET ( _HAP_DOCPROGRAMS
	plot_rng_distributions  # histograms of the distributions drawn through a proxy (8 figures)
)

FOREACH ( _dp ${_HAP_DOCPROGRAMS} )
	ADD_EXECUTABLE ( hap_${_dp} ${_dp}.cpp )
	# hap's own dependency closure only (hap <- common <- weft, in link order).
	TARGET_LINK_LIBRARIES ( hap_${_dp} ${HAP_LIBNAME} ${COMMON_LIBNAME} ${WEFT_LIBNAME} ${Boost_LIBRARIES} )
	# The program writes its plot files to the working directory, so it runs in the build
	# tree and never in the source tree (Invariant 17).
	ADD_TEST ( NAME hap_${_dp} COMMAND hap_${_dp}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} )
	SET_TESTS_PROPERTIES ( hap_${_dp} PROPERTIES LABELS "hap;docprogram" TIMEOUT 120 )
	GEM_TEST_SIZE ( hap_${_dp} S )
	INSTALL ( TARGETS hap_${_dp}
		DESTINATION ${INSTALL_PREFIX_DATA}/docprograms/hap )
ENDFOREACH ()

# The figures of an earlier run are removed before the programs run, and every figure the manual
# renders must then have been written by this run (the manual's renderer skips a missing one and
# keeps its old picture). The figure-titles check of geneva's docprograms does not apply: it reads
# the macros the plotting library writes -- pads, a canvas label, legend entries -- and these
# macros are written by hand and have none of them.
SET ( _HAP_DOCPROGRAM_FIGURES )
FOREACH ( _figure gauss dgauss even evenwb discrete discretewb bitsimple bitprob )
	LIST ( APPEND _HAP_DOCPROGRAM_FIGURES ${CMAKE_CURRENT_BINARY_DIR}/rng-${_figure}.C )
ENDFOREACH ()
ADD_TEST ( NAME hap_docprogram_figures_removed
	COMMAND ${CMAKE_COMMAND} -E rm -f ${_HAP_DOCPROGRAM_FIGURES} )
SET_TESTS_PROPERTIES ( hap_docprogram_figures_removed PROPERTIES LABELS "hap;docprogram"
	FIXTURES_SETUP hap_docprogram_fresh )
GEM_TEST_SIZE ( hap_docprogram_figures_removed S )
FOREACH ( _dp ${_HAP_DOCPROGRAMS} )
	SET_PROPERTY ( TEST hap_${_dp} APPEND PROPERTY FIXTURES_REQUIRED hap_docprogram_fresh )
	SET_PROPERTY ( TEST hap_${_dp} APPEND PROPERTY FIXTURES_SETUP hap_docprogram_figures )
ENDFOREACH ()
# sha256sum fails on a file that is not there.
ADD_TEST ( NAME hap_docprogram_figures_written
	COMMAND ${CMAKE_COMMAND} -E sha256sum ${_HAP_DOCPROGRAM_FIGURES} )
SET_TESTS_PROPERTIES ( hap_docprogram_figures_written PROPERTIES LABELS "hap;docprogram"
	FIXTURES_REQUIRED hap_docprogram_figures )
GEM_TEST_SIZE ( hap_docprogram_figures_written S )

GEM_AGGREGATE_TARGET( "docprograms-hap" )
