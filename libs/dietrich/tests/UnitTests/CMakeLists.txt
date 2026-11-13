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

################################################################################
# Helper: declare a Dietrich-library Catch2 test executable. Each call
#
#     geneva_dietrich_test(<exe-name> <size> <source-files...>)
#
# creates an executable, links it against ONLY the dietrich + common libraries
# (dietrich is a leaf peer on top of common; it must not pull in hap / courtier /
# geneva), plus Boost + Catch2::Catch2WithMain, registers it with CTest under the
# "dietrich" label plus its size label, installs it, and appends it to the
# `tests-dietrich-unit` aggregate target.
#
# <size> is S, M, L or XL, measured -- see the "Running the test suite" section of
# INSTALL. It is a required argument on purpose: a new test cannot be added without
# saying what it costs, because the size is what decides whether a gate runs it.

SET(_GENEVA_DIETRICH_TEST_TARGETS "")

FUNCTION(geneva_dietrich_test name size)
	ADD_EXECUTABLE(${name} ${ARGN})

	TARGET_LINK_LIBRARIES(${name}
		${DIETRICH_LIBNAME} ${COMMON_LIBNAME} ${Boost_LIBRARIES} Catch2::Catch2WithMain)

	ADD_TEST(${name} ${name})
	SET_TESTS_PROPERTIES(${name} PROPERTIES LABELS "dietrich")
	GENEVA_TEST_SIZE(${name} ${size})
	INSTALL(TARGETS ${name}
		DESTINATION ${INSTALL_PREFIX_DATA}/tests/dietrich/UnitTests)

	# Append to the parent-scope aggregate list.
	SET(_GENEVA_DIETRICH_TEST_TARGETS "${_GENEVA_DIETRICH_TEST_TARGETS};${name}"
		PARENT_SCOPE)
ENDFUNCTION()

################################################################################
# Test executables.

geneva_dietrich_test(GPlotDesignerTests           S  GPlotDesignerTests.cpp)

################################################################################
# Aggregate `tests-dietrich-unit` target builds every Dietrich test exe at once.

ADD_CUSTOM_TARGET( "tests-dietrich-unit"
	DEPENDS ${_GENEVA_DIETRICH_TEST_TARGETS}
	COMMENT "Building the unit tests for the \"dietrich\" library."
)
