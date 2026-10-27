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
# Orchestrator for the self-contained Common library subtree.
#
# Layout (per the 2026-05-28 repo-layout reorganisation):
#   common/include/common/*.hpp   public headers (install layout 1:1)
#   common/src/*.cpp              implementation + the gemfony-common target
#   common/tests/                 unit + manual tests
#   common/examples/              standalone examples
#   common/benchmarks/            performance benchmarks
#
# Tests / examples / benchmarks are gated on the top-level options so a
# minimal build still only descends into include and src.
################################################################################

ADD_SUBDIRECTORY ( include/common )
ADD_SUBDIRECTORY ( src )
# Core build-time tooling (the config-overlay helper), built unconditionally after the library it links.
ADD_SUBDIRECTORY ( tools )

IF ( GENEVA_BUILD_TESTS )
	ADD_SUBDIRECTORY ( tests )
ENDIF ()

IF ( GENEVA_BUILD_EXAMPLES )
	ADD_SUBDIRECTORY ( examples )
ENDIF ()

IF ( GENEVA_BUILD_BENCHMARKS )
	ADD_SUBDIRECTORY ( benchmarks )
ENDIF ()

# Per-library Doxygen target (doc-common). The CMakeLists short-circuits
# when Doxygen was not found.
ADD_SUBDIRECTORY ( doc )

# Self-contained aggregate over EVERYTHING in this lib's subtree (lib +
# tests + examples + benchmarks). The top-level `gemfony-build-all` depends
# on this so a plain `make gemfony-build-all` still builds all of common.
GENEVA_AGGREGATE_TARGET( "common-all" )
