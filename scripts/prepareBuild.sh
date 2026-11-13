#!/bin/bash

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
# This script is a wrapper around the rather complicated cmake call,
# allowing you to permanently specify some options you'd otherwise
# have to provide repeatedly on the command line. Call the script
# with a file setting the variables below. See the script directory
# in the Geneva root directory for an example (genevaConfig.gcfg).
# Note that configuration files need to end with ".gcfg" .
####################################################################

####################################################################
# Make a note of the build-root-directory
GENEVA_BUILDROOT="${PWD}"

####################################################################
# Parse command line arguments
DRYRUN=0
CLEAN=0
BUILD=0
GENERATE_PRESET=0
YES=0
CONFIGFILE=""

for arg in "$@"; do
	case "$arg" in
		--help|-h)
			echo -e "\nUsage: $(basename "$0") [<config.gcfg>] [--clean] [--build] [--dryrun] [--generate-preset] [--help|-h]"
			echo -e "\nOptions:"
			echo -e "  <config.gcfg>       Optional Geneva configuration file (must end in .gcfg)."
			echo -e "                      If omitted, built-in defaults are used or a single .gcfg"
			echo -e "                      file in the current directory is used automatically."
			echo -e "  --clean             Remove all files from the build directory except .gcfg"
			echo -e "                      files. When used alone it exits after cleaning with a"
			echo -e "                      hint on how to reconfigure. May be combined with --build"
			echo -e "                      and/or a .gcfg file, in which case the directory is"
			echo -e "                      cleaned and then configured (and built if --build). Cannot"
			echo -e "                      be combined with --dryrun or --generate-preset. When"
			echo -e "                      --clean is omitted but the directory is already configured,"
			echo -e "                      the script prompts before reconfiguring."
			echo -e "  --build             Build the project after configuration using all available"
			echo -e "                      cores (make -j\$(nproc)). If the build directory is already"
			echo -e "                      configured and no .gcfg file or --clean is given, cmake is"
			echo -e "                      skipped and only the build step is run."
			echo -e "  --dryrun            Print the full cmake command that would be executed,"
			echo -e "                      including all -D options derived from the config file,"
			echo -e "                      without actually running cmake. Useful for copying the"
			echo -e "                      command into an IDE such as JetBrains CLion."
			echo -e "  --generate-preset   Write a CMakeUserPresets.json to the current directory"
			echo -e "                      encoding all settings from the .gcfg file, then exit."
			echo -e "                      Copy the file to the project root so that CLion (or"
			echo -e "                      VS Code) picks up the configuration automatically."
			echo -e "                      Does not alter the build directory or run cmake."
			echo -e "  -y, --yes           Automatically answer yes to the 'Proceed? [y/N]'"
			echo -e "                      prompt that --clean (and reconfigure-without-clean)"
			echo -e "                      would otherwise show. Intended for non-interactive use."
			echo -e "  --help, -h          Show this help message.\n"
			exit 0
			;;
		--clean)
			CLEAN=1
			;;
		--build)
			BUILD=1
			;;
		--dryrun)
			DRYRUN=1
			;;
		--generate-preset)
			GENERATE_PRESET=1
			;;
		-y|--yes)
			YES=1
			;;
		-*)
			echo -e "\nUnknown option: '$arg'. Use --help for usage information.\nLeaving...\n"
			exit 1
			;;
		*)
			if [ -n "${CONFIGFILE}" ]; then
				echo -e "\nError: multiple config files specified. Leaving...\n"
				exit 1
			fi
			CONFIGFILE="$arg"
			;;
	esac
done

# Geneva REQUIRES an out-of-source build. The build directory is the directory this script is invoked
# from (GENEVA_BUILDROOT = PWD). Refuse to proceed when that is the source tree itself -- a common
# footgun when the script is run from the checkout instead of a dedicated build directory. Left
# unguarded, --clean would delete the sources (find PWD ... -exec rm -rf) and a configure/build would
# litter the source tree with CMake artifacts. This check is independent of all other options.
_SRC_ROOT="$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)"
_BUILD_ROOT_CANON="$(cd "${GENEVA_BUILDROOT}" 2>/dev/null && pwd)"
if [ -n "${_SRC_ROOT}" ] && [ "${_BUILD_ROOT_CANON}" = "${_SRC_ROOT}" ]; then
	echo -e "\nError: refusing to use the Geneva source tree as the build directory:"
	echo -e "  ${_SRC_ROOT}"
	echo -e "Geneva requires an out-of-source build. Run this script from a dedicated build"
	echo -e "directory, for example:\n"
	echo -e "  mkdir -p ~/build && cd ~/build"
	echo -e "  $(cd "$(dirname "$0")" && pwd)/$(basename "$0") /path/to/myConfig.gcfg --clean -y --build\n"
	exit 1
fi

# --clean may not be combined with --dryrun or --generate-preset.
if [ "${CLEAN}" = "1" ]; then
	if [ "${DRYRUN}" = "1" ] || [ "${GENERATE_PRESET}" = "1" ]; then
		echo -e "\nError: --clean may not be combined with --dryrun or --generate-preset."
		echo -e "Usage: $(basename "$0") [<config.gcfg>] [--clean] [--build]\nLeaving...\n"
		exit 1
	fi
fi

####################################################################
# Helper: interactively confirm and wipe the build directory.
# Exits with code 1 if the user declines.
_confirm_and_clean() {
	if [ "${YES}" = "1" ]; then
		echo -en "\nCleaning build directory '${GENEVA_BUILDROOT}' (auto-confirmed via -y) ..."
	else
		echo -e "\nBuild directory '${GENEVA_BUILDROOT}' is already configured."
		echo -e "Warning: ALL files (except *.gcfg) will be permanently deleted."
		printf "Proceed? [y/N] "
		read -r _confirm
		case "${_confirm}" in
			[yY]|[yY][eE][sS]) ;;
			*)
				echo -e "Aborted. Leaving...\n"
				exit 1
				;;
		esac
		echo -en "Cleaning build directory '${GENEVA_BUILDROOT}' ..."
	fi
	find "${GENEVA_BUILDROOT}" -mindepth 1 -maxdepth 1 ! -name "*.gcfg" -exec rm -rf {} +
	echo -e " done\n"
}

####################################################################
# Handle --clean: clean the build directory.  When used alone (no
# --build, no config file) exit immediately with a reconfigure hint;
# otherwise fall through to the configure (and optionally build) step.
if [ "${CLEAN}" = "1" ]; then
	if [ -e "${GENEVA_BUILDROOT}/CMakeCache.txt" ]; then
		_confirm_and_clean
	else
		echo -e "\nBuild directory '${GENEVA_BUILDROOT}' does not appear to be configured."
		echo -e "Nothing to clean.\n"
	fi
	if [ "${BUILD}" = "0" ] && [ -z "${CONFIGFILE}" ]; then
		echo -e "To configure CMake, call $(basename "$0") again without arguments"
		echo -e "or with a suitable .gcfg file:\n"
		echo -e "  $(basename "$0")"
		echo -e "  $(basename "$0") /path/to/myConfig.gcfg\n"
		exit 0
	fi
fi

####################################################################
# Build-only shortcut: --build requested, directory already configured,
# no --clean and no explicit config file — skip cmake, go straight to make.
if [ "${BUILD}" = "1" ] && [ "${CLEAN}" = "0" ] && [ -z "${CONFIGFILE}" ] \
		&& [ -e "${GENEVA_BUILDROOT}/CMakeCache.txt" ]; then
	_NCORES=$(nproc 2>/dev/null || echo 1)
	echo -e "\nBuild directory already configured. Running: make -C ${GENEVA_BUILDROOT} -j${_NCORES}\n"
	echo -e "---------------------------------------------------------------------\n"
	make -C "${GENEVA_BUILDROOT}" -j"${_NCORES}"
	exit $?
fi

####################################################################
# Auto-detect a .gcfg file in the call directory if none was given
if [ -z "${CONFIGFILE}" ]; then
	gcfg_files=( "${GENEVA_BUILDROOT}"/*.gcfg )
	if [ -e "${gcfg_files[0]}" ]; then
		if [ "${#gcfg_files[@]}" -eq 1 ]; then
			CONFIGFILE="${gcfg_files[0]}"
			_AUTODETECTED=1
		else
			echo -e "\nError: multiple .gcfg files found in '${GENEVA_BUILDROOT}'."
			echo -e "Please specify the configuration file explicitly. Leaving...\n"
			exit 1
		fi
	fi
fi

####################################################################
# Set built-in defaults, then override with the config file if provided.
CMAKE="/usr/bin/cmake"
BUILDMODE="Debug"
BUILDTESTCODE="1"
BUILDEXAMPLES="1"
BUILDBENCHMARKS="1"
VERBOSEMAKEFILE="0"
INSTALLDIR="/opt/geneva"
MPIROOT=""
BUILDMPICONSUMER="0"
BUILDGPUCONSUMER="auto"
USECUDARNG="auto"
SKIPALLCUDA="0"
FORCECUDA="0"
GIMAGE_USE_FLOAT="1"
RANDOMSOURCE="queue"
WITHCOVERAGE="0"
SANITIZER="none"
CUDA_NVCC=""
CUDA_ROOT=""
COMPILER="clang"

if [ -n "${CONFIGFILE}" ]; then
	case "${CONFIGFILE}" in
		*.gcfg) ;;
		*)
			echo -e "\nFile '${CONFIGFILE}' does not seem to be a Geneva config file, as it"
			echo -e "does not end in '.gcfg' as expected. Leaving...\n"
			exit 1
			;;
	esac
	if [ ! -e "${CONFIGFILE}" ]; then
		echo -e "\nError: File '${CONFIGFILE}' does not seem to exist.\nLeaving...\n"
		exit 1
	fi
	if [ "${_AUTODETECTED}" = "1" ]; then
		echo -e "\nUsing auto-detected configuration file '${CONFIGFILE}'"
	else
		echo -e "\nUsing configuration file '${CONFIGFILE}'"
	fi
	# shellcheck source=/dev/null
	. "${CONFIGFILE}"
else
	echo -e "\nNo Geneva config file provided — using built-in defaults."
	echo -e "See the Geneva 'scripts' directory for an example (genevaConfig.gcfg).\n"
fi

####################################################################
# Validate variables

_check_bool() {
	if [ ! "${2}" = "0" ] && [ ! "${2}" = "1" ]; then
		echo -e "\nError: Variable ${1} must be 0 or 1. Got '${2}'\nLeaving...\n"
		exit 1
	fi
}

# The two CUDA opt-ins (USECUDARNG, BUILDGPUCONSUMER) are tri-state: auto|on|off.
# Legacy 0|1 values are still accepted (and forwarded as-is; CMake maps them).
_check_cuda_tristate() {
	case "${2}" in
		auto|on|off|0|1) ;;
		*) echo -e "\nError: Variable ${1} must be auto|on|off (or legacy 0|1). Got '${2}'\nLeaving...\n"; exit 1 ;;
	esac
}

if [ ! -x "${CMAKE}" ]; then
	echo -e "\nError: Could not find cmake executable '${CMAKE}'"
	echo -e "Please provide the correct path in the configuration file."
	echo -e "Leaving...\n"
	exit 1
fi

# Validate Boost location variables.  Three valid states:
#   (a) all empty  — let CMake find Boost automatically
#   (b) BOOSTROOT only — standard prefix installation
#   (c) BOOSTLIBS + BOOSTINCL together — split installation
# Everything else is an error.
if [ -z "${BOOSTROOT}" ] && [ -z "${BOOSTLIBS}" ] && [ -z "${BOOSTINCL}" ]; then
	echo "Variable BOOSTROOT wasn't set. Letting CMake search for a system Boost installation."
elif [ -n "${BOOSTROOT}" ] && [ -z "${BOOSTLIBS}" ] && [ -z "${BOOSTINCL}" ]; then
	: # BOOSTROOT only — valid
elif [ -z "${BOOSTROOT}" ] && [ -n "${BOOSTLIBS}" ] && [ -n "${BOOSTINCL}" ]; then
	if [ ! -e "${BOOSTINCL}/boost/version.hpp" ]; then
		echo -e "\nError: incomplete Boost installation."
		echo -e "Expected to find 'boost/version.hpp' in BOOSTINCL='${BOOSTINCL}'.\nLeaving...\n"
		exit 1
	fi
else
	echo -e "\nError: inconsistent Boost location variables."
	echo -e "Set either BOOSTROOT, or both BOOSTLIBS and BOOSTINCL, or none at all. Got:"
	echo -e "\tBOOSTROOT = ${BOOSTROOT}"
	echo -e "\tBOOSTLIBS = ${BOOSTLIBS}"
	echo -e "\tBOOSTINCL = ${BOOSTINCL}"
	echo -e "Leaving...\n"
	exit 1
fi

case "${BUILDMODE}" in
	Release|Debug|RelWithDebInfo|MinSizeRel|Sanitize) ;;
	*) echo -e "\nError: Invalid build mode '${BUILDMODE}'. Leaving...\n"; exit 1 ;;
esac

_check_bool BUILDTESTCODE    "${BUILDTESTCODE}"
_check_bool BUILDEXAMPLES    "${BUILDEXAMPLES}"
_check_bool BUILDBENCHMARKS  "${BUILDBENCHMARKS}"
_check_bool VERBOSEMAKEFILE  "${VERBOSEMAKEFILE}"
_check_bool BUILDMPICONSUMER "${BUILDMPICONSUMER}"
_check_cuda_tristate USECUDARNG       "${USECUDARNG}"
_check_cuda_tristate BUILDGPUCONSUMER "${BUILDGPUCONSUMER}"
_check_bool SKIPALLCUDA      "${SKIPALLCUDA}"
_check_bool FORCECUDA        "${FORCECUDA}"
_check_bool GIMAGE_USE_FLOAT "${GIMAGE_USE_FLOAT}"
_check_bool WITHCOVERAGE     "${WITHCOVERAGE}"

case "${RANDOMSOURCE}" in
	queue|local|staged|quarantine) ;;
	*) echo -e "\nError: RANDOMSOURCE must be one of queue|local|staged|quarantine (got '${RANDOMSOURCE}'). Leaving...\n"; exit 1 ;;
esac

# Sanitizer: validate and, when enabled, force CUDA + the MPI consumer OFF
# (nvcc cannot compile with -fsanitize; MPI internals flood ThreadSanitizer).
# The actual -fsanitize flags are applied centrally by CMake via the
# GENEVA_SANITIZER cache variable passed below.
case "${SANITIZER}" in
	none|thread|address|undefined) ;;
	*) echo -e "\nError: SANITIZER must be none|thread|address|undefined. Got '${SANITIZER}'. Leaving...\n"; exit 1 ;;
esac
if [ "${SANITIZER}" != "none" ]; then
	echo -e "\nSanitizer '${SANITIZER}' enabled — forcing CUDA, the MPI consumer and the GPU consumer OFF for this build."
	SKIPALLCUDA="1"
	USECUDARNG="off"
	BUILDMPICONSUMER="0"
	BUILDGPUCONSUMER="off"
	CUDA_NVCC=""
	CUDA_ROOT=""
fi

# Validate CUDA path: if set, the file must exist and be executable.
if [ -n "${CUDA_NVCC}" ] && [ ! -x "${CUDA_NVCC}" ]; then
	echo -e "\nWarning: CUDA_NVCC='${CUDA_NVCC}' is set but is not an executable — ignoring."
	CUDA_NVCC=""
	CUDA_ROOT=""
fi

# Resolve COMPILER to actual executable paths.
_C_COMPILER=""
_CXX_COMPILER=""
case "${COMPILER}" in
	clang)
		_C_COMPILER="$(command -v clang 2>/dev/null)"
		_CXX_COMPILER="$(command -v clang++ 2>/dev/null)"
		if [ -z "${_C_COMPILER}" ] || [ -z "${_CXX_COMPILER}" ]; then
			echo -e "\nError: COMPILER=clang but clang/clang++ not found in PATH. Leaving...\n"
			exit 1
		fi
		;;
	gcc)
		_C_COMPILER="$(command -v gcc 2>/dev/null)"
		_CXX_COMPILER="$(command -v g++ 2>/dev/null)"
		if [ -z "${_C_COMPILER}" ] || [ -z "${_CXX_COMPILER}" ]; then
			echo -e "\nError: COMPILER=gcc but gcc/g++ not found in PATH. Leaving...\n"
			exit 1
		fi
		;;
	""|auto)
		: # Let CMake choose the system default compiler
		;;
	*)
		echo -e "\nError: COMPILER must be 'clang', 'gcc', or '' (auto). Got '${COMPILER}'. Leaving...\n"
		exit 1
		;;
esac

####################################################################
# Find the project root (CMakeLists.txt must be one level above the
# scripts/ directory in which this script lives).
PROJECTROOT=$(dirname "$0")/..
if [ ! -e "${PROJECTROOT}/CMakeLists.txt" ]; then
	echo -e "Error: could not find CMakeLists.txt relative to this script's location."
	echo -e "Expected it at '${PROJECTROOT}/CMakeLists.txt'. Leaving...\n"
	exit 1
fi

####################################################################
# Optionally generate CMakeUserPresets.json and exit immediately.
# The file is written to the call-site (build directory), not to the
# project root, so it cannot accidentally overwrite an existing preset.
# The user is shown explicit copy instructions.

_preset_add() {
	[ -n "${_PRESET_VARS}" ] && _PRESET_VARS="${_PRESET_VARS},"
	_PRESET_VARS="${_PRESET_VARS}
        \"${1}\": { \"type\": \"${2}\", \"value\": \"${3}\" }"
}

if [ "${GENERATE_PRESET}" = "1" ]; then
	_PRESET_VARS=""
	_preset_add "CMAKE_INSTALL_PREFIX"            "PATH"   "${INSTALLDIR}"
	_preset_add "CMAKE_VERBOSE_MAKEFILE"          "BOOL"   "${VERBOSEMAKEFILE}"
	_preset_add "GENEVA_BUILD_TYPE"               "STRING" "${BUILDMODE}"
	_preset_add "GENEVA_BUILD_TESTS"              "BOOL"   "${BUILDTESTCODE}"
	_preset_add "GENEVA_BUILD_EXAMPLES"           "BOOL"   "${BUILDEXAMPLES}"
	_preset_add "GENEVA_BUILD_BENCHMARKS"         "BOOL"   "${BUILDBENCHMARKS}"
	_preset_add "GENEVA_BUILD_WITH_MPI_CONSUMER"  "BOOL"   "${BUILDMPICONSUMER}"
	_preset_add "GENEVA_BUILD_WITH_GPU_CONSUMER"  "STRING" "${BUILDGPUCONSUMER}"
	_preset_add "GENEVA_USE_CUDA_RNG"             "STRING" "${USECUDARNG}"

	if [ -n "${BOOSTROOT}" ]; then
		_preset_add "BOOST_ROOT"       "PATH" "${BOOSTROOT}"
	elif [ -n "${BOOSTLIBS}" ]; then
		_preset_add "BOOST_LIBRARYDIR" "PATH" "${BOOSTLIBS}"
		_preset_add "BOOST_INCLUDEDIR" "PATH" "${BOOSTINCL}"
	fi

	[ -n "${MPIROOT}" ]          && _preset_add "MPI_HOME"               "PATH"   "${MPIROOT}"
	_preset_add "GENEVA_SKIP_CUDA"              "BOOL"   "${SKIPALLCUDA}"
	_preset_add "GENEVA_FORCE_CUDA"             "BOOL"   "${FORCECUDA}"
	_preset_add "GIMAGE_USE_FLOAT"             "BOOL"   "${GIMAGE_USE_FLOAT}"
	_preset_add "HAP_RANDOM_SOURCE"            "STRING" "${RANDOMSOURCE}"
	_preset_add "GENEVA_BUILD_WITH_COVERAGE"    "BOOL"   "${WITHCOVERAGE}"
	_preset_add "GENEVA_SANITIZER"              "STRING" "${SANITIZER}"
	[ -n "${CUDA_NVCC}" ] && [ "${SKIPALLCUDA}" = "0" ] && _preset_add "CMAKE_CUDA_COMPILER" "FILEPATH" "${CUDA_NVCC}"
	[ -n "${CUDA_ROOT}" ] && [ "${SKIPALLCUDA}" = "0" ] && _preset_add "CUDAToolkit_ROOT"   "PATH"     "${CUDA_ROOT}"
	[ -n "${_C_COMPILER}" ]      && _preset_add "CMAKE_C_COMPILER"        "FILEPATH" "${_C_COMPILER}"
	[ -n "${_CXX_COMPILER}" ]    && _preset_add "CMAKE_CXX_COMPILER"      "FILEPATH" "${_CXX_COMPILER}"
	[ -n "${CXXEXTRAFLAGS}" ]    && _preset_add "CMAKE_CXX_FLAGS"         "STRING"   "${CXXEXTRAFLAGS}"
	[ -n "${LINKEREXTRAFLAGS}" ] && _preset_add "CMAKE_EXE_LINKER_FLAGS"  "STRING"   "${LINKEREXTRAFLAGS}"

	PRESET_FILE="${GENEVA_BUILDROOT}/CMakeUserPresets.json"
	cat > "${PRESET_FILE}" <<ENDOFPRESET
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 27, "patch": 0 },
  "configurePresets": [
    {
      "name": "geneva-from-script",
      "displayName": "Geneva (prepareBuild config)",
      "description": "Auto-generated by prepareBuild.sh -- do not edit manually",
      "generator": "Unix Makefiles",
      "binaryDir": "${GENEVA_BUILDROOT}",
      "cacheVariables": {${_PRESET_VARS}
      }
    }
  ]
}
ENDOFPRESET

	if [ -n "${CMAKEEXTRAFLAGS}" ]; then
		echo -e "Note: CMAKEEXTRAFLAGS='${CMAKEEXTRAFLAGS}' was not written to ${PRESET_FILE}."
		echo -e "      Add these flags manually to the preset's 'cacheVariables' if needed.\n"
	fi
	echo -e "Written '${PRESET_FILE}'."
	echo -e "\nTo use this preset in CLion or VS Code, copy it to the project root:"
	echo -e "  cp \"${PRESET_FILE}\" \"$(cd "${PROJECTROOT}" && pwd)/CMakeUserPresets.json\""
	echo -e "\nCMake was NOT run. To configure the build directory run prepareBuild.sh"
	echo -e "again without --generate-preset (optionally with a .gcfg file).\n"
	exit 0
fi

####################################################################
# Guard against reconfiguring an already-configured build directory.
# (--clean and --generate-preset are handled earlier and exit before
# reaching this point.)
if [ -e "${GENEVA_BUILDROOT}/CMakeCache.txt" ] && [ "${DRYRUN}" = "0" ]; then
	_confirm_and_clean
fi

####################################################################
# Build the cmake argument list.  Using an array avoids word-splitting
# problems with paths or flags that contain spaces.
cmake_args=()
[ -n "${BOOSTROOT}" ] && cmake_args+=("-DBOOST_ROOT=${BOOSTROOT}")
[ -n "${BOOSTLIBS}" ] && cmake_args+=("-DBOOST_LIBRARYDIR=${BOOSTLIBS}" "-DBOOST_INCLUDEDIR=${BOOSTINCL}")
cmake_args+=(
	"-DGENEVA_BUILD_TYPE=${BUILDMODE}"
	"-DGENEVA_BUILD_TESTS=${BUILDTESTCODE}"
	"-DGENEVA_BUILD_EXAMPLES=${BUILDEXAMPLES}"
	"-DGENEVA_BUILD_BENCHMARKS=${BUILDBENCHMARKS}"
	"-DCMAKE_VERBOSE_MAKEFILE=${VERBOSEMAKEFILE}"
	"-DCMAKE_INSTALL_PREFIX=${INSTALLDIR}"
	"-DGENEVA_BUILD_WITH_MPI_CONSUMER=${BUILDMPICONSUMER}"
	"-DGENEVA_BUILD_WITH_GPU_CONSUMER=${BUILDGPUCONSUMER}"
	"-DGENEVA_USE_CUDA_RNG=${USECUDARNG}"
	"-DGENEVA_SKIP_CUDA=${SKIPALLCUDA}"
	"-DGENEVA_FORCE_CUDA=${FORCECUDA}"
	"-DGIMAGE_USE_FLOAT=${GIMAGE_USE_FLOAT}"
	"-DHAP_RANDOM_SOURCE=${RANDOMSOURCE}"
	"-DGENEVA_BUILD_WITH_COVERAGE=${WITHCOVERAGE}"
	"-DGENEVA_SANITIZER=${SANITIZER}"
)
[ -n "${MPIROOT}" ]          && cmake_args+=("-DMPI_HOME=${MPIROOT}")
if [ -n "${CUDA_NVCC}" ] && [ "${SKIPALLCUDA}" = "0" ]; then
	cmake_args+=("-DCMAKE_CUDA_COMPILER=${CUDA_NVCC}")
	if [ -n "${CUDA_ROOT}" ] && [ -d "${CUDA_ROOT}" ]; then
		cmake_args+=("-DCUDAToolkit_ROOT=${CUDA_ROOT}")
	fi
fi
[ -n "${_C_COMPILER}" ]      && cmake_args+=("-DCMAKE_C_COMPILER=${_C_COMPILER}")
[ -n "${_CXX_COMPILER}" ]    && cmake_args+=("-DCMAKE_CXX_COMPILER=${_CXX_COMPILER}")
[ -n "${CXXEXTRAFLAGS}" ]    && cmake_args+=("-DCMAKE_CXX_FLAGS=${CXXEXTRAFLAGS}")
[ -n "${LINKEREXTRAFLAGS}" ] && cmake_args+=("-DCMAKE_EXE_LINKER_FLAGS=${LINKEREXTRAFLAGS}")
if [ -n "${CMAKEEXTRAFLAGS}" ]; then
	# CMAKEEXTRAFLAGS holds multiple tokens — intentional word splitting
	read -ra _extra_flags <<< "${CMAKEEXTRAFLAGS}"
	cmake_args+=("${_extra_flags[@]}")
fi

####################################################################
echo -e "\nConfiguring with command: \"${CMAKE} -S ${PROJECTROOT} -B ${GENEVA_BUILDROOT} ${cmake_args[*]}\"\n"
echo -e "---------------------------------------------------------------------\n"

if [ "${DRYRUN}" = "1" ]; then
	echo -e "Dry run: cmake was NOT executed. Copy the command above into your"
	echo -e "IDE (e.g. JetBrains CLion) as the CMake options / command line.\n"
	if [ "${BUILD}" = "1" ]; then
		_NCORES=$(nproc 2>/dev/null || echo 1)
		echo -e "Build step that would follow: make -j${_NCORES}\n"
	fi
else
	if "${CMAKE}" -S "${PROJECTROOT}" -B "${GENEVA_BUILDROOT}" "${cmake_args[@]}"; then
		echo -e "\n---------------------------------------------------------------------"
		if [ "${BUILD}" = "1" ]; then
			_NCORES=$(nproc 2>/dev/null || echo 1)
			echo -e "\nConfiguration complete. Building with: make -C ${GENEVA_BUILDROOT} -j${_NCORES}\n"
			echo -e "---------------------------------------------------------------------\n"
			make -C "${GENEVA_BUILDROOT}" -j"${_NCORES}"
		else
			echo -e "\nYou may now build and install Geneva in the usual way:"
			echo -e "make\t\t# Use '-jn', where 'n' is the number of cores in your system"
			echo -e "make install\n"
		fi
	fi
fi

####################################################################
# Done
