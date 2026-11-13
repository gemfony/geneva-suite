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
#include <memory>

// Boost headers go here
#include <boost/config.hpp> // BOOST_SYMBOL_EXPORT

// Geneva headers go here
#include "common/GModuleManifest.hpp" // the unified module manifest (GenevaCompat + contributions)
#include "geneva/GMarshallerSetup.hpp" // GGPUMarshallerProviderBase / GGPUMarshallerProviderT / the store

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief The type a loaded GPU-marshaller contribution hands back: a provider over the marshaller base, the
 * exact provider type @c marshallerProviderStore() holds. The module builds it (where the concrete marshaller
 * type is known) and the loader registers it type-erased, so the loader stays free of the concrete marshaller.
 */
using GMarshallerProviderPtr = std::shared_ptr<GGPUMarshallerProviderBase>;

/******************************************************************************/
/**
 * @brief Builds the module manifest for a runtime-loadable GPU marshaller, entirely in C++ (no macro).
 *
 * The marshaller analogue of @c oaManifest(). A loadable marshaller's source hand-writes ONE small entry
 * point that delegates here:
 * @code
 *   extern "C" BOOST_SYMBOL_EXPORT const GenevaModuleManifest *geneva_module_manifest() {
 *       return Gem::Geneva::marshallerManifest<MyMarshaller, "cuda", "config/MyGPU.json">();
 *   }
 * @endcode
 * The @c extern @c "C" wrapper is irreducible (the loader resolves the fixed, unmangled symbol
 * @c geneva_module_manifest via dlsym); everything else is this typed template. A single module may pack a
 * marshaller contribution ALONGSIDE its individual contribution (append both to a multi-entry manifest), so
 * one @c .so carries both a GPU problem and its device adapter -- the @c --individual @c foo.so @c --consumer
 * @c gpu case.
 *
 * Like an OA (and unlike a claim-once individual), a marshaller is resolved by device target against a shared
 * store, so the contribution's factory thunk hands back a fully-built @c GGPUMarshallerProviderT (as a
 * @c GMarshallerProviderPtr on the heap, the plain-C @c void* boundary); the loader moves it out and
 * @c setOnce-registers it into @c marshallerProviderStore() under its device target. Registering a device
 * target a compiled-in marshaller (or another module) already holds is a hard error -- one problem per
 * process means at most one marshaller per target.
 *
 * The @c DeviceTarget doubles as the store key / mnemonic; the @c ConfigPath is the GPU-consumer config file
 * (backend + kernel selection) the marshaller's kernel needs -- both baked in at authoring time, so the whole
 * marshaller contribution (target, config path, and -- via the produced handle -- the device scalar kind)
 * travels inside the provider with no change to the manifest ABI.
 *
 * @tparam Marshaller The concrete marshaller (a @c GBaseGPUMarshallerT<scalar_type> subclass); default-constructed by the provider
 * @tparam DeviceTarget The device target / store key (a string literal, e.g. "cuda")
 * @tparam ConfigPath The GPU-consumer config file (a string literal) the marshaller's kernel needs
 * @return A pointer to this module's process-lifetime manifest
 */
template <typename Marshaller, Gem::Common::GFixedString DeviceTarget, Gem::Common::GFixedString ConfigPath>
const GenevaModuleManifest *marshallerManifest() {
    // Captureless thunk -> void*(*)(void): build the marshaller provider (device target + config path baked in
    // from the compile-time NTTPs) on the heap; the loader moves-from and deletes it, then registers the
    // provider in marshallerProviderStore().
    static constexpr auto factory_thunk = +[]() -> void * {
        return new GMarshallerProviderPtr(std::make_shared<GGPUMarshallerProviderT<Marshaller>>(
            DeviceTarget.c_str(), ConfigPath.c_str()));
    };
    static const GenevaContribution contribution{
        GENEVA_CONTRIBUTION_MARSHALLER, DeviceTarget.c_str(), factory_thunk};
    static const GenevaModuleManifest manifest{
        GENEVA_MODULE_ABI_STAMP, DeviceTarget.c_str(), GENEVA_VERSION_STRING, &contribution, 1u};
    return &manifest;
}

/******************************************************************************/

} /* namespace Gem::Geneva */
