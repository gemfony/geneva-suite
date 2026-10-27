/**
* @file GImageHelperFunctions.hpp
 */

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

// Includes check for correct Boost version(s)
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <string>
#include <vector>

// Third party header files go here
#include <png.h>

// Geneva header files go here
#include "common/GCommonHelperFunctionsT.hpp"

/**
 * This file is meant to hold functions to load and save RGB data structures
 * to various graphics formats on disc. The file currently only holds implementations
 * for the PNG format
 */
namespace Gem::Common {
/** @brief Loads a PNG file from disk and outputs an RGB array (8 bits/channel) */
bool loadPngToRGB(const std::string &filename, std::vector<unsigned char> &, int &, int &);

/** @brief Writes a raw 8-bit RGB array (3 bytes per pixel) to a PNG file */
bool writeRGBtoPNG(const std::string &, const std::vector<unsigned char> &, int, int);

/** @brief Transfers an image to a local data structure in RGB format */
bool loadImageToRGB(const std::string &, std::vector<unsigned char> &, int &, int &);

/** @brief Transfers an image to a local data structure with color channels encoded as floats */
bool loadImageToFloat(const std::string &, std::vector<float> &, int &, int &);

/** @brief Writes an image in RGB format to disc */
bool saveRGBImageToFile(
    const std::string &,
    const std::vector<unsigned char> &,
    const int,
    const int
);

/** @brief Writes an image in float format to disc */
bool saveFloatImageToFile(const std::string &, const std::vector<float> &, const int, const int);
} /* namespace Gem::Common */
