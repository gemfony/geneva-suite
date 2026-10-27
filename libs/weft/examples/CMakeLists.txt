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

# Each example links ONLY gemfony-weft + Boost.JSON (not the rest of Geneva), so
# the examples also demonstrate that Weft is usable as a standalone library.
SET ( _WEFT_EXAMPLES
	quickstart       # intrusive member serialize, binary + JSON codecs
	nonintrusive     # a third-party type via an ADL free gem_archive_serialize
	polymorphic      # base-pointer round trip via GEM_REGISTER_ARCHIVABLE
)

FOREACH ( _ex ${_WEFT_EXAMPLES} )
	ADD_EXECUTABLE ( weft_${_ex} ${_ex}.cpp )
	TARGET_LINK_LIBRARIES ( weft_${_ex} ${WEFT_LIBNAME} Boost::json )
	INSTALL ( TARGETS weft_${_ex}
		DESTINATION ${INSTALL_PREFIX_DATA}/examples/weft )
ENDFOREACH ()
