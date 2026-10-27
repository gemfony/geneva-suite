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

# Include guard
IF(NOT IDENTIFY_SYSTEM_PARAMETERS_INCLUDED)
SET(IDENTIFY_SYSTEM_PARAMETERS_INCLUDED TRUE)

###############################################################################
# Includes

INCLUDE(CheckCXXCompilerFlag)

###############################################################################
# Global variables

# Clang settings
SET(CLANG_DEF_IDENTIFIER "Clang")
SET(CLANG_DEF_MIN_CXX14_VERSION "3.4")
SET(CLANG_DEF_MIN_CXX1Z_VERSION "3.9")
SET(CLANG_DEF_CXX14_STANDARD_FLAG "-std=c++14")
SET(CLANG_DEF_CXX1Z_STANDARD_FLAG "-std=c++1z")

# GCC settings
SET(GNU_DEF_IDENTIFIER "GNU")
SET(GNU_DEF_MIN_CXX14_VERSION "4.9")
SET(GNU_DEF_MIN_CXX1Z_VERSION "6.0")
SET(GNU_DEF_CXX14_STANDARD_FLAG "-std=c++14")
SET(GNU_DEF_CXX1Z_STANDARD_FLAG "-std=c++1z")

# Default compiler settings
SET(NONE_DEF_STANDARD_FLAG "")

###############################################################################
# Set the available build types for multi-config generators, and validate
# and set the chosen build type in single-config generators. In this last
# case the build type is set from the value of GENEVA_BUILD_TYPE if defined,
# otherwise the value of CMAKE_BUILD_TYPE is used if defined, or the
# value 'Debug' is used as default otherwise.
#
MACRO (
	VALIDATE_BUILD_TYPE
)

	#--------------------------------------------------------------------------
	SET(GENEVA_CONFIGURATION_TYPES "Debug;Release;RelWithDebInfo;MinSizeRel;Sanitize")

	# Set the available build types on multi-config generators
	IF(CMAKE_CONFIGURATION_TYPES)
		# Due to issue http://www.cmake.org/Bug/view.php?id=5811,
		# the new variable is not used in the first run, only if it is
		# already in the cache... so we force a rerun
		IF (NOT "${CMAKE_CONFIGURATION_TYPES}" STREQUAL "${GENEVA_CONFIGURATION_TYPES}")
			SET(CMAKE_CONFIGURATION_TYPES ${GENEVA_CONFIGURATION_TYPES})
			SET(CMAKE_CONFIGURATION_TYPES ${CMAKE_CONFIGURATION_TYPES} CACHE
			    STRING "The available build types" FORCE)

			MESSAGE("\n\n")
			MESSAGE("#############################################################\n")
			MESSAGE("   The configuration type values changed, to generate")
			MESSAGE("   the right IDE project files CMake must be run again!\n")
			MESSAGE("   Please re-run the same command once more...\n")
			MESSAGE("#############################################################\n\n")
			MESSAGE(FATAL_ERROR "\nThis is NOT an error, but a request to re-run this command!\n")
		ENDIF()
	ENDIF()

	# CMake sets the variable 'NDEBUG' for Release modi, but Geneva uses
	# '#ifdef DEBUG' for compiling debugging code...
	SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")

	# The Sanitize option falls back to Debug on unsupported platforms
	SET(CMAKE_CXX_FLAGS_SANITIZE ${CMAKE_CXX_FLAGS_DEBUG} CACHE
	    STRING "Flags used by the C++ compiler during sanitize builds" FORCE)
	SET(CMAKE_C_FLAGS_SANITIZE ${CMAKE_C_FLAGS_DEBUG} CACHE
	    STRING "Flags used by the C compiler during sanitize builds" FORCE)
	SET(CMAKE_EXE_LINKER_FLAGS_SANITIZE ${CMAKE_EXE_LINKER_FLAGS_DEBUG} CACHE
	    STRING "Flags used for linking binaries during sanitize builds" FORCE)
	SET(CMAKE_SHARED_LINKER_FLAGS_SANITIZE ${CMAKE_SHARED_LINKER_FLAGS_DEBUG} CACHE
	    STRING "Flags used by the shared libraries linker during sanitize builds" FORCE)
	MARK_AS_ADVANCED(
		CMAKE_CXX_FLAGS_SANITIZE
		CMAKE_C_FLAGS_SANITIZE
		CMAKE_EXE_LINKER_FLAGS_SANITIZE
		CMAKE_SHARED_LINKER_FLAGS_SANITIZE
	)
	# Update the documentation string of CMAKE_BUILD_TYPE for GUIs
	SET(CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE
	    STRING "Choose the build type, options are: Debug Release RelWithDebInfo MinSizeRel Sanitize" FORCE)

	# The build type is not needed on multi-config generators
	IF(NOT CMAKE_CONFIGURATION_TYPES)
		IF(GENEVA_BUILD_TYPE)
			IF(NOT ";${GENEVA_CONFIGURATION_TYPES};" MATCHES ";${GENEVA_BUILD_TYPE};")
				MESSAGE (FATAL_ERROR "Unknown build type GENEVA_BUILD_TYPE=${GENEVA_BUILD_TYPE}!")
			ENDIF()
			SET(MSG "Setting the build type to ${GENEVA_BUILD_TYPE}")
			SET(CMAKE_BUILD_TYPE ${GENEVA_BUILD_TYPE})
			IF(${GENEVA_BUILD_TYPE} STREQUAL "Sanitize")
				SET(MSG "${MSG}\nThis will default to Debug on systems that do not support this setting")
			ENDIF()
		ELSEIF(CMAKE_BUILD_TYPE) # GENEVA_BUILD_TYPE is unset, but CMAKE_BUILD_TYPE is set and not empty
			IF(NOT ";${GENEVA_CONFIGURATION_TYPES};" MATCHES ";${CMAKE_BUILD_TYPE};")
				MESSAGE (FATAL_ERROR "Unknown build type CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}!")
			ENDIF()
			SET(MSG "Using build type CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
			SET(GENEVA_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
		ELSE() # Both variables are unset
			SET(MSG "Setting the build type to default value Debug")
			SET(CMAKE_BUILD_TYPE "Debug")
			SET(GENEVA_BUILD_TYPE "Debug")
		ENDIF()
		MESSAGE ("\n${MSG}\n")
	ENDIF ()
	#--------------------------------------------------------------------------

ENDMACRO()

###############################################################################
# Identifies the operating system and version of the host system
#
FUNCTION (
    FIND_HOST_OS
    GENEVA_OS_NAME_OUT
    GENEVA_OS_VERSION_OUT
)
    execute_process(COMMAND uname -r OUTPUT_VARIABLE LINUX_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    SET(${GENEVA_OS_NAME_OUT} "Linux" PARENT_SCOPE)
    SET(${GENEVA_OS_VERSION_OUT} "${LINUX_VERSION}" PARENT_SCOPE)
ENDFUNCTION()

################################################################################
# Sets the compiler flags for this platform and compiler
#
FUNCTION (
    SET_COMPILER_FLAGS
    GENEVA_OS_NAME_IN
    GENEVA_OS_VERSION_IN
    GENEVA_BUILD_MODE_IN
)
    SET(FLAGS_LOCAL "${CMAKE_CXX_FLAGS}")

    IF(CMAKE_CXX_COMPILER_ID MATCHES ${CLANG_DEF_IDENTIFIER})
        SET(FLAGS_LOCAL "${FLAGS_LOCAL} -Wall -Wno-unused -Wno-attributes -Wno-parentheses-equality -Wno-deprecated-register")
        SET(FLAGS_LOCAL "${FLAGS_LOCAL} -ftemplate-depth=512 -pthread")
        SET(CMAKE_CXX_FLAGS_SANITIZE "${CMAKE_CXX_FLAGS_SANITIZE} -fsanitize=thread" PARENT_SCOPE)
    ELSEIF(CMAKE_CXX_COMPILER_ID MATCHES ${GNU_DEF_IDENTIFIER})
        SET(FLAGS_LOCAL "${FLAGS_LOCAL} -fno-unsafe-math-optimizations -fno-finite-math-only")
        SET(FLAGS_LOCAL "${FLAGS_LOCAL} -fmessage-length=0 -ftemplate-depth=1024 -pthread")
        SET(CMAKE_CXX_FLAGS_SANITIZE "${CMAKE_CXX_FLAGS_SANITIZE} -fsanitize=thread" PARENT_SCOPE)
    ELSE()
        MESSAGE(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Geneva requires GCC or Clang on Linux.")
    ENDIF()

    SET(CMAKE_CXX_FLAGS "${FLAGS_LOCAL}" PARENT_SCOPE)
ENDFUNCTION()

################################################################################
# Sets the linker flags for this platform and compiler
#
FUNCTION (
    SET_LINKER_FLAGS
    GENEVA_OS_NAME_IN
    GENEVA_OS_VERSION_IN
    GENEVA_BUILD_MODE_IN
)
    IF(CMAKE_CXX_COMPILER_ID MATCHES ${GNU_DEF_IDENTIFIER})
        # For GCC version < 9.0 add the filesystem library explicitly
        IF(${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS 9.0)
            SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lstdc++fs" PARENT_SCOPE)
            SET (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -lstdc++fs" PARENT_SCOPE)
        ENDIF()
    ENDIF()
ENDFUNCTION()

###############################################################################
# Determines other build flags for this platform and compiler
#
FUNCTION (
    GET_BUILD_FLAGS
    GENEVA_OS_NAME_IN
    GENEVA_OS_VERSION_IN
    GENEVA_BUILD_MODE_IN
    PLATFORM_NEEDS_LIBRARY_LINKING_OUT
)
    # Linux does not need explicit library linking in ADD_LIBRARY targets
    SET(${PLATFORM_NEEDS_LIBRARY_LINKING_OUT} FALSE PARENT_SCOPE)
ENDFUNCTION()

###############################################################################
# Identifies unsupported setups as early as possible
#
FUNCTION (
    FLAG_UNSUPPORTED_SETUPS
    GENEVA_OS_NAME_IN
    GENEVA_OS_VERSION_IN
    GENEVA_BUILD_MODE_IN
)
    IF(NOT ${GENEVA_OS_NAME_IN} STREQUAL "Linux")
        MESSAGE(FATAL_ERROR "Geneva only supports Linux.")
    ENDIF()

    # The compiler floor is the GCC and Clang that Ubuntu 26.10, the recommended platform, ships:
    # GCC 15 and Clang 22. Both cover the C++23 library surface Geneva relies on.
    IF(${CMAKE_CXX_COMPILER_ID} STREQUAL ${CLANG_DEF_IDENTIFIER})
        SET(COMPILER_MIN_VER 22.0)
    ELSEIF(${CMAKE_CXX_COMPILER_ID} STREQUAL ${GNU_DEF_IDENTIFIER})
        SET(COMPILER_MIN_VER 15.0)
    ELSE()
        MESSAGE(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Geneva requires GCC >= 15 or Clang >= 22.")
    ENDIF()

    IF(${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS ${COMPILER_MIN_VER})
        MESSAGE(FATAL_ERROR "Compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old. Need >= ${COMPILER_MIN_VER}.")
    ENDIF()
ENDFUNCTION()

###############################################################################
# End of the include-guard

ENDIF(NOT IDENTIFY_SYSTEM_PARAMETERS_INCLUDED)

###############################################################################
# Done
