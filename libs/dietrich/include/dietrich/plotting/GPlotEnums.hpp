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

// Standard header files go here
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GCommonEnums.hpp"
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GCommonInterfaceT.hpp"
#include "common/GCommonMathHelperFunctionsT.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GExpectationChecksT.hpp"
#include "common/GLogger.hpp"
#include "common/GContainerT.hpp"
#include "common/GSerializationHelperFunctionsT.hpp"
#include "common/GTupleIO.hpp"
#include "common/GTypeTraitsT.hpp"

namespace Gem::Dietrich {


// Re-export the shared numeric enum stream operators (see numeric_enum_io_v in
// GCommonEnums.hpp) into Gem::Dietrich, so that argument-dependent lookup finds
// them for the plotting enums below (a using-DIRECTIVE is invisible to ADL;
// these using-declarations are not).
using Gem::Common::operator<<;
using Gem::Common::operator>>;

// dietrich builds on a small, explicit set of common's names that participate in its
// own API surface (enum base type, comparison tokens, member reflection, dimension
// tags). They are imported per-name here -- the public headers deliberately do NOT
// re-export all of Gem::Common with a using-directive (that leaked every common name
// to all consumers and has already caused a shadowing incident with to_string).
using Gem::Common::compare_t;
using Gem::Common::dimensions;
using Gem::Common::ENUMBASETYPE;
using Gem::Common::expectation;
using Gem::Common::g_convert_and_compare;
using Gem::Common::g_load_members;
using Gem::Common::GCommonInterfaceT;
using Gem::Common::GPtrContainerT;
using Gem::Common::getMinMax;
using Gem::Common::GToken;
using Gem::Common::make_member;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * An enum for some basic colors (to be extended over time)
 */
enum class gColor : ENUMBASETYPE {
    white = 0,
    black = 1,
    red = 2,
    green = 3,
    blue = 4,
    grey = 14 // note the id of this color, compared to preceding values
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * An enum for basic marker types (to be extended over time)
 */
enum class gMarker : ENUMBASETYPE {
    none = 0,
    openCircle = 4,
    closedCircle = 20,
    closedTriangle = 22,
    openTriangle = 26,
    closedStar = 29,
    openStar = 30
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/*******************************************************************	***********/
/**
 * An enum for basic line styles (to be extended over time)
 */
enum class gLineStyle : ENUMBASETYPE {
    straight = 1,
    shortdashed = 2,
    dotted = 3,
    shortdashdot = 4,
    // Previously also `= 4` (collision with shortdashdot — `>>`/`<<`
    // round-trips would collapse the two names onto a single line style).
    // Moved to the next free slot below shortdashdot/longdashed. No call
    // sites reference `longdashdot` directly, so changing the value is
    // observably a fix rather than a breaking change.
    longdashdot = 5,
    longdashdotdot = 6, ///< maps to ROOT TAttLine style 6
    longdashed = 7
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * Determines whether a scatter plot or a curve should be recorded
 */
enum class graphPlotMode : ENUMBASETYPE {
    SCATTER = 0,
    CURVE = 1
};

/******************************************************************************/
/**
 * Where a 3-d graph drawn as a surface shows its colour map in addition to the surface itself:
 * nowhere (the plain surface), on the floor of the plot box (at the lowest z of the data), or on
 * its ceiling (at the highest). A projected map is the surface seen from above, so markers drawn at
 * that height -- an overlay whose z is the data's minimum or maximum -- land on the map.
 */
enum class surfaceProjection : ENUMBASETYPE {
    NONE = 0,
    FLOOR = 1,
    CEILING = 2
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * An enum for 2D-drawing options
 */
enum class tddropt : ENUMBASETYPE {
    TDEMPTY = 0,
    SURFONE = 1,
    SURFTWOZ = 2,
    SURFTHREE = 3,
    SURFFOUR = 4,
    CONTZ = 5,
    CONTONE = 6,
    CONTTWO = 7,
    CONTTHREE = 8,
    TEXT = 9,
    SCAT = 10,
    BOX = 11,
    ARR = 12,
    COLZ = 13,
    LEGO = 14,
    LEGOONE = 15,
    SURFONEPOL = 16,
    SURFONECYL = 17
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

//Some default values

constexpr std::uint32_t DEFCXDIM = 1024;
constexpr std::uint32_t DEFCYDIM = 768;

constexpr std::size_t DEFNINDENTIONSPACES = 3;

constexpr std::size_t DEFNSAMPLES = 100;

const graphPlotMode DEFPLOTMODE = graphPlotMode::CURVE;

constexpr double DEFMINMARKERSIZE = 0.001;
constexpr double DEFMAXMARKERSIZE = 1.;

// Forward declaration in order to allow a friend statement in GBasePlotter
class GPlotDesigner;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////

} /* namespace Gem::Dietrich */

/******************************************************************************/
// All plotting enums stream as their underlying numeric value through the shared
// machinery in GCommonEnums.hpp (the marker specializations live in the trait's
// own namespace; the operators themselves are re-exported into Gem::Dietrich
// above).
namespace Gem::Common {
template <> inline constexpr bool numeric_enum_io_v<Gem::Dietrich::gColor> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Dietrich::gMarker> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Dietrich::gLineStyle> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Dietrich::graphPlotMode> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Dietrich::surfaceProjection> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Dietrich::tddropt> = true;
} /* namespace Gem::Common */
