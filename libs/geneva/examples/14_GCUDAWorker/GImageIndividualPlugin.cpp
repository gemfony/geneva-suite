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
 * @file
 * The plugin glue that turns the Mona-Lisa problem of example 14 into a RUNTIME-LOADABLE module carrying
 * TWO contributions in one shared object -- the optimization individual (GImageIndividual) AND its GPU
 * marshaller (GMonaLisaGPUMarshaller). This is the GPU-problem pattern of example 21 applied to a real,
 * image-fitting problem: with both in one .so, a launcher runs a runtime-loaded image problem on the device
 * (`--consumer gpu`) or cross-checks it on the CPU (`--consumer stc`, the individual's own evaluate()).
 *
 * The single-contribution author helpers (individualManifest / oaManifest / marshallerManifest) each emit a
 * one-entry manifest; a module carrying SEVERAL contributions hand-assembles the manifest from the same
 * building blocks, as shown here -- a plain array of GenevaContribution the loader walks by kind.
 *
 * Unlike example 18 / 21, the individual's archive tag is registered by its OWN translation unit
 * (GEM_REGISTER_ARCHIVABLE in GImageIndividual.cpp,
 * compiled into this same module), so this glue TU only assembles the manifest -- it must NOT export the
 * class again.
 */

// Boost headers
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Standard headers
#include <memory>

// Geneva headers
#include "common/GBuildFingerprint.hpp"      // GENEVA_BUILD_FINGERPRINT
#include "common/GModuleManifest.hpp"        // GenevaContribution / GenevaModuleManifest / kinds / version string
#include "geneva/GMarshallerPlugin.hpp"      // GMarshallerProviderPtr / GGPUMarshallerProviderT
#include "geneva/genome/GIndividualFactory.hpp" // Gem::Geneva::Genome::GIndividualFactory<>
#include "geneva/genome/GIndividualPlugin.hpp"  // GIndividualFactoryPtr

// Example-local headers
#include "GImageIndividual.hpp"       // the problem individual + GImageIndividualFactory alias
#include "GImageScalar.hpp"           // GIMAGE_DEFAULT_GPUCONFIG (matches the compiled scalar)
#include "GMonaLisaGPUMarshaller.hpp" // the problem's GPU marshaller

namespace {

// GenevaContribution / GenevaModuleManifest are plain-C types at global scope (the module manifest is a
// language-agnostic ABI boundary), so they are named unqualified below.

/** @brief Contribution 1 -- the individual (claim-once). The thunk hands back a heap GIndividualFactoryPtr
 *  across the plain-C void* boundary; the loader moves it out and returns it for Go2 to claim. The factory
 *  is the standard config-driven flat-individual factory on GImageIndividual. */
void *make_individual() {
    return new Gem::Geneva::GIndividualFactoryPtr(
        std::make_shared<Gem::Geneva::GImageIndividualFactory>("./config/GImageIndividual.json"));
}

/** @brief Contribution 2 -- the GPU marshaller, registered under the "cuda" device target, reading its
 *  GPU-consumer config (backend + kernel selection) from the config matching the compiled scalar
 *  (GIMAGE_DEFAULT_GPUCONFIG: GGPUConsumer.json for double, GGPUConsumerFloat.json for float). */
void *make_marshaller() {
    return new Gem::Geneva::GMarshallerProviderPtr(
        std::make_shared<Gem::Geneva::GGPUMarshallerProviderT<Gem::Geneva::MonaLisa::GMonaLisaGPUMarshaller>>(
            "cuda", GIMAGE_DEFAULT_GPUCONFIG));
}

/** @brief The module manifest: two contributions in one .so, walked by kind by the loader. */
const GenevaContribution g_contributions[] = {
    {GENEVA_CONTRIBUTION_INDIVIDUAL, "GImageIndividual", &make_individual},
    {GENEVA_CONTRIBUTION_MARSHALLER, "cuda", &make_marshaller},
};

const GenevaModuleManifest g_manifest{
    GENEVA_MODULE_ABI_STAMP, "GImageIndividual", GENEVA_VERSION_STRING, g_contributions, 2u};

} // anonymous namespace

// The fixed, unmangled entry point the loader resolves via dlsym.
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest();
extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
    return &g_manifest;
}
