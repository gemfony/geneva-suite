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
# GEM_TEST_SIZE(<test-name> <S|M|L|XL>)
#
# Gives a registered CTest test its SIZE label -- the one label the gates select on.
# Every registered test carries exactly one; the semantic labels it also carries
# (the owning library, `example`, `benchmark`) stay purely informational.
#
# The rule for choosing the letter, the boundaries, and what the gates do with it
# are documented in ONE place: the "Running the test suite" section of INSTALL.
# This file deliberately does not restate them.
#
################################################################################

FUNCTION(GEM_TEST_SIZE _test _size)
	IF(NOT "${_size}" MATCHES "^(S|M|L|XL)$")
		MESSAGE(FATAL_ERROR
			"GEM_TEST_SIZE(${_test} ${_size}): the size must be exactly one of S, M, L, XL. "
			"See the \"Running the test suite\" section of INSTALL for what each means and how to "
			"measure one.")
	ENDIF()
	IF(NOT TEST ${_test})
		MESSAGE(FATAL_ERROR
			"GEM_TEST_SIZE(${_test} ${_size}): no test named \"${_test}\" is registered in this "
			"directory. Call this AFTER the ADD_TEST that registers it.")
	ENDIF()
	SET_PROPERTY(TEST ${_test} APPEND PROPERTY LABELS ${_size})
ENDFUNCTION()
