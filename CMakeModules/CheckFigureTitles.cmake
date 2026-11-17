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
# Checks the titles of ROOT figures written by the plotting library: a figure
# shows each text once, and a figure of one pad has one title.
#
# A ROOT macro of the plotting library shows, per pad, these texts:
#   - the canvas label, the title of the whole figure, unless it is switched off:
#       TPaveLabel* canvasTitle = new TPaveLabel(..., "<text>");
#   - the title of the pad: the one set apart from the series,
#       n->SetTitle("<text>");
#     and otherwise ROOT's choice, the title of the first object drawn into the
#     pad -- the label of the pad's first series,
#       <object>->SetTitle("<text>");
#   - the entries of the pad's legend:
#       legend->AddEntry(<object>, "<text>", "<option>");
# The check fails, naming the file and the text, where one text occurs twice
# among the canvas label, a pad's title and that pad's legend entries (a pad
# without a title of its own shows its first series' label, which is also the
# legend's first entry), and where a figure of one pad shows both a canvas
# label and a pad title. A pad is the part of the macro after graphPad->cd(<n>).
# A blank text is no text: the plotting library gives an unlabelled series the
# title " ", which ROOT shows as nothing.
#
# The macros are read where their programs wrote them; each registration removes
# them before the programs run (a fixture), so that a program that no longer
# writes one fails the check instead of leaving an earlier run's macro to pass it.
#
# Input (passed via -D):
#   FIGURES   the ROOT macros to check (a list; each must exist)
#
################################################################################

IF(NOT FIGURES)
	MESSAGE(FATAL_ERROR "CheckFigureTitles.cmake needs -DFIGURES=<macro;macro;...>")
ENDIF()

# The text of a string literal as the plotting library writes it (rootEscape() escapes " and \).
SET(_lit "\"(([^\"\\\\]|\\\\.)*)\"")

LIST(REMOVE_ITEM FIGURES "")
SET(_problems "")
FOREACH(_figure IN LISTS FIGURES)
	IF(NOT EXISTS "${_figure}")
		STRING(APPEND _problems "\n  ${_figure}: missing (the program that writes it did not run)")
		CONTINUE()
	ENDIF()
	GET_FILENAME_COMPONENT(_name "${_figure}" NAME)
	FILE(STRINGS "${_figure}" _lines)

	SET(_canvas_label "")
	SET(_n_pads 0)
	# Per pad, as variables _pad<n>_title / _pad<n>_first / _pad<n>_entries (texts are never put
	# into CMake lists: a semicolon or a "[" in a text would split or swallow elements).
	FOREACH(_line IN LISTS _lines)
		IF(_line MATCHES "new TPaveLabel\\(.*, ${_lit}\\);")
			STRING(STRIP "${CMAKE_MATCH_1}" _canvas_label)
		ELSEIF(_line MATCHES "graphPad->cd\\([0-9]+\\);")
			MATH(EXPR _n_pads "${_n_pads} + 1")
			SET(_pad${_n_pads}_title "")
			SET(_pad${_n_pads}_first "")
			SET(_pad${_n_pads}_n_entries 0)
		ELSEIF(_n_pads GREATER 0 AND _line MATCHES "^[ \t]*n->SetTitle\\(${_lit}\\);")
			STRING(STRIP "${CMAKE_MATCH_1}" _pad${_n_pads}_title)
		ELSEIF(_n_pads GREATER 0 AND _line MATCHES "^[ \t]*[A-Za-z_][A-Za-z_0-9]*->SetTitle\\(${_lit}\\);")
			IF("${_pad${_n_pads}_first}" STREQUAL "")
				STRING(STRIP "${CMAKE_MATCH_1}" _pad${_n_pads}_first)
			ENDIF()
		ELSEIF(_n_pads GREATER 0 AND _line MATCHES "legend->AddEntry\\([^,]*, ${_lit}, ")
			STRING(STRIP "${CMAKE_MATCH_1}" _entry)
			IF(NOT "${_entry}" STREQUAL "")
				MATH(EXPR _e "${_pad${_n_pads}_n_entries} + 1")
				SET(_pad${_n_pads}_entry${_e} "${_entry}")
				SET(_pad${_n_pads}_n_entries ${_e})
			ENDIF()
		ENDIF()
	ENDFOREACH()

	IF(_n_pads EQUAL 0)
		STRING(APPEND _problems "\n  ${_name}: no pad found (graphPad->cd(<n>))")
		CONTINUE()
	ENDIF()

	FOREACH(_p RANGE 1 ${_n_pads})
		# The title the pad shows: its own, or the label of its first series.
		SET(_shown_title "${_pad${_p}_title}")
		IF("${_shown_title}" STREQUAL "")
			SET(_shown_title "${_pad${_p}_first}")
		ENDIF()

		# Every text of the pad, numbered, with where it comes from.
		SET(_n_texts 0)
		IF(NOT "${_canvas_label}" STREQUAL "")
			MATH(EXPR _n_texts "${_n_texts} + 1")
			SET(_text${_n_texts} "${_canvas_label}")
			SET(_where${_n_texts} "the canvas label")
		ENDIF()
		IF(NOT "${_shown_title}" STREQUAL "")
			MATH(EXPR _n_texts "${_n_texts} + 1")
			SET(_text${_n_texts} "${_shown_title}")
			SET(_where${_n_texts} "the title of pad ${_p}")
		ENDIF()
		IF(_pad${_p}_n_entries GREATER 0)
			FOREACH(_e RANGE 1 ${_pad${_p}_n_entries})
				MATH(EXPR _n_texts "${_n_texts} + 1")
				SET(_text${_n_texts} "${_pad${_p}_entry${_e}}")
				SET(_where${_n_texts} "legend entry ${_e} of pad ${_p}")
			ENDFOREACH()
		ENDIF()

		IF(_n_texts GREATER 1)
			MATH(EXPR _last "${_n_texts} - 1")
			FOREACH(_i RANGE 1 ${_last})
				MATH(EXPR _from "${_i} + 1")
				FOREACH(_j RANGE ${_from} ${_n_texts})
					IF("${_text${_i}}" STREQUAL "${_text${_j}}")
						STRING(APPEND _problems
							"\n  ${_name}: \"${_text${_i}}\" is ${_where${_i}} and ${_where${_j}}")
					ENDIF()
				ENDFOREACH()
			ENDFOREACH()
		ENDIF()
	ENDFOREACH()

	# The title pad 1 shows, as above.
	SET(_shown_title "${_pad1_title}")
	IF("${_shown_title}" STREQUAL "")
		SET(_shown_title "${_pad1_first}")
	ENDIF()
	IF(_n_pads EQUAL 1 AND NOT "${_canvas_label}" STREQUAL "" AND NOT "${_shown_title}" STREQUAL "")
		STRING(APPEND _problems "\n  ${_name}: one pad, two titles: the canvas label \"${_canvas_label}\" "
			"and the pad's \"${_shown_title}\"")
	ENDIF()
ENDFOREACH()

IF(NOT "${_problems}" STREQUAL "")
	MESSAGE(FATAL_ERROR "CheckFigureTitles: a figure shows a text twice, or two titles:${_problems}")
ENDIF()
LIST(LENGTH FIGURES _n_figures)
MESSAGE(STATUS "CheckFigureTitles: ${_n_figures} figure(s), every text shown once")
