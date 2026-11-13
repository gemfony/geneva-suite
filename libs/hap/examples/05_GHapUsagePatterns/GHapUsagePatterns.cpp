/**
 * @file GHapUsagePatterns.cpp
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

// Standard header files go here
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

// Boost header files go here

// Geneva header files go here
#include "hap/GRandomDistributionsT.hpp"
#include "hap/GRandomT.hpp"

const int NPROD = 1000;

// All GRandom-related code is in the namespace Gem::Hap
using namespace Gem::Hap;

/***********************************************************************************/
/**
 * The main function
 */
int main(int argc, char **argv) {
    // Instantiate a random number generator
    // NOTE: You could use GRandomT<QUEUE> instead
    GRandom gr;

    std::uniform_real_distribution<double> uniform_real_distribution(0., 1.);
    std::normal_distribution<double> normal_distribution;
    Gem::Hap::bi_normal_distribution<double>
        bi_normal_distribution(1. /*mean*/, 2. /*sigma1*/, 1. /*sigma2*/, 3. /*distance*/);
    std::bernoulli_distribution uniform_bool;        // defaults to a probability of 0.5
    std::bernoulli_distribution weighted_bool(0.25); // 25% "true" values
    std::uniform_int_distribution<std::int32_t> uniform_int_distribution;

    for(int i = 0; i < NPROD; i++) {
        {
            // Random numbers with an even distribution of
            // double values in the range [0,1[
            double const d_even_01 = uniform_real_distribution(gr);

            // Note: GRandomBase defines an operator(), hence
            // you could also use gr_() to obtain a random number
            // of this type.
        }

        {
            // Random numbers with an even distribution of
            // double values in the range [0.,max[
            double const max = 10.;
            double const d_even_0_max = uniform_real_distribution(
                gr,
                std::uniform_real_distribution<double>::param_type(0., max)
            );
        }

        {
            // Random numbers with an even distribution of
            // double values in the range [min,max[
            double const min = -10.;
            double const max = 10.;
            double const d_even_min_max = uniform_real_distribution(
                gr,
                std::uniform_real_distribution<double>::param_type(min, max)
            );
        }

        {
            // A normal ("gaussian") distribution of random numbers
            // with mean 0 and sigma 1
            double const d_std_gauss = normal_distribution(gr);
        }

        {
            // A normal ("gaussian") distribution of random numbers
            // with mean "mean" and sigma "sigma"
            double const mean = 1.;
            double const sigma = 2.;
            double const d_gauss_mean_sigma =
                normal_distribution(gr, std::normal_distribution<double>::param_type(mean, sigma));
        }

        {
            // This function adds two gaussians with sigmas "sigma1", "sigma2" and a distance
            // of "distance" from each other, centered around mean. The idea is to use
            // this function in conjunction with evolutionary strategies, so we avoid
            // searching with the highest likelihood at a location where we already
            // know a good value exists. Rather we want to shift the highest likelihood
            // for probes a bit further away from the candidate solution.
            double const d_bi_gauss_difsigma = bi_normal_distribution(gr);
            double const d_bi_gauss_difsigma2 =
                bi_normal_distribution(gr, bi_normal_distribution.param());
        }

        {
            // This function produces boolean values with a 50% likelihood each for true and false
            bool const bool_rnd = uniform_bool(gr);
        }

        {
            // This function returns true with a probability "0.25", otherwise false.
            bool const bool_rnd_weight = weighted_bool(gr);
        }

        {
            // This function produces integer random numbers in the range of [min, max] .
            // Note that max may also be < 0.
            std::int32_t const min = -10;
            std::int32_t const max = 10;
            std::int32_t const int_rand_min_max = uniform_int_distribution(
                gr,
                std::uniform_int_distribution<std::int32_t>::param_type(min, max)
            );
        }
    }

    return 0;
}
