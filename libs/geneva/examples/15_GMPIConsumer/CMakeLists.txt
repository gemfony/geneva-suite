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

SET(EXECUTABLENAME GMPIConsumer)

SET(${EXECUTABLENAME}_SRCS
        GMPIConsumer.cpp
        )

ADD_EXECUTABLE(${EXECUTABLENAME}
        ${${EXECUTABLENAME}_SRCS}
        )

TARGET_LINK_LIBRARIES(${EXECUTABLENAME}
        ${GENEVA_LIBRARIES}
        ${Boost_LIBRARIES}
        ${MPI_LIBRARIES}
        # GFunctionIndividual is no longer in the geneva library; compiled in via its object library.
        GFunctionIndividual-obj
        )


GENEVA_MATERIALIZE_CONFIGS ( ${EXECUTABLENAME} ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME}/config )

# The MPI consumer fixes its rank layout at launch, so this example is exercised as a multi-process
# program under the launcher CMake detected (1 master + 3 workers) rather than as a plain binary --
# the same shape as the courtier MPI tests. The whole directory is already conditional on
# GENEVA_BUILD_WITH_MPI_CONSUMER (geneva/examples/CMakeLists.txt), which implies FIND_PACKAGE(MPI),
# so MPIEXEC_EXECUTABLE is a real, configure-time-detected dependency here.
#
# --maxIterations caps the run at 5 iterations; the example's own default is 200, which takes half a
# minute and verifies nothing the first few iterations do not (Inv 22).
ADD_TEST(NAME ${EXECUTABLENAME}
        COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 4
                ${MPIEXEC_PREFLAGS} $<TARGET_FILE:${EXECUTABLENAME}> ${MPIEXEC_POSTFLAGS}
                --maxIterations 5
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
#
# PROCESSORS 4 tells the `ctest -j` scheduler what this entry really costs: one CTest slot but four
# live ranks. Without it the scheduler counts it as one and oversubscribes the machine.
SET_TESTS_PROPERTIES(${EXECUTABLENAME} PROPERTIES LABELS "geneva;example" TIMEOUT 300 PROCESSORS 4)
GENEVA_TEST_SIZE(${EXECUTABLENAME} M)

INSTALL(TARGETS ${EXECUTABLENAME} DESTINATION ${INSTALL_PREFIX_DATA}/examples/${EXECUTABLENAME})
