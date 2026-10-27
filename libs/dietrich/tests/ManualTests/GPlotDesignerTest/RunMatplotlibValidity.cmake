################################################################################
#
# Driver for the GPlotDesignerMatplotlibValidity ctest. It confirms that the
# Python/matplotlib script GPlotDesigner generates (via the MATPLOTLIB backend) is
# VALID, RENDERING matplotlib input by running it through the real python3 +
# matplotlib and checking the rendered artifact.
#
# The generated script is terminal-agnostic (it selects the headless Agg backend and
# builds `fig` but does NOT call savefig); this driver appends a
# `fig.savefig('gpd_mpl.png')` before running it. Success is asserted via a zero exit
# code, a produced (non-trivial) PNG AND the absence of a Python traceback / error in
# stderr -- a "PNG produced" check alone once missed an empty-plot bug.
#
# Inputs (passed via -D): GEN_EXE, PYTHON_EXE, WORKDIR.
#
################################################################################

# 1. Run the generator -> WORKDIR/result.py (it also writes result.C / result.gp).
execute_process(
	COMMAND "${GEN_EXE}"
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE gen_rc
)
if(NOT gen_rc EQUAL 0)
	message(FATAL_ERROR "GPlotDesignerTest generator failed (rc=${gen_rc})")
endif()
if(NOT EXISTS "${WORKDIR}/result.py")
	message(FATAL_ERROR "generator did not produce result.py in ${WORKDIR}")
endif()

# 2. Build a probe script: the generated, terminal-agnostic body (which already sets
#    the Agg backend and builds `fig`) followed by a savefig of a known PNG.
file(READ "${WORKDIR}/result.py" _script)
file(WRITE "${WORKDIR}/gpd_mpl_validate.py"
	"${_script}\nfig.savefig('gpd_mpl.png')\n")
file(REMOVE "${WORKDIR}/gpd_mpl.png")

# 3. Run it through python3 headless (Agg is forced both in the script and via env).
execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env "MPLBACKEND=Agg" "${PYTHON_EXE}" gpd_mpl_validate.py
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE _py_rc
	OUTPUT_VARIABLE _out
	ERROR_VARIABLE _err
	TIMEOUT 300
)

# 4. Reject a Python error even if it somehow still produced a PNG: a Traceback or an
#    "Error" in stderr means the script did not render cleanly.
if(_err MATCHES "Traceback" OR _err MATCHES "Error")
	message(FATAL_ERROR
		"python/matplotlib reported an error while running the generated script.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()

# 5. Validate via both python's exit code and the rendered artifact.
if(NOT _py_rc EQUAL 0)
	message(FATAL_ERROR
		"python3 exited non-zero (rc=${_py_rc}) -- the generated script is not valid matplotlib input.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()
if(NOT EXISTS "${WORKDIR}/gpd_mpl.png")
	message(FATAL_ERROR
		"matplotlib did not render the generated script -- no PNG was produced.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()
file(SIZE "${WORKDIR}/gpd_mpl.png" _sz)
if(_sz LESS 1000)
	message(FATAL_ERROR "matplotlib produced a suspiciously small PNG (${_sz} bytes); the script likely did not render.")
endif()
message(STATUS "GPlotDesignerMatplotlibValidity: matplotlib rendered a ${_sz}-byte PNG from the generated script.")
