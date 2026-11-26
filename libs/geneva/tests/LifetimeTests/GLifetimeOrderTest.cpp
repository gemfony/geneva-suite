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

/*
 * The start and the orderly end of a Geneva program, once, in a process of its own. The order it
 * checks is written down in the class comment of Gem::Geneva::GenevaInitializer.
 *
 * main() holds a GenevaInitializer, as a program does, and runs a short optimization on courtier's
 * launcher (runOptimization()). When that has returned, the launcher has released the consumer, and
 * the random-number factory is still live, with its producers running. The rest of the end happens
 * after main() has returned, in the destructors of the libraries' statics; the last of them is
 * hap's lifecycle guard, which finalizes the factory, and then the factory itself is destroyed. A
 * check runs after all of that: it is registered before any library is initialized (from
 * .preinit_array, which the dynamic loader runs before the initializers of the shared libraries),
 * and for no shared object (abi::__cxa_atexit() with a null handle), so that it runs after the
 * dynamic loader has ended the program's and every library's statics; std::atexit() in a program
 * would register it for the program, whose statics end first. The factory must have been destroyed,
 * and every thread a library reserved in the thread budget must have been returned, i.e. joined. A
 * failed check prints its reason and ends the process with the exit code 1.
 */

// Standard header files go here
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <exception>
#include <memory>
#include <print>

// Geneva header files go here
#include "common/concurrency/GThreadBudget.hpp"
#include "courtier/GConsumerRegistry.hpp"
#include "geneva/GOptimizationDescription.hpp"
#include "geneva/GOptimizer.hpp"
#include "geneva/GenevaInitializer.hpp"
#include "geneva/individuals/GFunctionIndividual.hpp"
#include "geneva/oa/GEvolutionaryAlgorithm.hpp"
#include "hap/GRandomFactory.hpp"

using namespace Gem::Geneva;

namespace {

/** @brief The factory of the run, watched without being kept. Never destroyed: the end check reads
 *  it after every static destructor has run.
 *  @return The watch */
std::weak_ptr<Gem::Hap::GRandomFactory> &factoryWatch() {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) -- deliberately never destroyed (see above)
    static auto *const watch = new std::weak_ptr<Gem::Hap::GRandomFactory>();
    return *watch;
}

/** @brief The end check; it runs after the static destructors of every library (file comment) */
void checkTheEnd(void * /* unused */) {
    if(not factoryWatch().expired()) {
        std::fputs("FAIL: the random-number factory was not destroyed at the end\n", stderr);
        std::_Exit(1);
    }
    if(const unsigned int reserved = Gem::Common::Concurrency::threadBudget().reserved();
       reserved != 0) {
        std::fprintf(stderr, "FAIL: %u reserved threads were not returned at the end\n", reserved);
        std::_Exit(1);
    }
    std::fputs(
        "OK: the factory was finalized and destroyed, every reserved thread joined\n",
        stdout
    );
}

/** @brief Registers the end check before the initializers of the shared libraries run, for no
 *  shared object, so that it runs after their static destructors (file comment) */
void registerTheEndCheck(int /* argc */, char ** /* argv */, char ** /* envp */) {
    if(abi::__cxa_atexit(&checkTheEnd, nullptr, nullptr) != 0) {
        std::fputs("FAIL: the end check could not be registered\n", stderr);
        std::_Exit(1);
    }
}

// The dynamic loader calls the functions of .preinit_array before any shared library is initialized
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) -- read by the loader only
[[gnu::used, gnu::section(".preinit_array")]] void (*preinitEndCheck)(int, char **, char **) =
    &registerTheEndCheck;

/** @brief The submitter's part: two iterations of a small population (Inv 22)
 *  @param go The GOptimizer of the run
 *  @return 0 */
int optimize(GOptimizer &go) {
    const auto factory =
        std::make_shared<gind::GFunctionIndividualFactory>("./config/GFunctionIndividual.json");
    go.registerContentCreator(factory);
    const auto sample = factory->get_as<gind::GFunctionIndividual>();
    go.registerAdaptionConfig("ea", factory->getAdaptionConfig(*sample));

    const auto ea = std::make_shared<oa::GEvolutionaryAlgorithm>();
    ea->setPopulationSizes(8, 2);
    ea->setMaxIteration(2);
    go & ea;
    (void)go.optimize();
    return 0;
}

/** @brief The start, the run and what the end of the run leaves; see the file comment
 *  @param argc The number of command line arguments
 *  @param argv The command line arguments
 *  @return 0 if every check passed */
int runTest(int argc, char **argv) {
    const GenevaInitializer gi; // a program's start of Geneva; it ends with this function

    const int rc = runOptimization(argc, argv, {.produce = [](GOptimizer &go, const auto &) {
                                       return optimize(go);
                                   }});
    if(rc != 0) {
        std::println(stderr, "FAIL: runOptimization() returned {}", rc);
        return 1;
    }
    if(Gem::Courtier::GConsumerRegistryT<gen::GGenome>::instance().consumer()) {
        std::println(stderr, "FAIL: the consumer is still registered after the run");
        return 1;
    }
    const auto factory = Gem::Hap::randomFactory();
    if(factory->finalized() || Gem::Common::Concurrency::threadBudget().reserved() == 0) {
        std::println(stderr, "FAIL: the factory's producers no longer run after the run");
        return 1;
    }
    factoryWatch() = factory;
    return 0;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) -- only the printing in the handlers could throw
int main(int argc, char **argv) {
    try {
        return runTest(argc, argv);
    }
    catch(const std::exception &e) {
        std::println(stderr, "FAIL: {}", e.what());
    }
    catch(...) {
        std::println(stderr, "FAIL: an unknown exception");
    }
    return 1;
}
