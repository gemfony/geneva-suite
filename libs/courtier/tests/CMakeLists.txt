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

# The work items of the tests, the manual tests and the documentation program (not part of the library)
ADD_SUBDIRECTORY ( support )
ADD_SUBDIRECTORY ( UnitTests )
ADD_SUBDIRECTORY ( ManualTests )

# courtier builds on common only: neither the library nor courtier's MPI library needs a library of
# hap, dietrich or geneva at load time.
IF ( GENEVA_BUILD_WITH_MPI_CONSUMER )
	SET ( _courtier_libs $<TARGET_FILE:${COURTIER_LIBNAME}> $<TARGET_FILE:${COURTIER_MPI_LIBNAME}> )
ELSE ()
	SET ( _courtier_libs $<TARGET_FILE:${COURTIER_LIBNAME}> )
ENDIF ()
ADD_TEST ( NAME CourtierNeedsOnlyCommon
	COMMAND ${CMAKE_COMMAND}
		"-DLIBS=${_courtier_libs}"
		"-DPATTERN=libgemfony-(hap|dietrich|geneva)"
		"-DADVICE=courtier builds on common only: see the link of libs/courtier/src/CMakeLists.txt"
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoDriverLink.cmake )
SET_TESTS_PROPERTIES ( CourtierNeedsOnlyCommon PROPERTIES LABELS "courtier" )
GENEVA_TEST_SIZE ( CourtierNeedsOnlyCommon S )

# With the CUDA backend of the GPU consumer built in: courtier, which carries the backend, may not
# need the CUDA driver library at load time, so that a CUDA build still starts on machines without
# an NVIDIA driver. (ldd lists the whole closure, so common and weft are covered; hap and geneva
# are checked by GenevaNoDriverLink in libs/geneva/tests, whose closure holds them.)
IF ( GENEVA_GPU_CONSUMER_ENABLED AND CMAKE_CUDA_COMPILER )
	ADD_TEST ( NAME CourtierNoDriverLink
		COMMAND ${CMAKE_COMMAND}
			"-DLIBS=$<TARGET_FILE:${COURTIER_LIBNAME}>"
			-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoDriverLink.cmake )
	SET_TESTS_PROPERTIES ( CourtierNoDriverLink PROPERTIES LABELS "courtier" )
	GENEVA_TEST_SIZE ( CourtierNoDriverLink S )
ENDIF ()

# With the MPI consumer built: courtier (and with it common and weft, which ldd lists as part of its
# closure) needs no MPI library at load time, so that a courtier program that does not use MPI
# starts on a machine without it; MPI is linked by courtier's MPI library only, which in turn must
# link it (it holds the process's MPI environment). hap and geneva: GenevaNoMpiLink.
IF ( GENEVA_BUILD_WITH_MPI_CONSUMER )
	ADD_TEST ( NAME CourtierNoMpiLink
		COMMAND ${CMAKE_COMMAND}
			"-DLIBS=$<TARGET_FILE:${COURTIER_LIBNAME}>"
			"-DPATTERN=libmpi\\.so"
			-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoDriverLink.cmake )
	SET_TESTS_PROPERTIES ( CourtierNoMpiLink PROPERTIES LABELS "courtier" )
	GENEVA_TEST_SIZE ( CourtierNoMpiLink S )

	ADD_TEST ( NAME CourtierMpiLinksMpi
		COMMAND ${CMAKE_COMMAND}
			"-DLIBS=$<TARGET_FILE:${COURTIER_MPI_LIBNAME}>"
			"-DPATTERN=libmpi\\.so"
			-DEXPECT=present
			-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoDriverLink.cmake )
	SET_TESTS_PROPERTIES ( CourtierMpiLinksMpi PROPERTIES LABELS "courtier" )
	GENEVA_TEST_SIZE ( CourtierMpiLinksMpi S )

	# The MPI cascade and its members need nothing of Geneva: its test program, which links
	# courtier's MPI library, does not depend on a library of Geneva at load time.
	ADD_TEST ( NAME GCourtierMpiCascadeTestNoGeneva
		COMMAND ${CMAKE_COMMAND}
			"-DLIBS=$<TARGET_FILE:GCourtierMpiCascadeTest>"
			"-DPATTERN=libgemfony-geneva"
			-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckNoDriverLink.cmake )
	SET_TESTS_PROPERTIES ( GCourtierMpiCascadeTestNoGeneva PROPERTIES LABELS "courtier" )
	GENEVA_TEST_SIZE ( GCourtierMpiCascadeTestNoGeneva S )

	# The driver of the cascade's entries counts a launcher that ends by a signal after the job as a
	# non-zero end, not as a job that did not end
	ADD_TEST ( NAME GCourtierCheckJobEndsLauncherSignal
		COMMAND ${CMAKE_COMMAND}
			"-DCOMMAND=sh;-c;echo the job said this && kill -SEGV $$"
			-DEXPECT_EXIT=nonzero
			"-DEXPECT=the job said this"
			-DLIMIT=20
			-P ${CMAKE_CURRENT_SOURCE_DIR}/CheckJobEnds.cmake )
	SET_TESTS_PROPERTIES ( GCourtierCheckJobEndsLauncherSignal PROPERTIES LABELS "courtier" )
	GENEVA_TEST_SIZE ( GCourtierCheckJobEndsLauncherSignal S )
ENDIF ()

GENEVA_AGGREGATE_TARGET( "tests-courtier" )
