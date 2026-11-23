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

# Test support shared by the unit tests of the libraries above common (geneva, dietrich): the
# header-only GEqualityPrinter in support/. An INTERFACE target that carries the include path and
# depends on common only, so every library that may use common may link it.
ADD_LIBRARY ( gem-test-support INTERFACE )
TARGET_INCLUDE_DIRECTORIES ( gem-test-support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/support )
TARGET_LINK_LIBRARIES ( gem-test-support INTERFACE ${COMMON_LIBNAME} )

ADD_SUBDIRECTORY ( UnitTests )
ADD_SUBDIRECTORY ( ManualTests )

GEM_AGGREGATE_TARGET( "tests-common" )
