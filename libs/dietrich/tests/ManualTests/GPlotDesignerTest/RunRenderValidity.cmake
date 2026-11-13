################################################################################
#
# Driver for the GPlotDesignerRenderValidity ctest. It confirms that the EXTERNAL
# renderer (scripts/geneva_plot_render.py) reproduces a figure from a Geneva DATA
# export -- i.e. that rendering can live entirely OUTSIDE the C++ library, consuming
# only the raw series data plus the self-describing manifest the DATA backend writes.
#
# It renders BOTH a comprehensive .npz (result_data_full.npz: every kind the renderer
# understands -- graph_2d + a secondary overlay, graph_2d_err, graph_3d, graph_4d,
# hist_1d, hist_2d, on a 2x3 canvas) AND the human-inspectable .csv (result_data.csv),
# proving both input paths. Success is asserted via a zero exit code, a produced
# (non-trivial) PNG AND the absence of a Python traceback / error -- a "PNG produced"
# check alone once missed an empty-plot bug.
#
# Inputs (passed via -D): GEN_EXE, RENDER_PY, PYTHON_EXE, WORKDIR.
#
################################################################################

# 1. Run the generator -> WORKDIR/result_data_full.npz and result_data.csv (among others).
execute_process(
	COMMAND "${GEN_EXE}"
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE gen_rc
)
if(NOT gen_rc EQUAL 0)
	message(FATAL_ERROR "GPlotDesignerTest generator failed (rc=${gen_rc})")
endif()
foreach(_f result_data_full.npz result_data.csv)
	if(NOT EXISTS "${WORKDIR}/${_f}")
		message(FATAL_ERROR "generator did not produce ${_f} in ${WORKDIR}")
	endif()
endforeach()

# Render one input through the external tool and assert it produced a non-trivial PNG
# with no Python error. MPLCONFIGDIR keeps matplotlib from touching $HOME when run
# outside its usual environment.
function(render_and_check _input _png)
	file(REMOVE "${WORKDIR}/${_png}")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
			"MPLBACKEND=Agg" "MPLCONFIGDIR=${WORKDIR}/.mplconfig"
			"${PYTHON_EXE}" "${RENDER_PY}" "${_input}" "-o" "${_png}"
		WORKING_DIRECTORY "${WORKDIR}"
		RESULT_VARIABLE _py_rc
		OUTPUT_VARIABLE _out
		ERROR_VARIABLE _err
		TIMEOUT 300
	)
	if(_err MATCHES "Traceback" OR _err MATCHES "Error")
		message(FATAL_ERROR
			"geneva_plot_render reported an error rendering ${_input}.\n"
			"stdout:\n${_out}\nstderr:\n${_err}")
	endif()
	if(NOT _py_rc EQUAL 0)
		message(FATAL_ERROR
			"geneva_plot_render exited non-zero (rc=${_py_rc}) rendering ${_input}.\n"
			"stdout:\n${_out}\nstderr:\n${_err}")
	endif()
	if(NOT EXISTS "${WORKDIR}/${_png}")
		message(FATAL_ERROR "the renderer produced no PNG for ${_input}.\nstdout:\n${_out}\nstderr:\n${_err}")
	endif()
	file(SIZE "${WORKDIR}/${_png}" _sz)
	if(_sz LESS 1000)
		message(FATAL_ERROR "the renderer produced a suspiciously small PNG for ${_input} (${_sz} bytes).")
	endif()
	message(STATUS "GPlotDesignerRenderValidity: rendered ${_input} -> ${_sz}-byte PNG.")
endfunction()

# 2. Render the comprehensive .npz and the .csv.
render_and_check("result_data_full.npz" "render_full.png")
render_and_check("result_data.csv" "render_csv.png")
