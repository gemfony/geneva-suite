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
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <random>
#include <type_traits>

// Boost headers go here

// Geneva headers go here

#include "hap/GRandomT.hpp"

namespace Gem::Hap {

/******************************************************************************/
/**
 * Fast uniform draw in [0,1) from a 64-bit uniform-random-bit generator, bypassing
 * std::generate_canonical. The latter recomputes std::log2 of the generator's range in
 * long double on EVERY call (to decide how many generator invocations to consume); for a
 * 2^64-range generator (xoshiro256++) that long-double log dominated the float adaption path
 * -- ~19% of host time, measured. Here the high bits are simply scaled: 53 for double, 24 for
 * float. Assumes a 64-bit generator (Hap's G_CPU_BASE_GENERATOR).
 *
 * @tparam fp_type The floating point type to produce (float or double)
 * @tparam URBG The uniform-random-bit-generator type (must yield 64-bit values via operator())
 * @param g The generator to draw a 64-bit value from
 * @return A uniformly distributed value in the half-open interval [0,1)
 */
template <std::floating_point fp_type, class URBG>
inline fp_type fast_uniform_01(URBG &g) {
    const auto x = static_cast<std::uint64_t>(g());
    if constexpr(std::is_same_v<fp_type, double>) {
        return static_cast<double>(x >> 11) * 0x1.0p-53; // 53-bit mantissa
    } else {
        return static_cast<float>(x >> 40) * 0x1.0p-24f; // 24-bit mantissa
    }
}

/******************************************************************************/
/**
 * @brief A fast normal (Gaussian) distribution drawing uniforms via fast_uniform_01().
 *
 * A normal (Gaussian) distribution that is API-compatible with std::normal_distribution
 * (param_type{mean,stddev}, operator()(g), operator()(g,param), param()/reset()), but draws its
 * uniforms via fast_uniform_01() instead of std::generate_canonical -- so std::log runs only on
 * fp_type (logf / log), never the slow long-double logl that generate_canonical pulled in. Uses
 * the Marsaglia polar method and caches the second deviate.
 *
 * @tparam fp_type The floating point type of the generated deviates (float or double)
 */
template <std::floating_point fp_type>
class g_normal_distribution {
public:
    using result_type = fp_type;

    class param_type {
    public:
        using distribution_type = g_normal_distribution<fp_type>;
        param_type() = default;
        /** @param mean The mean of the distribution. @param stddev Its standard deviation. */
        explicit param_type(fp_type mean, fp_type stddev = fp_type(1)) : mean_(mean), stddev_(stddev) {}
        /** @return The configured mean. */
        [[nodiscard]] fp_type mean() const { return mean_; }
        /** @return The configured standard deviation. */
        [[nodiscard]] fp_type stddev() const { return stddev_; }
        /** @param a The left-hand-side parameters. @param b The right-hand-side parameters.
         *  @return true if mean and standard deviation are equal. */
        friend bool operator==(const param_type &a, const param_type &b) {
            return a.mean_ == b.mean_ && a.stddev_ == b.stddev_;
        }
        /** @param a The left-hand-side parameters. @param b The right-hand-side parameters.
         *  @return true if the parameters differ. */
        friend bool operator!=(const param_type &a, const param_type &b) { return not(a == b); }
    private:
        fp_type mean_{0};
        fp_type stddev_{1};
    };

    g_normal_distribution() = default;
    /** @param mean The mean of the distribution. @param stddev Its standard deviation. */
    explicit g_normal_distribution(fp_type mean, fp_type stddev = fp_type(1)) : params_(mean, stddev) {}
    /** @param p The distribution parameters to initialize from. */
    explicit g_normal_distribution(const param_type &p) : params_(p) {}

    /** @brief Discards any cached (spare) deviate so the next draw recomputes a fresh pair. */
    void reset() { have_spare_ = false; }
    /** @return The currently configured distribution parameters. */
    [[nodiscard]] param_type param() const { return params_; }
    /** @brief Sets the distribution parameters. @param p The new distribution parameters. */
    void param(const param_type &p) { params_ = p; }
    /** @return The configured mean. */
    [[nodiscard]] fp_type mean() const { return params_.mean(); }
    /** @return The configured standard deviation. */
    [[nodiscard]] fp_type stddev() const { return params_.stddev(); }

    /** @tparam URBG The generator type. @param g The generator. @return A normal deviate using the stored parameters. */
    template <class URBG>
    result_type operator()(URBG &g) { return (*this)(g, params_); }

    /** @tparam URBG The generator type. @param g The generator. @param p The distribution parameters to use.
     *  @return A normally distributed deviate with the given mean and standard deviation. */
    template <class URBG>
    result_type operator()(URBG &g, const param_type &p) {
        if(have_spare_) {
            have_spare_ = false;
            return p.mean() + (p.stddev() * spare_);
        }
        fp_type u; // NOLINT(cppcoreguidelines-init-variables)
        fp_type v; // NOLINT(cppcoreguidelines-init-variables)
        fp_type s; // NOLINT(cppcoreguidelines-init-variables)
        do {
            u = (fp_type(2) * fast_uniform_01<fp_type>(g)) - fp_type(1);
            v = (fp_type(2) * fast_uniform_01<fp_type>(g)) - fp_type(1);
            s = (u * u) + (v * v);
        } while(s >= fp_type(1) || s == fp_type(0));
        const fp_type f = std::sqrt(fp_type(-2) * std::log(s) / s);
        spare_          = v * f;
        have_spare_     = true;
        return p.mean() + (p.stddev() * (u * f));
    }

private:
    param_type params_{};
    fp_type spare_{0};
    bool have_spare_{false};
};

/******************************************************************************/
/**
 * @brief A fast Bernoulli distribution drawing its uniform via fast_uniform_01().
 *
 * A Bernoulli distribution that is API-compatible with std::bernoulli_distribution
 * (param_type{p}, operator()(g), operator()(g,param), p()/param()/reset()), but draws its
 * uniform via fast_uniform_01() rather than std::generate_canonical. std::bernoulli_distribution
 * pulls in generate_canonical's per-call long-double log2 just like the normal distribution did;
 * this is the per-parameter adaption-probability coin flip (weighted_bool_), so it is on the
 * hottest path of every adaption.
 */
class g_bernoulli_distribution {
public:
    using result_type = bool;

    class param_type {
    public:
        using distribution_type = g_bernoulli_distribution;
        param_type() = default;
        /** @param p The probability of drawing true, in [0,1]. */
        explicit param_type(double p) : p_(p) {}
        /** @return The configured probability of drawing true. */
        [[nodiscard]] double p() const { return p_; }
        /** @param a The left-hand-side parameters. @param b The right-hand-side parameters.
         *  @return true if both probabilities are equal. */
        friend bool operator==(const param_type &a, const param_type &b) { return a.p_ == b.p_; }
        /** @param a The left-hand-side parameters. @param b The right-hand-side parameters.
         *  @return true if the probabilities differ. */
        friend bool operator!=(const param_type &a, const param_type &b) { return not(a == b); }
    private:
        double p_{0.5};
    };

    g_bernoulli_distribution() = default;
    /** @param p The probability of drawing true, in [0,1]. */
    explicit g_bernoulli_distribution(double p) : params_(p) {}
    /** @param p The distribution parameters to initialize from. */
    explicit g_bernoulli_distribution(const param_type &p) : params_(p) {}

    /** @brief No-op reset; this distribution holds no cached state. */
    void reset() {}
    /** @return The currently configured distribution parameters. */
    [[nodiscard]] param_type param() const { return params_; }
    /** @brief Sets the distribution parameters. @param p The new distribution parameters. */
    void param(const param_type &p) { params_ = p; }
    /** @return The configured probability of drawing true. */
    [[nodiscard]] double p() const { return params_.p(); }

    /** @tparam URBG The generator type. @param g The generator. @return true with the stored probability. */
    template <class URBG>
    result_type operator()(URBG &g) { return (*this)(g, params_); }

    /** @tparam URBG The generator type. @param g The generator. @param p The distribution parameters to use.
     *  @return true with probability p.p(), false otherwise. */
    template <class URBG>
    result_type operator()(URBG &g, const param_type &p) {
        return fast_uniform_01<double>(g) < p.p();
    }

private:
    param_type params_;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A random distribution consisting of two adjacent normal distributions (peaks).
 *
 * This class implements a random distribution consisting of two adjacent
 * normal distributions. It models the API common for std C++11 random distributions.
 *
 * @tparam fp_type The floating point type of the generated deviates (float or double)
 */
template <std::floating_point fp_type>
class bi_normal_distribution {
public:
    using input_type = fp_type;
    using result_type = fp_type;

    /**************************************************************************/
    /**
	  * @brief Holds the parameters needed for the bi_normal_distribution.
	  *
	  * This embedded class identifies parameters needed for the bi_normal_distribution
	  */
    class param_type { // NOLINT(cppcoreguidelines-special-member-functions)
    public:
        using distribution_type = bi_normal_distribution<fp_type>;

        param_type() = delete;

        // Do not allow "empty" parameter types
        param_type(param_type &&) = delete;
        param_type &operator=(param_type &&) = delete;

        /**
 			* @brief Constructs the parameters of a bi_normal_distribution.
 			*
 			* Constructs the parameters of a bi_normal_distribution<fp_type>
 			*
 			* @param mean The position midway between the two peaks
 			* @param sigma1 The standard deviation (width) of the left peak
 			* @param sigma2 The standard deviation (width) of the right peak
 			* @param distance The distance between the two peaks
 			*/
        param_type(fp_type mean, fp_type sigma1, fp_type sigma2, fp_type distance)
          : mean_(mean)
          , sigma1_(sigma1)
          , sigma2_(sigma2)
          , distance_(distance) { /* nothing */
        }

        /** @brief The copy constructor (memberwise over four scalars) */
        param_type(const param_type &) = default;
        /** @brief The copy-assignment operator (memberwise over four scalars) */
        param_type &operator=(param_type const &) = default;

        /**
		   * @brief Access to the mean() value.
		   *
		   * @return The position midway between the two peaks
		   */
        [[nodiscard]] fp_type mean() const {
            return mean_;
        }
        /**
		   * @brief Access to the sigma1 value.
		   *
		   * @return The standard deviation (width) of the left peak
		   */
        [[nodiscard]] fp_type sigma1() const {
            return sigma1_;
        }
        /**
		   * @brief Access to the sigma2 value.
		   *
		   * @return The standard deviation (width) of the right peak
		   */
        [[nodiscard]] fp_type sigma2() const {
            return sigma2_;
        }
        /**
		   * @brief Access to the distance value.
		   *
		   * @return The distance between the two peaks
		   */
        [[nodiscard]] fp_type distance() const {
            return distance_;
        }

        /** @brief Compare for equality with another param_type object (memberwise) */
        friend bool operator==(const param_type &, const param_type &) = default;

    private:
        fp_type mean_ = 0.;
        fp_type sigma1_ = 0.;
        fp_type sigma2_ = 0.;
        fp_type distance_ = 0.;
    };

    /********************************************************************************************/
    // Deleted or defaulted constructors and assignment operators

    bi_normal_distribution() = default;

    bi_normal_distribution(bi_normal_distribution<fp_type> &&) = delete;
    bi_normal_distribution<fp_type> &operator=(bi_normal_distribution<fp_type> &&) = delete;

    /********************************************************************************************/

    /**
	  * @brief The standard constructor.
	  *
	  * @param mean The position midway between the two peaks
	  * @param sigma1 The standard deviation (width) of the left peak
	  * @param sigma2 The standard deviation (width) of the right peak
	  * @param distance The distance between the two peaks
	  */
    bi_normal_distribution(fp_type mean, fp_type sigma1, fp_type sigma2, fp_type distance)
      : params_(mean, sigma1, sigma2, distance)
      , params_store_(params_) { /* nothing */
    }

    /**
	  * @brief Initialization with a param_type object.
	  *
	  * @param params The distribution parameters to use
	  */
    explicit bi_normal_distribution(param_type const &params)
      : params_(params)
      , params_store_(params) { /* nothing */
    }

    /** @brief The copy constructor (memberwise, so it cannot silently drop a member; the former
     *  hand-written version skipped the inner normal distribution and the boolean source).
     *  Copy assignment is implicitly deleted through the const params_store_ member -- a
     *  distribution carrying a const reset target is not assignable (the former hand-written
     *  operator assigned that const member and was ill-formed; it merely was never instantiated). */
    bi_normal_distribution(bi_normal_distribution<fp_type> const &) = default;

    /**
     * @brief Returns the middle between both peaks.
     *
     * @return The position midway between the two peaks
     */
    [[nodiscard]] fp_type mean() const {
        return params_.mean();
    }
    /**
     * @brief Returns the sigma value of the first peak.
     *
     * @return The standard deviation (width) of the left peak
     */
    [[nodiscard]] fp_type sigma1() const {
        return params_.sigma1();
    }
    /**
     * @brief Returns the sigma value of the second peak.
     *
     * @return The standard deviation (width) of the right peak
     */
    [[nodiscard]] fp_type sigma2() const {
        return params_.sigma2();
    }
    /**
     * @brief Returns the distance between both peaks.
     *
     * @return The distance between the two peaks
     */
    [[nodiscard]] fp_type distance() const {
        return params_.distance();
    }

    /**
     * @brief Returns a parameter object holding information on the distribution parameters.
     *
     * @return A const reference to the internally stored distribution parameters
     */
    [[nodiscard]] const typename bi_normal_distribution<fp_type>::param_type &param() const {
        return params_;
    }

    /**
     * @brief Sets the distribution parameters from another param object.
     *
     * @param params The distribution parameters to adopt
     */
    void param(const typename bi_normal_distribution<fp_type>::param_type &params) {
        params_ = params;
    }

    /**
     * @brief Returns the minimum value of the distribution.
     *
     * Returns the minimum value of the distribution. As we are
     * essentially dealing with gaussian distributions, any floating
     * point value is allowed.
     *
     * @return The lowest representable value of fp_type
     */
    [[nodiscard]] fp_type(min)() const {
        return std::numeric_limits<fp_type>::lowest();
    }

    /**
      * @brief Returns the maximum value of the distribution.
      *
     * Returns the maximum value of the distribution. As we are
     * essentially dealing with gaussian distributions, any floating
     * point value is allowed.
      *
      * @return The largest representable value of fp_type
      */
    [[nodiscard]] fp_type(max)() const {
        return (std::numeric_limits<fp_type>::max)();
    }

    /**
     * @brief Resets the distribution to the values used for its construction.
     */
    void reset() {
        params_ = params_store_;
    };

    /**
      * @brief Returns the next random number with a bi_normal distribution.
      *
      * Returns a the next random number with a bi_normal distribution
      * according to the data contained in the param_type object
      *
      * @tparam T_Generator The uniform-random-bit-generator type
      * @param g The generator to draw from
      * @param params The distribution parameters to use
      * @return A bi-normally distributed deviate
      */
    template <class T_Generator>
    result_type operator()(T_Generator &g, const param_type &params) {
        if(uniform_bool_(g)) {
            fp_type mean_left = params.mean() - std::abs(params.distance() / 2.);
            fp_type sigma_left = params.sigma1();
            return (sigma_left * normal_distribution_(g)) + mean_left;
        }
                    fp_type mean_right = params.mean() + std::abs(params.distance() / 2.);
            fp_type sigma_right = params.sigma2();
            return (sigma_right * normal_distribution_(g)) + mean_right;
       
    }

    /**
      * @brief Returns the next random number with a bi_normal distribution.
      *
      * Returns a the next random number with a bi_normal distribution,
      * using the distribution parameters stored internally.
      *
      * @tparam T_Generator The uniform-random-bit-generator type
      * @param g The generator to draw from
      * @return A bi-normally distributed deviate using the stored parameters
      */
    template <class T_Generator>
    result_type operator()(T_Generator &g) {
        return (*this)(g, params_);
    }

private:
    g_normal_distribution<fp_type>
        normal_distribution_{}; ///< Needed to form each gaussian "hill" of the distribution
    g_bernoulli_distribution
        uniform_bool_; ///< Needed to decide whether a gaussian is created for the left or right peak

    param_type params_{
        fp_type(DEF_BINORM_MEAN),
        fp_type(DEF_BINORM_SIGMA1),
        fp_type(DEF_BINORM_SIGMA2),
        fp_type(DEF_BINORM_DISTANCE)
    }; ///< The actual parameter values being used
    const param_type params_store_{
        params_
    }; ///< The values the distribution will be reset to when reset() is called
};

/******************************************************************************/
/**
 * @brief Checks two bi_normal_distribution distributions for equality.
 *
 * @tparam fp_type The floating point type of the deviates (float or double)
 * @param lhs The left-hand-side distribution
 * @param rhs The right-hand-side distribution
 * @return true if both distributions share the same parameters, false otherwise
 */
template <std::floating_point fp_type>
bool operator==(
    const bi_normal_distribution<fp_type> &lhs,
    const bi_normal_distribution<fp_type> &rhs
) {
    return lhs.param() == rhs.param();
};

/******************************************************************************/

} /* namespace Gem::Hap */
