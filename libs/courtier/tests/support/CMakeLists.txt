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

# courtier's test support: the work items that courtier's tests, its manual tests and its
# documentation program submit (GDemoProcessingContainers: GSimpleContainer, GRandomNumberContainer,
# GFaultyContainer with its fault injection). They are test code, so they are not part of the
# courtier library and are neither installed nor exported. The library links courtier only (and
# through it common and weft): the programs that use it stay within courtier's own closure.
ADD_LIBRARY ( courtier-test-support STATIC
	GDemoProcessingContainers.cpp GDemoProcessingContainers.hpp )
TARGET_INCLUDE_DIRECTORIES ( courtier-test-support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} )
TARGET_LINK_LIBRARIES ( courtier-test-support PUBLIC ${COURTIER_LIBNAME} )
