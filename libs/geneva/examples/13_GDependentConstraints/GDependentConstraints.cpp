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
#include "geneva/GOptimizer.hpp"

// The individual that should be optimized
#include "geneva/individuals/GFunctionIndividual.hpp"

using namespace Gem::Geneva;

// NOLINTNEXTLINE(readability-function-size) -- example 13's main(): a single linear setup script (CLI options, GOptimizer construction, constraint-combiner wiring, adaption-config registration, run); splitting would scatter tightly sequential one-shot setup steps
int main(int argc, char **argv) {
    //---------------------------------------------------------------------------
    // We want to add additional command line options

    bool printValid = false;
    bool useRawFitness = false;
    std::string monitorSpec = "empty";
    bool observeBoundaries = false;

    // The program's own options, in the launcher's command line and help text
    auto user_options = [&](Gem::Common::GParserBuilder &gpb) {
        gpb.registerCLParameter<bool>(
            "validOnly",
            printValid,
            false,
            "Enforces output of valid solutions only",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<bool>(
            "useRawFitness",
            useRawFitness,
            false,
            "Plot untransformed fitness value, even if a transformation takes place for the "
            "purpose of optimization",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
        gpb.registerCLParameter<std::string>(
            "monitorSpec",
            monitorSpec,
            std::string("empty"),
            "Allows you to specify variables to be monitored like this: \"d(0, -10, 10)\" (a "
            "double, by its index)"
        );
        gpb.registerCLParameter<bool>(
            "observeBoundaries",
            observeBoundaries,
            false,
            "Only plot inside of specified boundaries (no effect, when monitorSpec hasn't been "
            "set)",
            Gem::Common::GCL_IMPLICIT_ALLOWED,
            true
        );
    };

    return runOptimization(
        argc,
        argv,
        {.userOptions = user_options,
         .produce = [&](GOptimizer &go, const GOptimizationDescription &) -> int {
             //---------------------------------------------------------------------------
             // Create a factory for GFunctionIndividual objects and perform
             // any necessary initial work.
             auto const gfi_ptr = std::make_shared<Individuals::GFunctionIndividualFactory>(
                 "./config/GFunctionIndividual.json"
             );

             // We want the GFunctionIndividual objects to always use bounded parameters. This is expressed in the
             // config file (config/GFunctionIndividual.json: "parameter_type": 3 == BOUNDED_PER_PARAMETER) rather
             // than through a factory setter -- the generic GIndividualFactory re-applies its config file on
             // every produced object, so a programmatic setter would not stick.

             //---------------------------------------------------------------------------
             // Register a progress plotter with the global optimization algorithm factory
             if(monitorSpec != "empty") {
                 auto const progplot_ptr = std::make_shared<GProgressPlotter>();

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
                 std::shared_ptr<Individuals::GFunctionIndividual> const p =
                     gfi_ptr->get_as<Individuals::GFunctionIndividual>();

                 // Create the constraint objects
                 auto const doublesum_constraint_ptr =
                     std::make_shared<Individuals::GDoubleSumConstraint>(1.);
                 auto const sphere_constraint_ptr =
                     std::make_shared<Individuals::GSphereConstraint>(3.);
                 auto const gap_constraint = std::make_shared<Individuals::GDoubleSumGapConstraint>(
                     1.,
                     0.05
                 ); // The sum of all variables must be 1 +/- 0.05

                 // Create a check combiner and add the constraint objects to it. Constraints are
                 // expressed directly as C++ constraint objects (subclasses of GGenomeConstraint),
                 // which is the general, type-safe way to formulate arbitrary dependent constraints.
                 auto const combiner_ptr = std::make_shared<GCheckCombinerT<Genome::GGenome>>();
                 combiner_ptr->setCombinerPolicy(
                     Gem::Geneva::validityCheckCombinerPolicy::MULTIPLYINVALID
                 );

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
             // authors. Register it for the adapting algorithms (EA / SA) so GOptimizer hands it
             // over before they run.
             {
                 auto sample = gfi_ptr->get_as<Individuals::GFunctionIndividual>();
                 auto cfg = gfi_ptr->getAdaptionConfig(*sample);
                 go.registerAdaptionConfig("ea", cfg);
                 go.registerAdaptionConfig("sa", cfg);
             }

             // Perform the actual optimization
             auto const p =
                 go.optimize().getBestGlobalIndividual<Individuals::GFunctionIndividual>();

             // Here you can do something with the best individual ("p") found.
             // We simply print its content here, by means of an operator<< implemented
             // in the GFunctionIndividual code.
             std::cout << "Best result found:" << '\n' << p << '\n';
             return 0;
         }}
    );
}
