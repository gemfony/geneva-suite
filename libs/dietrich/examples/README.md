# Dietrich examples

Five small, self-contained demos of the `dietrich` plotting library, meant to be
read alongside the manual. Each is a single `.cpp` that writes plot scripts and/or
data files to the current working directory. They use only the C++ standard library
for any sample data, so they depend on nothing but `dietrich` + `common`.

Build with `-DGENEVA_BUILD_EXAMPLES=TRUE` (the executables are named
`dietrich_<name>`); run each from a scratch directory.

| Demo | Shows | Writes |
|---|---|---|
| **quickstart** | The modern, plotter-object-free `GDataLog` API: declare one `GPlotSpec` series, push rows, write the *same* log to every backend. | `quickstart.{C,gp,py,m,csv}` |
| **gallery** | One 2×2 canvas with several data-driven plot kinds — a 2-d curve, a 3-d helix, a 1-d histogram and a fixed-range 2-d histogram — through the histogram-capable backends. | `gallery.{C,py,m}` |
| **overlays** | A primary + secondary plotter sharing one pad via `overlaySeries()`, and the two plot modes (`SCATTER` measurements vs. a `CURVE` fit). | `overlays.{C,gp}` |
| **functions** | Formula-driven `GFunctionPlotter1D/2D` via the lower-level plotter-object API, and the backend-capability boundary (function plotters are ROOT-only). | `functions.C` |
| **external_render** | The DATA backend: export raw series + a self-describing manifest as CSV and as a numpy `.npz`, then render *outside* C++ with `scripts/geneva_plot_render.py`. | `exported.{csv,npz}` |

Backends covered across the set: **ROOT** (`.C`), **gnuplot** (`.gp`), **matplotlib**
(`.py`), **Octave / MATLAB** (`.m`), and the **DATA** export (`.csv` / `.npz`) consumed
by the bundled external renderer.

To turn the generated scripts into images, run the matching tool, e.g.
`root -l -b -q gallery.C`, `gnuplot overlays.gp`, `python3 gallery.py`,
`octave gallery.m`, or
`python3 ../scripts/geneva_plot_render.py exported.npz -o exported.png`.
