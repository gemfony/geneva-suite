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

# The serialization engine itself is header-only (template codecs + a header
# registry); GWeft.cpp is a genuine identity/version source that anchors the
# shared object so Weft builds and installs uniformly with the other libraries.
SET ( WEFTSOURCES
	GWeft.cpp
)

set_source_files_properties(
    ${WEFTSOURCES}
    PROPERTIES
    LANGUAGE CXX
)

ADD_LIBRARY ( ${WEFT_LIBNAME} SHARED ${WEFTSOURCES} )

SET_TARGET_PROPERTIES(
    ${WEFT_LIBNAME}
    PROPERTIES
    VERSION ${GENEVA_VERSION}
	EXPORT_NAME weft
)

# Stamp the full version string (incl. any pre-release tag) into the library so
# Gem::Weft::version() reports it without reaching for Gem::Common's version macros.
TARGET_COMPILE_DEFINITIONS(
    ${WEFT_LIBNAME}
    PRIVATE
    WEFT_VERSION_STRING="${GENEVA_VERSION_FULL}"
)

# Publish the include directory through the target so that both the in-tree build
# and downstream consumers (via the exported Geneva::weft target) pick up the Weft
# headers. PUBLIC so the requirement propagates to dependents (common links weft).
TARGET_INCLUDE_DIRECTORIES(
    ${WEFT_LIBNAME}
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libs/weft/include>
    $<INSTALL_INTERFACE:include>
)

# Weft is the dependency-free foundation layer: it links NOTHING from Geneva
# (common depends on weft, not the other way round). Its only external dependency
# is Boost.JSON, used by the JSON codec. The C++ standard is selected centrally in
# CMakeModules/CommonGenevaBuild.cmake.
TARGET_LINK_LIBRARIES(
    ${WEFT_LIBNAME}
    PUBLIC
    Boost::json
)

# Coverage instrumentation: only applied when explicitly enabled (driven by the
# parent option GENEVA_BUILD_WITH_COVERAGE). Costly at runtime; do not enable for
# production builds.
IF (GENEVA_BUILD_WITH_COVERAGE)
    IF (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        TARGET_COMPILE_OPTIONS(${WEFT_LIBNAME} PUBLIC
            -fprofile-instr-generate -fcoverage-mapping)
        TARGET_LINK_OPTIONS(${WEFT_LIBNAME} PUBLIC
            -fprofile-instr-generate -fcoverage-mapping)
    ELSEIF (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        TARGET_COMPILE_OPTIONS(${WEFT_LIBNAME} PUBLIC --coverage)
        TARGET_LINK_OPTIONS(${WEFT_LIBNAME} PUBLIC --coverage)
    ELSE ()
        MESSAGE(WARNING
            "GENEVA_BUILD_WITH_COVERAGE: unsupported compiler "
            "${CMAKE_CXX_COMPILER_ID}; coverage flags not applied.")
    ENDIF ()
ENDIF ()

INSTALL ( TARGETS ${WEFT_LIBNAME} EXPORT GenevaTargets DESTINATION ${INSTALL_PREFIX_LIBS} )
