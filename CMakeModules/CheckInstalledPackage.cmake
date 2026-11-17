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
# Driver for the test GenevaInstalledPackageProbes: the build is installed, the
# installation is moved, and programs outside the tree are built against the
# moved installation and run, as a user's own project would.
#
#  1. "cmake --install" of the build into a fresh directory (mktemp -d) as prefix;
#  2. that installation is renamed: nothing in it may still name the prefix it
#     was installed to (the exported targets, the config package, the run paths);
#     a staged install (DESTDIR) is usable in the same way; of the collection's
#     CMake modules it carries FindGeneva.cmake only (not CommonGenevaBuild.cmake,
#     which builds the collection itself);
#     every installed library, module and program must then resolve all its
#     dependencies through its own run path (ldd, without LD_LIBRARY_PATH);
#  3. the quickstart (libs/geneva/quickstart) is configured standalone with
#     find_package(Geneva CONFIG) against the moved installation and GStarter is
#     built, which also runs it once in the build tree (its configuration files are
#     written by the program itself at build time); the quickstart is then
#     installed, configured with CMAKE_INSTALL_RPATH_USE_LINK_PATH, and the
#     installed GStarter is run. A program linked by hand with -L, -l and
#     -Wl,-rpath is run as well. INSTALL ("Finding the libraries at run time on
#     Linux") states what these runs show; all of them run without LD_LIBRARY_PATH;
#  4. with the MPI consumer: the MPI module probe
#     (libs/geneva/tests/OutOfTree/MpiModuleProbe) is configured, built and run,
#     without LD_LIBRARY_PATH, so the run paths of the installed libraries and of
#     the module have to suffice;
#  5. FindGeneva.cmake, the module-mode fallback, is used against the moved
#     installation (the module the installation carries) by a small project
#     written here: it must define the same targets the config package exports
#     (with the MPI consumer also Geneva::courtier-mpi and Geneva::geneva-mpi,
#     and GENEVA_MPI); with the MPI consumer the probe's source is built and run
#     against those targets. The module and the config package must ask for the
#     same Boost version and components;
#  6. both the config package and FindGeneva.cmake say whether the installation has
#     the MPI consumer (GENEVA_MPI) and the GPU consumer (GENEVA_GPU), as the build
#     was configured, both give Geneva::hap the compile definition the libraries
#     were compiled with (the source of Gem::Hap::GRandom), and both keep the
#     layers: Geneva::courtier does not link Geneva::hap, Geneva::geneva does.
# Configuring a project against the installation (steps 3, 5, 6) prints no CMake
# warning.
#
# The ldd check and steps 3 to 6 do not depend on each other. Each runs as a "lane": this script
# started once more with -DLANE=<name>, all lanes at the same time, each writing
# its own logs. The test then takes as long as its longest lane (the quickstart),
# not as the sum of them. The probes are built with Ninja where it is found
# (an object library and the program that uses it are compiled at the same time;
# make compiles them one after the other) and with the build's compiler launcher
# (e.g. ccache), if it has one.
#
# On success the directory is removed; on failure it is kept and named, with the
# output of every lane that failed.
#
# Inputs (passed via -D):
#   BUILD_DIR        the Geneva build tree to install
#   SOURCE_DIR       the Geneva source tree (for the quickstart and the probe)
#   CXX_COMPILER     the C++ compiler of the build
#   CXX_LAUNCHER     the compiler launcher of the build (may be empty)
#   BUILD_TYPE       the build type of the build
#   BOOST_DIR        Boost's CMake package directory, as the build found it
#   CATCH2_DIR       Catch2's CMake package directory (a testing-enabled install needs it)
#   WITH_MPI         ON if the build has the MPI consumer
#   WITH_GPU         TRUE if the build has the GPU consumer
#   HAP_DEFINE       hap's public compile definition (empty for the default source)
#   JOBS             parallel build jobs for each probe
# and, set by this script for a lane only: LANE, ROOT, GENERATOR.
#
################################################################################

FOREACH(_var BUILD_DIR SOURCE_DIR CXX_COMPILER BUILD_TYPE BOOST_DIR JOBS)
	IF(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
		MESSAGE(FATAL_ERROR "CheckInstalledPackage.cmake needs -D${_var}=...")
	ENDIF()
ENDFOREACH()

# Runs a command, writes its output to <log>, and fails with the log if it fails.
FUNCTION(gip_run _what _log)
	EXECUTE_PROCESS(
		COMMAND ${ARGN}
		OUTPUT_FILE "${_log}"
		ERROR_FILE "${_log}.err"
		RESULT_VARIABLE _rc
	)
	IF(NOT _rc EQUAL 0)
		FILE(READ "${_log}" _out)
		FILE(READ "${_log}.err" _err)
		MESSAGE(FATAL_ERROR
			"${_what} failed (rc=${_rc}).\n"
			"--- output ---\n${_out}\n--- errors ---\n${_err}")
	ENDIF()
	MESSAGE(STATUS "${_what}: done")
ENDFUNCTION()

# Fails if configuring a probe printed a CMake warning (written to <log>.err by gip_run()):
# finding an installed Geneva must not warn in the project that finds it.
FUNCTION(gip_no_warnings _what _log)
	FILE(READ "${_log}.err" _err)
	IF(_err MATCHES "CMake Warning")
		MESSAGE(FATAL_ERROR "${_what} printed a CMake warning:\n${_err}")
	ENDIF()
ENDFUNCTION()

################################################################################
# The lanes (steps 3 to 6). Each is run by a process of its own; see the end of the file.

# What every probe project is configured with.
FUNCTION(gip_common_args _out)
	SET(_args
		"-DCMAKE_PREFIX_PATH=${ROOT}/moved"
		"-DBoost_DIR=${BOOST_DIR}"
		"-DCatch2_DIR=${CATCH2_DIR}"
		"-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
		"-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
	IF(NOT "${CXX_LAUNCHER}" STREQUAL "")
		LIST(APPEND _args "-DCMAKE_CXX_COMPILER_LAUNCHER=${CXX_LAUNCHER}")
	ENDIF()
	IF(NOT "${GENERATOR}" STREQUAL "")
		LIST(APPEND _args -G "${GENERATOR}")
	ENDIF()
	SET(${_out} "${_args}" PARENT_SCOPE)
ENDFUNCTION()

# 2. Every library, module and program of the moved installation finds what it needs on its own.
FUNCTION(gip_lane_ldd)
	FIND_PROGRAM(_ldd ldd)
	IF(NOT _ldd)
		MESSAGE(STATUS "ldd: not found, the run paths are checked by the probes' runs only")
		RETURN()
	ENDIF()
	FILE(GLOB_RECURSE _files LIST_DIRECTORIES FALSE "${ROOT}/moved/*")
	SET(_n_elf 0)
	SET(_n_libs 0)
	SET(_unresolved "")
	FOREACH(_file IN LISTS _files)
		IF(IS_SYMLINK "${_file}")
			CONTINUE()
		ENDIF()
		FILE(READ "${_file}" _magic LIMIT 4 HEX)
		IF(NOT _magic STREQUAL "7f454c46")
			CONTINUE()
		ENDIF()
		MATH(EXPR _n_elf "${_n_elf} + 1")
		IF(_file MATCHES "/lib/libgemfony-[^/]*\\.so[^/]*$")
			MATH(EXPR _n_libs "${_n_libs} + 1")
		ENDIF()
		EXECUTE_PROCESS(
			COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${_ldd}" "${_file}"
			OUTPUT_VARIABLE _ldd_out
			ERROR_VARIABLE _ldd_out
		)
		IF(_ldd_out MATCHES "not found")
			STRING(REGEX MATCHALL "[^\n]*not found" _missing "${_ldd_out}")
			STRING(REPLACE "${ROOT}/moved/" "" _rel "${_file}")
			STRING(APPEND _unresolved "${_rel}: ${_missing}\n")
		ENDIF()
	ENDFOREACH()
	IF(_n_libs EQUAL 0)
		MESSAGE(FATAL_ERROR "No libgemfony-* library in ${ROOT}/moved/lib")
	ENDIF()
	IF(NOT _unresolved STREQUAL "")
		MESSAGE(FATAL_ERROR
			"Installed files that do not find all their dependencies through their run paths:\n"
			"${_unresolved}")
	ENDIF()
	MESSAGE(STATUS
		"ldd: ${_n_elf} installed libraries, modules and programs resolve on their own")
ENDFUNCTION()

# 3. The quickstart: the config package must be the one of the moved installation.
FUNCTION(gip_lane_quickstart)
	gip_common_args(_args)
	gip_run("configure the quickstart" "${ROOT}/quickstart-cmake.log"
		"${CMAKE_COMMAND}" -S "${SOURCE_DIR}/libs/geneva/quickstart" -B "${ROOT}/quickstart"
		${_args} "-DCMAKE_INSTALL_PREFIX=${ROOT}/quickstart-installed"
		-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON)
	gip_no_warnings("configure the quickstart" "${ROOT}/quickstart-cmake.log")
	FILE(STRINGS "${ROOT}/quickstart/CMakeCache.txt" _geneva_dir REGEX "^Geneva_DIR:")
	IF(NOT _geneva_dir STREQUAL "Geneva_DIR:PATH=${ROOT}/moved/lib/cmake/Geneva")
		MESSAGE(FATAL_ERROR
			"The quickstart did not find the moved installation's config package (${_geneva_dir})")
	ENDIF()
	# What "make install" installs is built; running GStarter in the build tree is part of it.
	gip_run("build the quickstart and run GStarter in its build tree" "${ROOT}/quickstart-build.log"
		"${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH
		"${CMAKE_COMMAND}" --build "${ROOT}/quickstart" -j ${JOBS}
		--target GStarter GStarterGenericOptimizer GStarterIndividual)
	# The user's own install keeps the run path with CMAKE_INSTALL_RPATH_USE_LINK_PATH (INSTALL).
	gip_run("install the quickstart" "${ROOT}/quickstart-install.log"
		"${CMAKE_COMMAND}" --install "${ROOT}/quickstart")
	gip_run("run the installed GStarter without LD_LIBRARY_PATH" "${ROOT}/quickstart-run.log"
		"${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH
		"${ROOT}/quickstart-installed/bin/GStarter" --help)
ENDFUNCTION()

# 3b. A program linked by hand with -L and -l finds Geneva's libraries through the run path it is
#     given, and they find what they need themselves (INSTALL).
FUNCTION(gip_lane_handlinked)
	FILE(MAKE_DIRECTORY "${ROOT}/handlinked")
	FILE(WRITE "${ROOT}/handlinked/main.cpp"
		"namespace Gem::Common { unsigned int getNHardwareThreads(); }\n"
		"int main() { return Gem::Common::getNHardwareThreads() > 0 ? 0 : 1; }\n")
	gip_run("link a program by hand with a run path" "${ROOT}/handlinked-build.log"
		"${CXX_COMPILER}" "${ROOT}/handlinked/main.cpp" -o "${ROOT}/handlinked/main"
		"-L${ROOT}/moved/lib" -lgemfony-common "-Wl,-rpath,${ROOT}/moved/lib")
	gip_run("run it without LD_LIBRARY_PATH" "${ROOT}/handlinked-run.log"
		"${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${ROOT}/handlinked/main")
ENDFUNCTION()

# 4. The MPI module probe.
FUNCTION(gip_lane_mpi_probe)
	gip_common_args(_args)
	gip_run("configure the MPI module probe" "${ROOT}/mpi-probe-cmake.log"
		"${CMAKE_COMMAND}" -S "${SOURCE_DIR}/libs/geneva/tests/OutOfTree/MpiModuleProbe"
		-B "${ROOT}/mpi-probe" ${_args})
	gip_run("build the MPI module probe" "${ROOT}/mpi-probe-build.log"
		"${CMAKE_COMMAND}" --build "${ROOT}/mpi-probe" -j ${JOBS})
	gip_run("run the MPI module probe without LD_LIBRARY_PATH" "${ROOT}/mpi-probe-run.log"
		"${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${ROOT}/mpi-probe/GenevaMpiModuleProbe")
ENDFUNCTION()

# The Boost version and components a file of the moved installation asks find_dependency() for,
# as a sorted list in _out ("<version>;<component>;...").
FUNCTION(gip_boost_request _file _out)
	FILE(READ "${_file}" _text)
	STRING(REGEX MATCH "[Ff][Ii][Nn][Dd]_[Dd][Ee][Pp][Ee][Nn][Dd][Ee][Nn][Cc][Yy] *\\( *Boost ([^)]*)\\)"
		_call "${_text}")
	IF(NOT _call)
		MESSAGE(FATAL_ERROR "${_file} asks for no Boost")
	ENDIF()
	STRING(REGEX REPLACE "[ \t\n]+" ";" _words "${CMAKE_MATCH_1}")
	LIST(REMOVE_ITEM _words "" CONFIG REQUIRED COMPONENTS)
	LIST(POP_FRONT _words _version)
	LIST(SORT _words)
	SET(${_out} "${_version};${_words}" PARENT_SCOPE)
ENDFUNCTION()

# 5. FindGeneva.cmake against the same installation.
FUNCTION(gip_lane_findgeneva)
	gip_common_args(_args)
	gip_boost_request("${ROOT}/moved/CMakeModules/FindGeneva.cmake" _module_boost)
	gip_boost_request("${ROOT}/moved/lib/cmake/Geneva/GenevaConfig.cmake" _package_boost)
	IF(NOT _module_boost STREQUAL _package_boost)
		MESSAGE(FATAL_ERROR "FindGeneva.cmake asks for Boost ${_module_boost}, the config "
			"package for Boost ${_package_boost}")
	ENDIF()
	SET(_fg_src "${ROOT}/findgeneva-src")
	FILE(MAKE_DIRECTORY "${_fg_src}")
	SET(_probe_cpp "${SOURCE_DIR}/libs/geneva/tests/OutOfTree/MpiModuleProbe/GenevaMpiModuleProbe.cpp")
	STRING(CONCAT _fg_lists
		"cmake_minimum_required(VERSION 3.27)\n"
		"project(FindGenevaProbe CXX)\n"
		"list(APPEND CMAKE_MODULE_PATH \"${ROOT}/moved/CMakeModules\")\n"
		"set(GENEVA_ROOT \"${ROOT}/moved\")\n"
		"find_package(Geneva MODULE REQUIRED)\n"
		"foreach(_t weft common dietrich hap courtier geneva)\n"
		"  if(NOT TARGET Geneva::\${_t})\n"
		"    message(FATAL_ERROR \"FindGeneva defines no Geneva::\${_t}\")\n"
		"  endif()\n"
		"endforeach()\n")
	IF(WITH_MPI)
		STRING(APPEND _fg_lists
			"if(NOT GENEVA_MPI OR NOT TARGET Geneva::courtier-mpi OR NOT TARGET Geneva::geneva-mpi)\n"
			"  message(FATAL_ERROR \"FindGeneva does not know the MPI libraries\")\n"
			"endif()\n"
			"add_executable(GenevaMpiModuleProbe \"${_probe_cpp}\")\n"
			"target_link_libraries(GenevaMpiModuleProbe\n"
			"  PRIVATE Geneva::geneva-mpi Geneva::courtier-mpi Geneva::geneva)\n")
	ELSE()
		STRING(APPEND _fg_lists
			"if(GENEVA_MPI)\n"
			"  message(FATAL_ERROR \"FindGeneva reports MPI for an installation without it\")\n"
			"endif()\n")
	ENDIF()
	FILE(WRITE "${_fg_src}/CMakeLists.txt" "${_fg_lists}")
	gip_run("configure a project with FindGeneva.cmake" "${ROOT}/findgeneva-cmake.log"
		"${CMAKE_COMMAND}" -S "${_fg_src}" -B "${ROOT}/findgeneva" ${_args})
	gip_no_warnings("configure a project with FindGeneva.cmake" "${ROOT}/findgeneva-cmake.log")
	IF(WITH_MPI)
		gip_run("build the MPI module probe with FindGeneva's targets" "${ROOT}/findgeneva-build.log"
			"${CMAKE_COMMAND}" --build "${ROOT}/findgeneva" -j ${JOBS})
		gip_run("run it without LD_LIBRARY_PATH" "${ROOT}/findgeneva-run.log"
			"${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${ROOT}/findgeneva/GenevaMpiModuleProbe")
	ENDIF()
ENDFUNCTION()

# 6. What the installation has: GENEVA_MPI and GENEVA_GPU, from the config package (_mode
#    CONFIG) or from FindGeneva.cmake (MODULE), must say what the build was configured with; and
#    Geneva::courtier must not link Geneva::hap, while Geneva::geneva must (a substring check: the
#    exported list may hold generator expressions).
FUNCTION(gip_lane_flags _mode)
	gip_common_args(_args)
	IF(WITH_MPI)
		SET(_want_mpi TRUE)
	ELSE()
		SET(_want_mpi FALSE)
	ENDIF()
	IF(WITH_GPU)
		SET(_want_gpu TRUE)
	ELSE()
		SET(_want_gpu FALSE)
	ENDIF()
	SET(_flags_src "${ROOT}/flags-${_mode}-src")
	FILE(MAKE_DIRECTORY "${_flags_src}")
	STRING(CONCAT _flags_lists
		"cmake_minimum_required(VERSION 3.27)\n"
		"project(GenevaFlagsProbe CXX)\n"
		"list(APPEND CMAKE_MODULE_PATH \"${ROOT}/moved/CMakeModules\")\n"
		"set(GENEVA_ROOT \"${ROOT}/moved\")\n"
		"find_package(Geneva ${_mode} REQUIRED)\n"
		"foreach(_f MPI GPU)\n"
		"  if(GENEVA_\${_f})\n"
		"    set(_have TRUE)\n"
		"  else()\n"
		"    set(_have FALSE)\n"
		"  endif()\n"
		"  message(STATUS \"GENEVA_\${_f}=\${_have}\")\n"
		"endforeach()\n"
		"if(GENEVA_MPI)\n  set(_mpi TRUE)\nelse()\n  set(_mpi FALSE)\nendif()\n"
		"if(GENEVA_GPU)\n  set(_gpu TRUE)\nelse()\n  set(_gpu FALSE)\nendif()\n"
		"if(NOT _mpi STREQUAL \"${_want_mpi}\" OR NOT _gpu STREQUAL \"${_want_gpu}\")\n"
		"  message(FATAL_ERROR \"${_mode}: GENEVA_MPI=\${_mpi} GENEVA_GPU=\${_gpu}, the build has "
		"MPI=${_want_mpi} GPU=${_want_gpu}\")\n"
		"endif()\n"
		"get_target_property(_hap_defines Geneva::hap INTERFACE_COMPILE_DEFINITIONS)\n"
		"if(NOT _hap_defines)\n  set(_hap_defines \"\")\nendif()\n"
		"if(NOT \"\${_hap_defines}\" STREQUAL \"${HAP_DEFINE}\")\n"
		"  message(SEND_ERROR \"${_mode}: Geneva::hap has the compile definitions "
		"'\${_hap_defines}', the libraries were compiled with '${HAP_DEFINE}'\")\n"
		"endif()\n"
		"get_target_property(_courtier_links Geneva::courtier INTERFACE_LINK_LIBRARIES)\n"
		"get_target_property(_geneva_links Geneva::geneva INTERFACE_LINK_LIBRARIES)\n"
		"if(\"\${_courtier_links}\" MATCHES \"Geneva::hap\")\n"
		"  message(SEND_ERROR \"${_mode}: Geneva::courtier links Geneva::hap "
		"(\${_courtier_links}); courtier builds on common only\")\n"
		"endif()\n"
		"if(NOT \"\${_geneva_links}\" MATCHES \"Geneva::hap\")\n"
		"  message(SEND_ERROR \"${_mode}: Geneva::geneva does not link Geneva::hap "
		"(\${_geneva_links})\")\n"
		"endif()\n")
	FILE(WRITE "${_flags_src}/CMakeLists.txt" "${_flags_lists}")
	gip_run("say MPI and GPU through find_package(Geneva ${_mode})" "${ROOT}/flags-${_mode}.log"
		"${CMAKE_COMMAND}" -S "${_flags_src}" -B "${ROOT}/flags-${_mode}" ${_args})
	gip_no_warnings("find_package(Geneva ${_mode})" "${ROOT}/flags-${_mode}.log")
ENDFUNCTION()

IF(DEFINED LANE)
	IF(LANE STREQUAL "ldd")
		gip_lane_ldd()
	ELSEIF(LANE STREQUAL "quickstart")
		gip_lane_quickstart()
	ELSEIF(LANE STREQUAL "handlinked")
		gip_lane_handlinked()
	ELSEIF(LANE STREQUAL "mpi-probe")
		gip_lane_mpi_probe()
	ELSEIF(LANE STREQUAL "findgeneva")
		gip_lane_findgeneva()
	ELSEIF(LANE MATCHES "^flags-(CONFIG|MODULE)$")
		gip_lane_flags(${CMAKE_MATCH_1})
	ELSE()
		MESSAGE(FATAL_ERROR "Unknown lane ${LANE}")
	ENDIF()
	RETURN()
ENDIF()

################################################################################
# Steps 1 and 2, then the lanes.

EXECUTE_PROCESS(
	COMMAND mktemp -d "${BUILD_DIR}/installed-package-XXXXXX"
	OUTPUT_VARIABLE ROOT
	OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE _rc
)
IF(NOT _rc EQUAL 0 OR NOT IS_DIRECTORY "${ROOT}")
	MESSAGE(FATAL_ERROR "mktemp -d failed in ${BUILD_DIR}")
ENDIF()

# Fails, keeping the directory, with the given text.
FUNCTION(gip_fail _text)
	MESSAGE(FATAL_ERROR "${_text}\nThe directory ${ROOT} is kept.")
ENDFUNCTION()

# 1. Install.
EXECUTE_PROCESS(
	COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${ROOT}/installed"
	OUTPUT_FILE "${ROOT}/install.log"
	ERROR_FILE "${ROOT}/install.log.err"
	RESULT_VARIABLE _rc
)
IF(NOT _rc EQUAL 0)
	FILE(READ "${ROOT}/install.log.err" _err)
	gip_fail("The install into ${ROOT}/installed failed (rc=${_rc}):\n${_err}")
ENDIF()

# 2. Move.
FILE(RENAME "${ROOT}/installed" "${ROOT}/moved")

# Of the collection's CMake modules the installation carries FindGeneva.cmake only. The modules that
# build the collection itself (CommonGenevaBuild.cmake and what it includes) are not installed: a
# project outside the tree finds Geneva with find_package(Geneva), as the lanes below do.
FILE(GLOB_RECURSE _build_modules "${ROOT}/moved/*CommonGenevaBuild.cmake")
FILE(GLOB _modules RELATIVE "${ROOT}/moved/CMakeModules" "${ROOT}/moved/CMakeModules/*")
IF(_build_modules OR NOT _modules STREQUAL "FindGeneva.cmake")
	gip_fail("The installation carries CMake modules beyond FindGeneva.cmake: ${_build_modules} "
		"CMakeModules/{${_modules}}")
ENDIF()

# The ldd check of step 2 and steps 3 to 6: the lanes, all at the same time.
SET(_lanes ldd quickstart handlinked findgeneva flags-CONFIG flags-MODULE)
IF(WITH_MPI)
	LIST(APPEND _lanes mpi-probe)
ENDIF()
FIND_PROGRAM(_ninja ninja)
IF(_ninja)
	SET(_generator "Ninja")
ELSE()
	SET(_generator "")
ENDIF()
SET(_lane_args "")
FOREACH(_var BUILD_DIR SOURCE_DIR CXX_COMPILER CXX_LAUNCHER BUILD_TYPE BOOST_DIR CATCH2_DIR
		WITH_MPI WITH_GPU HAP_DEFINE JOBS ROOT)
	STRING(APPEND _lane_args " '-D${_var}=${${_var}}'")
ENDFOREACH()
STRING(APPEND _lane_args " '-DGENERATOR=${_generator}' -P '${CMAKE_CURRENT_LIST_FILE}'")
SET(_sh "#!/bin/sh\n")
FOREACH(_lane IN LISTS _lanes)
	STRING(APPEND _sh
		"('${CMAKE_COMMAND}' '-DLANE=${_lane}'${_lane_args} > '${ROOT}/lane-${_lane}.out' 2>&1;"
		" echo $? > '${ROOT}/lane-${_lane}.rc') &\n")
ENDFOREACH()
STRING(APPEND _sh "wait\n")
FILE(WRITE "${ROOT}/lanes.sh" "${_sh}")
EXECUTE_PROCESS(COMMAND sh "${ROOT}/lanes.sh" RESULT_VARIABLE _rc)
IF(NOT _rc EQUAL 0)
	gip_fail("The lanes could not be run (rc=${_rc}).")
ENDIF()

SET(_failed "")
FOREACH(_lane IN LISTS _lanes)
	SET(_lane_rc "missing")
	IF(EXISTS "${ROOT}/lane-${_lane}.rc")
		FILE(STRINGS "${ROOT}/lane-${_lane}.rc" _lane_rc LIMIT_COUNT 1)
	ENDIF()
	FILE(READ "${ROOT}/lane-${_lane}.out" _lane_out)
	IF(_lane_rc STREQUAL "0")
		MESSAGE(STATUS "lane ${_lane}:\n${_lane_out}")
	ELSE()
		STRING(APPEND _failed "\n=== lane ${_lane} (rc=${_lane_rc}) ===\n${_lane_out}")
	ENDIF()
ENDFOREACH()
IF(NOT _failed STREQUAL "")
	gip_fail("A probe against the moved installation failed:${_failed}")
ENDIF()

FILE(REMOVE_RECURSE "${ROOT}")
MESSAGE(STATUS "The moved installation serves programs outside the tree")
