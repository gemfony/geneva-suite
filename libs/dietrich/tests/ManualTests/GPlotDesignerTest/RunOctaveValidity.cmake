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
#    After the print the probe checks every legend against what the gnuplot toolkit can key: it
#    keys line objects only, so every text of a legend must name a line object of its axes (a
#    sample drawn as a patch was silently dropped from the key).
set(_legend_probe [=[
lines_ = findobj(gcf, 'Type', 'line');
shown_ = get(lines_, 'displayname');
if ischar(shown_), shown_ = {shown_}; end
for leg_ = findobj(gcf, 'Tag', 'legend')'
   names_ = get(leg_, 'string');
   if ischar(names_), names_ = {names_}; end
   keyed_ = 0;
   for k_ = 1:numel(names_)
      if any(strcmp(shown_, names_{k_})), keyed_ = keyed_ + 1; end
   end
   printf('GPD-OCT-LEGEND %d %d\n', numel(names_), keyed_);
end
]=])
file(WRITE "${WORKDIR}/gpd_oct_validate.m"
	"set(0, 'defaultfigurevisible', 'off');\ntry; graphics_toolkit('gnuplot'); catch; end;\n${_script}\nprint('-dpng', 'gpd_oct.png');\n${_legend_probe}")
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
# 6. Every text of every legend names a line object, which the gnuplot toolkit keys.
string(REGEX MATCHALL "GPD-OCT-LEGEND [0-9]+ [0-9]+" _legends "${_out}")
if(NOT _legends)
	message(FATAL_ERROR "octave reported no legend, but the generated script has pads with one.\nstdout:\n${_out}")
endif()
foreach(_legend IN LISTS _legends)
	string(REGEX REPLACE "GPD-OCT-LEGEND ([0-9]+) ([0-9]+)" "\\1" _names "${_legend}")
	string(REGEX REPLACE "GPD-OCT-LEGEND ([0-9]+) ([0-9]+)" "\\2" _keyed "${_legend}")
	if(NOT _names EQUAL _keyed)
		message(FATAL_ERROR "A legend names ${_names} series but only ${_keyed} line objects carry them; the others never reach the gnuplot toolkit's key.\nstdout:\n${_out}")
	endif()
endforeach()
if(_sz LESS 1000)
	message(FATAL_ERROR "octave produced a suspiciously small PNG (${_sz} bytes); the script likely did not render.")
endif()
message(STATUS "GPlotDesignerOctaveValidity: octave rendered a ${_sz}-byte PNG from the generated script.")
