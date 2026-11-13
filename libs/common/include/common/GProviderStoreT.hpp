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
#include <memory>

// Geneva headers go here
#include "common/GGlobalOptionsT.hpp"
#include "common/GProviderT.hpp"
#include "common/GSingletonT.hpp"

namespace Gem::Common {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief A process-global, mnemonic-keyed registry of providers for an interface @c T.
 *
 * This is the single, reusable spelling of the "map of named providers" idiom: a
 * @c GSingletonT wrapping a @c GGlobalOptionsT (the sanctioned keyed store over
 * @c GThreadSafeKeyedStoreT, Inv 2) of @c GProviderT<T> handles. Each provider self-registers under its
 * mnemonic at static init and hands out a usable @c T on @c provide() (see @c GProviderT for the two
 * flavours -- a config-driven factory, or a shared prototype). The optimization-algorithm registry
 * (@c GOAStore) is one instantiation; the consumer and marshaller registries are meant to be further
 * instantiations of THIS template rather than new bespoke stores.
 *
 * @par The two-store rule (binding)
 * There are exactly two store shapes in @c common/, and any new "registry" or "global constant" must be an
 * instantiation of one of them, never a hand-rolled third:
 *  - @c GProviderStoreT<T> (this) -- a mutable, keyed, iterable map of named providers; and
 *  - @c Gem::Common::Concurrency::GLoadOnceCellT<T> -- one load-once-then-immutable payload.
 * Growing a bespoke registry (a table + switch, an ad-hoc mutex + map) or a bespoke load-once constant (a
 * hand-rolled @c once_flag + @c atomic) instead of these is an Inv-2 violation.
 *
 * @tparam T The interface type the registered providers hand out (e.g. an optimization-algorithm base)
 */
template <typename T>
using GProviderStoreT = GSingletonT<GGlobalOptionsT<std::shared_ptr<GProviderT<T>>>>;

/******************************************************************************/
/**
 * @brief Returns the process-global provider store for interface @c T (created on first access).
 *
 * @tparam T The interface type the registered providers hand out
 * @return A shared pointer to the singleton keyed provider store (never nullptr)
 */
template <typename T>
[[nodiscard]] inline std::shared_ptr<typename GProviderStoreT<T>::STYPE> providerStore() {
    return GProviderStoreT<T>::instance();
}

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */
