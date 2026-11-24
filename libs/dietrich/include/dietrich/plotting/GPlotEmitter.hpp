/********************************************************************************
 *
 * This file is part of the Geneva library collection. The following license
 * applies to this file:
 *
 * ------------------------------------------------------------------------------
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ------------------------------------------------------------------------------
 *
 * Note that other files in the Geneva library collection may use a different
 * license. Please see the licensing information in each file.
 *
 ********************************************************************************
 *
 * See the NOTICE file in the top-level directory of the Geneva library
 * collection for a list of contributors and copyright information.
 *
 ********************************************************************************/

#pragma once

#include <memory>
#include <string>

namespace Gem::Dietrich {

/******************************************************************************/

class GPlotDesigner; // forward declaration

/******************************************************************************/
/**
 * The set of backends a GPlotDesigner can emit through. ROOT (the historical default)
 * generates a ROOT macro; GNUPLOT generates a gnuplot script for the graph plotters;
 * MATPLOTLIB generates a Python/matplotlib script and OCTAVE an Octave/MATLAB (.m) script,
 * both covering graph plotters and histograms; DATA exports the raw series data (CSV text
 * or a numpy .npz archive) rather than a rendered plot. See the per-enumerator notes below.
 */
enum class plotBackend {
    ROOT,       ///< Emit a ROOT macro (the default; every plot kind)
    GNUPLOT,    ///< Emit a gnuplot script (graph plotters only)
    MATPLOTLIB, ///< Emit a Python/matplotlib script (graph plotters and histograms)
    OCTAVE, ///< Emit an Octave / MATLAB script (.m; graph plotters and histograms, common-core syntax)
    DATA    ///< Emit the raw series data (NOT a rendered plot); CSV text or a numpy .npz archive
};

/******************************************************************************/
/**
 * The on-disk format the DATA backend (GDataEmitter) exports. CSV (the default) is a
 * single, human-inspectable text file; NPZ is a single binary numpy `.npz` archive
 * that numpy.load() reads back as a dict of named float64 arrays (the "binary CSV").
 */
enum class dataFormat {
    CSV, ///< One human-inspectable text file, one section per plotter (file extension ".csv")
    NPZ  ///< One binary numpy .npz archive of named float64 arrays (file extension ".npz")
};

/******************************************************************************/
/**
 * The strategy interface that separates backend-specific document emission from
 * the data collection performed by GPlotDesigner / the GBasePlotter hierarchy. A
 * concrete emitter turns a fully-populated GPlotDesigner into a backend-specific
 * script (a ROOT macro, a gnuplot script, ...).
 */
class IPlotEmitter {
public:
    /** @brief The (defaulted) virtual destructor */
    virtual ~IPlotEmitter() = default;

    /**
     * @brief Emits the complete backend document for a populated designer
     * @param gpd The designer holding the plotters and canvas configuration
     * @return The complete backend-specific document as a string
     */
    [[nodiscard]] virtual std::string emitDocument(const GPlotDesigner &gpd) const = 0;

    /**
     * @brief The conventional file-name extension for this backend's documents
     * @return The extension, including the leading dot (e.g. ".C" or ".gp")
     */
    [[nodiscard]] virtual std::string fileExtension() const = 0;
};

/******************************************************************************/
/**
 * The ROOT backend. emitDocument() writes a ROOT macro: a TCanvas, a TPaveLabel title, a
 * Divide()'d TPad and, for every pad, the declarations of its series' data, the data and the
 * drawing. Like the other backends it draws every series from what its plotter reports -- its
 * GPlotSpec, its data columns and its draw style (GBasePlotter::plotSpec(), dataColumns(),
 * drawStyle()) -- and supports every plot kind.
 */
class GRootEmitter : public IPlotEmitter {
public:
    /**
     * @brief Emits the complete ROOT macro for a populated designer
     */
    [[nodiscard]] std::string emitDocument(const GPlotDesigner &gpd) const override;

    /**
     * @brief The ROOT-macro file extension
     */
    [[nodiscard]] std::string fileExtension() const override;
};

/******************************************************************************/
/**
 * The gnuplot backend. emitDocument() supports ONLY the graph plotters
 * (GGraph2D / GGraph2ED / GGraph3D / GGraph4D); any other plotter type triggers a
 * clear gem_exception directing the caller to the ROOT backend. The emitted
 * script is terminal-agnostic (a validity harness prepends its own `set terminal`
 * / `set output`), laid out as a `set multiplot` grid of per-pad plots.
 *
 * Each series is drawn as its draw style says (GBasePlotter::drawStyle()), and the key names the
 * series the pad's legend lists (GBasePlotter::legendEntries()); a pad without a legend has no key.
 * Limitation: gnuplot draws a key sample from its dataset's own style, not from a sample of its
 * own -- so it shows the draw style as far as gnuplot draws it: a 3-d graph with ROOT's colour-map
 * arguments is drawn as points (gnuplot's pm3d needs gridded data, which a projection gives), and a
 * projected surface or map shows gnuplot's own pm3d sample.
 */
class GnuplotEmitter : public IPlotEmitter {
public:
    /**
     * @brief Emits the complete gnuplot script for a populated designer
     */
    [[nodiscard]] std::string emitDocument(const GPlotDesigner &gpd) const override;

    /**
     * @brief The gnuplot-script file extension
     */
    [[nodiscard]] std::string fileExtension() const override;
};

/******************************************************************************/
/**
 * The matplotlib backend. emitDocument() supports the graph plotters
 * (GGraph2D / GGraph2ED / GGraph3D / GGraph4D) and the histogram plotters
 * (GHistogram1D / GHistogram1I / GHistogram2D); any other plotter type (e.g. a function plotter)
 * triggers a clear gem_exception directing the caller to the ROOT backend. The
 * emitted script is a self-contained Python program that selects the headless Agg
 * backend, builds a `fig` with one Axes per pad and plots into it, but deliberately
 * ends WITHOUT `fig.savefig(...)` so it stays terminal-agnostic -- a validity
 * harness (or the caller) appends its own `fig.savefig('out.png')`.
 *
 * Each series is drawn, and its legend sample made, from its draw style as far as matplotlib draws
 * it (scriptStyle()): a 1-d histogram always as bars and a 2-d histogram always as a colour map,
 * whatever ROOT draw option it carries.
 */
class MatplotlibEmitter : public IPlotEmitter {
public:
    /**
     * @brief Emits the complete matplotlib (Python) script for a populated designer
     */
    [[nodiscard]] std::string emitDocument(const GPlotDesigner &gpd) const override;

    /**
     * @brief The matplotlib-script file extension
     */
    [[nodiscard]] std::string fileExtension() const override;
};

/******************************************************************************/
/**
 * The Octave / MATLAB backend. emitDocument() supports the graph plotters
 * (GGraph2D / GGraph2ED / GGraph3D / GGraph4D) and the histogram plotters
 * (GHistogram1D / GHistogram1I / GHistogram2D); any other plotter type (e.g. a function plotter)
 * triggers a clear gem_exception directing the caller to the ROOT backend. The
 * emitted script is a self-contained `.m` program using only the Octave-and-MATLAB
 * COMMON CORE (base functions, no toolboxes / packages): a `figure()` with one
 * `subplot` per pad. It deliberately ends WITHOUT a `print` / `saveas`, so it stays
 * terminal-agnostic -- a validity harness (or the caller) appends its own
 * `print('-dpng', 'out.png')`.
 *
 * Each series is drawn, and its legend sample made, from its draw style as far as Octave draws it
 * (scriptStyle()): a 1-d histogram always as bars and a 2-d histogram always as a colour map. The
 * legend's samples are line objects without data, which the gnuplot toolkit keys (a patch without
 * data it does not; other toolkits were not tried); the plotted objects carry no name.
 */
class OctaveEmitter : public IPlotEmitter {
public:
    /**
     * @brief Emits the complete Octave / MATLAB (.m) script for a populated designer
     */
    [[nodiscard]] std::string emitDocument(const GPlotDesigner &gpd) const override;

    /**
     * @brief The Octave / MATLAB script file extension
     */
    [[nodiscard]] std::string fileExtension() const override;
};

/******************************************************************************/
/**
 * The DATA backend. emitDocument() exports each registered plotter's raw columnar
 * series data -- it does NOT render a plot. The graph plotters (GGraph2D / GGraph2ED /
 * GGraph3D / GGraph4D) and the histogram plotters (GHistogram1D / GHistogram1I / GHistogram2D) export
 * their axis columns; the function plotters (GFunctionPlotter1D / GFunctionPlotter2D)
 * carry no sampled data and are skipped (with a comment in CSV mode).
 *
 * Two formats are offered (selected via the constructor or
 * GPlotDesigner::setDataFormat()):
 *   - CSV (the default): a single human-inspectable text file, one section per plotter
 *     separated by a blank line, each section a `# series ...` comment header, a
 *     column-name header row, then the data rows (locale-independent, full precision).
 *   - NPZ: a single binary numpy `.npz` archive (an uncompressed ZIP of float64 `.npy`
 *     members `series_0`, `series_1`, ... plus a `manifest.json`), readable by
 *     numpy.load(). emitDocument() returns the raw bytes as a std::string; the file
 *     MUST be written in binary (GPlotDesigner::writeToFile keys off fileExtension()).
 */
class GDataEmitter : public IPlotEmitter {
public:
    /** @brief The default constructor selects CSV mode */
    GDataEmitter() = default;
    /**
     * @brief Constructs the emitter in the requested export format
     */
    explicit GDataEmitter(dataFormat format);

    /**
     * @brief Emits the raw series data for a populated designer
     */
    [[nodiscard]] std::string emitDocument(const GPlotDesigner &gpd) const override;

    /**
     * @brief The file extension for the selected format
     */
    [[nodiscard]] std::string fileExtension() const override;

    /**
     * @brief The export format this emitter was constructed with
     */
    [[nodiscard]] dataFormat getDataFormat() const;

private:
    dataFormat format_ = dataFormat::CSV; ///< The selected on-disk export format
};

/******************************************************************************/
/**
 * @brief The standard emitter of a backend: what GPlotDesigner::setPlotBackend() installs, and
 * what tells a writer the extension of the backend's files (IPlotEmitter::fileExtension()); the
 * DATA backend in its default format, CSV
 */
[[nodiscard]] std::shared_ptr<IPlotEmitter> makeEmitter(plotBackend backend);

/******************************************************************************/

} /* namespace Gem::Dietrich */
