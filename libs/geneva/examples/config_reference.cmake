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
# Helper driver for the config-reference / config-reference-check targets, invoked with `cmake -P`.
# It generates the standard configuration set (timestamp-free) from a representative Go2 binary into a
# temporary directory, then -- depending on MODE -- either copies it into the checked-in reference tree
# (MODE=generate) or fails if it differs from the checked-in tree (MODE=check, never writing into source).
#
# Expected -D arguments: BIN (the generator binary), GEN (a scratch dir), DEST (docs/.../config), MODE.

if(NOT BIN OR NOT GEN OR NOT DEST OR NOT MODE)
	message(FATAL_ERROR "config_reference.cmake: BIN, GEN, DEST and MODE must all be set")
endif()

# Generate the standard config set, timestamp-free, into a clean scratch dir. GENEVA_CONFIG_REFERENCE makes
# the emitted headers byte-stable; the binary writes into ./config relative to its working directory.
file(REMOVE_RECURSE "${GEN}")
file(MAKE_DIRECTORY "${GEN}/config")
execute_process(
		COMMAND ${CMAKE_COMMAND} -E env GENEVA_CONFIG_REFERENCE=1 "${BIN}" --update-configs
		WORKING_DIRECTORY "${GEN}"
		RESULT_VARIABLE _rc
		OUTPUT_QUIET)
if(NOT _rc EQUAL 0)
	message(FATAL_ERROR "config-reference: generating the reference set failed (exit ${_rc})")
endif()

file(GLOB _gen_files RELATIVE "${GEN}/config" "${GEN}/config/*.json")
list(SORT _gen_files)

if(MODE STREQUAL "generate")
	# Replace the checked-in set wholesale, so a config the code no longer owns disappears too.
	file(REMOVE_RECURSE "${DEST}")
	file(MAKE_DIRECTORY "${DEST}")
	foreach(_f IN LISTS _gen_files)
		configure_file("${GEN}/config/${_f}" "${DEST}/${_f}" COPYONLY)
	endforeach()
	message(STATUS "config-reference: regenerated ${DEST} (${_gen_files})")
elseif(MODE STREQUAL "check")
	# Fail if the freshly generated set differs from the checked-in reference (a stale reference).
	file(GLOB _ref_files RELATIVE "${DEST}" "${DEST}/*.json")
	list(SORT _ref_files)
	if(NOT _gen_files STREQUAL _ref_files)
		message(FATAL_ERROR
				"config-reference is STALE: file set differs.\n"
				"  generated: ${_gen_files}\n"
				"  checked-in: ${_ref_files}\n"
				"Run 'make config-reference' to update docs/config-reference/config/.")
	endif()
	foreach(_f IN LISTS _gen_files)
		execute_process(
				COMMAND ${CMAKE_COMMAND} -E compare_files "${GEN}/config/${_f}" "${DEST}/${_f}"
				RESULT_VARIABLE _diff)
		if(NOT _diff EQUAL 0)
			message(FATAL_ERROR
					"config-reference is STALE: ${_f} differs from the code's current defaults.\n"
					"Run 'make config-reference' to update docs/config-reference/config/.")
		endif()
	endforeach()
	message(STATUS "config-reference: docs/config-reference/config/ is up to date (${_gen_files})")
else()
	message(FATAL_ERROR "config_reference.cmake: unknown MODE '${MODE}' (expected generate or check)")
endif()
