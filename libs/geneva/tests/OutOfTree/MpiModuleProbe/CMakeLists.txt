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

# An out-of-tree program against an INSTALLED Geneva built with the MPI consumer. The release
# harness (ReleaseTests, outoftree/mpi-module) and the container image build
# (containers/geneva/Containerfile) configure it standalone, with CMAKE_PREFIX_PATH naming the
# installation, as a user's own project would be. It checks that the installed package exports
# Geneva::courtier-mpi with what its headers need, that the MPI consumer's header defines the
# consumer without the collection's own compile definitions, that a program can link it, and that
# the "mpi" consumer is loaded at run time from the module installed next to libgemfony-geneva
# (which no program links). The normal build compiles, links and runs the same source with the same usage
# requirements and nothing else (../CMakeLists.txt).

cmake_minimum_required(VERSION 3.27)
project(GenevaMpiModuleProbe CXX)

# The package finds MPI itself when Geneva was built with it: Geneva::courtier-mpi carries
# MPI::MPI_CXX.
find_package(Geneva CONFIG REQUIRED)

add_executable(GenevaMpiModuleProbe GenevaMpiModuleProbe.cpp)
target_link_libraries(GenevaMpiModuleProbe
    PRIVATE Geneva::courtier-mpi Geneva::geneva)
