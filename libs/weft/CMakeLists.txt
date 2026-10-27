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
# Orchestrator for Weft, the standalone serialization engine and foundation layer
# of the Geneva library collection. Weft depends on nothing from Geneva (common
# depends on Weft, not the other way round) and only on Boost.JSON externally, so
# it can be reused in other projects.
#
# Layout:
#   weft/include/weft/                 public headers (install layout 1:1)
#     GWeft.hpp                          umbrella header + library identity/version
#     GArchive.hpp                       CRTP codec dispatch bases + serialize contract
#     GBinaryArchive.hpp                 compact little-endian binary value codec
#     GJsonArchive.hpp                   human-readable JSON value codec (Boost.JSON)
#     GPolymorphicRegistry.hpp           content-addressed polymorphic type registry
#     GArchivePolymorphic.hpp            polymorphic-pointer registry-thunk dispatch
#     GWeftError.hpp                     dependency-free error facility
#   weft/src/GWeft.cpp                 identity/version anchor for the shared object
#   weft/tests/                        UnitTests (Catch2)
#   weft/examples/                     standalone usage examples
#
# Tests / examples gated on the top-level options. The "weft-all" aggregate is
# picked up by gemfony-build-all at top level.
################################################################################

ADD_SUBDIRECTORY ( include/weft )
ADD_SUBDIRECTORY ( src )

IF ( GENEVA_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt" )
	ADD_SUBDIRECTORY ( tests )
ENDIF ()

IF ( GENEVA_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt" )
	ADD_SUBDIRECTORY ( examples )
ENDIF ()

# Per-library Doxygen target (doc-weft).
ADD_SUBDIRECTORY ( doc )

GENEVA_AGGREGATE_TARGET( "weft-all" )
