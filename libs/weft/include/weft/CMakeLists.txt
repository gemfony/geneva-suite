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

SET ( GENEVAWEFTINCLUDES
	GWeft.hpp
	GArchive.hpp
	GArchivePolymorphic.hpp
	GBinaryArchive.hpp
	GJsonArchive.hpp
	GPolymorphicRegistry.hpp
	GWeftError.hpp
)

# This is a workaround for a CLion-problem -- see CPP270 in the JetBrains issue tracker
ADD_CUSTOM_TARGET(CLION_ALL_WEFT_HEADERS SOURCES ${GENEVAWEFTINCLUDES})

# Directory install: ship the whole header tree (Inv 19), so the installed interface is
# self-contained and cannot drift. (The SET list above is retained only for the CLion
# header target / IDE indexing.)
INSTALL ( DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${INSTALL_PREFIX_INCLUDES}/weft
	FILES_MATCHING PATTERN "*.hpp" )
