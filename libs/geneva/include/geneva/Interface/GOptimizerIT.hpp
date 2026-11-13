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
#include <concepts>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Boost header files go here

// Geneva headers go here
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva::Interface {

/******************************************************************************/
/**
 * @brief Common interface that every optimization algorithm must implement.
 *
 * Provides type-safe access (via dynamic_pointer_cast / clone) to the best individuals
 * found so far -- both globally and within the current iteration -- as well as basic
 * algorithm metadata and the optimization trigger. The concrete behaviour is supplied by
 * derived classes through the private virtual hooks (the underscore-suffixed members).
 *
 * @tparam optimizer_type The concrete optimization algorithm type returned by optimize().
 */
template <typename optimizer_type>
class GOptimizerIT {
public:
    /***************************************************************************/
    // Deleted move construction and move assignment operators. For now moving
    // optimization algorithms seems too complex and of very limited use, so
    // we prevent it until the need arises.

    /** @brief Deleted move constructor (moving optimization algorithms is not supported). */
    GOptimizerIT(GOptimizerIT<optimizer_type> &&) noexcept = delete;
    /** @brief Deleted move assignment (moving optimization algorithms is not supported). */
    GOptimizerIT<optimizer_type> &
    operator=(GOptimizerIT<optimizer_type> &&) noexcept = delete;

    /***************************************************************************/
    /**
	  * @brief Triggers the optimization cycle, starting to count iterations at a given offset.
	  *
	  * @param offset The iteration number at which to start counting (default 0)
	  * @return A pointer to this optimization algorithm once the cycle has completed
	  */
    optimizer_type const *optimize(std::uint32_t offset = 0) {
        return this->optimize_(offset);
    }

    /***************************************************************************/
    /**
	  * @brief Retrieves the best individual found so far and converts it to a given target type.
	  *
	  * The individual handed back is a fresh copy, solely owned by the caller: it cannot be used to modify
	  * the optimizer's own best. Throws if the cast to individual_type fails.
	  *
	  * @tparam individual_type The concrete individual type to cast the best individual to (must derive from gen::GGenome)
	  * @return A copy of the best individual found in the optimization run
	  */
    //! [results-accessor-signatures#1]
    template <typename individual_type>
        requires std::derived_from<individual_type, gen::GGenome>
    std::unique_ptr<individual_type> getBestGlobalIndividual() const {
        std::scoped_lock<std::mutex> const iteration_best_lock(get_best_mutex_);
        return castBest_<individual_type>(
            this->getBestGlobalIndividual_(), "getBestGlobalIndividual");
    }
    //! [results-accessor-signatures#1]

    /***************************************************************************/
    /**
	  * @brief Retrieves a list of the best individuals found so far and converts them to a given target type.
	  *
	  * The individuals handed back are fresh copies, solely owned by the caller: they cannot be used to
	  * modify the optimizer's own bests. Throws if the collection is empty or if any cast fails.
	  *
	  * @tparam individual_type The concrete individual type to cast each best individual to (must derive from gen::GGenome)
	  * @return A list of copies of the best individuals found in the optimization run
	  */
    //! [results-accessor-signatures#2]
    template <typename individual_type>
        requires std::derived_from<individual_type, gen::GGenome>
    std::vector<std::unique_ptr<individual_type>> getBestGlobalIndividuals() const {
        std::scoped_lock<std::mutex> const iteration_best_lock(get_best_mutex_);

        std::vector<std::unique_ptr<individual_type>> best_individuals;
        std::vector<std::unique_ptr<gen::GGenome>> best_base_individuals =
            this->getBestGlobalIndividuals_();

        // Cross check that we indeed got a valid set of individuals
        if(best_base_individuals.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GOptimizerIT<optimizer_type>::getBestGlobalIndividuals(): Error!"
                << '\n'
                << "Received empty collection of best individuals." << '\n'
            );
        }

        for(auto &ind_ptr : best_base_individuals) {
            best_individuals.push_back(
                castBest_<individual_type>(std::move(ind_ptr), "getBestGlobalIndividuals"));
        }

        return best_individuals;
    }
    //! [results-accessor-signatures#2]

    /***************************************************************************/
    /**
	  * @brief Retrieves the best individual found in the iteration and converts it to a given target type.
	  *
	  * The individual handed back is a fresh copy, solely owned by the caller: it cannot be used to modify
	  * the optimizer's own best. Retrieval of this copy is protected by a lock, so that potentially
	  * costly operations on results may be performed in parallel (i.e. a copy of the best individual
	  * is retrieved under protection, any action on this individual may then be carried out in parallel).
	  * Throws if the cast to individual_type fails.
	  *
	  * @tparam individual_type The concrete individual type to cast the best individual to (must derive from gen::GGenome)
	  * @return A copy of the best individual found in the iteration
	  */
    //! [results-accessor-signatures#3]
    template <typename individual_type>
        requires std::derived_from<individual_type, gen::GGenome>
    std::unique_ptr<individual_type> getBestIterationIndividual() const {
        std::scoped_lock<std::mutex> const iteration_best_lock(get_best_mutex_);
        return castBest_<individual_type>(
            this->getBestIterationIndividual_(), "getBestIterationIndividual");
    }
    //! [results-accessor-signatures#3]

    /***************************************************************************/
    /**
	  * @brief Retrieves a list of the best individuals found in the iteration and converts them to a given target type.
	  *
	  * The individuals handed back are fresh copies, solely owned by the caller: they cannot be used to
	  * modify the optimizer's own bests. Throws if the collection is empty or if any cast fails.
	  *
	  * @tparam individual_type The concrete individual type to cast each best individual to (must derive from gen::GGenome)
	  * @return A list of copies of the best individuals found in the iteration
	  */
    //! [results-accessor-signatures#4]
    template <typename individual_type>
        requires std::derived_from<individual_type, gen::GGenome>
    std::vector<std::unique_ptr<individual_type>> getBestIterationIndividuals() const {
        std::scoped_lock<std::mutex> const iteration_best_lock(get_best_mutex_);

        std::vector<std::unique_ptr<individual_type>> best_individuals;
        std::vector<std::unique_ptr<gen::GGenome>> best_base_individuals =
            this->getBestIterationIndividuals_();

        // Cross check that we indeed got a valid set of individuals
        if(best_base_individuals.empty()) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GOptimizerIT<optimizer_type>::getBestIterationIndividuals(): "
                   "Error!"
                << '\n'
                << "Received empty collection of best individuals." << '\n'
            );
        }

        for(auto &ind_ptr : best_base_individuals) {
            best_individuals.push_back(
                castBest_<individual_type>(std::move(ind_ptr), "getBestIterationIndividuals"));
        }

        return best_individuals;
    }
    //! [results-accessor-signatures#4]

    /***************************************************************************/
    /**
	  * @brief Returns one-word information about the type of optimization algorithm.
	  *
	  * @return A short string identifying the algorithm's personality type
	  */
    std::string getAlgorithmPersonalityType() const {
        return this->getAlgorithmPersonalityType_();
    }

    /***************************************************************************/
    /**
	  * @brief Returns a descriptive name assigned to this algorithm.
	  *
	  * @return The human-readable name of this optimization algorithm
	  */
    std::string getAlgorithmName() const {
        return this->getAlgorithmName_();
    }

    /***************************************************************************/
    /**
	  * @brief Retrieves the current iteration of this object.
	  *
	  * @return The current iteration counter
	  */
    std::uint32_t getIteration() const {
        return this->getIteration_();
    }

protected:
    /***************************************************************************/
    // Defaulted or constructors / destructors / assignment operators

    /** @brief The default constructor. */
    GOptimizerIT() = default;
    /** @brief The copy constructor. */
    GOptimizerIT(GOptimizerIT<optimizer_type> const &) = default;

    /**
 	  * @brief The destructor.
 	  *
 	  * Making this function protected and non-virtual follows
 	  * this discussion: http://www.gotw.ca/publications/mill18.htm
 	  */
    ~GOptimizerIT() = default;

    /** @brief The copy assignment operator. */
    GOptimizerIT<optimizer_type> &
    operator=(GOptimizerIT<optimizer_type> const &) = default;

    /***************************************************************************/

private:
    /**
     * @brief Performs the actual optimization cycle, starting to count iterations at a given offset.
     *
     * @param offset The iteration number at which to start counting
     * @return A pointer to this optimization algorithm once the cycle has completed
     */
    virtual optimizer_type const *optimize_(std::uint32_t offset) = 0;

    /** @brief Calculates the fitness of all required individuals; to be re-implemented in derived classes */
    virtual void evaluatePopulation_() = 0;

    /** @brief Retrieves the best individual found globally */
    virtual std::unique_ptr<gen::GGenome> getBestGlobalIndividual_() const = 0;
    /** @brief Retrieves a list of the best individuals found globally*/
    virtual std::vector<std::unique_ptr<gen::GGenome>>
    getBestGlobalIndividuals_() const = 0;
    /** @brief Retrieves the best individual found in the current iteration*/
    virtual std::unique_ptr<gen::GGenome> getBestIterationIndividual_() const = 0;
    /** @brief Retrieves a list of the best individuals found in the current iteration */
    virtual std::vector<std::unique_ptr<gen::GGenome>>
    getBestIterationIndividuals_() const = 0;

    /** @brief Returns one-word information about the type of optimization algorithm. */
    virtual std::string getAlgorithmPersonalityType_() const = 0;
    /** @brief Returns a descriptive name assigned to this algorithm */
    virtual std::string getAlgorithmName_() const = 0;
    /** @brief Retrieves the current iteration of this object */
    virtual std::uint32_t getIteration_() const = 0;

    /***************************************************************************/
    /**
     * @brief Converts a solely-owned best individual to the requested type, or throws.
     *
     * The four accessors above all receive sole ownership from their hooks, so the conversion is a
     * dynamic_cast plus a transfer of ownership -- there is no pointer-cast helper for unique_ptr, and
     * writing it once keeps the four accessors' failure diagnostics identical.
     *
     * @tparam individual_type The type the individual is converted to
     * @param ind_ptr The individual to convert (ownership is taken)
     * @param accessor The calling accessor's name, for the diagnostic
     * @return The same individual, owned as an individual_type
     */
    template <typename individual_type>
    static std::unique_ptr<individual_type> castBest_(
        std::unique_ptr<gen::GGenome> ind_ptr, std::string_view accessor
    ) {
        if(auto *converted = dynamic_cast<individual_type *>(ind_ptr.get()); converted != nullptr) {
            ind_ptr.release(); // ownership passes to the converted pointer
            return std::unique_ptr<individual_type>(converted);
        }
        throw geneva_exception(
            g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
            << "In GOptimizerIT<>::" << accessor << "(): Error!" << '\n'
            << "The best individual is not of the requested individual_type." << '\n'
        );
    }

    /***************************************************************************/
    // Data

    mutable std::mutex get_best_mutex_; ///< Protects access to the best individuals (global and per-iteration)
};

/******************************************************************************/

} /* namespace Gem::Geneva::Interface */

/******************************************************************************/
