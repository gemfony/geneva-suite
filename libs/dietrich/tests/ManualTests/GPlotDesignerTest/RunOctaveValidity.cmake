################################################################################
#
# Driver for the GPlotDesignerOctaveValidity ctest. It confirms that the Octave /
# MATLAB script GPlotDesigner generates (via the OCTAVE backend) is VALID, RENDERING
# input by running it through the real `octave` and checking the rendered artifact.
#
# The generated script is terminal-agnostic (it builds the figure but does NOT call
# print / saveas); this driver prepends a headless setup (invisible figures + the
# gnuplot graphics toolkit when available, so no X display is needed) and appends a
# `print('-dpng', 'gpd_oct.png')`. Success is asserted via a zero exit code, a produced
# (non-trivial) PNG AND the absence of an Octave error in stderr -- a "PNG produced"
# check alone could miss an empty-plot bug.
#
# Inputs (passed via -D): GEN_EXE, OCTAVE_EXE, WORKDIR.
#
################################################################################

# 1. Run the generator -> WORKDIR/result.m (it also writes result.C / result.gp / result.py).
execute_process(
	COMMAND "${GEN_EXE}"
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE gen_rc
)
if(NOT gen_rc EQUAL 0)
	message(FATAL_ERROR "GPlotDesignerTest generator failed (rc=${gen_rc})")
endif()
if(NOT EXISTS "${WORKDIR}/result.m")
	message(FATAL_ERROR "generator did not produce result.m in ${WORKDIR}")
endif()

# 2. Build a probe script: a headless preamble (invisible figures + the gnuplot toolkit
#    when present, both no-ops when unsupported), the generated terminal-agnostic body,
#    then a print of a known PNG.
file(READ "${WORKDIR}/result.m" _script)
file(WRITE "${WORKDIR}/gpd_oct_validate.m"
	"set(0, 'defaultfigurevisible', 'off');\ntry; graphics_toolkit('gnuplot'); catch; end;\n${_script}\nprint('-dpng', 'gpd_oct.png');\n")
file(REMOVE "${WORKDIR}/gpd_oct.png")

# 3. Run it through octave headless (no window system needed).
execute_process(
	COMMAND "${OCTAVE_EXE}" --norc --no-window-system gpd_oct_validate.m
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE _oct_rc
	OUTPUT_VARIABLE _out
	ERROR_VARIABLE _err
	TIMEOUT 300
)

# 4. Reject an Octave error even if it somehow still produced a PNG: an octave error /
#    parse-error line means the script did not render cleanly. The match is line-anchored
#    so a benign environment line (e.g. "Fontconfig error:") is not mistaken for one.
#
#    Octave can also emit "error: ignoring const execution_exception& while preparing to
#    exit" from its at-exit cleanup -- e.g. when the gnuplot toolkit tears down its pipe in
#    a headless or polluted-FONTCONFIG environment. That message is benign: it appears AFTER
#    a successful render, octave still exits 0, and the PNG below is produced. So it must not
#    be mistaken for a script error. Strip that one known line before the check; a genuine
#    render error still surfaces as a different error:/parse error: line, a non-zero exit
#    code (step 5), or a missing/too-small PNG.
string(REGEX REPLACE "[^\n]*ignoring const execution_exception[^\n]*(\n|$)" "" _err_check "${_err}")
if(_err_check MATCHES "(^|\n)error:" OR _err_check MATCHES "(^|\n)parse error:")
	message(FATAL_ERROR
		"octave reported an error while running the generated script.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()

# 5. Validate via both octave's exit code and the rendered artifact.
if(NOT _oct_rc EQUAL 0)
	message(FATAL_ERROR
		"octave exited non-zero (rc=${_oct_rc}) -- the generated script is not valid Octave input.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()
if(NOT EXISTS "${WORKDIR}/gpd_oct.png")
	message(FATAL_ERROR
		"octave did not render the generated script -- no PNG was produced.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()
file(SIZE "${WORKDIR}/gpd_oct.png" _sz)
if(_sz LESS 1000)
	message(FATAL_ERROR "octave produced a suspiciously small PNG (${_sz} bytes); the script likely did not render.")
endif()
message(STATUS "GPlotDesignerOctaveValidity: octave rendered a ${_sz}-byte PNG from the generated script.")
