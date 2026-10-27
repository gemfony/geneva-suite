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


################################################################################
#
# This file is part of the Geneva library collection. Apache License 2.0.
# See the NOTICE file in the top-level directory for contributors/copyright.
#
################################################################################
#
# Orchestrator for the Hap library subtree.
#
# Hap mirrors Hap with identical class names in namespace Gem::Hap, library
# gemfony-hap and include prefix <hap/...>, so it can act as a drop-in
# replacement. Both libraries build in parallel; the GHapVsHap benchmark links
# both for side-by-side comparison.
#
# Layout:
#   hap/include/hap/*.hpp   public headers
#   hap/src/*.cpp *.cu       implementation + the gemfony-hap target
################################################################################

ADD_SUBDIRECTORY ( include/hap )
ADD_SUBDIRECTORY ( src )

IF ( GENEVA_BUILD_TESTS )
	ADD_SUBDIRECTORY ( tests )
ENDIF ()

IF ( GENEVA_BUILD_EXAMPLES )
	ADD_SUBDIRECTORY ( examples )
ENDIF ()

IF ( GENEVA_BUILD_BENCHMARKS )
	ADD_SUBDIRECTORY ( benchmarks )
ENDIF ()

# Per-library Doxygen target (doc-hap), mirroring hap/.
ADD_SUBDIRECTORY ( doc )

GENEVA_AGGREGATE_TARGET( "hap-all" )
