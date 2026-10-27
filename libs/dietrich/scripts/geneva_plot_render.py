#!/usr/bin/env python3
# ******************************************************************************
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
# ******************************************************************************
#
# See the NOTICE file in the top-level directory of the Geneva library
# collection for a list of contributors and copyright information.
#
# ******************************************************************************
"""Render a Geneva data export (the DATA backend / GDataEmitter output) to an image.

This is the EXTERNAL renderer of the data / plot-choice / plot-generation separation:
Geneva's DATA backend writes the raw series data plus a self-describing manifest, and
this tool recombines them into a matplotlib figure. Rendering therefore lives entirely
outside the C++ library -- format churn and new backends never touch libgemfony-common.

It reads either format the DATA backend emits:

  * a numpy ``.npz`` archive: ``series_0.npy`` ... ``series_N.npy`` (each a float64
    array of shape ``(rows, ncols)``) plus a ``manifest.json`` member, OR
  * a ``.csv`` text file: a leading ``# canvas:`` comment, then one ``# series N:``
    section per series (header comment, column-name row, data rows).

The manifest describes the canvas (title + pad grid) and, per series, its plot kind,
labels, columns, histogram bin counts and the pad it draws into (``secondary`` series
overlay the primary in the same pad). The plot kinds mirror the C++ matplotlib backend:
graph_2d / graph_2d_err / graph_3d / graph_4d / hist_1d / hist_1i / hist_2d (an
``hist_1i`` integer histogram is stored as a true int32 array in the ``.npz``).

Usage:
    geneva_plot_render.py INPUT[.npz|.csv] [-o OUTPUT.png]

With no ``-o`` the output defaults to the input path with a ``.png`` suffix.
"""

import argparse
import json
import sys
import zipfile


# ---------------------------------------------------------------------------
# Loading: turn either input format into a common (canvas, series) structure.
# A "series" is a dict carrying its spec fields plus a 2-D ``data`` array
# (list of columns: data[c][r]).
# ---------------------------------------------------------------------------

def _load_npz(path):
    """Load an .npz export into (canvas dict, [series dict])."""
    import numpy  # local import: only the .npz path needs numpy

    with zipfile.ZipFile(path) as z:
        if "manifest.json" not in z.namelist():
            raise ValueError("no manifest.json in " + path)
        manifest = json.loads(z.read("manifest.json").decode())

    canvas = manifest["canvas"]
    arrays = numpy.load(path)  # dict-like: series_0, series_1, ...
    series = []
    for i, spec in enumerate(manifest["series"]):
        key = "series_%d" % i
        arr = arrays[key]
        # arr is (rows, ncols) C-order; split into per-column lists.
        cols = [arr[:, c].tolist() for c in range(arr.shape[1])] if arr.size else []
        s = dict(spec)
        s["data"] = cols
        series.append(s)
    return canvas, series


def _load_csv(path):
    """Load a .csv export into (canvas dict, [series dict]). Mirrors emitCsv()."""
    canvas = {"label": "", "c_x_div": 1, "c_y_div": 1}
    series = []
    current = None  # the series dict being filled

    def _kv(header, key, cast=str, default=None):
        # extract `key=value` from a `# ...` header line (value is a single token).
        marker = " " + key + "="
        idx = header.find(marker)
        if idx < 0:
            return default
        rest = header[idx + len(marker):]
        token = rest.split()[0] if rest.split() else ""
        try:
            return cast(token)
        except (ValueError, IndexError):
            return default

    with open(path) as f:
        for raw in f:
            line = raw.rstrip("\n")
            stripped = line.strip()
            if not stripped:
                continue
            if stripped.startswith("# canvas:"):
                # title is quoted; grid via c_x_div / c_y_div tokens.
                if '"' in stripped:
                    canvas["label"] = stripped.split('"', 2)[1]
                canvas["c_x_div"] = _kv(stripped, "c_x_div", int, 1)
                canvas["c_y_div"] = _kv(stripped, "c_y_div", int, 1)
                continue
            if stripped.startswith("# series"):
                current = {
                    # `plotkind` is the canonical plotKind string (graph_2d, hist_1d, ...);
                    # `kind` in the CSV header is the C++ class name (GGraph2D) -- not used.
                    "kind": _kv(stripped, "plotkind", str, "graph_2d"),
                    "role": _kv(stripped, "role", str, ""),
                    "name": stripped.split('"', 2)[1] if '"' in stripped else "",
                    "pad": _kv(stripped, "pad", int, 0),
                    "secondary": bool(_kv(stripped, "secondary", int, 0)),
                    "columns": None,   # filled from the next (column-name) row
                    "data": None,
                }
                series.append(current)
                continue
            if stripped.startswith("#"):
                continue
            # A non-comment line: either the column-name header row (alpha) or data.
            if current is not None and current["columns"] is None and stripped[0].isalpha():
                current["columns"] = stripped.split(",")
                current["data"] = [[] for _ in current["columns"]]
                continue
            if current is not None and current["data"] is not None:
                vals = [float(v) for v in stripped.split(",")]
                for c, v in enumerate(vals):
                    current["data"][c].append(v)
    # The integer-histogram kind is mapped back via the kind string from the C++ name;
    # for CSV the GGraph* etc kind strings are already the manifest kinds.
    return canvas, series


def load(path):
    """Dispatch on the file extension."""
    if path.endswith(".npz"):
        return _load_npz(path)
    if path.endswith(".csv"):
        return _load_csv(path)
    raise ValueError("unrecognized input extension (expected .npz or .csv): " + path)


# ---------------------------------------------------------------------------
# Rendering: lay the pads out on a grid and draw each series per its kind.
# Mirrors the C++ MatplotlibEmitter (markers, errorbar, 3-d scatter, hist).
# ---------------------------------------------------------------------------

_THREE_D = {"graph_3d", "graph_4d"}


def _draw_series(ax, fig, s):
    """Draw one series into Axes ax according to its kind."""
    kind = s["kind"]
    data = s["data"] or []
    label = s.get("name", "")
    if kind == "graph_2d":
        ax.plot(data[0], data[1], marker="o", label=label)
    elif kind == "graph_2d_err":
        # stored (x, ex, y, ey) -> errorbar wants x, y, xerr, yerr.
        ax.errorbar(data[0], data[2], xerr=data[1], yerr=data[3], fmt="o", label=label)
    elif kind == "graph_3d":
        ax.plot(data[0], data[1], data[2], label=label)
    elif kind == "graph_4d":
        sc = ax.scatter(data[0], data[1], data[2], c=data[3], cmap="viridis", label=label)
        fig.colorbar(sc, ax=ax)
    elif kind in ("hist_1d", "hist_1i"):
        # 1-d histogram of the raw samples (hist_1i carries true int32 values).
        nb = s.get("n_bins_x") or 10
        ax.hist(data[0], bins=nb, label=label)
    elif kind == "hist_2d":
        nbx = s.get("n_bins_x") or 10
        nby = s.get("n_bins_y") or 10
        h = ax.hist2d(data[0], data[1], bins=[nbx, nby])
        fig.colorbar(h[3], ax=ax)
    else:
        # function_1d / function_2d carry no exported sample data and are never present
        # in a DATA export; ignore defensively.
        raise ValueError("cannot render kind %r from data export" % kind)


def render(canvas, series, output):
    """Render (canvas, series) to an image file via matplotlib (headless Agg)."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d import Axes3D  # noqa: F401 (registers the 3d projection)

    cols = max(1, int(canvas.get("c_x_div", 1)))
    rows = max(1, int(canvas.get("c_y_div", 1)))
    # Group series by pad; grow the grid if a pad index exceeds the declared grid.
    by_pad = {}
    for s in series:
        by_pad.setdefault(int(s.get("pad", 0)), []).append(s)
    max_pad = max(by_pad) if by_pad else 0
    while rows * cols <= max_pad:
        rows += 1

    fig = plt.figure(figsize=(cols * 5, rows * 4))
    fig.suptitle(canvas.get("label", ""))

    for pad in sorted(by_pad):
        members = by_pad[pad]
        # A pad is 3-d if its primary (first member) is a 3-d kind.
        three_d = members[0]["kind"] in _THREE_D
        ax = fig.add_subplot(rows, cols, pad + 1, projection="3d" if three_d else None)
        for s in members:
            _draw_series(ax, fig, s)
        # Labels/title from the pad's primary.
        prim = members[0]
        ax.set_xlabel(prim.get("x_label", "x"))
        ax.set_ylabel(prim.get("y_label", "y"))
        if three_d:
            ax.set_zlabel(prim.get("z_label", "z"))
        ax.set_title(prim.get("name", ""))

    fig.tight_layout()
    fig.savefig(output)
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description="Render a Geneva data export to an image.")
    parser.add_argument("input", help="the .npz or .csv data export to render")
    parser.add_argument("-o", "--output", help="output image path (default: INPUT with .png)")
    args = parser.parse_args(argv)

    output = args.output
    if output is None:
        base = args.input
        for ext in (".npz", ".csv"):
            if base.endswith(ext):
                base = base[: -len(ext)]
                break
        output = base + ".png"

    canvas, series = load(args.input)
    if not series:
        sys.stderr.write("geneva_plot_render: no renderable series in %s\n" % args.input)
        return 1
    render(canvas, series, output)
    print("geneva_plot_render: wrote %s (%d series, %dx%d pads)"
          % (output, len(series), canvas.get("c_x_div", 1), canvas.get("c_y_div", 1)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
