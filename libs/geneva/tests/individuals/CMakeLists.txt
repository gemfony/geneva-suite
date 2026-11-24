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

# The test individuals: problem definitions that exist only for the unit tests, a benchmark and a manual
# test (GTestIndividual1: adaption, processing and personality; GTestIndividual2: serialization size and
# random walks; GTestIndividual3: plain-data access). None is part of the geneva library or its installed
# headers. Each is an object library <name>-obj that its consumers link; they include "<name>.hpp".
GENEVA_DECLARE_INDIVIDUAL(GTestIndividual1
	MODE    compile
	SOURCES GTestIndividual1.cpp)

GENEVA_DECLARE_INDIVIDUAL(GTestIndividual2
	MODE    compile
	SOURCES GTestIndividual2.cpp)

GENEVA_DECLARE_INDIVIDUAL(GTestIndividual3
	MODE    compile
	SOURCES GTestIndividual3.cpp)
