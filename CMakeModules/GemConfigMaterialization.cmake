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
# Build-time config materialization (GEM_MATERIALIZE_CONFIGS). The function
# body lives in this shared, installable module so the SAME mechanism is
# available to an out-of-tree project that consumes an installed Geneva via
# find_package(Geneva) -- GenevaConfig.cmake includes it, exactly like the
# individual-packaging helpers in GenevaIndividualModule.cmake. It expects
# GemRunQuiet.cmake to sit next to this file (both in-tree in CMakeModules/
# and in the installed package directory).
#
################################################################################

###############################################################################
# GEM_MATERIALIZE_CONFIGS(<target> <install-config-dest>)
#
# Wires a config-owning binary so its configuration directory is MATERIALIZED FROM CODE at build time
# rather than copied from a hand-maintained set of shipped files. After <target> is built it is run once
# with --update-configs, which (re)creates its configuration files from the code's registered defaults
# into <build-dir>/config; that directory's intentional overrides (config/config-overrides.json in the
# source tree, if present) are then overlaid onto them via the GConfigOverlay helper. The materialized
# directory is installed to <install-config-dest> (pass an empty string to skip installation).
#
# Any extra arguments after <install-dest> are passed to the binary before --update-configs (e.g. a
# plugin loader's "--individual <plugin.so>"); if such an argument references another target via a
# $<TARGET_FILE:...> genex, make <target> depend on it so it is built first.
#
# This replaces the former per-directory FILE(COPY config) + INSTALL(FILES ...). It MUST be called from
# the same CMakeLists.txt that defines <target>, because a POST_BUILD command may only be attached to a
# target in the current directory -- i.e. in place of the former ADD_SUBDIRECTORY(config).
#
# The binaries run here emit their normal runtime output (GLogger notes about created config files
# etc.); to keep that from interleaving with the build output, each run is routed through the
# GemRunQuiet.cmake helper, which captures stdout+stderr to a log file in the target's build
# directory and replays it only if the run fails.
SET(GEM_RUN_QUIET_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/GemRunQuiet.cmake)
FUNCTION(GEM_MATERIALIZE_CONFIGS _target _install_dest)
	SET(_extra_args ${ARGN})
	SET(_cfg_dir ${CMAKE_CURRENT_BINARY_DIR}/config)
	SET(_overrides ${CMAKE_CURRENT_SOURCE_DIR}/config/config-overrides.json)

	# Ensure the directory exists at configure time so the INSTALL(DIRECTORY) rule below is valid; the
	# POST_BUILD step (re)populates it from code.
	FILE(MAKE_DIRECTORY ${_cfg_dir})

	# The overlay helper must exist before this target's POST_BUILD step runs it.
	IF(TARGET GConfigOverlay)
		ADD_DEPENDENCIES(${_target} GConfigOverlay)
	ENDIF()

	# POST_BUILD: (re)materialize the config directory from the code defaults. Running the freshly built
	# binary with --update-configs creates any missing config from defaults and rewrites it canonically.
	# The run's output goes to config-materialization.log (replayed only on failure).
	SET(_mat_cmd "$<TARGET_FILE:${_target}>")
	FOREACH(_mat_arg IN LISTS _extra_args)
		STRING(APPEND _mat_cmd "|${_mat_arg}")
	ENDFOREACH()
	STRING(APPEND _mat_cmd "|--update-configs")
	ADD_CUSTOM_COMMAND(TARGET ${_target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E rm -rf ${_cfg_dir}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${_cfg_dir}
			COMMAND ${CMAKE_COMMAND}
				"-DRQ_CMD=${_mat_cmd}"
				"-DRQ_WD=${CMAKE_CURRENT_BINARY_DIR}"
				"-DRQ_LOG=${CMAKE_CURRENT_BINARY_DIR}/config-materialization.log"
				"-DRQ_DESC=Config materialization for ${_target}"
				-P ${GEM_RUN_QUIET_SCRIPT}
			WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
			COMMENT "Materializing ${_target} configuration from code defaults"
			VERBATIM)

	# Overlay this directory's intentional overrides, if it ships a fragment.
	IF(EXISTS ${_overrides})
		ADD_CUSTOM_COMMAND(TARGET ${_target} POST_BUILD
				COMMAND ${CMAKE_COMMAND}
					"-DRQ_CMD=$<TARGET_FILE:GConfigOverlay>|${_cfg_dir}|${_overrides}"
					"-DRQ_WD=${CMAKE_CURRENT_BINARY_DIR}"
					"-DRQ_LOG=${CMAKE_CURRENT_BINARY_DIR}/config-overlay.log"
					"-DRQ_DESC=Config-override overlay for ${_target}"
					-P ${GEM_RUN_QUIET_SCRIPT}
				COMMENT "Overlaying ${_target} configuration overrides"
				VERBATIM)
	ENDIF()

	# Install the materialized directory's contents into <install-config-dest>.
	IF(NOT "${_install_dest}" STREQUAL "")
		INSTALL(DIRECTORY ${_cfg_dir}/ DESTINATION ${_install_dest})
	ENDIF()
ENDFUNCTION()
