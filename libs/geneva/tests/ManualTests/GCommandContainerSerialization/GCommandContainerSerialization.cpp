/**
 * @file GCommandContainerSerialization.cpp
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

/**
 * This test creates GFunctionIndividual objects, wraps them in GCommandContainerT
 * (with a GGenome payload), and verifies round-trip serialization fidelity:
 * a) It serializes and de-serializes a command container and checks that the payload
 *    contents are identical after the round-trip.
 * b) It processes the payload (evaluates the individual) and performs another
 *    serialize/de-serialize round-trip, checking that the results match.
 *
 * Additional tests of the GCommandContainerT and GGenome classes have been
 * implemented as part of the unit tests.
 */

// Standard header files go here
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Boost header files go here

// Geneva header files go here
#include "common/GConfigEmission.hpp"
#include "courtier/GCommandContainerT.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "hap/GRandomT.hpp"

using namespace Gem::Geneva;
using namespace Gem::Common;
using namespace boost;

const std::uint32_t DEFEXAMPLENTESTS = 5000;
const Gem::Common::serializationMode DEFEXAMPLESERMOD = Gem::Common::serializationMode::GEM_BINARY;
const Gem::Common::serializationMode PRINTOUTSERMOD = Gem::Common::serializationMode::GEM_JSON;

// NOLINTNEXTLINE(readability-function-size) -- main() of a manual serialization round-trip test: CL parsing, then one coherent per-iteration round-trip/process/round-trip check sequence with inline error reporting; splitting would scatter one linear test script
int main(int argc, char **argv) {
    // --update-configs: materialize the one config this test owns (the GFunctionIndividual factory's)
    // from code defaults, then exit without running the serialization test.
    if(Gem::Common::configEmissionRequested(argc, argv)) {
        Gem::Common::beginConfigEmission();
        gind::GFunctionIndividualFactory("./config/GFunctionIndividual.json").get();
        Gem::Common::finishConfigEmission();
    }

    //-----------------------------------------------------------------------------
    // Declare some local parameters
    std::uint32_t nTests = DEFEXAMPLENTESTS;
    Gem::Common::serializationMode serMod = DEFEXAMPLESERMOD;
    bool printLastWorkItem = false;

    //-----------------------------------------------------------------------------
    // Read in command line parameters

    // Create the parser builder
    Gem::Common::GParserBuilder gpb;

    // Add an option for the number of tests
    gpb.registerCLParameter<std::uint32_t>(
        "nTests,n",
        nTests,
        DEFEXAMPLENTESTS,
        "The number of tests to run"
    );

    // Add an option for the desired serialization mode
    gpb.registerCLParameter<Gem::Common::serializationMode>(
        "serializationMode,s",
        serMod,
        DEFEXAMPLESERMOD,
        "The serialization mode: (0) TEXT, (1) XML, (2) BINARY"
    );

    // Add an option to print the last (raw and de-de-serialized) work item as XML
    gpb.registerCLParameter<bool>(
        "printLastWorkItem,p",
        printLastWorkItem,
        false,
        "Whether the last work item should be printed (as XML, before and after submission / "
        "return)"
    );

    // Parse the command line and leave if the help flag was given. The parser
    // will emit an appropriate help message by itself
    if(Gem::Common::GCL_HELP_REQUESTED == gpb.parseCommandLine(argc, argv, true /*verbose*/)) {
        return 0; // Do not continue. This is expected.
    }

    //-----------------------------------------------------------------------------
    // Create a factory for GFunctionIndividual objects and perform
    // any necessary initial work.
    gind::GFunctionIndividualFactory gfi("./config/GFunctionIndividual.json");

    // Loop the specified number of times
    for(std::size_t i = 0; i < nTests; i++) {
        if(i % 100 == 0) {
            std::cout << "In iteration " << i << '\n';
        }

        // Retrieve a new work item from the factory
        auto fi_ptr = gfi.get_as<gind::GFunctionIndividual>();

        // Randomly initialize the object
        fi_ptr->randomInit(activityMode::ALLPARAMETERS);

        // Add the object to a new WORK frame
        Gem::Courtier::GCommandContainerT<gen::GGenome> gcc1(
            Gem::Courtier::GFrameKind::WORK, fi_ptr->clone());

        // Prepare a frame for de-serialization
        Gem::Courtier::GCommandContainerT<gen::GGenome> gcc2(Gem::Courtier::GFrameKind::NONE);

        // Serialize and de-serialize the object
        Gem::Courtier::container_from_string(
            Gem::Courtier::container_to_string(gcc1, serMod),
            gcc2,
            serMod
        );

        // Check that payloads 1+2 point to different objects
        if(gcc1.item() == gcc2.item()) {
            std::cout << "Error: payload 1+2 seem to point to the same object" << '\n';
            return 1; // Indicate an error to the calling process
        }

        // Check that the payloads of gcc1 and gcc2 are identical
        if(*(gcc1.item()) != *(gcc2.item())) {
            std::cout << "Error: Content of payload 1+2 differs" << '\n';
            return 1; // Indicate an error to the calling process
        }

        // Process payloads 1+2 and compare -- they should now again be identical
        gcc1.process();
        gcc2.process();
        if(gcc1.item() == gcc2.item()) {
            std::cout << "Error: payload 1+2 seem to point to the same object" << '\n';
            return 1; // Indicate an error to the calling process
        }

        // Prepare a frame for de-serialization
        Gem::Courtier::GCommandContainerT<gen::GGenome> gcc3(Gem::Courtier::GFrameKind::NONE);

        // Serialize and de-serialize the object
        Gem::Courtier::container_from_string(
            Gem::Courtier::container_to_string(gcc2, serMod),
            gcc3,
            serMod
        );

        // Check that payloads 2+3 point to different objects
        if(gcc2.item() == gcc3.item()) {
            std::cout << "Error: payload 2+3 seem to point to the same object" << '\n';
            return 1; // Indicate an error to the calling process
        }

        // Check that the payloads of gcc2 and gcc3 are identical
        if(*(gcc2.item()) != *(gcc3.item())) {
            std::cout << "Error: Content of payload 2+3 differs" << '\n';
            return 1; // Indicate an error to the calling process
        }

        // Check that the payloads of gcc1 and gcc3 are identical
        if(*(gcc1.item()) != *(gcc3.item())) {
            std::cout << "Error: Content of payload 1+3 differs" << '\n';
            return 1; // Indicate an error to the calling process
        }

        if(printLastWorkItem && i == (nTests - 1)) {
            std::cout << "==========================================================" << '\n'
                      << Gem::Courtier::container_to_string(gcc1, PRINTOUTSERMOD) << '\n'
                      << "==========================================================" << '\n'
                      << Gem::Courtier::container_to_string(gcc3, PRINTOUTSERMOD) << '\n'
                      << "==========================================================" << '\n';
        }
    }

    //-----------------------------------------------------------------------------
    // Done
    return 0;
}
