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
#     a staged install (DESTDIR) is usable in the same way;
#     every installed libgemfony-* library and module must then resolve all its
#     dependencies through its own run path (ldd, without LD_LIBRARY_PATH);
#  3. the quickstart (libs/geneva/quickstart) is configured standalone with
#     find_package(Geneva CONFIG) against the moved installation and GStarter is
#     built, which also runs it once (its configuration files are written by the
#     program itself at build time);
#  4. with the MPI consumer: the MPI module probe
#     (libs/geneva/tests/OutOfTree/MpiModuleProbe) is configured, built and run,
#     without LD_LIBRARY_PATH, so the run paths of the installed libraries and of
#     the module have to suffice;
#  5. FindGeneva.cmake, the module-mode fallback, is used against the moved
#     installation by a small project written here: it must define the same
#     targets the config package exports (with the MPI consumer also
#     Geneva::courtier-mpi and Geneva::geneva-mpi, and GENEVA_MPI); with the MPI
#     consumer the probe's source is built and run against those targets;
#  6. both the config package and FindGeneva.cmake say whether the installation has
#     the MPI consumer (GENEVA_MPI) and the GPU consumer (GENEVA_GPU), as the build
#     was configured.
#
# On success the directory is removed; on failure it is kept and named.
#
# Inputs (passed via -D):
#   BUILD_DIR        the Geneva build tree to install
#   SOURCE_DIR       the Geneva source tree (for the quickstart and the probe)
#   CXX_COMPILER     the C++ compiler of the build
#   BUILD_TYPE       the build type of the build
#   BOOST_DIR        Boost's CMake package directory, as the build found it
#   CATCH2_DIR       Catch2's CMake package directory (a testing-enabled install needs it)
#   WITH_MPI         ON if the build has the MPI consumer
#   WITH_GPU         TRUE if the build has the GPU consumer
#   JOBS             parallel build jobs for the probes
#
################################################################################

FOREACH(_var BUILD_DIR SOURCE_DIR CXX_COMPILER BUILD_TYPE BOOST_DIR JOBS)
	IF(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
		MESSAGE(FATAL_ERROR "CheckInstalledPackage.cmake needs -D${_var}=...")
	ENDIF()
ENDFOREACH()

# Runs a command, writes its output to <log>, and fails with the log's tail if it fails.
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
			"${_what} failed (rc=${_rc}); the directory ${ROOT} is kept.\n"
			"--- output ---\n${_out}\n--- errors ---\n${_err}")
	ENDIF()
	MESSAGE(STATUS "${_what}: done")
ENDFUNCTION()

EXECUTE_PROCESS(
	COMMAND mktemp -d "${BUILD_DIR}/installed-package-XXXXXX"
	OUTPUT_VARIABLE ROOT
	OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE _rc
)
IF(NOT _rc EQUAL 0 OR NOT IS_DIRECTORY "${ROOT}")
	MESSAGE(FATAL_ERROR "mktemp -d failed in ${BUILD_DIR}")
ENDIF()

# 1. Install.
gip_run("install into ${ROOT}/installed" "${ROOT}/install.log"
	"${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${ROOT}/installed")

# 2. Move.
FILE(RENAME "${ROOT}/installed" "${ROOT}/moved")
SET(_prefix "${ROOT}/moved")
SET(_common_args
	"-DCMAKE_PREFIX_PATH=${_prefix}"
	"-DBoost_DIR=${BOOST_DIR}"
	"-DCatch2_DIR=${CATCH2_DIR}"
	"-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
	"-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")

# The libraries and modules of the moved installation find what they need on their own.
FIND_PROGRAM(_ldd ldd)
IF(_ldd)
	FILE(GLOB _installed_libs "${_prefix}/lib/libgemfony-*.so*")
	SET(_n_libs 0)
	FOREACH(_lib IN LISTS _installed_libs)
		IF(IS_SYMLINK "${_lib}")
			CONTINUE()
		ENDIF()
		MATH(EXPR _n_libs "${_n_libs} + 1")
		EXECUTE_PROCESS(
			COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${_ldd}" "${_lib}"
			OUTPUT_VARIABLE _ldd_out
			ERROR_VARIABLE _ldd_out
		)
		IF(_ldd_out MATCHES "not found")
			MESSAGE(FATAL_ERROR
				"${_lib} does not find all its dependencies through its run path; the "
				"directory ${ROOT} is kept.\n${_ldd_out}")
		ENDIF()
	ENDFOREACH()
	IF(_n_libs EQUAL 0)
		MESSAGE(FATAL_ERROR
			"No libgemfony-* library in ${_prefix}/lib; the directory ${ROOT} is kept.")
	ENDIF()
	MESSAGE(STATUS "ldd: ${_n_libs} installed libraries and modules resolve on their own")
ENDIF()

# 3. The quickstart: the config package must be the one of the moved installation.
gip_run("configure the quickstart" "${ROOT}/quickstart-cmake.log"
	"${CMAKE_COMMAND}" -S "${SOURCE_DIR}/libs/geneva/quickstart" -B "${ROOT}/quickstart"
	${_common_args})
FILE(STRINGS "${ROOT}/quickstart/CMakeCache.txt" _geneva_dir REGEX "^Geneva_DIR:")
IF(NOT _geneva_dir STREQUAL "Geneva_DIR:PATH=${_prefix}/lib/cmake/Geneva")
	MESSAGE(FATAL_ERROR
		"The quickstart did not find the moved installation's config package (${_geneva_dir}); "
		"the directory ${ROOT} is kept.")
ENDIF()
gip_run("build and run the quickstart (GStarter)" "${ROOT}/quickstart-build.log"
	"${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH
	"${CMAKE_COMMAND}" --build "${ROOT}/quickstart" --target GStarter -j ${JOBS})

# 4. The MPI module probe.
IF(WITH_MPI)
	gip_run("configure the MPI module probe" "${ROOT}/mpi-probe-cmake.log"
		"${CMAKE_COMMAND}" -S "${SOURCE_DIR}/libs/geneva/tests/OutOfTree/MpiModuleProbe"
		-B "${ROOT}/mpi-probe" ${_common_args})
	gip_run("build the MPI module probe" "${ROOT}/mpi-probe-build.log"
		"${CMAKE_COMMAND}" --build "${ROOT}/mpi-probe" -j ${JOBS})
	gip_run("run the MPI module probe without LD_LIBRARY_PATH" "${ROOT}/mpi-probe-run.log"
		"${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${ROOT}/mpi-probe/GenevaMpiModuleProbe")
ENDIF()

# 5. FindGeneva.cmake against the same installation.
SET(_fg_src "${ROOT}/findgeneva-src")
FILE(MAKE_DIRECTORY "${_fg_src}")
SET(_probe_cpp "${SOURCE_DIR}/libs/geneva/tests/OutOfTree/MpiModuleProbe/GenevaMpiModuleProbe.cpp")
STRING(CONCAT _fg_lists
	"cmake_minimum_required(VERSION 3.27)\n"
	"project(FindGenevaProbe CXX)\n"
	"list(APPEND CMAKE_MODULE_PATH \"${SOURCE_DIR}/CMakeModules\")\n"
	"set(GENEVA_ROOT \"${_prefix}\")\n"
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
	"${CMAKE_COMMAND}" -S "${_fg_src}" -B "${ROOT}/findgeneva" ${_common_args})
IF(WITH_MPI)
	gip_run("build the MPI module probe with FindGeneva's targets" "${ROOT}/findgeneva-build.log"
		"${CMAKE_COMMAND}" --build "${ROOT}/findgeneva" -j ${JOBS})
	gip_run("run it without LD_LIBRARY_PATH" "${ROOT}/findgeneva-run.log"
		"${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${ROOT}/findgeneva/GenevaMpiModuleProbe")
ENDIF()

# 6. What the installation has: GENEVA_MPI and GENEVA_GPU, from the config package and from
#    FindGeneva.cmake, must say what the build was configured with.
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
FOREACH(_mode CONFIG MODULE)
	SET(_flags_src "${ROOT}/flags-${_mode}-src")
	FILE(MAKE_DIRECTORY "${_flags_src}")
	STRING(CONCAT _flags_lists
		"cmake_minimum_required(VERSION 3.27)\n"
		"project(GenevaFlagsProbe CXX)\n"
		"list(APPEND CMAKE_MODULE_PATH \"${SOURCE_DIR}/CMakeModules\")\n"
		"set(GENEVA_ROOT \"${_prefix}\")\n"
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
		"endif()\n")
	FILE(WRITE "${_flags_src}/CMakeLists.txt" "${_flags_lists}")
	gip_run("say MPI and GPU through find_package(Geneva ${_mode})" "${ROOT}/flags-${_mode}.log"
		"${CMAKE_COMMAND}" -S "${_flags_src}" -B "${ROOT}/flags-${_mode}" ${_common_args})
ENDFOREACH()

FILE(REMOVE_RECURSE "${ROOT}")
MESSAGE(STATUS "The moved installation serves programs outside the tree")
