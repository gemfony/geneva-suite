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
#
# The link closure of a library's programs: a test that fails when a program of a library, or a
# library its programs are built with, links a library outside that library's own closure.
#
#     gem_check_link_closure(<test name> FORBID <regex> DIRECTORIES <dirs...>)
#
# called at the end of a library's CMakeLists.txt, after its subdirectories were added. It walks the
# given directories (relative to the calling one; a directory that was not added, as tests/ in a
# build without tests, is left out) and their subdirectories, reads the link libraries of every
# executable and library defined there, follows them through the libraries of this build (their
# interface link libraries), and registers the S test <test name> (label "conventions"). The test
# lists "<target>: <library>" for every library whose name matches <regex> and fails if there is
# one. The check reads the build description, not the binaries: with --as-needed the linker drops
# a library no symbol is taken from, so the binaries would not show a link that nothing uses yet --
# but such a link still makes the program wait for, and depend on, the library's build.
#
# The test runs this file in script mode (cmake -P) with what the configure found:
#   -DTEST_NAME=<test name> -DCHECKED=<number of targets> -DFORBID=<regex>
#   -DVIOLATIONS=<"target: library" entries, separated by "|">
#
################################################################################

IF(CMAKE_SCRIPT_MODE_FILE)
	IF(NOT VIOLATIONS STREQUAL "")
		STRING(REPLACE "|" "\n  " _list "${VIOLATIONS}")
		MESSAGE(FATAL_ERROR "${TEST_NAME}: of ${CHECKED} programs and libraries, these link a library "
			"matching '${FORBID}':\n  ${_list}")
	ENDIF()
	MESSAGE(STATUS "${TEST_NAME}: none of ${CHECKED} programs and libraries links a library matching "
		"'${FORBID}'")
	RETURN()
ENDIF()

# Collects into <out> the targets defined in <dir> and in the directories below it
FUNCTION(_gem_link_closure_targets dir out)
	GET_PROPERTY(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
	GET_PROPERTY(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
	FOREACH(_sub IN LISTS _subdirs)
		_gem_link_closure_targets("${_sub}" _below)
		LIST(APPEND _targets ${_below})
	ENDFOREACH()
	SET(${out} ${_targets} PARENT_SCOPE)
ENDFUNCTION()

FUNCTION(gem_check_link_closure test_name)
	CMAKE_PARSE_ARGUMENTS(PARSE_ARGV 1 arg "" "FORBID" "DIRECTORIES")
	IF(NOT arg_FORBID OR NOT arg_DIRECTORIES OR arg_UNPARSED_ARGUMENTS)
		MESSAGE(FATAL_ERROR "gem_check_link_closure(<test name> FORBID <regex> DIRECTORIES <dirs...>)")
	ENDIF()

	# The directories that were added below the calling one
	GET_PROPERTY(_added DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)
	SET(_targets "")
	FOREACH(_dir IN LISTS arg_DIRECTORIES)
		SET(_full "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}")
		IF(_full IN_LIST _added)
			_gem_link_closure_targets("${_full}" _found)
			LIST(APPEND _targets ${_found})
		ENDIF()
	ENDFOREACH()

	SET(_checked 0)
	SET(_violations "")
	FOREACH(_target IN LISTS _targets)
		GET_TARGET_PROPERTY(_type ${_target} TYPE)
		IF(NOT _type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
			CONTINUE()
		ENDIF()
		MATH(EXPR _checked "${_checked} + 1")

		# Breadth first through the libraries of this build; imported targets (Boost, MPI, CUDA,
		# Catch2) are not followed
		GET_TARGET_PROPERTY(_queue ${_target} LINK_LIBRARIES)
		IF(NOT _queue)
			SET(_queue "")
		ENDIF()
		SET(_seen "")
		LIST(LENGTH _queue _left)
		WHILE(_left GREATER 0)
			LIST(POP_FRONT _queue _item)
			LIST(LENGTH _queue _left)
			# A private dependency of a static library reaches its users as $<LINK_ONLY:...>
			STRING(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _item "${_item}")
			IF(_item STREQUAL "" OR _item MATCHES "\\$<" OR _item IN_LIST _seen)
				CONTINUE()
			ENDIF()
			LIST(APPEND _seen "${_item}")
			IF(_item MATCHES "${arg_FORBID}")
				LIST(APPEND _violations "${_target}: ${_item}")
			ENDIF()
			IF(TARGET ${_item})
				GET_TARGET_PROPERTY(_imported ${_item} IMPORTED)
				IF(NOT _imported)
					GET_TARGET_PROPERTY(_more ${_item} INTERFACE_LINK_LIBRARIES)
					IF(_more)
						LIST(APPEND _queue ${_more})
						LIST(LENGTH _queue _left)
					ENDIF()
				ENDIF()
			ENDIF()
		ENDWHILE()
	ENDFOREACH()

	IF(_checked EQUAL 0)
		MESSAGE(FATAL_ERROR "gem_check_link_closure(${test_name}): no program or library found in "
			"${arg_DIRECTORIES} -- the walk is broken")
	ENDIF()
	LIST(JOIN _violations "|" _violations_text)
	ADD_TEST(
		NAME ${test_name}
		COMMAND ${CMAKE_COMMAND} "-DTEST_NAME=${test_name}" "-DCHECKED=${_checked}"
			"-DFORBID=${arg_FORBID}" "-DVIOLATIONS=${_violations_text}"
			-P ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
	)
	SET_TESTS_PROPERTIES(${test_name} PROPERTIES LABELS "conventions")
	GEM_TEST_SIZE(${test_name} S)
ENDFUNCTION()
