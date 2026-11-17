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
# from them, so that each fact is stated once (GENEVA_MIN_BOOST_VERSION, GENEVA_BOOST_LIBS in
# CommonGenevaBuild.cmake, GENEVA_HAP_PUBLIC_DEFINE in the top-level CMakeLists.txt).
CONFIGURE_FILE ( FindGeneva.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/FindGeneva.cmake @ONLY )

SET ( CMAKEFILES
	README
	CommonGenevaBuild.cmake
	${CMAKE_CURRENT_BINARY_DIR}/FindGeneva.cmake
	# Other macro files
	CleanCmakeTemporaries.cmake
	IdentifySystemParameters.cmake
	IndependentBuild.cmake
)

INSTALL ( FILES ${CMAKEFILES} DESTINATION ${INSTALL_PREFIX_DATA}/CMakeModules )
