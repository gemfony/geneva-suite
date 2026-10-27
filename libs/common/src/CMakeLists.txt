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

SET ( COMMONSOURCES
    GCommonEnums.cpp
    GCommonHelperFunctions.cpp
    GConfigEmission.cpp
    GExceptions.cpp
	GExpectationChecksT.cpp
	GJsonIO.cpp
	GLogger.cpp
	GParserBuilder.cpp
	GSigHupHandler.cpp
	concurrency/GThreadGroup.cpp
	concurrency/GThreadPool.cpp
)


set_source_files_properties(
    ${COMMONSOURCES}
    PROPERTIES
    LANGUAGE CXX
)

ADD_LIBRARY ( ${COMMON_LIBNAME} SHARED ${COMMONSOURCES} )

SET_TARGET_PROPERTIES(
    ${COMMON_LIBNAME}
    PROPERTIES
    VERSION ${GENEVA_VERSION}
	EXPORT_NAME common
)

# Publish the include directory through the target so that both the in-tree
# build and downstream consumers (via the exported Geneva::common target) pick
# up the Geneva headers. PUBLIC so the requirement propagates to dependents.
TARGET_INCLUDE_DIRECTORIES(
    ${COMMON_LIBNAME}
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libs/common/include>
    $<INSTALL_INTERFACE:include>
)

# Propagate the Boost dependencies that appear in the public headers
# (json/program_options/regex/atomic). The C++ standard is
# selected centrally in CMakeModules/CommonGenevaBuild.cmake.
TARGET_LINK_LIBRARIES(
    ${COMMON_LIBNAME}
    PUBLIC
    ${WEFT_LIBNAME}
    Boost::json
    Boost::program_options
    Boost::regex
    Boost::atomic
)

# Coverage instrumentation: only applied when explicitly enabled, only to
# the Common library and its tests (driven by the parent CMakeLists.txt
# option `GENEVA_BUILD_WITH_COVERAGE`). Costly at runtime; do not enable
# for production builds.
IF (GENEVA_BUILD_WITH_COVERAGE)
    IF (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        TARGET_COMPILE_OPTIONS(${COMMON_LIBNAME} PUBLIC
            -fprofile-instr-generate -fcoverage-mapping)
        TARGET_LINK_OPTIONS(${COMMON_LIBNAME} PUBLIC
            -fprofile-instr-generate -fcoverage-mapping)
    ELSEIF (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        TARGET_COMPILE_OPTIONS(${COMMON_LIBNAME} PUBLIC --coverage)
        TARGET_LINK_OPTIONS(${COMMON_LIBNAME} PUBLIC --coverage)
    ELSE ()
        MESSAGE(WARNING
            "GENEVA_BUILD_WITH_COVERAGE: unsupported compiler "
            "${CMAKE_CXX_COMPILER_ID}; coverage flags not applied.")
    ENDIF ()
ENDIF ()

INSTALL ( TARGETS ${COMMON_LIBNAME} EXPORT GenevaTargets DESTINATION ${INSTALL_PREFIX_LIBS} )
