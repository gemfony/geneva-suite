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

SET(COMMUNICATIONSOURCES
        GCourtierHelperFunctions.cpp
        GCourtierEnums.cpp
        GMPIPeerTransport.cpp
        GMPIPollCore.cpp
        GWebsocketPeer.cpp
        GWebsocketPeerTransport.cpp
        )

# The MPI consumer is header-only here (consumers/GMPIConsumerT.hpp, transport/GMPITransportT.hpp):
# this library links no MPI. It is instantiated in Geneva's MPI module
# (libgemfony-geneva-mpi), which is loaded at run time, so no program needs libmpi to start. What
# must exist once per process and calls MPI lives in courtier's MPI library (below). The MPI poll
# and the transport's peers (GMPIPollCore.cpp, GMPIPeerTransport.cpp) are compiled here all the
# same: they make their MPI calls through transport/GMPICallsI.hpp, whose binding to MPI is part of
# courtier's MPI library.

# The GPU consumer is optional, and is folded into this one courtier library: the device backends compile in (and CUDA links in) only when requested. The backend
# source list + toolkit detection come from gpu/GPUBackends.cmake (which sets COURTIER_GPU_SRCS and the
# COURTIER_GPU_HAVE_CUDA flag used in the link/definitions block below).
IF (GENEVA_GPU_CONSUMER_ENABLED)
    INCLUDE( ${CMAKE_CURRENT_SOURCE_DIR}/gpu/GPUBackends.cmake )
    SET( COMMUNICATIONSOURCES
            ${COMMUNICATIONSOURCES}
            ${COURTIER_GPU_SRCS})
ENDIF ()

set_source_files_properties(
        ${COMMUNICATIONSOURCES}
        PROPERTIES
        LANGUAGE CXX
)

ADD_LIBRARY(${COURTIER_LIBNAME} SHARED ${COMMUNICATIONSOURCES})

SET_TARGET_PROPERTIES(
        ${COURTIER_LIBNAME}
        PROPERTIES
        VERSION ${GENEVA_VERSION}
        EXPORT_NAME courtier
)

# Export the include directory. The C++ standard is selected centrally in
# CMakeModules/CommonGenevaBuild.cmake.
TARGET_INCLUDE_DIRECTORIES(
        ${COURTIER_LIBNAME}
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/libs/courtier/include>
        $<INSTALL_INTERFACE:include>
)

# Inter-library dependency: courtier builds on common (and through it on weft) -- not on hap, dietrich
# or geneva (tests CourtierNeedsOnlyCommon and LibraryLayersRespected).
TARGET_LINK_LIBRARIES(${COURTIER_LIBNAME} PUBLIC ${COMMON_LIBNAME})

# The GPU consumer's device dependencies (CUDA driver/NVRTC) are implementation details of the
# backend .cpp files -> link them PRIVATE so they do not leak into courtier's exported interface. The
# GPUGEN_HAVE_* feature macros are likewise PRIVATE: only GGPUBackendFactory.cpp (a courtier translation
# unit) switches on them to register the available backends; no public GPU header references them, so a
# program that builds a GPU marshaller (e.g. example 14) does not need them.
IF (GENEVA_GPU_CONSUMER_ENABLED)
    IF (COURTIER_GPU_HAVE_CUDA)
        TARGET_COMPILE_DEFINITIONS(${COURTIER_LIBNAME} PRIVATE GPUGEN_HAVE_CUDA)
        # NVRTC (a toolkit library) for runtime kernel compilation is linked. The CUDA driver API is
        # NOT: libcuda.so.1 comes with the NVIDIA driver, so linking it would keep every Geneva program
        # from starting on a machine without one, even for a run on the CPU consumers. gpu/GCUDADriver
        # loads it with Boost.DLL when a CUDA backend is first initialized; the CUDA headers come with
        # CUDA::nvrtc, and Boost.DLL needs only the dynamic loading library.
        TARGET_LINK_LIBRARIES(${COURTIER_LIBNAME} PRIVATE CUDA::nvrtc ${CMAKE_DL_LIBS})
        MESSAGE(STATUS "${COURTIER_LIBNAME}: GPU consumer CUDA backend enabled (NVRTC; CUDA driver loaded at run time)")
    ELSE ()
        MESSAGE(STATUS "${COURTIER_LIBNAME}: GPU consumer CUDA backend DISABLED (no CUDA toolkit)")
    ENDIF ()
    # The CUDA toolkit's bundled host compiler can inject an older libstdc++ into the link step; link
    # the CXX compiler's own libstdc++.so by full path to satisfy the required CXXABI (see top-level
    # CMakeLists notes on the CUDA link fix).
    IF (COURTIER_GPU_HAVE_CUDA AND DEFINED GENEVA_CXX_RUNTIME_LIB_DIR
        AND EXISTS "${GENEVA_CXX_RUNTIME_LIB_DIR}/libstdc++.so")
        TARGET_LINK_LIBRARIES(${COURTIER_LIBNAME} PRIVATE "${GENEVA_CXX_RUNTIME_LIB_DIR}/libstdc++.so")
    ENDIF ()
    MESSAGE(STATUS "${COURTIER_LIBNAME}: GPU consumer built in (consumer mnemonic 'gpu' available)")
ENDIF ()

INSTALL(TARGETS ${COURTIER_LIBNAME} EXPORT GenevaTargets DESTINATION ${INSTALL_PREFIX_LIBS})

################################################################################
# Courtier's MPI library (libgemfony-courtier-mpi, out of tree Geneva::courtier-mpi), built only
# with the MPI consumer. It holds the binding of the MPI transport's calls to MPI (mpi/GMPICalls.cpp,
# the GMPICallsI of the poll in libgemfony-courtier), the MPI cascade (mpi/GMPICascade.cpp: a client
# shares each evaluation with a group of MPI processes) with its polled waits
# (mpi/GMPIPolledCalls.cpp), and the process's one MPI environment (mpi/GMPIEnvironmentAccess.cpp):
# that state must be defined in exactly one shared object, so that a program, Geneva's MPI module
# and any other library of the process share it, and it calls MPI, which libgemfony-courtier must
# not link (tests/CheckNoDriverLink.cmake). Nothing links it unless it uses MPI: Geneva's MPI module
# does, and so does a program that names the functions of courtier/mpi/. Its headers include mpi.h,
# so MPI is part of its interface.
IF (GENEVA_BUILD_WITH_MPI_CONSUMER)
    ADD_LIBRARY(${COURTIER_MPI_LIBNAME} SHARED
            mpi/GMPICalls.cpp
            mpi/GMPICascade.cpp
            mpi/GMPIEnvironmentAccess.cpp
            mpi/GMPIPolledCalls.cpp)
    SET_TARGET_PROPERTIES(
            ${COURTIER_MPI_LIBNAME}
            PROPERTIES
            VERSION ${GENEVA_VERSION}
            EXPORT_NAME courtier-mpi
    )
    TARGET_LINK_LIBRARIES(${COURTIER_MPI_LIBNAME} PUBLIC ${COURTIER_LIBNAME} MPI::MPI_CXX)
    INSTALL(TARGETS ${COURTIER_MPI_LIBNAME} EXPORT GenevaTargets DESTINATION ${INSTALL_PREFIX_LIBS})
ENDIF ()
