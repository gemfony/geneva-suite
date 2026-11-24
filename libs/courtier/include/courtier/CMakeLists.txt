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

# The IDE's header target (a workaround for a CLion problem, CPP270 in the JetBrains issue tracker):
# every header below this directory, found again whenever one is added or removed.
FILE(GLOB_RECURSE COURTIERINCLUDES CONFIGURE_DEPENDS "*.hpp" "*.cuh")
ADD_CUSTOM_TARGET(CLION_ALL_COURTIER_HEADERS SOURCES ${COURTIERINCLUDES})
set_source_files_properties(${COURTIERINCLUDES} PROPERTIES LANGUAGE CXX)

# Directory install: ship the whole header tree (Inv 19), so the installed interface is
# self-contained and cannot drift.
INSTALL ( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${INSTALL_PREFIX_INCLUDES}/courtier
	FILES_MATCHING PATTERN "*.hpp" )
