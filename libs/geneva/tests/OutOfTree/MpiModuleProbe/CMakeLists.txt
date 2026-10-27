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

# An out-of-tree program against an INSTALLED Geneva built with the MPI consumer. It is not part of the
# in-tree build: the release harness (ReleaseTests, outoftree/mpi-module) and the container image build
# (containers/geneva/Containerfile) configure it standalone, with CMAKE_PREFIX_PATH naming the
# installation, as a user's own project would be. It checks that the installed package exports
# Geneva::geneva-mpi, that a program can link it, and that the "mpi" consumer is loaded at run time
# from the module installed next to libgemfony-geneva.

cmake_minimum_required(VERSION 3.27)
project(GenevaMpiModuleProbe CXX)

find_package(Geneva CONFIG REQUIRED)
# The module links no Geneva library and no MPI usage requirement travels with it, so a program that
# uses its classes links Geneva and MPI itself.
find_package(MPI REQUIRED)

add_executable(GenevaMpiModuleProbe GenevaMpiModuleProbe.cpp)
target_link_libraries(GenevaMpiModuleProbe PRIVATE Geneva::geneva-mpi Geneva::geneva MPI::MPI_CXX)
