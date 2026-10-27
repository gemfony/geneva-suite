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
# Orchestrator for the self-contained Courtier library subtree.
#
# Layout (per the 2026-05-28 repo-layout reorganisation):
#   courtier/include/courtier/             public headers (install layout 1:1),
#       *.hpp + consumers/*.hpp            including the consumers/ subgroup
#   courtier/src/*.cpp                     implementation + the gemfony-courtier
#                                          target (with the MPI consumer code
#                                          under GENEVA_BUILD_WITH_MPI_CONSUMER)
#   courtier/tests/                        UnitTests
#   courtier/examples/                     stub (no examples yet, the aggregate
#                                          target lives here for symmetry)
#   courtier/docprograms/                  compiled listings for the manual,
#                                          each registered as an S-size test
#   courtier/benchmarks/                   GBufferPortTTest, GConsumerPerformance
#
# Tests / examples / benchmarks gated on the top-level options. The
# "courtier-all" aggregate is picked up by gemfony-build-all at top level.
################################################################################

ADD_SUBDIRECTORY ( include/courtier )
ADD_SUBDIRECTORY ( src )

IF ( GENEVA_BUILD_TESTS )
	ADD_SUBDIRECTORY ( tests )
ENDIF ()

IF ( GENEVA_BUILD_EXAMPLES )
	ADD_SUBDIRECTORY ( examples )
ENDIF ()

IF ( GENEVA_BUILD_TESTS )
	ADD_SUBDIRECTORY ( docprograms )
ENDIF ()

IF ( GENEVA_BUILD_BENCHMARKS )
	ADD_SUBDIRECTORY ( benchmarks )
ENDIF ()

# Per-library Doxygen target (doc-courtier).
ADD_SUBDIRECTORY ( doc )

GENEVA_AGGREGATE_TARGET( "courtier-all" )
