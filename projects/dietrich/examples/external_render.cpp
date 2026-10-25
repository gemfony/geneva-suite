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

/**
 * @file external_render.cpp
 * @brief The DATA backend: export the raw series data instead of a rendered plot.
 *
 * The DATA backend writes the columnar series data plus a self-describing manifest
 * (canvas + per-series plot kind / labels / bins), so rendering can happen entirely
 * OUTSIDE the C++ library. Two on-disk formats are shown: a human-inspectable CSV and
 * a binary numpy .npz. The bundled scripts/geneva_plot_render.py turns either back
 * into a figure.
 */

#include <iostream>

#include "dietrich/GPlotDesigner.hpp"

using namespace Gem::Dietrich;

int main() {
    GDataLog log("Exported data", 1, 1);

    GPlotSpec spec(plotKind::graph_2d);
    spec.plot_mode = graphPlotMode::CURVE;
    spec.name = "y = x^2";
    spec.x_label = "x";
    spec.y_label = "x^2";
    spec.columns = {"x", "y"};
    const auto id = log.declareSeries(spec);
    for(int i = 0; i <= 100; ++i) {
        const double x = 0.1 * static_cast<double>(i) - 5.;
        log.append(id, x, x * x);
    }

    //! [dietrich-data-export]
    // CSV is the DATA backend's default format -- one writeToFile() through GDataLog.
    log.writeToFile("exported.csv", plotBackend::DATA);

    // The binary numpy .npz needs the designer-level format control, so realize the
    // log into a GPlotDesigner and select the format explicitly.
    GPlotDesigner gpd = log.toDesigner();
    gpd.setPlotBackend(plotBackend::DATA);
    gpd.setDataFormat(dataFormat::NPZ);
    gpd.writeToFile("exported.npz");
    //! [dietrich-data-export]

    std::cout << "external_render: wrote exported.csv and exported.npz\n"
              << "Render outside C++ with the bundled renderer, e.g.:\n"
              << "  python3 dietrich/scripts/geneva_plot_render.py exported.npz -o exported.png\n";
    return 0;
}
