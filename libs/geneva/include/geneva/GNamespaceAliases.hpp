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
 * Geneva's short namespace aliases: `oa` for the optimization algorithms, `gen` for the
 * genome, `gind` for the individuals and `gbm` for the benchmarks. Every Geneva header that
 * uses one of them includes this file, and so does the root of the genome classes
 * (genome/GGenome.hpp). A header provides the aliases if it includes one of these, directly or
 * through another header -- GOptimizer.hpp and the headers of the individuals' and the algorithms'
 * classes do. Not every Geneva header does (genome/GGenomeBuilder.hpp, GOptimizationEnums.hpp
 * and oa/GLineSearch.hpp, for example): a program that uses an alias and includes only such
 * headers includes this file itself. The target namespaces are only declared here, which adds
 * no dependency.
 * (The namespace Gem::Geneva::Interface has no short alias; it is named in full.)
 *
 * The aliases are Geneva's alone: the lower libraries of the collection (weft, common, hap,
 * dietrich, courtier) neither declare nor use them, so a program built on those alone is free
 * to use these names for itself.
 */

namespace Gem::Geneva::OptimizationAlgorithms {}
namespace Gem::Geneva::Genome {}
namespace Gem::Geneva::Individuals {}
namespace Gem::Geneva::Benchmarks {}

namespace oa = Gem::Geneva::OptimizationAlgorithms;
namespace gen = Gem::Geneva::Genome;
namespace gind = Gem::Geneva::Individuals;
namespace gbm = Gem::Geneva::Benchmarks;
