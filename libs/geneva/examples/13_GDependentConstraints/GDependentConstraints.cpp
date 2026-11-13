/**
 * @file GDependentConstraints.cpp
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

// Boost header files go here

// Geneva header files go here
#include "geneva/Go2.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;
namespace po = boost::program_options;

// NOLINTNEXTLINE(readability-function-size) -- example 13's main(): a single linear setup script (CLI options, Go2 construction, constraint-combiner wiring, adaption-config registration, run); splitting would scatter tightly sequential one-shot setup steps
int main(int argc, char **argv) {
    //---------------------------------------------------------------------------
    // We want to add additional command line options

    bool printValid = false;
    bool useRawFitness = false;
    std::string monitorSpec = "empty";
    bool observeBoundaries = false;

    // Assemble command line options
    boost::program_options::options_description user_options;
    user_options.add_options() (
		"validOnly"
		, po::value<bool>(&printValid)->implicit_value(true)->default_value(false) // This allows you say both --validOnly and --validOnly=true
		, "Enforces output of valid solutions only"
	)(
		"useRawFitness"
		, po::value<bool>(&useRawFitness)->implicit_value(true)->default_value(false) // This allows you say both --useRawFitness and --useRawFitness=true
		, "Plot untransformed fitness value, even if a transformation takes place for the purpose of optimization"
	)(
		"monitorSpec"
		, po::value<std::string>(&monitorSpec)->default_value(std::string("empty"))
		, "Allows you to specify variables to be monitored like this: \"d(var0 -10 10)\""
	)(
		"observeBoundaries"
		, po::value<bool>(&observeBoundaries)->implicit_value(true)->default_value(false) // This allows you say both --observeBoundaries and --observeBoundaries=true
		, "Only plot inside of specified boundaries (no effect, when monitorSpec hasn't been set)"
	);

    Go2 go(argc, argv, "./config/Go2.json", user_options);

    //---------------------------------------------------------------------------
    // Client mode
    if(go.clientMode()) {
        return go.clientRun();
    } // Execution will end here in client mode

    //---------------------------------------------------------------------------
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    std::shared_ptr<gind::GFunctionIndividualFactory> const gfi_ptr(
        new gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json")
    );

    // We want the GFunctionIndividual objects to always use bounded parameters. This is expressed in the
    // config file (config/GFunctionIndividual.json: "parameter_type": 3 == BOUNDED_PER_PARAMETER) rather
    // than through a factory setter -- the generic GIndividualFactory re-applies its config file on
    // every produced object, so a programmatic setter would not stick.

    //---------------------------------------------------------------------------
    // Register a progress plotter with the global optimization algorithm factory
    if(monitorSpec != "empty") {
        std::shared_ptr<GProgressPlotter> const progplot_ptr(new GProgressPlotter());

        progplot_ptr->setProfileSpec(monitorSpec);
        progplot_ptr->setObserveBoundaries(observeBoundaries);
        progplot_ptr->setMonitorValidOnly(
            printValid
        ); // Only record valid parameters, when printValid is set to true
        progplot_ptr->setUseRawEvaluation(
            useRawFitness
        ); // Use untransformed evaluation values for logging

        go.registerPluggableOM(progplot_ptr);
    }

    //---------------------------------------------------------------------------
    // Add a number of start values to the go object. We also add some constraint definitions here.
    for(std::size_t i = 0; i < 10; i++) {
        std::shared_ptr<gind::GFunctionIndividual> const p = gfi_ptr->get_as<gind::GFunctionIndividual>();

        // Create the constraint objects
        std::shared_ptr<gind::GDoubleSumConstraint> const doublesum_constraint_ptr(
            new gind::GDoubleSumConstraint(1.)
        );
        std::shared_ptr<gind::GSphereConstraint> const sphere_constraint_ptr(new gind::GSphereConstraint(3.));
        std::shared_ptr<gind::GDoubleSumGapConstraint> const gap_constraint(
            new gind::GDoubleSumGapConstraint(1., 0.05)
        ); // The sum of all variables must be 1 +/- 0.05

        // Create a check combiner and add the constraint objects to it. Constraints are
        // expressed directly as C++ constraint objects (subclasses of GGenomeConstraint),
        // which is the general, type-safe way to formulate arbitrary dependent constraints.
        std::shared_ptr<GCheckCombinerT<gen::GGenome>> const combiner_ptr(
            new GCheckCombinerT<gen::GGenome>()
        );
        combiner_ptr->setCombinerPolicy(Gem::Geneva::validityCheckCombinerPolicy::MULTIPLYINVALID);

        combiner_ptr->addCheck(doublesum_constraint_ptr);
        combiner_ptr->addCheck(sphere_constraint_ptr);
        combiner_ptr->addCheck(gap_constraint);

        // Register the combiner with the individual (note: we could also have registered
        // one of the "single" constraints here (see below for commented-out examples)
        p->registerConstraint(combiner_ptr);

        // p->registerConstraint(doublesum_constraint_ptr);
        // p->registerConstraint(sphere_constraint_ptr);
        // p->registerConstraint(gap_constraint);

        go.push_back(p);
    }

    //---------------------------------------------------------------------------

    // The genome carries only structure; the configured adaptor lives on an OA-owned config the factory
    // authors. Register it for the adapting algorithms (EA / SA) so Go2 hands it over before they run.
    {
        auto sample = gfi_ptr->get_as<gind::GFunctionIndividual>();
        auto cfg = gfi_ptr->getAdaptionConfig(*sample);
        go.registerAdaptionConfig("PERSONALITY_EA", cfg);
        go.registerAdaptionConfig("PERSONALITY_SA", cfg);
    }

    // Perform the actual optimization
    auto const p =
        go.optimize()->getBestGlobalIndividual<gind::GFunctionIndividual>();

    // Here you can do something with the best individual ("p") found.
    // We simply print its content here, by means of an operator<< implemented
    // in the GFunctionIndividual code.
    std::cout << "Best result found:" << '\n' << p << '\n';
}
