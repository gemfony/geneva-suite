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
# Orchestrator for the self-contained Dietrich plotting library subtree.
#
# Dietrich is the Geneva plotting library (named for the versatile 18th-century
# painter Christian Wilhelm Ernst Dietrich -- see dietrich/README.md). It turns
# data into ROOT macros, gnuplot / matplotlib scripts, raw CSV/.npz exports and
# a self-describing manifest for an external renderer. It depends ONLY on the
# common library; its tests/demos may additionally use hap to generate data, but
# the main library never links hap, courtier or geneva.
#
# Layout (mirrors the other per-library subtrees):
#   dietrich/include/dietrich/*.hpp   public headers (install layout 1:1)
#   dietrich/src/*.cpp                implementation + the gemfony-dietrich target
#   dietrich/tests/                   unit + manual (validity-gate) tests
#   dietrich/scripts/                 the bundled external renderer
#
# Tests are gated on the top-level options so a minimal build still only
# descends into include and src.
################################################################################

ADD_SUBDIRECTORY ( include/dietrich )
ADD_SUBDIRECTORY ( src )
ADD_SUBDIRECTORY ( scripts )

IF ( GENEVA_BUILD_TESTS )
	ADD_SUBDIRECTORY ( tests )
ENDIF ()

IF ( GENEVA_BUILD_EXAMPLES )
	ADD_SUBDIRECTORY ( examples )
ENDIF ()

# Per-library Doxygen target (doc-dietrich). The CMakeLists short-circuits
# when Doxygen was not found.
ADD_SUBDIRECTORY ( doc )

# Self-contained aggregate over EVERYTHING in this lib's subtree (lib + tests).
# The top-level `gemfony-build-all` depends on this so a plain
# `make gemfony-build-all` still builds all of dietrich.
GENEVA_AGGREGATE_TARGET( "dietrich-all" )
