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

/**
 * @file
 * @brief Umbrella header re-exporting the full Geneva plotting stack.
 *
 * The plotting code is split into focused headers under dietrich/plotting/. This umbrella keeps
 * a single #include "dietrich/GPlotDesigner.hpp" working; it pulls in the full plotting stack (each
 * plotting header also stands alone and chain-includes its own dependencies). The public plotting
 * API (GPlotDesigner, the GBasePlotter hierarchy, etc.) is declared and documented in
 * dietrich/plotting/GPlotDesigner.hpp and the sibling plotting headers it includes.
 */
#include "dietrich/plotting/GPlotDesigner.hpp"
