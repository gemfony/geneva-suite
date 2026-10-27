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

// Standard headers go here
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

// Boost headers go here

// Geneva headers go here
#include "common/GExceptions.hpp"
#include "geneva/GOptimizationEnums.hpp"
#include "geneva/genome/GGenome.hpp"
#include "geneva/oa/GHesseError.hpp"
#include "geneva/oa/GLineSearch.hpp"
#include "geneva/oa/GOptimizationAlgorithmBase.hpp"
#include "geneva/oa/GOptimizationAlgorithmT.hpp"
#include "geneva/oa/GConjugateGradientDescent_PersonalityTraits.hpp"

#ifdef GEM_TESTING

#ifdef GEM_TESTING
#include "geneva/individuals/GTestIndividual1.hpp"
#include "common/GSelfTestable.hpp"
#endif /* GEM_TESTING */

#endif /* GEM_TESTING */

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
/**
 * Selects how the search direction is built from the (finite-difference) gradient. Steepest descent is
 * the former, separate "gd" algorithm folded in as the beta == 0 special case.
 */
//! [algo-gd-gradient-method-enum]
enum class gradientMethod : std::uint8_t {
    CONJUGATE_PR_PLUS = 0, ///< Polak-Ribiere+ nonlinear conjugate gradient with restarts (the default)
    STEEPEST_DESCENT = 1,  ///< Plain steepest descent (beta == 0)
    CONJUGATE_FR = 2,      ///< Fletcher-Reeves (beta = g.g / g_prev.g_prev)
    CONJUGATE_HS = 3,      ///< Hestenes-Stiefel+ (beta = g.(g-g_prev) / d_prev.(g-g_prev))
    CONJUGATE_DY = 4,      ///< Dai-Yuan (beta = g.g / d_prev.(g-g_prev))
    LBFGS = 5              ///< Limited-memory BFGS quasi-Newton (two-loop recursion over the last m (s,y) pairs)
};
//! [algo-gd-gradient-method-enum]

/**
 * Selects whether (and how thoroughly) a MINUIT-style parameter-error estimate is computed at the
 * minimum once the descent has converged. Opt-in (off by default).
 */
enum class errorEstimationMode : std::uint8_t {
    NONE = 0,     ///< No error estimate (the default)
    DIAGONAL = 1, ///< Cheap, always-affordable parameter-fixed (parabolic) errors from the Hessian diagonal
    FULL = 2,     ///< Full Hessian -> covariance -> correlation-aware errors (small dimension only)
    MINOS = 3     ///< MINOS asymmetric errors: profiled (re-minimised) UP-contour bounds, low dimension only
};

// Numeric streaming opt-in for the two enums above; must precede their first
// streaming use (see numeric_enum_io_v in GCommonEnums.hpp).
} /* namespace Gem::Geneva::OptimizationAlgorithms */
namespace Gem::Common {
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::OptimizationAlgorithms::gradientMethod> = true;
template <> inline constexpr bool numeric_enum_io_v<Gem::Geneva::OptimizationAlgorithms::errorEstimationMode> = true;
} /* namespace Gem::Common */
namespace Gem::Geneva::OptimizationAlgorithms {

// The operator re-export for Gem::Geneva::OptimizationAlgorithms lives in
// GOptimizationAlgorithmBase.hpp (included above).

/**
 * Default values for the conjugate gradient descent. They mirror the plain
 * gradient descent so a user coming from a plain gradient descent can switch to "cgd" without re-tuning.
 */
constexpr std::size_t DEFAULTCGDSTARTINGPOINTS = 1;
constexpr double DEFAULTCGDFINITESTEP = 0.001;
constexpr double DEFAULTCGDSTEPSIZE = 0.1;
constexpr std::size_t DEFAULTCGDLBFGSMEMORY = 10; ///< Default L-BFGS history size m (gradient_method = 5 only)

/******************************************************************************/
/**
 * @brief An approximate non-linear conjugate-gradient / quasi-Newton local optimizer that needs no
 * analytic gradient: the gradient is estimated by finite differences and the step is taken along a
 * direction that is conjugate to (or quasi-Newton-scaled relative to) the previous ones.
 *
 * @details
 * For each of \f$ n_\text{start} \f$ independent starting points the population holds one "parent"
 * followed by the difference-quotient "children" used to probe the gradient. At the current point
 * \f$ \mathbf{x}_k \f$ the gradient component in direction \f$ j \f$ is a finite-difference quotient,
 * either @e forward (\f$ O(h) \f$, one probe per direction) or, optionally
 * (setCentralDifferences()), @e central (\f$ O(h^2) \f$, two probes per direction):
 * \f[
 *   g_{k,j} = \frac{f(\mathbf{x}_k + h_j\mathbf{e}_j) - f(\mathbf{x}_k)}{h_j}
 *   \qquad\text{or}\qquad
 *   g_{k,j} = \frac{f(\mathbf{x}_k + h_j\mathbf{e}_j) - f(\mathbf{x}_k - h_j\mathbf{e}_j)}{2 h_j},
 * \f]
 * where the probe size \f$ h_j \f$ is the configured finite step taken as a fraction of the parameter's
 * value range (so every coordinate is probed on its own scale).
 *
 * @par Search direction
 * The new search direction \f$ \mathbf{d}_k \f$ is built from the gradient and the previous direction
 * \f$ \mathbf{d}_{k-1} \f$,
 * \f[
 *   \mathbf{d}_k = -\mathbf{g}_k + \beta_k\,\mathbf{d}_{k-1},\qquad \mathbf{d}_0 = -\mathbf{g}_0,
 * \f]
 * where \f$ \beta_k \f$ selects the method (setGradientMethod()). With
 * \f$ \mathbf{y}_k=\mathbf{g}_k-\mathbf{g}_{k-1} \f$ the supported variants are
 * \f[
 *   \beta^{\mathrm{SD}}=0,\quad
 *   \beta^{\mathrm{FR}}=\frac{\mathbf{g}_k^{\!\top}\mathbf{g}_k}{\mathbf{g}_{k-1}^{\!\top}\mathbf{g}_{k-1}},\quad
 *   \beta^{\mathrm{PR+}}=\frac{\mathbf{g}_k^{\!\top}\mathbf{y}_k}{\mathbf{g}_{k-1}^{\!\top}\mathbf{g}_{k-1}},\quad
 *   \beta^{\mathrm{HS}}=\frac{\mathbf{g}_k^{\!\top}\mathbf{y}_k}{\mathbf{d}_{k-1}^{\!\top}\mathbf{y}_k},\quad
 *   \beta^{\mathrm{DY}}=\frac{\mathbf{g}_k^{\!\top}\mathbf{g}_k}{\mathbf{d}_{k-1}^{\!\top}\mathbf{y}_k},
 * \f]
 * for steepest descent (SD), Fletcher-Reeves (FR), Polak-Ribière (PR+, the default), Hestenes-Stiefel
 * (HS) and Dai-Yuan (DY). \f$ \beta_k \f$ is clamped to \f$ \max(0,\beta) \f$ (the "+" automatic
 * restart, keeping every variant globally convergent), capped to avoid blow-up when the denominator
 * nearly vanishes, and forced to \f$ 0 \f$ both periodically (a conjugate direction loses accuracy after
 * about \f$ n \f$ steps) and on a Powell restart when successive gradients are insufficiently orthogonal,
 * \f$ |\mathbf{g}_k^{\!\top}\mathbf{g}_{k-1}| / \lVert\mathbf{g}_k\rVert^2 \ge 0.1 \f$.
 *
 * @par L-BFGS option
 * With setGradientMethod(LBFGS) the direction is the quasi-Newton step \f$ \mathbf{d}_k=-H_k\mathbf{g}_k \f$,
 * where the inverse-Hessian approximation \f$ H_k \f$ is defined implicitly by the last \f$ m \f$ curvature
 * pairs \f$ (\mathbf{s}_i,\mathbf{y}_i) \f$ with \f$ \mathbf{s}_i=\mathbf{x}_i-\mathbf{x}_{i-1} \f$ and
 * \f$ \mathbf{y}_i=\mathbf{g}_i-\mathbf{g}_{i-1} \f$. It is recovered by the two-loop recursion (Nocedal)
 * without ever forming \f$ H_k \f$, using the initial scaling
 * \f$ \gamma = (\mathbf{s}^{\!\top}\mathbf{y})/(\mathbf{y}^{\!\top}\mathbf{y}) \f$ from the newest pair
 * (\f$ \gamma=1 \f$ when no pair is available, i.e. plain steepest descent).
 *
 * @par Step and batch model
 * A descent safeguard forces \f$ \mathbf{d}_k=-\mathbf{g}_k \f$ whenever a stale direction fails
 * \f$ \mathbf{g}_k^{\!\top}\mathbf{d}_k<0 \f$, after which the next point is
 * \f$ \mathbf{x}_{k+1}=\mathbf{x}_k+\lambda\,\mathbf{d}_k \f$. The line search uses a fixed,
 * range-scaled trial step \f$ \lambda \f$ (configurable multiplier) rather than a Wolfe line search,
 * which keeps the whole method compatible with Geneva's batch evaluation model (one submission of
 * the entire population per iteration). Only difference quotients of the objective are used; no analytic
 * gradient is required. Optional post-run parameter-error estimates (errorEstimationMode: DIAGONAL /
 * FULL / MINOS) can be computed from the Hessian but do not influence the optimization itself.
 */
class GConjugateGradientDescent // NOLINT(cppcoreguidelines-special-member-functions)
  : public GOptimizationAlgorithmT<GConjugateGradientDescent>
  , public Gem::Common::GSelfTestable {
public:
    // Identifiers consumed by the GOptimizationAlgorithmT scaffold.
    static constexpr std::string_view class_name = "GConjugateGradientDescent";
    static constexpr std::string_view oa_algorithm_name = "Conjugate Gradient Descent";
    static constexpr std::string_view oa_personality_type = "PERSONALITY_CGD";
    /** @brief This algorithm's own personality traits -- what getPersonalityTraits_() hands out,
     *  and the default second template argument of the algorithm factory scaffold. */
    using personality_traits_type = GConjugateGradientDescent_PersonalityTraits;

private:
    ///////////////////////////////////////////////////////////////////////
    friend struct Gem::Common::GReflectiveInterfaceAccess;

    /**
     * @brief Single declaration of this class'es local data members.
     *
     * @return A tuple of named members used by the comparison and serialization framework
     */
    // The member list is written ONCE, in the static template helper below; the two localMembers()
    // overloads are trivial forwarders. Self is deduced as the (const) class type.
    template <typename Self>
    auto localMembers_(this Self &self) {
        return std::make_tuple(
            Gem::Common::make_member("n_starting_points_", self.n_starting_points_),
            Gem::Common::make_member("n_fp_parms_first_", self.n_fp_parms_first_),
            Gem::Common::make_member("finite_step_", self.finite_step_),
            Gem::Common::make_member("step_size_", self.step_size_),
            Gem::Common::make_member("gradient_method_", self.gradient_method_),
            Gem::Common::make_member("error_estimation_", self.error_estimation_),
            Gem::Common::make_member("error_up_", self.error_up_),
            Gem::Common::make_member("central_differences_", self.central_differences_),
            Gem::Common::make_member("lbfgs_memory_", self.lbfgs_memory_)
        );
    }

    // serialize(), load_(), compare_(), name_() and clone_() are generated by the
    // Gem::Common::GReflectiveInterfaceT base (via GOptimizationAlgorithmT) from class_name and localMembers_().

    ///////////////////////////////////////////////////////////////////////

public:
    /** @brief The default constructor */
    GConjugateGradientDescent();
    /**
     * @brief Initialization with the number of starting points and step parameters.
     *
     * @param std::size_t const & The number of starting points in the parameter space
     * @param double const & The size of the finite step of the difference quotient
     * @param double const & The multiplier for the initial trial step along the search direction
     */
    GConjugateGradientDescent(const std::size_t &n_starting_points, const double &finite_step, const double &step_size);
    /**
     * @brief A standard copy constructor.
     *
     * @param GConjugateGradientDescent const & The object to copy from
     */
    GConjugateGradientDescent(const GConjugateGradientDescent &) = default;
    /** @brief The destructor */
    ~GConjugateGradientDescent() override = default;

    /**
     * @brief Retrieves the number of starting points of the algorithm.
     *
     * @return The number of starting positions in the parameter space
     */
    std::size_t getNStartingPoints() const;
    /**
     * @brief Allows to set the number of starting points for the conjugate gradient descent.
     *
     * @param std::size_t The number of starting positions in the parameter space
     */
    void setNStartingPoints(std::size_t n_starting_points);

    /**
     * @brief Set the size of the finite step of the difference quotient.
     *
     * @param double The difference-quotient step (per mill of the parameter range)
     */
    void setFiniteStep(double finite_step);
    /**
     * @brief Retrieve the size of the finite step of the difference quotient.
     *
     * @return The difference-quotient step (per mill of the parameter range)
     */
    double getFiniteStep() const;

    /**
     * @brief Sets a multiplier for the initial trial step along the search direction.
     *
     * @param double The multiplicative factor for the initial trial step (the line search refines it)
     */
    void setStepSize(double step_size);
    /**
     * @brief Retrieves the current step size.
     *
     * @return The multiplicative factor for the initial trial step
     */
    double getStepSize() const;

    /**
     * @brief Selects the search-direction rule (conjugate PR+ or plain steepest descent).
     *
     * @param gradientMethod The search-direction rule to use
     */
    void setGradientMethod(gradientMethod gm);
    /**
     * @brief Retrieves the search-direction rule currently in use.
     *
     * @return The search-direction rule currently configured
     */
    gradientMethod getGradientMethod() const;

    /**
     * @brief Enables the O(h^2) CENTRAL-difference gradient (g_j = (f(x+h)-f(x-h))/2h) instead of the
     * default O(h) forward difference. More accurate, but doubles the number of probe evaluations per
     * iteration (two perturbed children per direction instead of one).
     *
     * @param bool If true, the central-difference gradient is enabled; if false, the forward difference is used
     */
    void setCentralDifferences(bool central);
    /**
     * @brief Whether the central-difference gradient is in use.
     *
     * @return true if the central-difference gradient is enabled, false otherwise
     */
    [[nodiscard]] bool getCentralDifferences() const;

    /**
     * @brief Sets the L-BFGS history size m (the number of (s, y) curvature pairs kept per starting
     * point). Only used when gradientMethod::LBFGS is selected. Larger m -> a better inverse-Hessian
     * approximation at the cost of m*n storage and O(m*n) work per step. Clamped to >= 1.
     *
     * @param std::size_t The L-BFGS history size m (number of (s, y) pairs to keep)
     */
    void setLBFGSMemory(std::size_t m);
    /**
     * @brief Retrieves the L-BFGS history size m.
     *
     * @return The L-BFGS history size m (number of (s, y) pairs kept)
     */
    [[nodiscard]] std::size_t getLBFGSMemory() const;

    /**
     * @brief Selects whether/how a MINUIT-style parameter-error estimate is computed at convergence.
     *
     * @param errorEstimationMode The error-estimation mode to use
     */
    void setErrorEstimation(errorEstimationMode em);
    /**
     * @brief Retrieves the error-estimation mode currently in use.
     *
     * @return The error-estimation mode currently configured
     */
    errorEstimationMode getErrorEstimation() const;
    /**
     * @brief Sets the error definition UP (1 for chi^2-like, 0.5 for -logL); see MINUIT.
     *
     * @param double The MINUIT error definition UP value
     */
    void setErrorDefinition(double up);
    /**
     * @brief Retrieves the error definition UP.
     *
     * @return The MINUIT error definition UP value
     */
    double getErrorDefinition() const;
    /**
     * @brief Retrieves the most recent convergence error estimate (valid only if one was requested).
     *
     * @return The most recent error estimate result
     */
    GHesseErrorResult getLastErrorEstimate() const;

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
        return GOptimizationAlgorithmBase::modify_GUnitTests_();
#else  /* GEM_TESTING */
        Gem::Common::condnotset("GConjugateGradientDescent::modify_GUnitTests", "GEM_TESTING");
        return false;
#endif /* GEM_TESTING */
    }

    /***************************************************************************/
    // Virtual or overridden protected functions

private:
    /**
     * @brief Need-all algorithm: a missing or failed evaluation cannot be tolerated, so it submits
     * through courtier under full-success-or-fatal (matches the legacy throw-on-error).
     *
     * @return The full-success-or-fatal submission policy
     */
    Gem::Courtier::GSubmissionPolicy getSubmissionPolicy_() const override {
        return Gem::Courtier::GSubmissionPolicy::full_success_or_fatal();
    }

protected:
    /**
     * @brief Adds local configuration options to a GParserBuilder object.
     *
     * @param gpb The parser builder to which this algorithm's configuration options are added
     */
    void addConfigurationOptions_(Gem::Common::GParserBuilder &gpb) override;

    // load_() and compare_() are generated by the Gem::Common::GReflectiveInterfaceT base.

    /** @brief Resets the settings of this population to what was configured when the optimize()-call was issued */
    void resetToOptimizationStart_() override;

    /** @brief Does some preparatory work before the optimization starts */
    void init() override;
    /** @brief Does any necessary finalization work */
    void finalize() override;

    /** @brief Updates the difference-quotient children of every starting point */
    void updateChildParameters();

    /** @brief Performs a gradient step for every starting point (driver; see the phase helpers below) */
    void updateParentIndividuals();

    /** @brief Installs any missing conjugate-gradient / L-BFGS scratch blocks on a central slot's store */
    void ensureGradientScratch_(Genome::GAuxiliaryStore &cg_scratch) const;

    /**
     * @brief Computes the normalised finite-difference (proxy) gradient of one starting point
     * @param starting_point The index of the starting point whose gradient is computed
     * @param parent_fitness The (minimization-oriented) fitness of that starting point
     * @return The proxy gradient, one component per floating-point parameter
     */
    std::vector<double> computeProxyGradient_(std::size_t starting_point, double parent_fitness) const;

    /**
     * @brief Builds the L-BFGS quasi-Newton direction -H_k g_k, updating the curvature history
     * @param cg_scratch The central slot's scratch store holding the (s, y) history
     * @param parm_vec The starting point's current parameter vector
     * @param gradient The current proxy gradient
     * @param history_valid Whether the stored previous gradient/parameters are valid
     * @param periodic_restart Whether this iteration performs the classical periodic restart
     * @return The search direction
     */
    std::vector<double> lbfgsDirection_(
        Genome::GAuxiliaryStore &cg_scratch,
        const std::vector<double> &parm_vec,
        const std::vector<double> &gradient,
        bool history_valid,
        bool periodic_restart
    ) const;

    /**
     * @brief Builds the steepest-descent / conjugate-gradient-family search direction
     * @param gradient The current proxy gradient
     * @param prev_gradient The previous iteration's gradient (scratch-backed)
     * @param prev_direction The previous iteration's search direction (scratch-backed)
     * @param history_valid Whether the previous gradient/direction are valid
     * @param periodic_restart Whether this iteration performs the classical periodic restart
     * @return The search direction
     */
    std::vector<double> conjugateDirection_(
        const std::vector<double> &gradient,
        std::span<const double> prev_gradient,
        std::span<const double> prev_direction,
        bool history_valid,
        bool periodic_restart
    ) const;

    // name_(), clone_(), getAlgorithmName_(), getAlgorithmPersonalityType_() and the GUnitTests
    // stubs are generated by the GOptimizationAlgorithmT scaffold from the oa_* identifiers above.

    /***************************************************************************/

private:
    /***************************************************************************/
    // Virtual or overridden private functions

    /**
     * @brief The actual business logic to be performed during each iteration.
     *
     * @return A tuple holding the best achieved fitness (raw and transformed)
     */
    std::tuple<double, double> cycleLogic_() override;
    /** @brief Triggers fitness calculation of a number of individuals */
    void evaluatePopulation_() override;


    /**
     * @brief Retrieve a GPersonalityTraits object belonging to this algorithm.
     *
     * @return A shared pointer to a newly created personality-traits object for this algorithm
     */
    std::shared_ptr<GPersonalityTraits> getPersonalityTraits_() const override;
    /** @brief Resizes the population to the desired level and does some error checks */
    void adjustPopulation_() override;

    /** @brief Recomputes the dimensionless adjusted_finite_step_ (finite_step_/1000) */
    void updateDerivedQuantities();
    /** @brief (Re-)initialises the per-starting-point conjugate-gradient state */
    void resetCGState();
    /**
     * @brief Evaluates a batch of trial parameter vectors (line-search probes) as clones of the
     * given starting point, submitted through the same consumer the algorithm uses, and returns one
     * min-only fitness per probe.
     *
     * @param starting_point Index of the starting point whose central individual is cloned for each probe
     * @param points The trial parameter vectors to evaluate (one inner vector per probe)
     * @return One min-only fitness value per probe, in the same order as @p points
     */
    std::vector<double> evaluateProbes(
        std::size_t starting_point,
        std::vector<std::vector<double>> const &points
    );

    /***************************************************************************/
    // Data

    std::size_t n_starting_points_ =
        DEFAULTCGDSTARTINGPOINTS;   ///< The number of starting positions in the parameter space
    std::size_t n_fp_parms_first_ = 0; ///< The amount of active floating point values per individual

    double finite_step_ =
        DEFAULTCGDFINITESTEP; ///< The size of the difference-quotient step (per mill of the range)
    double step_size_ =
        DEFAULTCGDSTEPSIZE; ///< Multiplicative factor for the initial trial step (the line search refines it)

    gradientMethod gradient_method_ =
        gradientMethod::CONJUGATE_PR_PLUS; ///< Conjugate (PR+) by default; STEEPEST_DESCENT == the former GD

    bool central_differences_ = false; ///< O(h^2) central gradient (two probes/direction) vs O(h) forward (one)

    std::size_t lbfgs_memory_ = DEFAULTCGDLBFGSMEMORY; ///< L-BFGS history size m ((s,y) pairs kept); LBFGS mode only

    errorEstimationMode error_estimation_ =
        errorEstimationMode::NONE; ///< Whether to estimate parameter errors at convergence (opt-in)
    double error_up_ = 1.;         ///< The MINUIT error definition UP (1 = chi^2, 0.5 = -logL)

    GHesseErrorResult last_error_estimate_; ///< The most recent error estimate (transient; not serialized)

    double adjusted_finite_step_ =
        0.; ///< Dimensionless difference-quotient step (finite_step_/1000, a fraction of the normalized
            ///< parameter range); recomputed in init() (transient). Uniform across parameters now that the
            ///< step is taken in the normalized internal coordinate, so no per-parameter boundary vectors.

    // The per-starting-point conjugate-gradient memory (g_{k-1} / d_{k-1} / history-valid flag) now
    // lives on the OA scratch of each starting point's central individual slot, as POD blocks keyed
    // AUXKEY_CGD_PREV_GRADIENT / _PREV_DIRECTION / _HISTORY_VALID (see GConjugateGradientDescent.cpp).
    // It remains transient (recomputed during optimization, neither serialized nor restored).
};

/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

