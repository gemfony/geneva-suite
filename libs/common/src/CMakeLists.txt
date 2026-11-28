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
	GFatalReport.cpp
	GFileExchangeRun.cpp
	GJsonIO.cpp
	GLogger.cpp
	GModuleLoader.cpp
	GParserBuilder.cpp
	GProgramIdentity.cpp
	GRecordLogTarget.cpp
	GStopRequest.cpp
	concurrency/GThreadGroup.cpp
	concurrency/GThreadPool.cpp
	record/GCsvSink.cpp
	record/GFileSink.cpp
	record/GJsonLinesSink.cpp
	record/GRecorder.cpp
	record/GStream.cpp
	record/GTextSink.cpp
	${GEM_COMMIT_SOURCE}
)

# The commit of the sources, taken again before every build of the library (the top-level
# CMakeLists.txt says why). The script rewrites GEM_COMMIT_SOURCE only when the id changes, so
# that otherwise nothing is compiled or linked again. A changed id (a commit, or the first change of
# a tracked file after one: "-dirty") costs one compile and the relink of the library and of every
# target that links it -- intended, not a defect.
ADD_CUSTOM_TARGET ( gem-commit
	COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${GEM_COMMIT_SOURCE}
		-P ${PROJECT_SOURCE_DIR}/CMakeModules/GemCommit.cmake
	BYPRODUCTS ${GEM_COMMIT_SOURCE}
	COMMENT "Taking the commit of the Geneva sources"
	VERBATIM
)


set_source_files_properties(
    ${COMMONSOURCES}
    PROPERTIES
    LANGUAGE CXX
)

ADD_LIBRARY ( ${COMMON_LIBNAME} SHARED ${COMMONSOURCES} )
ADD_DEPENDENCIES ( ${COMMON_LIBNAME} gem-commit )

SET_TARGET_PROPERTIES(
    ${COMMON_LIBNAME}
    PROPERTIES
    VERSION ${GEM_VERSION}
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
# (json/program_options). The C++ standard is
# selected centrally in CMakeModules/CommonGemBuild.cmake.
TARGET_LINK_LIBRARIES(
    ${COMMON_LIBNAME}
    PUBLIC
    ${WEFT_LIBNAME}
    Boost::json
    Boost::program_options
)

# runExternalCommand starts programs through Boost.Process v2. PRIVATE: no public header names it,
# so programs that link common do not get Boost.Process's own dependencies (container, context,
# date_time, exception) on their link line. Boost is built with boost.process.fs=std (INSTALL), and
# the headers must be told so: Boost's CMake package does not pass BOOST_PROCESS_USE_STD_FS on.
TARGET_LINK_LIBRARIES(${COMMON_LIBNAME} PRIVATE Boost::process)
TARGET_COMPILE_DEFINITIONS(${COMMON_LIBNAME} PRIVATE BOOST_PROCESS_USE_STD_FS)
# Boost::process's CMake package names container, context, date_time and exception as link
# dependencies, which common does not use. --as-needed keeps them out of libgemfony-common's NEEDED
# entries; Ubuntu's gcc links so by default, its clang does not.
IF (UNIX AND NOT APPLE)
	TARGET_LINK_OPTIONS(${COMMON_LIBNAME} PRIVATE "LINKER:--as-needed")
ENDIF ()

# The module loader (GModuleLoader) uses Boost.DLL, which is header-only here and needs only
# the dynamic loading library, on platforms that have one.
TARGET_LINK_LIBRARIES(${COMMON_LIBNAME} PRIVATE ${CMAKE_DL_LIBS})

INSTALL ( TARGETS ${COMMON_LIBNAME} EXPORT GenevaTargets DESTINATION ${INSTALL_PREFIX_LIBS} )
