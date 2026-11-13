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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard headers
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

// Geneva headers
#include "GImageHelperFunctions.hpp"
#include "GImageScalar.hpp"

/**
 * Shared CPU math for the Mona-Lisa problem of example 14. The fitness of a candidate is the
 * deviation of the alpha-blended triangle superimposition from a TARGET image, summed per pixel and
 * channel through a rational saturation function -- exactly the metric example 14's GPU kernel uses.
 *
 * These pure host functions back the individual's evaluate() (CPU); the runtime CUDA kernel
 * (kernels/monalisa_eval_*.cu) replicates the SAME formulas, so a CPU run (via a CPU consumer such as
 * --consumer stc) and a GPU run give the same fitness and the GPU can be cross-checked against the CPU.
 *
 * The scalar type is selected at COMPILE TIME via gimage_fp_t (GImageScalar.hpp): DOUBLE by default,
 * or FLOAT when the example is built with GIMAGE_USE_FLOAT. Double precision makes the CPU/GPU
 * cross-check tight (parity to ~1e-9); float is the faster FP32 path. The matching device kernel is
 * selected through the default GPU-consumer config.
 *
 * Genome layout (flat, == GGenome::streamline order; alpha-sort disabled so order is canonical):
 *   per triangle (10 scalars): cx, cy, radius, angle1, angle2, angle3, r, g, b, alpha   (all in [0,1])
 *   then the background colour (3 scalars): bgR, bgG, bgB.   dim = 10*nTriangles + 3.
 */
namespace Gem::Geneva::MonaLisa {

constexpr gimage_fp_t RSF_FACTOR = gimage_fp_t(0.04); ///< rational-saturation-function factor (matches example 14)

/** @brief The target image, loaded once from a PNG: width/height and W*H*3 RGB scalars in [0,1]. */
struct Target {
    int width = 0;
    int height = 0;
    std::vector<gimage_fp_t> rgb; ///< size width*height*3
};

/** @brief The process-wide target (set once before any evaluation). */
inline Target &mutableTarget() {
    static Target t;
    return t;
}
inline const Target &target() { return mutableTarget(); }

/** @brief Loads @p path (PNG) into the process-wide target. Throws on failure. */
inline void loadTarget(const std::string &path) {
    std::vector<float> rgbf;
    int w = 0;
    int h = 0;
    if(not Gem::Common::loadImageToFloat(path, rgbf, w, h)) {
        throw std::runtime_error("GMonaLisaProblem: could not load target image '" + path + "'");
    }
    Target &t = mutableTarget();
    t.width = w;
    t.height = h;
    // loadImageToFloat always yields float; assign into the (possibly double) target buffer.
    t.rgb.assign(rgbf.begin(), rgbf.end());
}

/** @brief The three corner coordinates of a circle-triangle, scaled to image space. */
inline void calculateCorners(const gimage_fp_t *tri, int width, int height, gimage_fp_t *out6) {
    const gimage_fp_t cx = tri[0] * width;
    const gimage_fp_t cy = tri[1] * height;
    const gimage_fp_t radius = tri[2];
    const gimage_fp_t scale = static_cast<gimage_fp_t>(width < height ? width : height);
    const gimage_fp_t twoPi = gimage_fp_t(2) * std::numbers::pi_v<gimage_fp_t>;
    for(int k = 0; k < 3; ++k) {
        const gimage_fp_t ang = tri[3 + k] * twoPi;
        out6[k * 2 + 0] = cx + radius * std::cos(ang) * scale;
        out6[k * 2 + 1] = cy + radius * std::sin(ang) * scale;
    }
}

/** @brief Point-in-triangle test (bounding-box reject + three edge signs), matching the kernel. */
inline bool pointInTriangle(gimage_fp_t px, gimage_fp_t py, const gimage_fp_t *c) {
    const gimage_fp_t x1 = c[0], y1 = c[1], x2 = c[2], y2 = c[3], x3 = c[4], y3 = c[5];
    const gimage_fp_t minx = std::min(x1, std::min(x2, x3));
    const gimage_fp_t maxx = std::max(x1, std::max(x2, x3));
    const gimage_fp_t miny = std::min(y1, std::min(y2, y3));
    const gimage_fp_t maxy = std::max(y1, std::max(y2, y3));
    if(px < minx || px > maxx || py < miny || py > maxy) {
        return false;
    }
    const gimage_fp_t d1 = (px - x2) * (y1 - y2) - (py - y2) * (x1 - x2);
    const gimage_fp_t d2 = (px - x3) * (y2 - y3) - (py - y3) * (x2 - x3);
    const gimage_fp_t d3 = (px - x1) * (y3 - y1) - (py - y1) * (x3 - x1);
    const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return not(hasNeg && hasPos);
}

/**
 * @brief Renders the flat genome @p params (dim = 10*NT+3) over a @p W x @p H canvas and returns the
 * rational-saturation deviation from @p targetRGB (W*H*3). Corners are precomputed once per triangle.
 */
inline gimage_fp_t score(const gimage_fp_t *params, int dim, int W, int H, const gimage_fp_t *targetRGB,
                         std::vector<gimage_fp_t> &cornerScratch) {
    const int NT = (dim - 3) / 10;
    const gimage_fp_t bgR = params[10 * NT + 0];
    const gimage_fp_t bgG = params[10 * NT + 1];
    const gimage_fp_t bgB = params[10 * NT + 2];

    cornerScratch.resize(static_cast<std::size_t>(NT) * 6);
    for(int t = 0; t < NT; ++t) {
        calculateCorners(params + t * 10, W, H, cornerScratch.data() + t * 6);
    }

    gimage_fp_t sum = gimage_fp_t(0);
    for(int y = 0; y < H; ++y) {
        for(int x = 0; x < W; ++x) {
            const gimage_fp_t px = x + gimage_fp_t(0.5);
            const gimage_fp_t py = y + gimage_fp_t(0.5);
            gimage_fp_t r = bgR, g = bgG, b = bgB;
            for(int t = 0; t < NT; ++t) {
                if(pointInTriangle(px, py, cornerScratch.data() + t * 6)) {
                    const gimage_fp_t *tri = params + t * 10;
                    const gimage_fp_t a = tri[9];
                    r = (gimage_fp_t(1) - a) * r + a * tri[6];
                    g = (gimage_fp_t(1) - a) * g + a * tri[7];
                    b = (gimage_fp_t(1) - a) * b + a * tri[8];
                }
            }
            const std::size_t idx = (static_cast<std::size_t>(y) * W + x) * 3;
            const gimage_fp_t dr = targetRGB[idx] - r;
            const gimage_fp_t dg = targetRGB[idx + 1] - g;
            const gimage_fp_t db = targetRGB[idx + 2] - b;
            sum += dr * dr / (dr * dr + RSF_FACTOR);
            sum += dg * dg / (dg * dg + RSF_FACTOR);
            sum += db * db / (db * db + RSF_FACTOR);
        }
    }
    return sum;
}

/**
 * @brief Renders the flat genome @p params (dim = 10*NT+3) into an 8-bit RGB buffer (W*H*3 bytes,
 * row-major, 3 bytes per pixel) using the SAME alpha-blend rasterisation as score(). This is what the
 * image-output monitor saves to disk so the evolving candidate can be watched as a picture.
 */
inline void renderToRGB(const gimage_fp_t *params, int dim, int W, int H,
                        std::vector<unsigned char> &rgb_out) {
    const int NT = (dim - 3) / 10;
    const gimage_fp_t bgR = params[10 * NT + 0];
    const gimage_fp_t bgG = params[10 * NT + 1];
    const gimage_fp_t bgB = params[10 * NT + 2];

    std::vector<gimage_fp_t> corners(static_cast<std::size_t>(NT) * 6);
    for(int t = 0; t < NT; ++t) {
        calculateCorners(params + t * 10, W, H, corners.data() + t * 6);
    }

    rgb_out.resize(static_cast<std::size_t>(W) * H * 3);
    auto toByte = [](gimage_fp_t v) -> unsigned char {
        return static_cast<unsigned char>(std::clamp(v, gimage_fp_t(0), gimage_fp_t(1)) * gimage_fp_t(255) + gimage_fp_t(0.5));
    };
    for(int y = 0; y < H; ++y) {
        for(int x = 0; x < W; ++x) {
            const gimage_fp_t px = x + gimage_fp_t(0.5);
            const gimage_fp_t py = y + gimage_fp_t(0.5);
            gimage_fp_t r = bgR, g = bgG, b = bgB;
            for(int t = 0; t < NT; ++t) {
                if(pointInTriangle(px, py, corners.data() + t * 6)) {
                    const gimage_fp_t *tri = params + t * 10;
                    const gimage_fp_t a = tri[9];
                    r = (gimage_fp_t(1) - a) * r + a * tri[6];
                    g = (gimage_fp_t(1) - a) * g + a * tri[7];
                    b = (gimage_fp_t(1) - a) * b + a * tri[8];
                }
            }
            const std::size_t idx = (static_cast<std::size_t>(y) * W + x) * 3;
            rgb_out[idx + 0] = toByte(r);
            rgb_out[idx + 1] = toByte(g);
            rgb_out[idx + 2] = toByte(b);
        }
    }
}

/** @brief Convenience: score a genome against the process-wide target. */
inline gimage_fp_t scoreAgainstTarget(const gimage_fp_t *params, int dim) {
    const Target &t = target();
    static thread_local std::vector<gimage_fp_t> scratch;
    return score(params, dim, t.width, t.height, t.rgb.data(), scratch);
}

} /* namespace Gem::Geneva::MonaLisa */
