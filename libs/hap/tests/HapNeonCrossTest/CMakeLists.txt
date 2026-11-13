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
# Optional AArch64/NEON cross-architecture self-test for Hap.
#
# The build host is typically x86_64, so the NEON SIMD engine cannot be built by
# the project compiler. When BOTH an AArch64 cross-compiler AND qemu-aarch64
# (user-mode emulation) are present -- and the cross-compiler can actually
# produce a static C++20 binary -- this cross-compiles a standalone self-test
# and registers a ctest case that runs it under qemu. On any system lacking
# these tools (the common case) it is silently skipped: it is a developer
# validation aid, NOT a portability gate. The NEON code's correctness, once
# validated here, does not depend on re-running it everywhere.
#

FIND_PROGRAM ( HAP_AARCH64_CXX
	NAMES aarch64-linux-gnu-g++ aarch64-linux-gnu-g++-15 aarch64-linux-gnu-g++-14
	      aarch64-linux-gnu-g++-13 )
FIND_PROGRAM ( HAP_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static )

IF ( HAP_AARCH64_CXX AND HAP_QEMU_AARCH64 )
	# Configure-time probe: can this cross-compiler build a STATIC C++20 binary?
	# (qemu-user runs static binaries without a sysroot lib path; if the toolchain
	# lacks static libstdc++ we skip rather than break the build.)
	FILE ( WRITE ${CMAKE_CURRENT_BINARY_DIR}/_neon_probe.cpp "int main(){return 0;}\n" )
	EXECUTE_PROCESS (
		COMMAND ${HAP_AARCH64_CXX} -std=c++20 -static
		        ${CMAKE_CURRENT_BINARY_DIR}/_neon_probe.cpp
		        -o ${CMAKE_CURRENT_BINARY_DIR}/_neon_probe
		RESULT_VARIABLE _hap_neon_probe_rc
		OUTPUT_QUIET ERROR_QUIET )

	IF ( _hap_neon_probe_rc EQUAL 0 )
		SET ( _src ${CMAKE_CURRENT_SOURCE_DIR}/neon_crossarch_selftest.cpp )
		SET ( _bin ${CMAKE_CURRENT_BINARY_DIR}/neon_crossarch_selftest )
		SET ( _inc ${PROJECT_SOURCE_DIR}/libs/hap/include )

		ADD_CUSTOM_COMMAND (
			OUTPUT  ${_bin}
			COMMAND ${HAP_AARCH64_CXX} -std=c++20 -O2 -static -I ${_inc} ${_src} -o ${_bin}
			DEPENDS ${_src}
			        ${_inc}/hap/GXoshiro256pp.hpp
			        ${_inc}/hap/GXoshiro256ppSIMD.hpp
			COMMENT "Cross-compiling Hap NEON self-test for AArch64"
			VERBATIM )

		ADD_CUSTOM_TARGET ( HapNeonCrossTest ALL DEPENDS ${_bin} )
		ADD_TEST ( NAME HapNeonCrossTest COMMAND ${HAP_QEMU_AARCH64} ${_bin} )
		GENEVA_TEST_SIZE(HapNeonCrossTest S)

		MESSAGE ( STATUS "Hap: NEON cross-arch self-test enabled "
			"(${HAP_AARCH64_CXX} + ${HAP_QEMU_AARCH64})" )
	ELSE ()
		MESSAGE ( STATUS "Hap: NEON cross-arch self-test skipped "
			"(AArch64 cross-compiler cannot build a static C++20 binary)" )
	ENDIF ()
ELSE ()
	MESSAGE ( STATUS "Hap: NEON cross-arch self-test skipped "
		"(need aarch64-linux-gnu-g++ AND qemu-aarch64)" )
ENDIF ()
