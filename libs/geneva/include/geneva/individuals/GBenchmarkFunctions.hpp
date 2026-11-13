/**
 * @file GBenchmarkFunctions.hpp
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

/**
 * @defgroup GBenchmarkFunctions Shared benchmark function kernels
 *
 * @brief Header-only implementations of all GFunctionIndividual benchmark functions
 *        that compile for both CPU and CUDA GPU targets.
 *
 * ## Portability
 *
 * Each function is annotated with G_CALLABLE, which expands to
 * `__host__ __device__` when compiled by nvcc (__CUDACC__ defined), and to
 * nothing for plain C++ compilation.  This means the same source code is used
 * by GFunctionIndividual::evaluate() on the CPU and by the batch
 * CUDA kernel in GBenchmarkBatchEvaluator.cu on the GPU — no duplication.
 *
 * ## Dependencies
 *
 * Only standard C math functions (sin, cos, exp, sqrt, pow, fabs) are used.
 * These are available in both @c \<cmath\> (CPU) and CUDA device code without
 * additional includes.  Boost headers are intentionally absent so this file
 * can be included from .cu translation units.
 *
 * ## Usage from CPU code
 *
 * @code
 *   #include "geneva/individuals/GBenchmarkFunctions.hpp"
 *   double f = Gem::Geneva::Benchmarks::parabola(params.data(), params.size());
 * @endcode
 *
 * ## Usage from CUDA kernel code
 *
 * @code
 *   #include "geneva/individuals/GBenchmarkFunctions.hpp"
 *   __global__ void evalKernel(...) {
 *       double f = Gem::Geneva::Benchmarks::parabola(d_params + tid*n, n);
 *   }
 * @endcode
 *
 * ## Batch dispatch entry point
 *
 *   double Gem::Geneva::Benchmarks::eval(solverFunctionId id, const double* x, int n)
 *
 * dispatches to the correct function based on the integer function id (matching
 * the solverFunction enum values in GFunctionIndividual.hpp).
 *
 * @{
 */

// ── Portability macro ────────────────────────────────────────────────────────

#ifdef __CUDACC__
#define G_CALLABLE __host__ __device__
#else
#define G_CALLABLE
#endif

// ── Constants ────────────────────────────────────────────────────────────────

// Not using Boost or <numbers> so this file remains includable from .cu units.
// The value matches boost::math::constants::pi<double>() to full double precision.
#ifdef __CUDACC__
#include <math_constants.h> // CUDART_PI (CUDART_E does not exist)
#define GBM_PI CUDART_PI
// e = exp(1): no CUDART_E constant, use the literal directly.
#define GBM_E 2.718281828459045235360
#else
#include <cmath>
#include <numbers>
inline constexpr double GBM_PI = std::numbers::pi;
inline constexpr double GBM_E = std::numbers::e;
#endif

// ── Integer IDs (mirror solverFunction enum, avoids including GFunctionIndividual.hpp from .cu) ──

namespace Gem::Geneva::Benchmarks {

constexpr int FUNC_PARABOLA = 0;
constexpr int FUNC_NOISYPARABOLA = 1;
constexpr int FUNC_ROSENBROCK = 2;
constexpr int FUNC_ACKLEY = 3;
constexpr int FUNC_RASTRIGIN = 4;
constexpr int FUNC_SCHWEFEL = 5;
constexpr int FUNC_SALOMON = 6;
constexpr int FUNC_NEGPARABOLA = 7;
constexpr int FUNC_ACKLEY_CANONICAL = 8;
constexpr int FUNC_GRIEWANK = 9;
constexpr int FUNC_LEVY = 10;
constexpr int FUNC_STYBLINSKI_TANG = 11;
constexpr int FUNC_ELLIPSOID = 12;
constexpr int FUNC_MICHALEWICZ = 13;
constexpr int FUNC_ZAKHAROV = 14;
constexpr int FUNC_MAX = 14; ///< highest valid ID

// ── Individual function implementations ─────────────────────────────────────

/**
 * @brief Parabola: f(x) = Σxᵢ²
 *
 * Unimodal, convex, separable. Global minimum f=0 at origin.
 * Baseline function — every algorithm should solve this in few iterations.
 * Condition number = 1; no scale imbalance between dimensions.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The function value Σxᵢ²
 */
//! [appendix:benchmark-dispatch#1]
G_CALLABLE inline double parabola(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) {
        r += x[i] * x[i];
    }
    return r;
}
//! [appendix:benchmark-dispatch#1]

/**
 * @brief Berlich noisy parabola: f(x) = (cos(‖x‖²)+2)·‖x‖²
 *
 * Radially symmetric, non-separable. Global minimum f=0 at origin.
 * The cosine overlay creates a dense shell structure of local optima
 * centred on the origin while the global parabolic shape remains.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The function value (cos(‖x‖²)+2)·‖x‖²
 */
G_CALLABLE inline double noisyParabola(const double *x, int n) {
    double sq = 0.;
    for(int i = 0; i < n; ++i) {
        sq += x[i] * x[i];
    }
    return (cos(sq) + 2.) * sq;
}

/**
 * @brief Generalized Rosenbrock: f(x) = Σ[100(xᵢ₊₁-xᵢ²)²+(1-xᵢ)²], n≥2
 *
 * Non-separable, unimodal for n≤3. Global minimum f=0 at (1,...,1).
 * The narrow, curved banana-shaped valley is nearly flat along its floor,
 * making it hard for gradient-free methods and slow for gradient descent.
 *
 * @param x Pointer to the array of n parameter values (n≥2 expected)
 * @param n Number of parameters (problem dimension)
 * @return The Rosenbrock function value
 */
G_CALLABLE inline double rosenbrock(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n - 1; ++i) {
        double const t = x[i + 1] - (x[i] * x[i]);
        double const u = 1. - x[i];
        r += (100. * t * t) + (u * u);
    }
    return r;
}

/**
 * @brief Ackley pairwise variant (Geneva-specific): f = Σ[e⁻⁰·²√(xᵢ²+xᵢ₊₁²)+3(cos2xᵢ+sin2xᵢ₊₁)], n≥2
 *
 * Non-canonical form retained for backward compatibility.
 * For the standard CEC/BBOB benchmark, use ackleyCanonical().
 *
 * @param x Pointer to the array of n parameter values (n≥2 expected)
 * @param n Number of parameters (problem dimension)
 * @return The pairwise-variant Ackley function value
 */
G_CALLABLE inline double ackley(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n - 1; ++i) {
        double const s = (x[i] * x[i]) + (x[i + 1] * x[i + 1]);
        r += (exp(-0.2) * sqrt(s)) + (3. * (cos(2. * x[i]) + sin(2. * x[i + 1])));
    }
    return r;
}

/**
 * @brief Rastrigin: f(x) = 10n + Σ[xᵢ²-10cos(2πxᵢ)]
 *
 * Highly multimodal, separable. Global minimum f=0 at origin.
 * ~10ⁿ regularly spaced local minima of similar depth. Recommended domain [-5.12, 5.12].
 * Standard benchmark for multimodal robustness.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The Rastrigin function value
 */
G_CALLABLE inline double rastrigin(const double *x, int n) {
    double r = 10. * n;
    for(int i = 0; i < n; ++i) {
        r += (x[i] * x[i]) - (10. * cos(2. * GBM_PI * x[i]));
    }
    return r;
}

/**
 * @brief Schwefel: f(x) = -1/n · Σ xᵢsin(√|xᵢ|)
 *
 * Deceptive: global optimum at xᵢ≈420.97, far from origin and far from
 * all secondary optima. Recommended domain [-500, 500].
 * Note: Geneva normalises by 1/n; the standard formulation does not.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension); also the normalisation divisor
 * @return The (1/n-normalised) Schwefel function value
 */
G_CALLABLE inline double schwefel(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) {
        r += -x[i] * sin(sqrt(fabs(x[i])));
    }
    return r / n;
}

/**
 * @brief Salomon: f(x) = -cos(2π‖x‖) + 0.1‖x‖ + 1
 *
 * Multimodal, radially symmetric, non-separable. Global minimum f=0 at origin.
 * Recommended domain [-100, 100]. Concentric shells of local optima.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The Salomon function value
 */
G_CALLABLE inline double salomon(const double *x, int n) {
    double sq = 0.;
    for(int i = 0; i < n; ++i) {
        sq += x[i] * x[i];
    }
    const double r = sqrt(sq);
    return -cos(2. * GBM_PI * r) + (0.1 * r) + 1.;
}

/**
 * @brief Negative parabola: f(x) = -Σxᵢ²
 *
 * Global maximum f=0 at origin. Used only to verify Geneva's maximisation mode.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The negated parabola value -Σxᵢ²
 */
G_CALLABLE inline double negParabola(const double *x, int n) {
    return -parabola(x, n);
}

/**
 * @brief Canonical Ackley: f(x) = -20e^(-0.2√(1/n·Σxᵢ²)) - e^(1/n·Σcos(2πxᵢ)) + 20 + e
 *
 * Non-separable, multimodal. Global minimum f=0 at origin.
 * Recommended domain [-32.768, 32.768].
 * Almost flat outer plateau (near-zero gradient) followed by steep drop to global basin.
 * Standard benchmark in CEC and BBOB suites.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The canonical Ackley function value
 */
G_CALLABLE inline double ackleyCanonical(const double *x, int n) {
    double sq = 0.;
    double cs = 0.;
    for(int i = 0; i < n; ++i) {
        sq += x[i] * x[i];
        cs += cos(2. * GBM_PI * x[i]);
    }
    const double inv_n = 1. / n;
    return (-20. * exp(-0.2 * sqrt(sq * inv_n))) - exp(cs * inv_n) + 20. + GBM_E;
}

/**
 * @brief Griewank: f(x) = 1/4000·Σxᵢ² - Πcos(xᵢ/√i) + 1
 *
 * Weakly non-separable (product term), multimodal. Global minimum f=0 at origin.
 * Recommended domain [-600, 600]. Quadratic envelope with fine multimodal structure;
 * distinguishes global-structure exploitation from local exploration.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The Griewank function value
 */
G_CALLABLE inline double griewank(const double *x, int n) {
    double sq = 0.;
    double prod = 1.;
    for(int i = 0; i < n; ++i) {
        sq += x[i] * x[i];
        prod *= cos(x[i] / sqrt(static_cast<double>(i + 1)));
    }
    return (sq / 4000.) - prod + 1.;
}

/**
 * @brief Lévy: f = sin²(πw₁) + Σ(wᵢ-1)²(1+10sin²(πwᵢ₊₁)) + (wₙ-1)²(1+sin²(2πwₙ))
 *              with wᵢ = 1+(xᵢ-1)/4
 *
 * Separable, multimodal. Global minimum f=0 at (1,...,1).
 * Recommended domain [-10, 10]. Narrow closely-spaced basins test fine-grained precision.
 *
 * @param x Pointer to the array of n parameter values (x[0] and x[n-1] are used as endpoints)
 * @param n Number of parameters (problem dimension)
 * @return The Lévy function value
 */
G_CALLABLE inline double levy(const double *x, int n) {
    auto w = [](double xi) { return 1. + ((xi - 1.) / 4.); };

    const double w0 = w(x[0]);
    double r = sin(GBM_PI * w0) * sin(GBM_PI * w0);

    for(int i = 0; i < n - 1; ++i) {
        const double wi = w(x[i]);
        const double wi1 = w(x[i + 1]);
        const double sm = sin(GBM_PI * wi1);
        r += (wi - 1.) * (wi - 1.) * (1. + 10. * sm * sm);
    }
    const double wn = w(x[n - 1]);
    const double sn = sin(2. * GBM_PI * wn);
    r += (wn - 1.) * (wn - 1.) * (1. + sn * sn);
    return r;
}

/**
 * @brief Styblinski-Tang: f(x) = 1/2·Σ(xᵢ⁴-16xᵢ²+5xᵢ)
 *
 * Separable, multimodal, asymmetric. Global minimum ≈-39.166·n at xᵢ≈-2.9035.
 * Recommended domain [-5, 5]. The off-centre optimum exposes initialisation bias
 * and mutation symmetry artefacts of gradient-free algorithms.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The Styblinski-Tang function value
 */
G_CALLABLE inline double styblinskiTang(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) {
        const double xi = x[i];
        const double x2 = xi * xi;
        r += (x2 * x2) - (16. * x2) + (5. * xi);
    }
    return 0.5 * r;
}

/**
 * @brief Ellipsoid: f(x) = Σ 10^(6i/(n-1))·xᵢ²
 *
 * Unimodal, separable, condition number 10⁶. Global minimum f=0 at origin.
 * Recommended domain [-5, 5]. The extreme scale imbalance across dimensions tests
 * self-adaptive per-dimension step-size mechanisms (sigma in ES adaptors).
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension); drives the per-dimension scale exponent
 * @return The Ellipsoid function value
 */
G_CALLABLE inline double ellipsoid(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) {
        const double exp = (n > 1) ? 6. * i / (n - 1.) : 0.;
        r += pow(10., exp) * x[i] * x[i];
    }
    return r;
}

/**
 * @brief Michalewicz: f(x) = -Σ sin(xᵢ)·sin²⁰(i·xᵢ²/π)
 *
 * Separable, multimodal. Global minimum dimension-dependent, not analytically known.
 * Domain: [0, π] — this differs from all other functions.
 * NOTE: Set min_var=0, max_var≈3.14159 in the factory configuration!
 * The high exponent m=20 creates extremely narrow ridges. Tests fine-grained local search.
 *
 * @param x Pointer to the array of n parameter values (expected in [0, π])
 * @param n Number of parameters (problem dimension)
 * @return The Michalewicz function value
 */
G_CALLABLE inline double michalewicz(const double *x, int n) {
    double r = 0.;
    for(int i = 0; i < n; ++i) {
        const double s = sin(static_cast<double>(i + 1) * x[i] * x[i] / GBM_PI);
        // pow(s,20) with s possibly negative: use s*s raised to 10 to stay positive
        const double s2 = s * s;
        const double s20 = s2 * s2 * s2 * s2 * s2 * s2 * s2 * s2 * s2 * s2; // s^20
        r -= sin(x[i]) * s20;
    }
    return r;
}

/**
 * @brief Zakharov: f(x) = Σxᵢ² + (Σ0.5·i·xᵢ)² + (Σ0.5·i·xᵢ)⁴
 *
 * Unimodal, non-separable. Global minimum f=0 at origin.
 * Recommended domain [-5, 10]. The weighted linear coupling introduces dimension-
 * weighted interactions without multimodality; tests non-separable step adaptation.
 *
 * @param x Pointer to the array of n parameter values
 * @param n Number of parameters (problem dimension)
 * @return The Zakharov function value
 */
G_CALLABLE inline double zakharov(const double *x, int n) {
    double sq = 0.;
    double lin = 0.;
    for(int i = 0; i < n; ++i) {
        sq += x[i] * x[i];
        lin += 0.5 * (i + 1) * x[i];
    }
    return sq + (lin * lin) + (lin * lin * lin * lin);
}

// ── Central dispatch ─────────────────────────────────────────────────────────

/**
 * @brief Evaluates the benchmark function identified by @p func_id on parameter vector @p x.
 *
 * This is the single entry point used by both the CPU path in
 * GFunctionIndividual::evaluate() and the CUDA batch kernel.
 * func_id values match the solverFunction enum integers 0–14.
 *
 * @param func_id  Integer function identifier (0=PARABOLA … 14=ZAKHAROV)
 * @param x       Pointer to n parameter values
 * @param n       Number of parameters (dimension)
 * @return        Fitness value (lower = better for all minimisation functions)
 */
//! [appendix:benchmark-dispatch#2]
G_CALLABLE inline double eval(int func_id, const double *x, int n) {
    switch(func_id) {
    case FUNC_PARABOLA:
        return parabola(x, n);
    case FUNC_NOISYPARABOLA:
        return noisyParabola(x, n);
//! [appendix:benchmark-dispatch#2]
    case FUNC_ROSENBROCK:
        return rosenbrock(x, n);
    case FUNC_ACKLEY:
        return ackley(x, n);
    case FUNC_RASTRIGIN:
        return rastrigin(x, n);
    case FUNC_SCHWEFEL:
        return schwefel(x, n);
    case FUNC_SALOMON:
        return salomon(x, n);
    case FUNC_NEGPARABOLA:
        return negParabola(x, n);
    case FUNC_ACKLEY_CANONICAL:
        return ackleyCanonical(x, n);
    case FUNC_GRIEWANK:
        return griewank(x, n);
    case FUNC_LEVY:
        return levy(x, n);
    case FUNC_STYBLINSKI_TANG:
        return styblinskiTang(x, n);
    case FUNC_ELLIPSOID:
        return ellipsoid(x, n);
    case FUNC_MICHALEWICZ:
        return michalewicz(x, n);
    case FUNC_ZAKHAROV:
        return zakharov(x, n);
    default:
        return 0.; // unreachable with valid input
    }
}

} /* namespace Gem::Geneva::Benchmarks */

/** @} */ // end of GBenchmarkFunctions group
