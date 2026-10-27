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
#
# Small, self-contained demos illustrating the Dietrich plotting library for the
# manual. Each is a single .cpp that writes one or more plot scripts / data files
# to the working directory. They use only the standard library for any sample
# data, so they link ONLY the dietrich + common libraries (dietrich is a leaf peer
# on common -- never hap / courtier / geneva).
################################################################################

SET ( _DIETRICH_EXAMPLES
	quickstart       # the modern GDataLog API: one series -> every backend
	gallery          # one multi-pad canvas: graphs + histograms
	overlays         # primary + secondary plotters; CURVE vs SCATTER plot modes
	functions        # formula-driven function plotters (ROOT-only capability)
	external_render  # the DATA backend (CSV + .npz) for the external renderer
)

FOREACH ( _ex ${_DIETRICH_EXAMPLES} )
	ADD_EXECUTABLE ( dietrich_${_ex} ${_ex}.cpp )
	TARGET_LINK_LIBRARIES ( dietrich_${_ex}
		${DIETRICH_LIBNAME} ${COMMON_LIBNAME} ${Boost_LIBRARIES} )
	INSTALL ( TARGETS dietrich_${_ex}
		DESTINATION ${INSTALL_PREFIX_DATA}/examples/dietrich )
ENDFOREACH ()
