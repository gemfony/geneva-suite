/**
 * @file GOAFactoryT.hpp
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

// Global checks, defines and includes needed for all of Geneva
#include "common/GGlobalDefines.hpp"

// Standard header files go here
#include <chrono>
#include <filesystem>
#include <string>
#include <utility>

// Boost header files go here

// Geneva headers go here
#include "common/GCommonHelperFunctionsT.hpp"
#include "common/GFactoryT.hpp"
#include "common/GProviderT.hpp"
#include "courtier/GCourtierEnums.hpp"
#include "geneva/oa/GPluggableOptimizationMonitors.hpp"
#include "geneva/genome/GGenome.hpp"

#ifdef GEM_TESTING
#include <catch2/catch_test_macros.hpp>
#endif /* GEM_TESTING */

namespace Gem::Geneva::OptimizationAlgorithms {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * This class is a specialization of the GFactoryT<> class for optimization algorithms.
 *
 * It is at the same time the mnemonic-keyed *provider* the global algorithm store holds: an
 * optimization-algorithm factory IS the thing that hands out algorithms by mnemonic, so it implements
 * Gem::Common::GProviderT directly rather than being wrapped in a separate provider adapter. That makes the
 * registration seam a single one: a built-in algorithm (GInitializerT, at static init) and a runtime-loaded
 * algorithm module (oaManifest() -> GModuleLoader) both hand the very same object -- this factory -- to
 * registerOptimizationAlgorithm().
 *
 * @tparam oa_type The concrete optimization-algorithm type this factory produces
 */
template <typename oa_type>
class GOAFactoryT // NOLINT(cppcoreguidelines-special-member-functions)
  : public Gem::Common::GFactoryT<oa_type>
  , public Gem::Common::GProviderT<oa_type> {
public:
    /***************************************************************************/
    // Let the audience know what type of algorithm will be produced
    using pType = oa_type;

    /***************************************************************************/
    // Deleted default constructor
    GOAFactoryT() = delete;

    /***************************************************************************/
    /**
	  * Initialization with the name of a config file
	  *
	  * @param config_file The path to the configuration file describing the algorithm
	  */
    explicit GOAFactoryT(std::filesystem::path const &config_file)
      : Gem::Common::GFactoryT<oa_type>(config_file) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * A constructor which adds a content creation function
	  *
	  * @param config_file The path to the configuration file describing the algorithm
	  * @param content_creator_ptr A factory that produces the individuals used to populate the algorithm
	  */
    GOAFactoryT(
        std::filesystem::path const &config_file,
        std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>> content_creator_ptr
    )
      : Gem::Common::GFactoryT<oa_type>(config_file)
      , content_creator_ptr_(std::move(content_creator_ptr)) { /* nothing */
    }

    /***************************************************************************/
    /**
	  * The copy constructor
	  *
	  * @param cp Another GOAFactoryT object whose state (incl. content creator and monitor) is deep-copied
	  */
    GOAFactoryT(const GOAFactoryT<oa_type> &cp)
      : Gem::Common::GFactoryT<oa_type>(cp)
      , max_iteration_cl_(cp.max_iteration_cl_)
      , max_stall_iteration_cl_(cp.max_stall_iteration_cl_)
      , max_seconds_cl_(cp.max_seconds_cl_) {
        if(cp.content_creator_ptr_) {
            if(content_creator_ptr_) {
                content_creator_ptr_->load(cp.content_creator_ptr_);
            }
            else {
                content_creator_ptr_ = cp.content_creator_ptr_->clone();
            }
        }
        else {
            content_creator_ptr_.reset();
        }

        // The pluggable optimization monitor (a std::shared_ptr<GBasePluggableOM>)
        // must be deep-copied as well; otherwise a copied factory silently
        // produces algorithms with no monitor.
        if(cp.pluggable_om_) {
            pluggable_om_ = cp.pluggable_om_->template clone<GBasePluggableOM>();
        }
    }

    /***************************************************************************/
    /**
	  * The destructor
	  */
    ~GOAFactoryT() override = default;

    /***************************************************************************/
    /**
	  * Adds local command line options to a boost::program_options::options_description object.
	  * These are options common to all implemented algorithms. The command line parameter,
	  * however, needs to be specific to a given algorithm, so we can select which algorithm
	  * should receive which option. This happens with the help of the small mnemonic assigned
	  * to each algorithm (e.g. "ea" for evolutionary algorithms). In order not to "clutter"
	  * the output, some options are hidden and will only be shown upon explicit request by
	  * the user
	  *
	  * @param visible Command line options that should always be visible (currently unused here)
	  * @param hidden Command line options that should only be visible upon request
	  */
    void addCLOptions(
        [[maybe_unused]] boost::program_options::options_description & visible
        ,
        boost::program_options::options_description &hidden
    ) override {
        namespace po = boost::program_options;

        hidden.add_options()(
			 (this->getMnemonic() + std::string("MaxIterations")).c_str()
			 , po::value<std::int32_t>(&max_iteration_cl_)->default_value(CL_UNSET)
			 , (std::string("\t[FactoryT / ") + this->getMnemonic() +
				 "] The maximum allowed number of iterations or 0 to disable limit").c_str()
		 )(
			 (this->getMnemonic() + std::string("MaxStallIterations")).c_str()
			 , po::value<std::int32_t>(&max_stall_iteration_cl_)->default_value(CL_UNSET)
			 , (std::string("\t[FactoryT / ") + this->getMnemonic() +
				 "] The maximum allowed number of stalled iterations or 0 to disable limit").c_str()
		 )(
			 (this->getMnemonic() + std::string("MaxSeconds")).c_str()
			 , po::value<std::int32_t>(&max_seconds_cl_)->default_value(CL_UNSET)
			 , (std::string("\t[FactoryT / ") + this->getMnemonic() +
				 "] The maximum allowed duration in seconds or 0 to disable limit").c_str()
		 );
    }

    /***************************************************************************/
    /**
	  * Triggers the creation of objects of the desired type and converts them
	  * to a given target type. Will throw if conversion is unsuccessful.
	  *
	  * @tparam target_type The type the produced algorithm should be converted to
	  * @return A converted copy of the desired production type
	  */
    template <typename target_type>
    std::shared_ptr<target_type> get() {
        return Gem::Common::convertSmartPointer<oa_type, target_type>(
            Gem::Common::GFactoryT<oa_type>::get()
        );
    }

    /***************************************************************************/
    /**
	  * Allows to register a content creator
	  *
	  * @param cc_ptr A factory that produces the individuals used to populate the algorithm; must be non-empty
	  */
    void registerContentCreator(const std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>>& cc_ptr) {
        if(not cc_ptr) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GOptiomizationAlgorithmFactoryT<T>::registerContentCreator(): Error!"
                << '\n'
                << "Tried to register an empty pointer" << '\n'
            );
        }

        content_creator_ptr_ = cc_ptr;
    }

    /***************************************************************************/
    /**
	  * Allows to register a pluggable optimization monitor
	  *
	  * @param pluggable_om The pluggable optimization monitor to register; must be non-empty
	  */
    void registerPluggableOM(const std::shared_ptr<GBasePluggableOM>& pluggable_om) {
        if(pluggable_om) {
            pluggable_om_ = pluggable_om;
        }
        else {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In FactoryT<>::registerPluggableOM(): Tried to "
                   "register empty pluggable optimization monitor"
                << '\n'
            );
        }
    }

    /***************************************************************************/
    /**
	  * Allows to reset the local pluggable optimization monitor
	  */
    void resetPluggableOM() {
        pluggable_om_.reset();
    }

    /***************************************************************************/
    /**
	  * Gives access to the mnemonics / nickname describing an algorithm
	  *
	  * @return The short mnemonic / nickname identifying the algorithm (e.g. "ea")
	  */
    [[nodiscard]] std::string getMnemonic() const override = 0;

    /***************************************************************************/
    /**
	  * Gives access to a clear-text description of an algorithm
	  *
	  * @return A human-readable, clear-text name of the algorithm
	  */
    [[nodiscard]] virtual std::string getAlgorithmName() const = 0;

    /***************************************************************************/
    /**
	  * The provider interface's spelling of getAlgorithmName(). Final, so the algorithm's clear-text name
	  * has exactly one author-supplied source (getAlgorithmName()) no matter which of the two names a
	  * caller uses.
	  *
	  * @return A human-readable, clear-text name of the algorithm
	  */
    [[nodiscard]] std::string getName() const final { return this->getAlgorithmName(); }

    /***************************************************************************/
    /**
	  * Hands out a freshly configured algorithm -- the provider interface's spelling of the factory's
	  * get(). Every call re-reads the configuration file and bumps the instance id, which is what
	  * algorithm chaining (e.g. "ea,gd,swarm") relies on.
	  *
	  * @return A shared pointer to a newly created, configured optimization algorithm
	  */
    std::shared_ptr<oa_type> provide() override { return Gem::Common::GFactoryT<oa_type>::get(); }

    /***************************************************************************/
    /**
	  * Allows to manually set the maximum number of iterations as is usually specified on the command line
	  *
	  * @param max_iteration_cl The maximum allowed number of iterations
	  */
    void setMaxIterationCL(std::uint32_t max_iteration_cl) {
        max_iteration_cl_ = Gem::Common::narrow<std::int32_t>(max_iteration_cl);
    }

    /***************************************************************************/
    /**
	  * Allows to check whether the maximum number of iterations was set on the command line or using the manual function
	  *
	  * @return true if a maximum number of iterations was set, false otherwise
	  */
    [[nodiscard]] bool maxIterationsCLSet() const {
        return max_iteration_cl_ != CL_UNSET;
    }

    /***************************************************************************/
    /**
	  * Allows to retrieve the maximum number of iterations as set on the command line
	  *
	  * @return The maximum number of iterations set on the command line (throws if it was never set)
	  */
    [[nodiscard]] std::uint32_t getMaxIterationCL() const {
        if(max_iteration_cl_ != CL_UNSET) {
            return Gem::Common::narrow<std::uint32_t>(max_iteration_cl_);
        }
                    throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GOptimizationAlgorithmBase<>::getMaxIterationCL(): Error!" << '\n'
                << "max_iteration_cl_ wasn't set" << '\n'
            );

            // Make the compiler happy
            return static_cast<std::uint32_t>(0);
       
    }

    /***************************************************************************/
    /**
	  * Allows to manually set the maximum number of stall iterations as is usually specified on the command line
	  *
	  * @param max_stall_iteration_cl The maximum allowed number of stalled (improvement-free) iterations
	  */
    void setMaxStallIterationCL(std::uint32_t max_stall_iteration_cl) {
        max_stall_iteration_cl_ = Gem::Common::narrow<std::int32_t>(max_stall_iteration_cl);
    }

    /***************************************************************************/
    /**
	  * Allows to check whether the maximum number of stall iterations was set on the command line or using the manual function
	  *
	  * @return true if a maximum number of stall iterations was set, false otherwise
	  */
    [[nodiscard]] bool maxStallIterationsCLSet() const {
        return max_stall_iteration_cl_ != CL_UNSET;
    }

    /***************************************************************************/
    /**
	  * Allows to retrieve the maximum number of stall iterations as set on the command line
	  *
	  * @return The maximum number of stall iterations set on the command line (throws if it was never set)
	  */
    [[nodiscard]] std::uint32_t getMaxStallIterationCL() const {
        if(max_stall_iteration_cl_ != CL_UNSET) {
            return Gem::Common::narrow<std::uint32_t>(max_stall_iteration_cl_);
        }
                    throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GOptimizationAlgorithmBase<>::getMaxStallIterationCL(): Error!"
                << '\n'
                << "max_stall_iteration_cl_ wasn't set" << '\n'
            );

            // Make the compiler happy
            return static_cast<std::uint32_t>(0);
       
    }

    /***************************************************************************/
    /**
	  * Allows to manually set the maximum number of seconds for a run as is usually specified on the command line
	  *
	  * @param max_seconds_cl The maximum allowed run duration, in seconds
	  */
    void setMaxSecondsCL(std::uint32_t max_seconds_cl) {
        max_seconds_cl_ = Gem::Common::narrow<std::int32_t>(max_seconds_cl);
    }

    /***************************************************************************/
    /**
	  * Allows to check whether the maximum number of seconds was set on the command line or using the manual function
	  *
	  * @return true if a maximum run duration was set, false otherwise
	  */
    [[nodiscard]] bool maxSecondsCLSet() const {
        return max_seconds_cl_ != CL_UNSET;
    }

    /***************************************************************************/
    /**
	  * Allows to retrieve the maximum number of seconds as set on the command line
	  *
	  * @return The maximum run duration set on the command line, as a duration (throws if it was never set)
	  */
    [[nodiscard]] std::chrono::duration<double> getMaxTimeCL() const {
        if(max_seconds_cl_ != CL_UNSET) {
            std::chrono::duration<double> const max_duration =
                std::chrono::seconds(Gem::Common::narrow<long>(max_seconds_cl_));
            return max_duration;
        }
                    throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In GOptimizationAlgorithmBase<>::getMaxTimeCL(): Error!" << '\n'
                << "max_seconds_cl_ wasn't set" << '\n'
            );

            // Make the compiler happy
            return std::chrono::seconds(0);
       
    }

protected:
    /***************************************************************************/
    /**
      * Triggers the creation of objects of the desired type with the preset
      * parallelization mode.
      *
      * @return An object of the desired algorithm type
      */
    std::shared_ptr<oa_type> get_() override {
        // Retrieve a work item using the methods implemented in our parent class
        std::shared_ptr<oa_type> p_alg = Gem::Common::GFactoryT<oa_type>::get_();

        // If we have been given a factory function for individuals, fill the object with data
        if(content_creator_ptr_) { // Has a content creation object been registered ? If so, add individuals to the population
            for(std::size_t ind = 0; ind < p_alg->getDefaultPopulationSize(); ind++) {
                std::shared_ptr<gen::GGenome> const p_ind = (*content_creator_ptr_)();
                if(not p_ind) { // No valid item received, the factory has run empty
                    break;
                }
                p_alg->push_back(p_ind->clone());
               
            }
        }

        // Check if any pluggable optimization monitor was registered. If so,
        // load it into the optimization algorithm
        if(pluggable_om_) {
            p_alg->registerPluggableOM(pluggable_om_);
        }

        // Return the filled object to the audience
        return p_alg;
    }

    /***************************************************************************/
    /**
	  * Allows to describe configuration options
	  *
	  * @param gpb A reference to the parser-builder that collects the configuration options
	  */
    void describeLocalOptions_(Gem::Common::GParserBuilder &gpb) override {
        /* nothing */
    }

    /***************************************************************************/
    /**
	  * Allows to act on the configuration options received from the configuration file or from the command line
	  *
	  * @param p The freshly produced algorithm onto which the command-line limits are applied
	  */
    void postProcess_(std::shared_ptr<oa_type> &p) override {
        // Set local options

        // The maximum allowed number of iterations
        if(this->maxIterationsCLSet()) {
            p->oa_type::setMaxIteration(this->getMaxIterationCL());
        }

        // The maximum number of stalls until operation stops
        if(this->maxStallIterationsCLSet()) {
            p->oa_type::setMaxStallIteration(this->getMaxStallIterationCL());
        }

        // The maximum amount of time until operation stops
        if(this->maxSecondsCLSet()) {
            p->oa_type::setMaxTime(this->getMaxTimeCL());
        }
    }

    /***************************************************************************/

    std::shared_ptr<Gem::Common::GFactoryT<gen::GGenome>>
        content_creator_ptr_; ///< Holds an object capable of producing objects of the desired type
    std::shared_ptr<GBasePluggableOM>
        pluggable_om_; // A user-defined means for information retrieval

private:
    /***************************************************************************/
    /**
     * @brief Creates an algorithm object of this type.
     * @param gpb A reference to the parser-builder used to register the object's configuration options
     * @return A shared pointer to a freshly created algorithm object
     */
    std::shared_ptr<oa_type> getObject_(Gem::Common::GParserBuilder &) override = 0;

    /***************************************************************************/

    /// Sentinel for "not set on the command line". These are bound directly to a Boost.program_options
    /// value (which needs a concrete arithmetic type, so std::optional cannot be used), hence a signed
    /// type with a negative "unset" marker; the *CLSet() / get*() accessors present the optional-style
    /// "has value / get-or-throw" interface on top of it.
    static constexpr std::int32_t CL_UNSET = -1;

    std::int32_t max_iteration_cl_ = CL_UNSET;       ///< The maximum number of iterations (CL_UNSET == not set)
    std::int32_t max_stall_iteration_cl_ = CL_UNSET; ///< Max improvement-free generations before stopping (CL_UNSET == not set)
    std::int32_t max_seconds_cl_ = CL_UNSET;         ///< The maximum run duration in seconds (CL_UNSET == not set)
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva::OptimizationAlgorithms */

