################################################################################
#
# Driver for the GPlotDesignerRootValidity ctest. It confirms that the ROOT macro
# GPlotDesigner generates is VALID ROOT input by feeding it to the real ROOT
# interpreter (headless, via xvfb-run) and checking the rendered artifact.
#
# ROOT must touch a display even in batch mode on this host, hence xvfb-run; and its
# headless teardown returns a non-zero exit code AFTER a successful render, so this
# driver validates the produced PDF rather than ROOT's exit status.
#
# Inputs (passed via -D): GEN_EXE, ROOT_EXE, XVFB_RUN, WORKDIR.
#
################################################################################

# 1. Run the generator -> WORKDIR/result.C
execute_process(
	COMMAND "${GEN_EXE}"
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE gen_rc
)
if(NOT gen_rc EQUAL 0)
	message(FATAL_ERROR "GPlotDesignerTest generator failed (rc=${gen_rc})")
endif()
if(NOT EXISTS "${WORKDIR}/result.C")
	message(FATAL_ERROR "generator did not produce result.C in ${WORKDIR}")
endif()

# 2. Build a probe macro: the generated macro with a SaveAs + sentinel inserted before
#    its final closing brace (so a successful render leaves a verifiable artifact).
file(READ "${WORKDIR}/result.C" _macro)
string(FIND "${_macro}" "}" _last_brace REVERSE)
if(_last_brace LESS 0)
	message(FATAL_ERROR "result.C has no closing brace -- unexpected generator output")
endif()
string(SUBSTRING "${_macro}" 0 ${_last_brace} _head)
file(WRITE "${WORKDIR}/gpd_validate.C"
	"${_head}   cc->SaveAs(\"gpd_validated.pdf\");\n   printf(\"GPD-ROOT-VALID\\n\");\n}\n")
file(REMOVE "${WORKDIR}/gpd_validated.pdf")

# 3. Run it through ROOT headless.
execute_process(
	COMMAND "${XVFB_RUN}" -a "${ROOT_EXE}" -l -b -q gpd_validate.C
	WORKING_DIRECTORY "${WORKDIR}"
	OUTPUT_VARIABLE _out
	ERROR_VARIABLE _err
	TIMEOUT 300
)

# 4. Validate via the rendered artifact (NOT ROOT's exit code -- see header note).
if(NOT EXISTS "${WORKDIR}/gpd_validated.pdf")
	message(FATAL_ERROR
		"ROOT did not render the generated macro -- it is not valid ROOT input.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()
file(SIZE "${WORKDIR}/gpd_validated.pdf" _sz)
if(_sz LESS 1000)
	message(FATAL_ERROR "ROOT produced a suspiciously small PDF (${_sz} bytes); the macro likely did not render.")
endif()
message(STATUS "GPlotDesignerRootValidity: ROOT rendered a ${_sz}-byte PDF from the generated macro.")
