# dietrich — the Geneva plotting library

`dietrich` is the plotting library of the Geneva collection. It turns numeric data
into figures and figure-generating scripts, through a single small API and a set of
interchangeable rendering backends.

## About the name

The library is named for the Dresden court painter **Christian Wilhelm Ernst
Dietrich** ("Dietricy", 1712–1774), renowned for his **stylistic versatility**: he
painted fluently in the manner of Rembrandt, Ostade, Salvator Rosa, Watteau and
Claude. One painter, many idioms — mirrored by one library with many output
backends. (As a bonus, *Dietrich* is the German word for a skeleton key: one key,
many locks ≈ one library, many output formats.)

## What it does

A recorder declares the plots it wants and pushes data; the library renders the
result through any backend:

- **ROOT** — emits a `.C` ROOT macro (the historical default; see
  <https://root.cern>).
- **gnuplot** — emits a multiplot `.gp` script.
- **matplotlib** — emits a headless (Agg) Python plotting script.
- **Octave** — emits a GNU Octave plotting script.
- **data** — exports the raw series as CSV and as a binary NumPy `.npz`, alongside a
  self-describing JSON manifest.
- **external renderer** — `scripts/geneva_plot_render.py` consumes the data export +
  manifest and produces figures entirely outside the C++ library.

### Two API levels

- **`GDataLog`** (the modern, plotter-object-free API): declare each plot as a
  `GPlotSpec` value (`declareSeries` / `overlaySeries`), append data rows
  (`append`), then `writeToFile`. This is what all of Geneva's monitors use.
- **The plotter objects** (`GGraph2D`, `GHistogram2D`, `GPlotDesigner`, …): the
  lower-level building blocks `GDataLog` is realized on. Application/example code may
  still use these directly.

Everything lives in the **`Gem::Dietrich`** namespace. The umbrella header
`dietrich/GPlotDesigner.hpp` pulls in the whole stack; the focused headers under
`dietrich/plotting/` can also be included individually.

## Dependencies (strict)

- The **main library depends only on `common`** (it uses common's logging,
  serialization helpers, exception types and interface base). It does **not** link
  `hap`, `courtier` or `geneva`.
- The **tests and demos may additionally use `hap`** to generate data to plot; `hap`
  is never a dependency of the library itself.
- Resulting layer order: `common <- dietrich`, with `dietrich` a leaf peer of `hap`
  on top of `common`, consumed by `geneva`.

## Layout

```
dietrich/include/dietrich/GPlotDesigner.hpp   umbrella header
dietrich/include/dietrich/plotting/*.hpp      focused public headers
dietrich/src/GPlotDesigner.cpp                implementation (gemfony-dietrich)
dietrich/tests/UnitTests/                     Catch2 unit tests
dietrich/tests/ManualTests/GPlotDesignerTest/ byte-exact + backend validity gates
dietrich/scripts/geneva_plot_render.py        the bundled external renderer
```

## Building and testing

`dietrich` builds as part of the normal Geneva build (`make` / `make dietrich`). Its
tests run under CTest with the `dietrich` label, and the manual test additionally
runs optional validity gates that feed the generated artifacts to the real
`root` / `gnuplot` / `python3 + matplotlib` / `numpy` tools when those are present
(and skip gracefully otherwise).
