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
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GReflectiveInterfaceT.hpp"
#include "common/GParserBuilder.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/genome/GGenomeT.hpp"
#include "geneva/genome/GIndividualFactory.hpp"
#include "geneva/genome/GGenomeBuilder.hpp"
#include "geneva/genome/GGenomeFactory.hpp"
#include "geneva/genome/GGenomeMultiConstraint.hpp"
#include "common/GSelfTestable.hpp"

namespace Gem::Geneva::OptimizationAlgorithms {
class GAdaptionConfigBase;
} // namespace Gem::Geneva::OptimizationAlgorithms

namespace Gem::Geneva::Individuals {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Enumerates all available benchmark test functions.
 *
 * The functions span a range of difficulty classes, landscape topologies, and
 * algorithmic challenges, making them suitable for systematic algorithm benchmarking
 * across multiple dimensions. Each entry documents its landscape properties and the
 * class of algorithmic weakness it is designed to expose.
 *
 * Unless stated otherwise, all functions are defined for n ≥ 1 dimensions.
 * Recommended search domains per function are documented individually below.
 */
enum class solverFunction : Gem::Common::ENUMBASETYPE {
    /**
	 * Simple n-dimensional parabola: f(x) = Σxᵢ².
	 * Global minimum: f=0 at x=(0,...,0). Recommended domain: any (default [-10,10]).
	 * Unimodal, convex, separable. Serves as a baseline sanity check to verify
	 * that any algorithm can locate a trivially placed, convex global optimum.
	 * Performance on this function establishes the lower bound of expected runtime.
	 */
    PARABOLA = 0,

    /**
	 * Berlich noisy parabola: f(x) = (cos(‖x‖²)+2)·‖x‖².
	 * Global minimum: f=0 at x=(0,...,0). Recommended domain: [-4π, 4π].
	 * Multimodal, radially symmetric, non-separable. The cosine overlay creates
	 * a dense ring structure of local optima around the origin. Tests whether
	 * an algorithm can escape near-origin local optima while the overall
	 * gradient-free global structure remains simple (decreasing with ‖x‖).
	 */
    NOISYPARABOLA = 1,

    /**
	 * Generalized Rosenbrock function: f(x) = Σ[100(xᵢ₊₁-xᵢ²)²+(1-xᵢ)²].
	 * Requires n ≥ 2. Global minimum: f=0 at x=(1,...,1). Domain: [-2, 2].
	 * Unimodal for n≤3, strongly non-separable, with a narrow, curved banana-shaped
	 * valley. The gradient along the valley floor is nearly zero, making gradient
	 * descent slow and gradient-free methods prone to overshooting. A classic
	 * benchmark for non-separable slow-convergence scenarios.
	 */
    ROSENBROCK = 2,

    /**
	 * Modified Ackley variant (Geneva-specific, pairwise sum form).
	 * f(x) = Σ[exp(-0.2)·√(xᵢ²+xᵢ₊₁²) + 3·(cos(2xᵢ)+sin(2xᵢ₊₁))].
	 * Requires n ≥ 2. Non-separable, multimodal.
	 * NOTE: This is NOT the canonical Ackley function. It accumulates n-1 pairwise
	 * terms and has a different, numerically approximated global minimum location.
	 * Retained for backward compatibility with existing configurations.
	 * For the standard CEC/BBOB benchmark formulation, use ACKLEY_CANONICAL (8).
	 */
    ACKLEY = 3,

    /**
	 * Rastrigin function: f(x) = 10n + Σ[xᵢ²-10·cos(2πxᵢ)].
	 * Global minimum: f=0 at x=(0,...,0). Recommended domain: [-5.12, 5.12].
	 * Highly multimodal, separable. Contains approximately 10ⁿ regularly spaced
	 * local minima arranged in a regular grid, all of similar depth. Each dimension
	 * can in principle be treated independently, but the density of minima makes
	 * high-dimensional instances very challenging for population-based methods.
	 * A standard benchmark for multimodal optimisation in evolutionary computation.
	 */
    RASTRIGIN = 4,

    /**
	 * Schwefel function: f(x) = -1/n · Σ xᵢ·sin(√|xᵢ|).
	 * Global minimum: f≈-418.9829/n at xᵢ≈420.9687. Recommended domain: [-500, 500].
	 * NOTE: Geneva normalises by 1/n; the standard formulation does not.
	 * Deceptive: the global optimum lies far from the origin and near the boundary
	 * of the search space. Secondary optima cluster near the origin, attracting
	 * population members that initialise or drift inward. Tests resistance to
	 * deceptive attractor basins and global exploration far from the initialisation.
	 */
    SCHWEFEL = 5,

    /**
	 * Salomon function: f(x) = -cos(2π‖x‖) + 0.1·‖x‖ + 1.
	 * Global minimum: f=0 at x=(0,...,0). Recommended domain: [-100, 100].
	 * Multimodal, radially symmetric, non-separable. Creates concentric spherical
	 * shells of local optima at ‖x‖ = k for integer k. The very gradual linear
	 * increase of 0.1·‖x‖ provides weak directional guidance. Tests the ability
	 * to follow a radial gradient in the presence of strong perpendicular
	 * oscillations across the shells.
	 */
    SALOMON = 6,

    /**
	 * Negative parabola: f(x) = -Σxᵢ².
	 * Global maximum: f=0 at x=(0,...,0). Recommended domain: any.
	 * Used exclusively for maximisation tests to verify that Geneva's internal
	 * maximisation mode (Go2 maxMode) functions correctly. The
	 * landscape is identical to PARABOLA but sign-inverted.
	 */
    NEGPARABOLA = 7,

    /**
	 * Canonical n-dimensional Ackley function.
	 * f(x) = -20·exp(-0.2·√(1/n·Σxᵢ²)) - exp(1/n·Σcos(2πxᵢ)) + 20 + e.
	 * Global minimum: f=0 at x=(0,...,0). Recommended domain: [-32.768, 32.768].
	 * Non-separable, multimodal. The outer region of the landscape is almost flat
	 * (nearly zero gradient for large ‖x‖), suddenly dropping into a narrow global
	 * basin at the origin. Gradient-based methods stall on the plateau; stochastic
	 * and population-based methods are generally more effective. The canonical
	 * formulation used in all CEC and BBOB benchmark suites.
	 */
    ACKLEY_CANONICAL = 8,

    /**
	 * Griewank function: f(x) = 1/4000·Σxᵢ² - Πcos(xᵢ/√i) + 1.
	 * Global minimum: f=0 at x=(0,...,0). Recommended domain: [-600, 600].
	 * Weakly non-separable (the product term introduces cross-dimension coupling),
	 * multimodal. At large scale the landscape is nearly quadratic; the product
	 * of cosines creates a fine-grained multimodal structure at smaller scale.
	 * With growing dimension the local minima become denser but the global basin
	 * remains identifiable through the quadratic envelope. Distinguishes algorithms
	 * that exploit global structure from purely local explorers.
	 */
    GRIEWANK = 9,

    /**
	 * Lévy function.
	 * f(x) = sin²(πw₁) + Σᵢ₌₁ⁿ⁻¹[(wᵢ-1)²(1+10sin²(πwᵢ₊₁))] + (wₙ-1)²(1+sin²(2πwₙ)),
	 * with wᵢ = 1+(xᵢ-1)/4.
	 * Global minimum: f=0 at x=(1,...,1). Recommended domain: [-10, 10].
	 * Separable, multimodal. The regular sin²-based ripple structure creates narrow,
	 * closely spaced local minima around each dimension's valley. Tests fine-grained
	 * local search precision: the algorithm must resolve the global minimum from its
	 * immediate neighbours, which become progressively closer in value as dimension
	 * grows. Widely used in the IEEE CEC benchmark suite.
	 */
    LEVY = 10,

    /**
	 * Styblinski-Tang function: f(x) = 1/2·Σ(xᵢ⁴-16xᵢ²+5xᵢ).
	 * Global minimum: f≈-39.166·n at xᵢ≈-2.9035. Recommended domain: [-5, 5].
	 * Separable, multimodal, asymmetric. Each dimension has two local minima at
	 * different depths (≈-39.17 at x≈-2.90 and ≈-21.25 at x≈+2.75). Because the
	 * global minimum is not located at the origin, symmetric Gaussian mutation
	 * centred on the initial population introduces a systematic bias against the
	 * global basin. This function exposes initialisation and mutation symmetry
	 * artefacts that PARABOLA and RASTRIGIN cannot reveal.
	 */
    STYBLINSKI_TANG = 11,

    /**
	 * Axis-parallel Ellipsoid function: f(x) = Σ 10^(6i/(n-1))·xᵢ².
	 * Global minimum: f=0 at x=(0,...,0). Recommended domain: [-5, 5].
	 * Unimodal, separable, condition number 10⁶ (ratio of largest to smallest
	 * Hessian eigenvalue). All algorithms eventually find the minimum; the
	 * discriminating metric is convergence speed and final precision. Algorithms
	 * with isotropic mutation (uniform σ for all dimensions) converge slowly
	 * because x₀ requires very large steps while xₙ₋₁ requires very small steps.
	 * Tests the effectiveness of Geneva's self-adaptive σ mechanisms in the EA.
	 */
    ELLIPSOID = 12,

    /**
	 * Michalewicz function: f(x) = -Σ sin(xᵢ)·sin²ᵐ(i·xᵢ²/π), with m=10.
	 * Global minimum: dimension-dependent, not analytically known
	 * (≈-1.8013 for n=2, ≈-4.6877 for n=5, ≈-9.660 for n=10).
	 * Recommended domain: [0, π].
	 * NOTE: Unlike all other functions, the natural domain is [0, π].
	 * Set min_var=0 and max_var≈3.14159 explicitly in the factory configuration.
	 * The exponent m=10 creates extremely narrow ridges. The global minimum lies
	 * inside a steep, razor-thin valley; approaching it requires precise alignment
	 * of the search direction. Tests fine-grained local search and the ability to
	 * follow narrow ridges reliably. The analytically unknown optimum also makes
	 * this function useful for benchmarking solution quality across releases.
	 */
    MICHALEWICZ = 13,

    /**
	 * Zakharov function: f(x) = Σxᵢ² + (Σ0.5·i·xᵢ)² + (Σ0.5·i·xᵢ)⁴.
	 * Global minimum: f=0 at x=(0,...,0). Recommended domain: [-5, 10].
	 * Unimodal, non-separable. The quadratic and quartic terms of the weighted
	 * linear combination 0.5·Σi·xᵢ introduce dimension-weighted interactions:
	 * later dimensions (large index i) are penalised more strongly, creating an
	 * asymmetric, non-separable bowl. Gradient descent handles this well; gradient-
	 * free methods must adapt step sizes per dimension. Tests non-separable
	 * interaction without the confounding effect of multimodality.
	 */
    ZAKHAROV = 14
};

const solverFunction MAXDEMOFUNCTION = solverFunction::ZAKHAROV;

// solverFunction and parameterType stream as their underlying numeric values
// through the shared machinery in GCommonEnums.hpp (marker specializations at
// the end of this header). Re-export the operators so ADL finds them for this
// namespace's enums.
using Gem::Common::operator<<;
using Gem::Common::operator>>;

/**
 * @brief Selects the flat-genome structure a GFunctionIndividual is built with, along two orthogonal
 * axes: whether the parameters are bounded (folded into [min_var, max_var]) or unbounded (that range is
 * only the initialisation perimeter), and whether they share one adaptor group (one common sigma) or get
 * one group each (a per-parameter sigma).
 */
enum class parameterType : Gem::Common::ENUMBASETYPE {
    UNBOUNDED_SHARED = 0,        ///< unbounded parameters, one shared adaptor group
    BOUNDED_SHARED = 1,          ///< bounded parameters, one shared adaptor group
    UNBOUNDED_PER_PARAMETER = 2, ///< unbounded parameters, one adaptor group per parameter
    BOUNDED_PER_PARAMETER = 3    ///< bounded parameters, one adaptor group per parameter
};

// Numeric streaming opt-in for the two enums above; must precede their first
// streaming use (see numeric_enum_io_v in GCommonEnums.hpp).
} /* namespace Gem::Geneva::Individuals */
namespace Gem::Common {
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::Individuals::solverFunction> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::Individuals::parameterType> = true;
} /* namespace Gem::Common */
namespace Gem::Geneva::Individuals {

/******************************************************************************/
// A number of default settings for the factory
constexpr double GFI_DEF_ADPROB = 1.0;
constexpr double GFI_DEF_ADAPTADPROB = 0.1;
constexpr double GFI_DEF_MINADPROB = 0.05;
constexpr double GFI_DEF_MAXADPROB = 1.;
constexpr std::uint32_t GFI_DEF_ADAPTIONTHRESHOLD = 1;
constexpr bool GFI_DEF_USEBIGAUSSIAN = false;
constexpr double GFI_DEF_SIGMA1 = 0.025;
constexpr double GFI_DEF_SIGMASIGMA1 = 0.2;
constexpr double GFI_DEF_MINSIGMA1 = 0.001;
constexpr double GFI_DEF_MAXSIGMA1 = 1;
constexpr double GFI_DEF_SIGMA2 = 0.025;
constexpr double GFI_DEF_SIGMASIGMA2 = 0.2;
constexpr double GFI_DEF_MINSIGMA2 = 0.001;
constexpr double GFI_DEF_MAXSIGMA2 = 1;
constexpr double GFI_DEF_DELTA = 0.05;
constexpr double GFI_DEF_SIGMADELTA = 0.2;
constexpr double GFI_DEF_MINDELTA = 0.001;
constexpr double GFI_DEF_MAXDELTA = 1.;
constexpr std::size_t GFI_DEF_PARDIM = 2;
constexpr double GFI_DEF_MINVAR = -10.;
constexpr double GFI_DEF_MAXVAR = 10.;
const parameterType GFI_DEF_PARAMETERTYPE = parameterType::BOUNDED_PER_PARAMETER;
const solverFunction GO_DEF_EVALFUNCTION = solverFunction::PARABOLA;

/******************************************************************************/
/**
 * @brief An individual that evaluates one of several standard benchmark test functions.
 *
 * GFunctionIndividual is the standard benchmark vehicle for Geneva's optimization algorithms.
 * It supports 15 test functions (solverFunction enum, IDs 0–14) covering unimodal, multimodal,
 * separable, non-separable, ill-conditioned, deceptive, and asymmetric landscapes. The active
 * function is selected via setDemoFunction() or through the factory configuration file.
 *
 * All functions accept arbitrary parameter dimensionality n ≥ 1 (some require n ≥ 2).
 * The factory (GFunctionIndividualFactory) populates the individual's flat genome with n double
 * parameters within [min_var, max_var]; these bounds should match the recommended domain of the
 * selected function (see solverFunction enum documentation).
 *
 * @note For MICHALEWICZ the natural domain is [0, π]. Set min_var=0 and max_var≈3.14159
 *       explicitly; the factory default of [-10, 10] is not suitable for that function.
 */
class GFunctionIndividual
  : public gen::GGenomeT<GFunctionIndividual> // NOLINT(cppcoreguidelines-special-member-functions)
  , public Gem::Common::GSelfTestable
{
    ///////////////////////////////////////////////////////////////////////
    // The archive still default-constructs the concrete type on load; GReflectiveInterfaceAccess lets the mixin
    // reach the private localMembers_() (from which serialize/load_/compare_/clone_/name_ derive).
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() and compare token. */
    static constexpr std::string_view class_name = "GFunctionIndividual";

    using FACTORYTYPE = Gem::Geneva::Genome::GIndividualFactory<GFunctionIndividual>;

    /** @brief The default constructor */
    GFunctionIndividual() = default;
    /**
     * @brief Initialization with the desired demo function.
     * @param d_f The solverFunction this individual should evaluate
     */
    explicit GFunctionIndividual(const solverFunction &d_f);
    /**
     * @brief A standard copy constructor.
     * @param cp A constant reference to another GFunctionIndividual object
     */
    GFunctionIndividual(const GFunctionIndividual &cp) = default;

    /** @brief The standard destructor */
    ~GFunctionIndividual() override = default;

    /**
     * @brief Allows setting the demo function to be evaluated.
     * @param d_f The solverFunction this individual should evaluate
     */
    void setDemoFunction(solverFunction d_f);
    /**
     * @brief Allows retrieving the current demo function.
     * @return The solverFunction currently selected for evaluation
     */
    solverFunction getDemoFunction() const;

    /**
     * @brief Allows to cross check the parameter size.
     * @return The number of double parameters held by this individual's genome
     */
    std::size_t getParameterSize() const;

    //---------------------------------------------------------------------------
    /**
	  * @brief Converts a solverFunction id to a human-readable name string.
	  *
	  * Used primarily for plot labels in GOptimizationBenchmark and similar tools.
	  * The returned string matches the conventional name of the function in the
	  * evolutionary computation literature.
	  *
	  * | ID | Enum                | String                       |
	  * |----|---------------------|------------------------------|
	  * |  0 | PARABOLA            | "Parabola"                   |
	  * |  1 | NOISYPARABOLA       | "Berlich noisy parabola"     |
	  * |  2 | ROSENBROCK          | "Rosenbrock"                 |
	  * |  3 | ACKLEY              | "Ackley (pairwise variant)"  |
	  * |  4 | RASTRIGIN           | "Rastrigin"                  |
	  * |  5 | SCHWEFEL            | "Schwefel"                   |
	  * |  6 | SALOMON             | "Salomon"                    |
	  * |  7 | NEGPARABOLA         | "Negative parabola"          |
	  * |  8 | ACKLEY_CANONICAL    | "Ackley (canonical)"         |
	  * |  9 | GRIEWANK            | "Griewank"                   |
	  * | 10 | LEVY                | "Levy"                       |
	  * | 11 | STYBLINSKI_TANG     | "Styblinski-Tang"            |
	  * | 12 | ELLIPSOID           | "Ellipsoid"                  |
	  * | 13 | MICHALEWICZ         | "Michalewicz (m=10)"         |
	  * | 14 | ZAKHAROV            | "Zakharov"                   |
	  *
	  * @param df The solverFunction identifier
	  * @return Human-readable name of the function
	  */
    static std::string getStringRepresentation(const solverFunction &df) {
        std::string result; // NOLINT(cppcoreguidelines-init-variables)

        switch(df) {
        case solverFunction::PARABOLA:
            result = "Parabola";
            break;
        case solverFunction::NOISYPARABOLA:
            result = "Berlich noisy parabola";
            break;
        case solverFunction::ROSENBROCK:
            result = "Rosenbrock";
            break;
        case solverFunction::ACKLEY:
            result = "Ackley (pairwise variant)";
            break;
        case solverFunction::RASTRIGIN:
            result = "Rastrigin";
            break;
        case solverFunction::SCHWEFEL:
            result = "Schwefel";
            break;
        case solverFunction::SALOMON:
            result = "Salomon";
            break;
        case solverFunction::NEGPARABOLA:
            result = "Negative parabola";
            break;
        case solverFunction::ACKLEY_CANONICAL:
            result = "Ackley (canonical)";
            break;
        case solverFunction::GRIEWANK:
            result = "Griewank";
            break;
        case solverFunction::LEVY:
            result = "Levy";
            break;
        case solverFunction::STYBLINSKI_TANG:
            result = "Styblinski-Tang";
            break;
        case solverFunction::ELLIPSOID:
            result = "Ellipsoid";
            break;
        case solverFunction::MICHALEWICZ:
            result = "Michalewicz (m=10)";
            break;
        case solverFunction::ZAKHAROV:
            result = "Zakharov";
            break;
        }

        return result;
    }

    //---------------------------------------------------------------------------
    /**
	  * @brief Returns the 2D version of a function as a ROOT TFormula-compatible string.
	  *
	  * The returned string is suitable for use with ROOT's TF2 class
	  * (see https://root.cern.ch) and can be passed directly to GPlotDesigner.
	  * All formulas use ROOT syntax: "pi" for π, "exp(1.)" for e, "^" for power,
	  * and "sqrt"/"abs" for the respective standard functions.
	  *
	  * @param df The solverFunction identifier
	  * @return ROOT TFormula string for the 2D (n=2) version of the function
	  */
    static std::string get2DROOTFunction(const solverFunction &df) {
        std::string result; // NOLINT(cppcoreguidelines-init-variables)

        switch(df) {
        case solverFunction::PARABOLA:
            result = "x^2+y^2";
            break;
        case solverFunction::NOISYPARABOLA:
            result = "(cos(x^2+y^2)+2.)*(x^2+y^2)";
            break;
        case solverFunction::ROSENBROCK:
            result = "100.*(x^2-y)^2+(1.-x)^2";
            break;
        case solverFunction::ACKLEY:
            result = "exp(-0.2)*sqrt(x^2+y^2)+3.*(cos(2.*x)+sin(2.*y))";
            break;
        case solverFunction::RASTRIGIN:
            result = "20.+(x^2-10.*cos(2.*pi*x))+(y^2-10.*cos(2.*pi*y))";
            break;
        case solverFunction::SCHWEFEL:
            result = "-0.5*(x*sin(sqrt(abs(x)))+y*sin(sqrt(abs(y))))";
            break;
        case solverFunction::SALOMON:
            result = "-cos(2.*pi*sqrt(x^2+y^2))+0.1*sqrt(x^2+y^2)+1.";
            break;
        case solverFunction::NEGPARABOLA:
            result = "-(x^2+y^2)";
            break;
        case solverFunction::ACKLEY_CANONICAL:
            result =
                "-20.*exp(-0.2*sqrt((x^2+y^2)/2.))-exp((cos(2.*pi*x)+cos(2.*pi*y))/2.)+20.+exp(1.)";
            break;
        case solverFunction::GRIEWANK:
            result = "(x^2+y^2)/4000.-cos(x)*cos(y/sqrt(2.))+1.";
            break;
        case solverFunction::LEVY:
            result = "sin(pi*(1.+0.25*(x-1.)))^2+((0.25*(x-1.))^2)*(1.+10.*sin(pi*(1.+0.25*(y-1.)))"
                     "^2)+((0.25*(y-1.))^2)*(1.+sin(2.*pi*(1.+0.25*(y-1.)))^2)";
            break;
        case solverFunction::STYBLINSKI_TANG:
            result = "0.5*(x^4-16.*x^2+5.*x+y^4-16.*y^2+5.*y)";
            break;
        case solverFunction::ELLIPSOID:
            result = "x^2+1000000.*y^2";
            break;
        case solverFunction::MICHALEWICZ:
            result = "-sin(x)*pow(sin(x^2/pi),20.)-sin(y)*pow(sin(2.*y^2/pi),20.)";
            break;
        case solverFunction::ZAKHAROV:
            result = "x^2+y^2+(0.5*x+y)^2+(0.5*x+y)^4";
            break;
        }

        return result;
    }

    //---------------------------------------------------------------------------
    // GIndividualFactory<GFunctionIndividual> hooks. GFunctionIndividual is a Tier-2
    // (config-driven) flat individual: it supplies the static hooks the generic factory needs --
    // describeConfig (the configurable values), buildGenome (the genome structure for the five
    // parameter-type modes), buildAdaptionConfig (the OA-owned Gauss / bi-Gauss adaption config) and
    // applyConfig (the demo function, a per-object non-genome setting).

    /** @brief The configurable values parsed from the config file. */
    struct Config {
        double ad_prob = GFI_DEF_ADPROB;
        double adapt_ad_prob = GFI_DEF_ADAPTADPROB;
        double min_ad_prob = GFI_DEF_MINADPROB;
        double max_ad_prob = GFI_DEF_MAXADPROB;
        std::uint32_t adaption_threshold = GFI_DEF_ADAPTIONTHRESHOLD;
        bool use_bi_gaussian = GFI_DEF_USEBIGAUSSIAN;
        double sigma1 = GFI_DEF_SIGMA1;
        double sigma_sigma1 = GFI_DEF_SIGMASIGMA1;
        double min_sigma1 = GFI_DEF_MINSIGMA1;
        double max_sigma1 = GFI_DEF_MAXSIGMA1;
        double sigma2 = GFI_DEF_SIGMA2;
        double sigma_sigma2 = GFI_DEF_SIGMASIGMA2;
        double min_sigma2 = GFI_DEF_MINSIGMA2;
        double max_sigma2 = GFI_DEF_MAXSIGMA2;
        double delta = GFI_DEF_DELTA;
        double sigma_delta = GFI_DEF_SIGMADELTA;
        double min_delta = GFI_DEF_MINDELTA;
        double max_delta = GFI_DEF_MAXDELTA;
        std::size_t par_dim = GFI_DEF_PARDIM;
        double min_var = GFI_DEF_MINVAR;
        double max_var = GFI_DEF_MAXVAR;
        parameterType p_t = GFI_DEF_PARAMETERTYPE;
        solverFunction demo_function = GO_DEF_EVALFUNCTION;
    };

    /**
     * @brief Registers the config-file options, binding them to the passed Config.
     * @param gpb The GParserBuilder object to which the configurable values are added
     * @param c The Config instance whose members are bound to the parser (written on parse)
     */
    static void describeConfig(Gem::Common::GParserBuilder &gpb, Config &c);
    /**
     * @brief Builds the flat genome's structure for the configured parameter-type mode.
     * @param c The configuration describing dimension, bounds and parameter-type mode
     * @return The structure-only genome data for the selected mode
     */
    static gen::GenomeData buildGenome(const Config &c);
    /**
     * @brief The OA-owned adaption config: every double group gets the configured Gauss / bi-Gauss adaptor.
     * @param sample A sample flat genome whose group structure the config mirrors
     * @param c The configuration providing the Gauss / bi-Gauss adaptor settings
     * @return A shared pointer to the populated adaption config
     */
    static std::shared_ptr<OptimizationAlgorithms::GAdaptionConfigBase>
    buildAdaptionConfig(const gen::GGenome &sample, const Config &c);
    /**
     * @brief Per-object post-config hook: applies the (non-genome) demo function to a produced individual.
     * @param ind The individual to configure (modified in place)
     * @param c The configuration providing the demo function to apply
     */
    static void applyConfig(GFunctionIndividual &ind, const Config &c);
    /**
     * @brief Reads a GFunctionIndividual config file into a Config (for callers that build directly,
     *  e.g. the dimension-sweeping benchmarks).
     * @param configFile Path to the JSON configuration file
     * @return A Config populated from the file
     */
    static Config readConfig(std::filesystem::path const &configFile);
    /**
     * @brief Builds an individual fully configured the way the factory would: installs the genome from
     *  @p c (whose par_dim the caller may have overridden) and the demo function, AND applies the base
     *  GGenome options (eval_policy / maxmode / validity thresholds) from @p configFile -- a
     *  faithful drop-in for factory get_as<>() for callers that need a genome dimension differing from the
     *  config file (the dimension-sweeping benchmarks).
     * @param c The configuration providing genome dimension, bounds and demo function
     * @param configFile Path to the config file supplying the base GGenome options
     * @return A fully configured GFunctionIndividual, wrapped in a shared_ptr
     */
    static std::shared_ptr<GFunctionIndividual>
    buildConfigured(const Config &c, std::filesystem::path const &configFile);

protected:
    //---------------------------------------------------------------------------
    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;
    /** @brief Single declaration of this class'es local data members */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(Gem::Common::make_member("demo_function_", self.demo_function_));
    }

    /**
     * @brief The evaluation hook: evaluates the selected benchmark function on the individual's parameters.
     * @return The raw fitness as a one-element vector (a single-criterion problem)
     */
    std::vector<double> evaluate() final;

    //---------------------------------------------------------------------------

    /**
     * @brief Applies modifications to this object.
     * @return A boolean indicating whether any modifications were made
     */
    bool modify_GUnitTests_() override;
    /** @brief Performs self tests that are expected to succeed. */
    void specificTestsNoFailureExpected_GUnitTests_() override;
private:
    //---------------------------------------------------------------------------
    // Data

    solverFunction demo_function_ =
        solverFunction::PARABOLA; ///< Specifies which demo function should be used
};

/******************************************************************************/
/**
 * @brief Provides an easy way to print the individual's content.
 * @param os The output stream to write to
 * @param ind The individual whose content is written
 * @return A reference to the output stream
 */
std::ostream &
operator<<(std::ostream & s, const Gem::Geneva::Individuals::GFunctionIndividual & f);

/**
 * @brief Provides an easy way to print the individual's content via an owning pointer.
 * @param os The output stream to write to
 * @param ind_ptr A sole-ownership pointer to the individual whose content is written
 * @return A reference to the output stream
 */
std::ostream &
operator<<(std::ostream & s, const std::unique_ptr<Gem::Geneva::Individuals::GFunctionIndividual>& f_ptr);

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A factory for GFunctionIndividual objects: an alias for the generic, config-driven
 * GIndividualFactory, for which GFunctionIndividual supplies the static describeConfig /
 * buildGenome / buildAdaptionConfig / applyConfig hooks. Call sites use ctor(path), get()/get_as<>(),
 * getAdaptionConfig(), registerContentCreator() and serialization via GMetaOptimizer.
 */
using GFunctionIndividualFactory =
    Gem::Geneva::Genome::GIndividualFactory<GFunctionIndividual>;

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A simple constraint checker searching for valid solutions that fulfill
 * a given constraint. Here, the sum of all double variables needs to be smaller
 * than a given constant.
 */
//! [constraints:declaration#1]
class GDoubleSumConstraint
  : public Gem::Common::GReflectiveInterfaceT<GDoubleSumConstraint, gen::GGenomeConstraint>
  , public Gem::Common::GSelfTestable { // NOLINT(cppcoreguidelines-special-member-functions)
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(Gem::Common::make_member("c_", self.c_));
    }
    ///////////////////////////////////////////////////////////////////////
//! [constraints:declaration#1]
//! [constraints:declaration#2]
public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDoubleSumConstraint";

    /** @brief The default constructor */
    GDoubleSumConstraint() = default;
    /**
     * @brief Initialization with the constant.
     * @param c The constant that the sum of all double parameters must not exceed
     */
    explicit GDoubleSumConstraint(const double &c);
//! [constraints:declaration#2]
    /**
     * @brief The copy constructor.
     * @param cp A constant reference to another GDoubleSumConstraint object
     */
    GDoubleSumConstraint(const GDoubleSumConstraint &cp) = default;

    /** @brief The destructor */
    ~GDoubleSumConstraint() override = default;

//! [constraints:declaration#3]
protected:
//! [constraints:declaration#3]
    /***************************************************************************/
    /**
     * @brief Applies modifications to this object. This is needed for testing purposes
     *
     * This class contributes no checks of its own; it opts into the self-test facet so that the
     * category root's modify_GUnitTests_() keeps running for it under the standard tests.
     */
    bool modify_GUnitTests_() override {
#ifdef GEM_TESTING
        return Gem::Geneva::GPreEvaluationValidityCheckT<gen::GGenome>::modify_GUnitTests_();
#else  /* GEM_TESTING */
        Gem::Common::condnotset("GDoubleSumConstraint::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif /* GEM_TESTING */
    }

    //! [constraints:declaration#4]
    /**
     * @brief Checks whether the constraint (sum of parameters below the constant) is fulfilled.
     * @param cp The individual whose parameters are checked
     * @return A measure of constraint violation (0 if the constraint is satisfied)
     */
    double check_(const gen::GGenome *p) const override;
    //! [constraints:declaration#4]

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_(), compare_(), name_() and clone_() are generated by the Gem::Common::GReflectiveInterfaceT base.

//! [constraints:declaration#5]
private:
    double c_ = 1.; ///< The constant that should not be exceeded by the sum of parameters
};
//! [constraints:declaration#5]

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A constraint checker trying to enforce a condition x+y+z=C (note the equal
 * sign!) for double variables
 */
class GDoubleSumGapConstraint
  : public Gem::Common::GReflectiveInterfaceT<GDoubleSumGapConstraint, gen::GGenomeConstraint>
  , public Gem::Common::GSelfTestable { // NOLINT(cppcoreguidelines-special-member-functions)
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief The single declaration of this class'es local data members. */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("c_", self.c_),
            Gem::Common::make_member("gap_", self.gap_));
    }
    ///////////////////////////////////////////////////////////////////////
public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GDoubleSumGapConstraint";

    /** @brief The default constructor */
    GDoubleSumGapConstraint() = default;
    /**
     * @brief Initialization with the constant and the gap.
     * @param c The target value the sum of all double parameters should equal
     * @param gap The tolerance around c that is still considered valid
     */
    GDoubleSumGapConstraint(const double &c, const double &gap);
    /**
     * @brief The copy constructor.
     * @param cp A constant reference to another GDoubleSumGapConstraint object
     */
    GDoubleSumGapConstraint(const GDoubleSumGapConstraint &cp) = default;

    /** @brief The destructor */
    ~GDoubleSumGapConstraint() override = default;

protected:
    /***************************************************************************/
    /**
     * @brief Applies modifications to this object. This is needed for testing purposes
     *
     * This class contributes no checks of its own; it opts into the self-test facet so that the
     * category root's modify_GUnitTests_() keeps running for it under the standard tests.
     */
    bool modify_GUnitTests_() override {
#ifdef GEM_TESTING
        return Gem::Geneva::GPreEvaluationValidityCheckT<gen::GGenome>::modify_GUnitTests_();
#else  /* GEM_TESTING */
        Gem::Common::condnotset("GDoubleSumGapConstraint::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif /* GEM_TESTING */
    }

    /**
     * @brief Checks whether the equality constraint (sum of parameters near the constant) is fulfilled.
     * @param cp The individual whose parameters are checked
     * @return A measure of constraint violation (0 if within the allowed gap)
     */
    double check_(const gen::GGenome *p) const override;

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_(), compare_(), name_() and clone_() are generated by the Gem::Common::GReflectiveInterfaceT base.

private:
    double c_ = 1.;    ///< The target constant that the sum of parameters should equal (within the gap)
    double gap_ = 0.5; ///< A tolerance around C_ that is still considered to be valid
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A simple constraint checker searching for valid solutions that fulfill
 * a given constraint. Here, valid solutions lie in a sphere around 0
 */
class GSphereConstraint
  : public Gem::Common::GReflectiveInterfaceT<GSphereConstraint, gen::GGenomeConstraint>
  , public Gem::Common::GSelfTestable { // NOLINT(cppcoreguidelines-special-member-functions)
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /** @brief Single declaration of this class'es local data members */
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(Gem::Common::make_member("diameter_", self.diameter_));
    }
    ///////////////////////////////////////////////////////////////////////
public:
    /** @brief The class name, consumed by the GReflectiveInterfaceT-generated name_() / compare token. */
    static constexpr std::string_view class_name = "GSphereConstraint";

    /** @brief The default constructor */
    GSphereConstraint() = default;
    /**
     * @brief Initialization with the diameter.
     * @param cp The diameter of the sphere within which solutions are valid
     */
    explicit GSphereConstraint(const double &diameter);
    /**
     * @brief The copy constructor.
     * @param cp A constant reference to another GSphereConstraint object
     */
    GSphereConstraint(const GSphereConstraint & cp) = default;

    /** @brief The destructor */
    ~GSphereConstraint() override = default;

protected:
    /***************************************************************************/
    /**
     * @brief Applies modifications to this object. This is needed for testing purposes
     *
     * This class contributes no checks of its own; it opts into the self-test facet so that the
     * category root's modify_GUnitTests_() keeps running for it under the standard tests.
     */
    bool modify_GUnitTests_() override {
#ifdef GEM_TESTING
        return Gem::Geneva::GPreEvaluationValidityCheckT<gen::GGenome>::modify_GUnitTests_();
#else  /* GEM_TESTING */
        Gem::Common::condnotset("GSphereConstraint::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif /* GEM_TESTING */
    }

    /**
     * @brief Checks whether the constraint (solution within a sphere around 0) is fulfilled.
     * @param cp The individual whose parameters are checked
     * @return A measure of constraint violation (0 if inside the sphere)
     */
    double check_(const gen::GGenome *p) const override;

    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     * @param gpb The GParserBuilder object to which configuration options should be added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_(), compare_(), name_() and clone_() are generated by the Gem::Common::GReflectiveInterfaceT base.

private:
    /** @brief The diameter of the sphere */
    double diameter_ = 1.;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
} /* namespace Gem::Geneva::Individuals */

