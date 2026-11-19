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

# FindGeneva.cmake, the MODULE-mode fallback for finding an installed Geneva, names the Boost
# version and components the build was made with, and hap's compile definition; it is generated
# from them, so that each fact is stated once (GEM_MIN_BOOST_VERSION, read from
# GGlobalDefines.hpp, and GEM_BOOST_LIBS in CommonGemBuild.cmake,
# GEM_HAP_PUBLIC_DEFINE in the top-level CMakeLists.txt).
CONFIGURE_FILE ( FindGeneva.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/FindGeneva.cmake @ONLY )

# It is the only module installed into the installation's CMakeModules/ directory. The config
# package carries its own helpers (GenevaIndividualModule.cmake, GemConfigMaterialization.cmake,
# GemRunQuiet.cmake, GemProgramStamp.cmake with GenevaCommit.cmake; installed by the top-level
# CMakeLists.txt next to GenevaConfig.cmake). The other modules build the collection itself
# (CommonGemBuild.cmake and what it includes); a project outside the tree uses
# find_package(Geneva).
INSTALL ( FILES ${CMAKE_CURRENT_BINARY_DIR}/FindGeneva.cmake
	DESTINATION ${INSTALL_PREFIX_DATA}/CMakeModules )
