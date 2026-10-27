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
# Individual-packaging CMake helpers: GENEVA_ADD_INDIVIDUAL_MODULE and
# GENEVA_DECLARE_INDIVIDUAL. These build an optimization individual as a
# compiled-in object library and/or a runtime-loadable shared MODULE.
#
# This module is SHARED between the in-tree Geneva build (included from
# CommonGenevaBuild.cmake) and the INSTALLED CMake package (included from the
# generated GenevaConfig.cmake), so an out-of-tree project that consumes an
# installed Geneva via find_package(Geneva) gets the same packaging helpers.
#
# The Geneva header search paths are NOT hard-coded here (they differ between the
# in-tree source layout and an installed prefix). The includer MUST set
#
#   GENEVA_INDIVIDUAL_INCLUDE_DIRS
#
# to the include directories of the Geneva collection (common/hap/courtier/
# dietrich/geneva) before including this file. Boost's include dirs
# (${Boost_INCLUDE_DIRS}) and the caller's own source directory
# (${CMAKE_CURRENT_SOURCE_DIR}) are added per-invocation (call time), so Boost
# need only be found by the time an individual is actually declared.
#
################################################################################

IF(NOT GENEVA_INDIVIDUAL_MODULE_INCLUDED)
	SET(GENEVA_INDIVIDUAL_MODULE_INCLUDED TRUE)

	IF(NOT DEFINED GENEVA_INDIVIDUAL_INCLUDE_DIRS)
		MESSAGE(FATAL_ERROR
			"GenevaIndividualModule.cmake included without GENEVA_INDIVIDUAL_INCLUDE_DIRS set. "
			"Set it to the Geneva + Boost include directories before including this module.")
	ENDIF()

	# The C++ standard Geneva was built with. A loadable module links NONE of the Geneva libraries, so it
	# does not inherit the cxx_std_NN compile feature from them -- and an out-of-tree consumer project may not
	# set CMAKE_CXX_STANDARD at all. The helpers therefore pin the standard on the individual targets
	# explicitly (Geneva's headers require it). The includer sets this to Geneva's standard.
	IF(NOT DEFINED GENEVA_INDIVIDUAL_CXX_STANDARD)
		MESSAGE(FATAL_ERROR
			"GenevaIndividualModule.cmake included without GENEVA_INDIVIDUAL_CXX_STANDARD set. "
			"Set it to the C++ standard Geneva was built with (e.g. 23).")
	ENDIF()

	###############################################################################
	# GENEVA_ADD_INDIVIDUAL_MODULE(<target> <source> [<source> ...])
	#
	# Builds a runtime-loadable individual (optimization-problem) plugin as a shared MODULE, following the
	# reference pattern of example 18. The .so carries the problem code + its archive registration + the
	# geneva_module_manifest() entry point, and links NONE of the Geneva libraries -- it needs only their
	# HEADERS at compile time and resolves their symbols from the host process at load (the loader uses
	# RTLD_GLOBAL). Linking no Geneva libs also sidesteps the non-PIC static Catch2 that a testing-enabled
	# Geneva pulls in as a usage requirement, which cannot go into a shared object.
	#
	# The module keeps the "lib" prefix (CMake MODULE libraries drop it by default) so the artifact is named
	# lib<target>.so, matching the individual-plugin convention. The caller's own source directory is on the
	# include path so the glue translation unit finds its problem header.
	FUNCTION(GENEVA_ADD_INDIVIDUAL_MODULE _target)
		IF(${ARGC} LESS 2)
			MESSAGE(FATAL_ERROR
				"GENEVA_ADD_INDIVIDUAL_MODULE(${_target}): at least one source file is required")
		ENDIF()
		ADD_LIBRARY(${_target} MODULE ${ARGN})
		# MODULE libraries drop the "lib" prefix by default; keep it so the .so is named lib<target>.so.
		SET_TARGET_PROPERTIES(${_target} PROPERTIES PREFIX "lib")
		TARGET_COMPILE_FEATURES(${_target} PRIVATE cxx_std_${GENEVA_INDIVIDUAL_CXX_STANDARD})
		TARGET_COMPILE_OPTIONS(${_target} PRIVATE ${GENEVA_INDIVIDUAL_ABI_OPTIONS})
		# A loadable module is a production artifact and must NEVER carry the test framework: a
		# testing-enabled Geneva build defines GEM_TESTING tree-wide (ADD_DEFINITIONS), which would pull
		# Catch2 (REQUIRE/CHECK) into any individual whose *_GUnitTests_ bodies use it -- and since the module
		# links no Geneva libraries and the loader dlopens RTLD_NOW (eager), those unresolvable Catch2 symbols
		# would abort the load. Undefine GEM_TESTING for the module so its sources compile the non-testing
		# branch.
		#
		# This is ABI-safe by CONSTRUCTION, not by discipline. The self-test hooks live on their own
		# opt-in interface, Gem::Common::GSelfTestable (libs/common/include/common/GSelfTestable.hpp),
		# which a class inherits UNCONDITIONALLY or not at all -- no member of it is preprocessor-gated, so
		# core and module can never disagree about its layout. The category roots an individual actually
		# derives from (GGenome, GPersonalityTraits, GOptimizationAlgorithmBase, ...) do not inherit it, so
		# an individual that writes no tests has no test-related slots at all; one that does write them
		# inherits the same interface on both sides of the GEM_TESTING line, with only the BODIES of its
		# overrides conditional. The GSelfTestableTests unit test pins the "not a base of GCommonInterfaceT"
		# half of this guarantee.
		TARGET_COMPILE_OPTIONS(${_target} PRIVATE -UGEM_TESTING)
		# Boost_INCLUDE_DIRS is read here (call time), not when this module was included, so it is set even
		# if Boost is found after this module (in-tree) / by the config package's find_dependency (out-of-tree).
		TARGET_INCLUDE_DIRECTORIES(${_target} PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}
			${GENEVA_INDIVIDUAL_INCLUDE_DIRS}
			${Boost_INCLUDE_DIRS})
	ENDFUNCTION()

	###############################################################################
	# GENEVA_DECLARE_INDIVIDUAL(<name>
	#     MODE    <load|compile|both>
	#     SOURCES <individual.cpp> [<more.cpp> ...]
	#     [PLUGIN <glue.cpp>]
	#     [CONFIG <path>])
	#
	# The packaging declaration for a Geneva optimization individual. It encodes the maintainer's DA1b model:
	# the SAME individual source can be packaged two ways at once, and the author states the intent at the
	# definition site. It ONLY packages -- it never modifies or generates any C++ (serialization export stays
	# the author's hand-written GEM_REGISTER_ARCHIVABLE, one per serialized type, in the individual's own cpp).
	#
	#   MODE compile  -> an OBJECT library <name>-obj built from SOURCES. A binary that COMPILES the individual
	#                    in links this target, so the individual's GEM_REGISTER_ARCHIVABLE static
	#                    initializers (archive-tag registration) are pulled in and never stripped.
	#   MODE load     -> a shared MODULE lib<name>.so built from SOURCES + the PLUGIN glue TU. It links NONE of
	#                    the Geneva libraries (their symbols resolve from the host process at load via
	#                    RTLD_GLOBAL; only their headers are needed) and is loaded at runtime via --individual.
	#   MODE both     -> both of the above; the module reuses the object library's compiled objects
	#                    ($<TARGET_OBJECTS:...>) rather than recompiling SOURCES.
	#
	# The PLUGIN glue TU (the fixed extern "C" geneva_module_manifest() entry point) is compiled into the
	# module .so ONLY, never the object library: the manifest symbol name is fixed, so a compile-in binary that
	# links two individuals must not contain two definitions of it. Hence SOURCES = the individual (class +
	# export, in both targets) and PLUGIN = the glue (module only). The ONE hard rule the author must honour
	# (not enforceable in CMake): a single PROCESS must never both compile-in and load the same individual --
	# Geneva allows exactly one optimization problem per process and refuses the second claim, naming both.
	#
	# CONFIG, when given, is recorded on the module target (property GENEVA_INDIVIDUAL_CONFIG) for downstream
	# config materialization; it is the same path the author passes to individualManifest<> in the glue TU.
	FUNCTION(GENEVA_DECLARE_INDIVIDUAL _name)
		CMAKE_PARSE_ARGUMENTS(GDI "" "MODE;PLUGIN;CONFIG" "SOURCES" ${ARGN})

		IF(NOT GDI_MODE)
			MESSAGE(FATAL_ERROR "GENEVA_DECLARE_INDIVIDUAL(${_name}): MODE <load|compile|both> is required")
		ENDIF()
		IF(NOT GDI_MODE MATCHES "^(load|compile|both)$")
			MESSAGE(FATAL_ERROR
				"GENEVA_DECLARE_INDIVIDUAL(${_name}): MODE must be load, compile or both (got '${GDI_MODE}')")
		ENDIF()
		IF(NOT GDI_SOURCES)
			MESSAGE(FATAL_ERROR "GENEVA_DECLARE_INDIVIDUAL(${_name}): SOURCES <individual.cpp> is required")
		ENDIF()

		SET(_want_compile FALSE)
		SET(_want_load FALSE)
		IF(GDI_MODE STREQUAL "compile" OR GDI_MODE STREQUAL "both")
			SET(_want_compile TRUE)
		ENDIF()
		IF(GDI_MODE STREQUAL "load" OR GDI_MODE STREQUAL "both")
			SET(_want_load TRUE)
		ENDIF()

		# The PLUGIN glue TU is the module's manifest entry point: required for a loadable module, and
		# meaningless (and unsafe -- fixed symbol name) outside one.
		IF(_want_load AND NOT GDI_PLUGIN)
			MESSAGE(FATAL_ERROR
				"GENEVA_DECLARE_INDIVIDUAL(${_name}): MODE ${GDI_MODE} needs PLUGIN <glue.cpp> (the module entry point)")
		ENDIF()
		IF(GDI_PLUGIN AND NOT _want_load)
			MESSAGE(FATAL_ERROR
				"GENEVA_DECLARE_INDIVIDUAL(${_name}): PLUGIN is only valid for MODE load or both")
		ENDIF()

		# Boost_INCLUDE_DIRS is read here (call time), not when this module was included, so it is set even
		# if Boost is found after this module (in-tree) / by the config package's find_dependency (out-of-tree).
		SET(_gdi_incdirs
			${CMAKE_CURRENT_SOURCE_DIR}
			${GENEVA_INDIVIDUAL_INCLUDE_DIRS}
			${Boost_INCLUDE_DIRS})

		# (A) compile/both: the OBJECT library of the individual's own sources (class + archive registration).
		# PIC so its objects are equally usable in the shared module (both) and in any consumer.
		IF(_want_compile)
			ADD_LIBRARY(${_name}-obj OBJECT ${GDI_SOURCES})
			SET_TARGET_PROPERTIES(${_name}-obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
			TARGET_COMPILE_FEATURES(${_name}-obj PUBLIC cxx_std_${GENEVA_INDIVIDUAL_CXX_STANDARD})
			TARGET_COMPILE_OPTIONS(${_name}-obj PRIVATE ${GENEVA_INDIVIDUAL_ABI_OPTIONS})
			TARGET_INCLUDE_DIRECTORIES(${_name}-obj PUBLIC ${_gdi_incdirs})
			# NOTE on link order: every in-tree consumer lists <name>-obj BEFORE the Geneva and Boost libraries.
			# The reason this NOTE originally gave no longer holds -- it was about --as-needed dropping the
			# shared libboost_serialization, and Geneva links no Boost.Serialization at all since the move to
			# Gem::Weft. The convention is kept because it costs nothing and because CMake deduplicates a
			# shared library to its FIRST position, so a usage requirement declared here could not reorder one
			# anyway.
		ENDIF()

		# (B) load/both: the shared MODULE lib<name>.so. It reuses the object library's objects where they
		# exist (both) or compiles SOURCES itself (load-only), plus the PLUGIN glue (module ONLY). It links no
		# Geneva libraries -- exactly the GENEVA_ADD_INDIVIDUAL_MODULE recipe.
		IF(_want_load)
			IF(_want_compile)
				ADD_LIBRARY(${_name} MODULE $<TARGET_OBJECTS:${_name}-obj> ${GDI_PLUGIN})
			ELSE()
				ADD_LIBRARY(${_name} MODULE ${GDI_SOURCES} ${GDI_PLUGIN})
			ENDIF()
			# MODULE libraries drop the "lib" prefix by default; keep it so the .so is named lib<name>.so.
			SET_TARGET_PROPERTIES(${_name} PROPERTIES PREFIX "lib")
			TARGET_COMPILE_FEATURES(${_name} PRIVATE cxx_std_${GENEVA_INDIVIDUAL_CXX_STANDARD})
			TARGET_COMPILE_OPTIONS(${_name} PRIVATE ${GENEVA_INDIVIDUAL_ABI_OPTIONS})
			TARGET_INCLUDE_DIRECTORIES(${_name} PRIVATE ${_gdi_incdirs})
			IF(GDI_CONFIG)
				SET_TARGET_PROPERTIES(${_name} PROPERTIES GENEVA_INDIVIDUAL_CONFIG "${GDI_CONFIG}")
			ENDIF()
		ENDIF()
	ENDFUNCTION()

ENDIF(NOT GENEVA_INDIVIDUAL_MODULE_INCLUDED)
