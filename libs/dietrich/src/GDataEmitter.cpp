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

#include "dietrich/GPlotDesigner.hpp"
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctions.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <istream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>
#include "dietrich/plotting/detail/GPlotDetail.hpp"


namespace Gem::Dietrich {

// The plotting library builds on common's facilities (logging, serialization helpers,
// exception types, make_member, EmitStream, ...); make them visible here without
// per-name qualification. This affects lookup only within Gem::Dietrich.
using namespace Gem::Common;
// Dietrich declares its own to_string(plotKind), which would otherwise shadow common's
// numeric to_string(...) for unqualified calls; merge common's overloads back in.
using Gem::Common::to_string;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
// The DATA backend (GDataEmitter): exports the raw columnar series data (NOT a
// rendered plot) as either human-inspectable CSV text or a binary numpy .npz archive.

namespace {

/******************************************************************************/
/**
 * A single exportable series: one plotter's columnar data captured generically. `name`
 * is the plotter's plot label, `kind` its getPlotterName(), `column_names` the per-axis
 * labels (x, y, ...) and `columns` the per-axis value vectors (all equal length). The
 * DATA backend reads everything through the public column<I>() accessor, so the series
 * is decoupled from the concrete plotter type once captured.
 */
struct dataSeries {
    std::string name;                              ///< the plotter's plot label
    std::string kind;                              ///< the plotter's getPlotterName()
    std::vector<std::string> column_names;         ///< per-axis names (x, ex, y, ...)
    std::vector<GPlotColumn> columns;              ///< per-axis value vectors (type-tagged, parallel)
    GPlotSpec spec;                                ///< the plotter's full reported plot spec
    std::size_t pad = 0;                           ///< the canvas pad this series draws into
    bool secondary = false;                        ///< true if it overlays a primary in the same pad
};

/** @brief The canvas-level layout the data export records so an external renderer can
 *  reproduce the multi-pad figure: the canvas title and the pad grid (columns x rows). */
struct canvasInfo {
    std::string label;       ///< the canvas title
    std::size_t c_x_div = 1; ///< number of pad columns
    std::size_t c_y_div = 1; ///< number of pad rows
};

/** @brief Capture a plotter's columns as a dataSeries, or std::nullopt for a plotter that
 *  carries no exportable sampled data (the function plotters). The column data (float64
 *  or int32) and names are read generically through dataColumns() / plotSpec(), so the
 *  capture is decoupled from the concrete plotter type. */
std::optional<dataSeries> captureSeries(const GBasePlotter &p) {
    dataSeries s;
    s.name = p.plotLabel();
    s.kind = p.getPlotterName();
    s.spec = p.plotSpec();
    s.columns = p.dataColumns();

    // A plotter with no exportable columns (a function plotter) reports nothing here --
    // those are dataless and are skipped.
    if(s.columns.empty()) {
        return std::nullopt;
    }

    // The per-axis names are the spec's column labels, in the same storage order as
    // dataColumns() (e.g. {"x","ex","y","ey"} for GGraph2ED, {"value"} for GHistogram1D).
    s.column_names = s.spec.columns;
    return s;
}

/** @brief The number of data rows in a captured series (the common column length). */
std::size_t seriesRows(const dataSeries &s) {
    return s.columns.empty() ? 0 : columnSize(s.columns.front());
}

/******************************************************************************/
// CSV mode.

/** @brief Escape a label for a CSV header comment / a quoted CSV field: drop CRs/newlines
 *  (a comment line and a data row must stay single-line) and double any embedded `"`. */
std::string csvComment(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for(char const c : in) {
        if(c == '\n' || c == '\r') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

/** @brief Render all captured series as the CSV document: one section per series separated
 *  by a blank line; each section a `# series ...` comment header, a column-name header row,
 *  then the data rows. Values are full-precision and locale-independent (EmitStream). */
std::string emitCsv(const std::vector<dataSeries> &series, const canvasInfo &canvas) {
    EmitStream out; // NOLINT(cppcoreguidelines-init-variables)

    // A leading canvas comment so an external reader can reproduce the pad grid.
    out << "# canvas: \"" << csvComment(canvas.label) << "\" c_x_div=" << canvas.c_x_div
        << " c_y_div=" << canvas.c_y_div << '\n';

    for(std::size_t si = 0; si < series.size(); ++si) {
        const dataSeries &s = series[si];
        if(si != 0) {
            out << '\n'; // blank line between sections
        }

        // Comma-list of column names for the header comment.
        std::string const col_list = s.column_names | std::views::join_with(',')
            | std::ranges::to<std::string>();

        out << "# series " << si << ": \"" << csvComment(s.name) << "\" kind=" << s.kind
            << " plotkind=" << to_string(s.spec.kind) << " role=" << s.spec.role
            << " columns=" << col_list
            << " pad=" << s.pad << " secondary=" << (s.secondary ? 1 : 0) << '\n';

        // Column-name header row.
        for(std::size_t c = 0; c < s.column_names.size(); ++c) {
            out << (c == 0 ? "" : ",") << s.column_names[c];
        }
        out << '\n';

        // Data rows. Each cell streams its column's value at row r -- an int32 column
        // prints integers, a float64 column full-precision doubles.
        const std::size_t rows = seriesRows(s);
        for(std::size_t r = 0; r < rows; ++r) {
            for(std::size_t c = 0; c < s.columns.size(); ++c) {
                out << (c == 0 ? "" : ",");
                std::visit([&out, r](const auto *v) { out << (*v)[r]; }, s.columns[c]);
            }
            out << '\n';
        }
    }
    return out.str();
}

/******************************************************************************/
// NPZ mode: build a numpy .npz (an uncompressed ZIP of float64 .npy members) by hand,
// with no new C++ dependency. Helpers below assume a little-endian host (every platform
// Geneva targets) -- the float64 / uint bytes are emitted in native order.

/** @brief Append a uint16 little-endian to a byte buffer. */
void putU16(std::string &buf, std::uint16_t v) {
    buf.push_back(static_cast<char>(v & 0xFFu));
    buf.push_back(static_cast<char>((v >> 8) & 0xFFu));
}

/** @brief Append a uint32 little-endian to a byte buffer. */
void putU32(std::string &buf, std::uint32_t v) {
    buf.push_back(static_cast<char>(v & 0xFFu));
    buf.push_back(static_cast<char>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<char>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

/** @brief A table-based CRC-32 (the ISO-HDLC / ZIP polynomial 0xEDB88320), built once. */
const std::array<std::uint32_t, 256> &crc32Table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for(std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for(int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[n] = c;
        }
        return t;
    }();
    return table;
}

/** @brief CRC-32 of a byte range (ZIP local/central-directory checksum). */
std::uint32_t crc32(const std::string &data) {
    const auto &table = crc32Table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for(unsigned char const byte : data) {
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/** @brief Build the bytes of a numpy `.npy` (format v1.0) for a 2-D C-order little-endian
 *  array of shape (rows, cols). The dtype is taken from the columns: a series whose
 *  columns are int32 is written as `<i4` (a real numpy int32 array), otherwise `<f8`
 *  float64. A series' columns are uniform dtype (a plotter's axes are all double or all
 *  int32). The column-major `columns` are interleaved into the row-major payload; a
 *  1-column series is still written as shape (rows, 1). */
std::string buildNpy(const std::vector<GPlotColumn> &columns, std::size_t rows) {
    const std::size_t cols = columns.size();

    const bool is_int = !columns.empty()
        && std::holds_alternative<const std::vector<std::int32_t> *>(columns.front());
    const char *descr = is_int ? "<i4" : "<f8";
    const std::size_t elem_size = is_int ? sizeof(std::int32_t) : sizeof(double);

    // The ASCII dict header describing the array.
    std::string dict = std::string("{'descr': '") + descr + "', 'fortran_order': False, 'shape': (";
    dict += std::to_string(rows);
    dict += ", ";
    dict += std::to_string(cols);
    dict += "), }";

    // The header must be padded with spaces so that magic(6)+version(2)+len(2)+header is a
    // multiple of 64, with the final header byte a newline.
    const std::size_t prefix = 6 + 2 + 2; // magic + version + uint16 length field
    std::size_t const total = prefix + dict.size() + 1; // +1 for the trailing '\n'
    const std::size_t pad = (64 - (total % 64)) % 64;
    dict.append(pad, ' ');
    dict.push_back('\n');

    std::string npy;
    npy.append("\x93NUMPY", 6);    // magic
    npy.push_back('\x01');          // version major
    npy.push_back('\x00');          // version minor
    putU16(npy, static_cast<std::uint16_t>(dict.size())); // header length (LE)
    npy += dict;

    // The raw payload in C order: row-major, i.e. all columns of row 0, then row 1...
    // Each value is written in its native (little-endian) byte width matching `descr`.
    npy.reserve(npy.size() + rows * cols * elem_size);
    for(std::size_t r = 0; r < rows; ++r) {
        for(std::size_t c = 0; c < cols; ++c) {
            std::visit([&npy, r](const auto *v) {
                const auto val = (*v)[r];
                char bytes[sizeof(val)];
                std::memcpy(bytes, &val, sizeof(val)); // native (little-endian) order
                npy.append(bytes, sizeof(val));
            }, columns[c]);
        }
    }
    return npy;
}

/** @brief One member to be stored in the .npz ZIP: its archive name and raw bytes. */
struct zipMember {
    std::string name;
    std::string data;
};

/** @brief Pack the members into a single uncompressed ("store", method 0) ZIP -- which IS a
 *  .npz. Each member gets a local file header + its bytes; a central directory and an
 *  end-of-central-directory record close the archive. DOS date/time are left zero (numpy
 *  ignores them) so the bytes are deterministic. */
// NOLINTNEXTLINE(readability-function-size) -- one coherent ZIP binary-format kernel (local file headers, central directory, end-of-central-directory record); splitting would scatter the tightly-coupled offset/size bookkeeping across functions
std::string buildZip(const std::vector<zipMember> &members) {
    std::string out;
    struct cdEntry {
        std::string name;
        std::uint32_t crc;
        std::uint32_t size;
        std::uint32_t offset;
    };
    std::vector<cdEntry> directory;

    for(const auto &m : members) {
        const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32(m.data);
        const std::uint32_t size = static_cast<std::uint32_t>(m.data.size());

        // Local file header.
        putU32(out, 0x04034b50u);                                  // local file header signature
        putU16(out, 20);                                            // version needed to extract (2.0)
        putU16(out, 0);                                            // general purpose bit flag
        putU16(out, 0);                                            // compression method 0 (store)
        putU16(out, 0);                                            // last mod file time
        putU16(out, 0);                                            // last mod file date
        putU32(out, crc);                                          // CRC-32
        putU32(out, size);                                         // compressed size (== uncompressed)
        putU32(out, size);                                         // uncompressed size
        putU16(out, static_cast<std::uint16_t>(m.name.size()));    // file name length
        putU16(out, 0);                                            // extra field length
        out += m.name;                                            // file name
        out += m.data;                                            // the member bytes

        directory.push_back({m.name, crc, size, offset});
    }

    // Central directory.
    const std::uint32_t cd_offset = static_cast<std::uint32_t>(out.size());
    for(const auto &e : directory) {
        putU32(out, 0x02014b50u);                                  // central file header signature
        putU16(out, 20);                                           // version made by
        putU16(out, 20);                                           // version needed to extract
        putU16(out, 0);                                            // general purpose bit flag
        putU16(out, 0);                                            // compression method 0 (store)
        putU16(out, 0);                                            // last mod file time
        putU16(out, 0);                                            // last mod file date
        putU32(out, e.crc);                                        // CRC-32
        putU32(out, e.size);                                       // compressed size
        putU32(out, e.size);                                       // uncompressed size
        putU16(out, static_cast<std::uint16_t>(e.name.size()));    // file name length
        putU16(out, 0);                                            // extra field length
        putU16(out, 0);                                            // file comment length
        putU16(out, 0);                                            // disk number start
        putU16(out, 0);                                            // internal file attributes
        putU32(out, 0);                                            // external file attributes
        putU32(out, e.offset);                                     // relative offset of local header
        out += e.name;                                            // file name
    }
    const std::uint32_t cd_size = static_cast<std::uint32_t>(out.size()) - cd_offset;

    // End of central directory record.
    putU32(out, 0x06054b50u);                                      // EOCD signature
    putU16(out, 0);                                                // number of this disk
    putU16(out, 0);                                                // disk where CD starts
    putU16(out, static_cast<std::uint16_t>(directory.size()));     // CD records on this disk
    putU16(out, static_cast<std::uint16_t>(directory.size()));     // total CD records
    putU32(out, cd_size);                                          // size of central directory
    putU32(out, cd_offset);                                        // offset of central directory
    putU16(out, 0);                                                // comment length
    return out;
}

/** @brief Render all captured series as a numpy .npz: one float64 .npy member per series
 *  (`series_0`, `series_1`, ...) plus a `manifest.json`. The manifest is a self-describing
 *  object -- the canvas (title + pad grid) and an ordered `series` array -- so (data +
 *  manifest) fully describes the multi-pad figure for an external renderer. Each series
 *  entry is the plotter's GPlotSpec augmented with the `pad` it draws into and a
 *  `secondary` flag (true if it overlays a primary in that pad). */
std::string emitNpz(const std::vector<dataSeries> &series, const canvasInfo &canvas) {
    std::vector<zipMember> members;

    std::string manifest = "{\n";
    manifest += "  \"canvas\": {\"label\": \"" + GPlotSpec::jsonEscape(canvas.label)
        + "\", \"c_x_div\": " + std::to_string(canvas.c_x_div)
        + ", \"c_y_div\": " + std::to_string(canvas.c_y_div) + "},\n";
    manifest += "  \"series\": [\n";
    for(const auto &[si, s] : series | std::views::enumerate) {
        const std::size_t rows = seriesRows(s);

        // The .npy member.
        zipMember member;
        member.name = "series_" + std::to_string(si) + ".npy";
        member.data = buildNpy(s.columns, rows);
        members.push_back(std::move(member));

        // The manifest entry: the plotter's GPlotSpec JSON ({...}) augmented in-place
        // with its pad / secondary placement (splice before the closing brace).
        std::string spec_json = s.spec.toJson();
        spec_json.pop_back(); // drop the trailing '}'
        spec_json += ", \"pad\": " + std::to_string(s.pad)
            + ", \"secondary\": " + (s.secondary ? "true" : "false") + "}";
        manifest += "    " + spec_json;
        manifest += (si + 1 == std::ssize(series) ? "\n" : ",\n");
    }
    manifest += "  ]\n}\n";

    members.push_back({"manifest.json", manifest});

    return buildZip(members);
}

/** @brief Collect the exportable series from an ordered list of plotters (skipping the
 *  dataless function plotters), including each plotter's secondary plotters. Shared by
 *  both the CSV and NPZ paths; the plotter list is supplied by the friend emitter member
 *  (free functions cannot reach GPlotDesigner's private plotter container). */
std::vector<dataSeries> collectSeries(
    const std::vector<std::shared_ptr<GBasePlotter>> &plotters
) {
    std::vector<dataSeries> series;
    // The pad index is the primary's registration index (matching the render emitters,
    // which place plotter i into pad i); its secondary plotters overlay the same pad. A
    // dataless plotter (function plotter) still consumes its pad index, so the exported
    // pad numbers line up with where ROOT / matplotlib would draw each plot.
    for(const auto &[idx, p] : plotters | std::views::enumerate) {
        const auto pad = static_cast<std::size_t>(idx);
        if(auto s = captureSeries(*p)) {
            s->pad = pad;
            s->secondary = false;
            series.push_back(std::move(*s));
        }
        // Secondary plotters are independent datasets sharing the primary's pad.
        for(const auto &sp : p->secondaryPlotters()) {
            if(auto s = captureSeries(*sp)) {
                s->pad = pad;
                s->secondary = true;
                series.push_back(std::move(*s));
            }
        }
    }
    return series;
}

} // anonymous namespace

/******************************************************************************/
/**
 * Constructs the emitter in the requested export format (CSV or NPZ).
 *
 * @param format The on-disk format to export
 */
GDataEmitter::GDataEmitter(dataFormat format) : format_(format) { /* nothing */ }

/******************************************************************************/
/**
 * The file extension for the selected format.
 *
 * @return ".csv" in CSV mode, ".npz" in NPZ mode
 */
std::string GDataEmitter::fileExtension() const {
    return format_ == dataFormat::NPZ ? std::string(".npz") : std::string(".csv");
}

/******************************************************************************/
/**
 * The export format this emitter was constructed with.
 *
 * @return The current dataFormat
 */
dataFormat GDataEmitter::getDataFormat() const {
    return format_;
}

/******************************************************************************/
/**
 * Exports each registered plotter's raw columnar series data (NOT a rendered plot). The
 * graph plotters (GGraph2D / GGraph2ED / GGraph3D / GGraph4D) and the histogram plotters
 * (GHistogram1D / GHistogram1I / GHistogram2D) export their axis columns; the function plotters
 * (GFunctionPlotter1D / GFunctionPlotter2D) carry no sampled data and are skipped. The
 * result is either a human-inspectable CSV document or the raw bytes of a numpy .npz
 * archive (which numpy.load() reads back as a dict of float64 arrays).
 *
 * @param gpd The designer holding the plotters
 * @return The CSV text or the raw .npz bytes (a std::string holds embedded NULs intact)
 */
std::string GDataEmitter::emitDocument(const GPlotDesigner &gpd) const {
    const std::vector<dataSeries> series = collectSeries(gpd.plotters_cnt_);
    const canvasInfo canvas{gpd.getCanvasLabel(), gpd.c_x_div_, gpd.c_y_div_};
    return format_ == dataFormat::NPZ ? emitNpz(series, canvas) : emitCsv(series, canvas);
}

/******************************************************************************/
} /* namespace Gem::Dietrich */
