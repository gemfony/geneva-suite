################################################################################
#
# Driver for the GPlotDesignerDataValidity ctest. It confirms that the raw series
# data GPlotDesigner exports (via the DATA backend / GDataEmitter) is CORRECT in
# BOTH formats:
#   - result_data.npz : a binary numpy .npz archive, built by hand (an uncompressed
#     ZIP of float64 .npy members + a manifest.json). The check loads it with
#     numpy.load() and asserts that a known series' shape and a couple of values
#     ROUND-TRIP BIT-EXACTLY (the binary must be exactly the values the generator wrote).
#   - result_data.csv : the human-inspectable CSV. The check parses a couple of float
#     values back and confirms they match.
# Any Python Traceback / AssertionError fails the test. Enabled only when python3 +
# numpy are present; skipped gracefully otherwise.
#
# Inputs (passed via -D): GEN_EXE, PYTHON_EXE, WORKDIR.
#
################################################################################

# 1. Run the generator -> WORKDIR/result_data.csv and WORKDIR/result_data.npz
#    (it also writes result.C / result.gp / result.py).
execute_process(
	COMMAND "${GEN_EXE}"
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE gen_rc
)
if(NOT gen_rc EQUAL 0)
	message(FATAL_ERROR "GPlotDesignerTest generator failed (rc=${gen_rc})")
endif()
if(NOT EXISTS "${WORKDIR}/result_data.npz")
	message(FATAL_ERROR "generator did not produce result_data.npz in ${WORKDIR}")
endif()
if(NOT EXISTS "${WORKDIR}/result_data.csv")
	message(FATAL_ERROR "generator did not produce result_data.csv in ${WORKDIR}")
endif()

# 2. Write the Python validation script. It loads the .npz and asserts the known
#    series_0 (a GGraph2D with x=[0,1,2,3,4], y=[0,0.5,1,1.5,2]) round-trips exactly,
#    that the manifest member is present, and that the CSV holds the same values.
file(WRITE "${WORKDIR}/gpd_data_validate.py"
"import numpy, zipfile, json, sys\n"
"\n"
"# --- NPZ: binary round-trip ---\n"
"d = numpy.load('result_data.npz')\n"
"assert len(d.files) >= 1, 'no arrays in the .npz'\n"
"assert 'series_0' in d.files, 'series_0 missing: ' + repr(d.files)\n"
"a = d['series_0']\n"
"assert a.dtype == numpy.float64, 'series_0 dtype is ' + str(a.dtype)\n"
"assert a.shape == (5, 2), 'series_0 shape is ' + str(a.shape)\n"
"# bit-exact round-trip of a couple of values\n"
"assert a[0, 0] == 0.0, a[0, 0]\n"
"assert a[1, 1] == 0.5, a[1, 1]\n"
"assert a[4, 0] == 4.0, a[4, 0]\n"
"assert a[3, 1] == 1.5, a[3, 1]\n"
"# the manifest member must be present (it is not a numpy array, so read it via zipfile)\n"
"with zipfile.ZipFile('result_data.npz') as z:\n"
"    names = z.namelist()\n"
"    assert 'manifest.json' in names, 'manifest.json missing: ' + repr(names)\n"
"    # the manifest is a self-describing object: a canvas (title + pad grid) and an\n"
"    # ordered 'series' array of per-plotter GPlotSpec objects (each augmented with its\n"
"    # pad / secondary placement).\n"
"    man = json.loads(bytes(z.read('manifest.json')).decode())\n"
"    assert isinstance(man, dict), 'manifest is not a JSON object: ' + repr(type(man))\n"
"    assert 'canvas' in man and 'series' in man, 'manifest missing canvas/series: ' + repr(list(man.keys()))\n"
"    canvas = man['canvas']\n"
"    for key in ('label', 'c_x_div', 'c_y_div'):\n"
"        assert key in canvas, 'canvas missing ' + key + ': ' + repr(canvas)\n"
"    s = man['series']\n"
"    assert isinstance(s, list) and len(s) >= 1, 'series is not a non-empty array: ' + repr(s)\n"
"    # every entry must be a valid GPlotSpec + placement describing the plot\n"
"    for entry in s:\n"
"        for key in ('kind', 'role', 'name', 'columns', 'pad', 'secondary'):\n"
"            assert key in entry, 'series entry missing ' + key + ': ' + repr(entry)\n"
"    assert s[0]['name'] == 'data graph', s[0]\n"
"    assert s[0]['kind'] == 'graph_2d', s[0]\n"
"    assert s[0]['role'] == 'xy', s[0]\n"
"    assert s[0]['columns'] == ['x', 'y'], s[0]\n"
"    assert s[0]['pad'] == 0 and s[0]['secondary'] is False, s[0]\n"
"\n"
"# --- NPZ: an integer histogram exports as a TRUE int32 array ---\n"
"df = numpy.load('result_data_full.npz')\n"
"with zipfile.ZipFile('result_data_full.npz') as z:\n"
"    fman = json.loads(bytes(z.read('manifest.json')).decode())\n"
"# locate the hist_1i series by its kind, then check its array dtype and round-trip.\n"
"hi = [(i, e) for i, e in enumerate(fman['series']) if e['kind'] == 'hist_1i']\n"
"assert len(hi) == 1, 'expected exactly one hist_1i series: ' + repr([e['kind'] for e in fman['series']])\n"
"hi_idx = hi[0][0]\n"
"hi_arr = df['series_%d' % hi_idx]\n"
"assert hi_arr.dtype == numpy.int32, 'integer histogram is not int32: ' + str(hi_arr.dtype)\n"
"# value v was added (v+1) times for v in 0..9 -> 55 samples, exact integers 0..9.\n"
"assert hi_arr.shape == (55, 1), 'hist_1i shape is ' + str(hi_arr.shape)\n"
"flat = hi_arr.reshape(-1)\n"
"assert int(flat.min()) == 0 and int(flat.max()) == 9, (int(flat.min()), int(flat.max()))\n"
"assert int((flat == 9).sum()) == 10, 'value 9 should appear 10 times'\n"
"assert int((flat == 0).sum()) == 1, 'value 0 should appear once'\n"
"\n"
"# --- CSV: parse a couple of values back ---\n"
"rows = []\n"
"with open('result_data.csv') as f:\n"
"    for line in f:\n"
"        line = line.strip()\n"
"        if not line or line.startswith('#') or line[0].isalpha():\n"
"            continue\n"
"        rows.append([float(v) for v in line.split(',')])\n"
"# the first data graph rows are (0,0), (1,0.5), ...\n"
"assert [0.0, 0.0] in rows, rows[:5]\n"
"assert [1.0, 0.5] in rows, rows[:5]\n"
"print('GPlotDesignerDataValidity: numpy round-trip exact; CSV values matched.')\n"
)

# 3. Run it through python3. A Traceback / AssertionError -> failure.
execute_process(
	COMMAND "${PYTHON_EXE}" gpd_data_validate.py
	WORKING_DIRECTORY "${WORKDIR}"
	RESULT_VARIABLE _py_rc
	OUTPUT_VARIABLE _out
	ERROR_VARIABLE _err
	TIMEOUT 300
)

if(_err MATCHES "Traceback" OR _err MATCHES "AssertionError" OR _err MATCHES "Error")
	message(FATAL_ERROR
		"python reported an error validating the exported data.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()
if(NOT _py_rc EQUAL 0)
	message(FATAL_ERROR
		"python3 exited non-zero (rc=${_py_rc}) -- the exported data did not validate.\n"
		"stdout:\n${_out}\nstderr:\n${_err}")
endif()
message(STATUS "GPlotDesignerDataValidity: ${_out}")
