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
#include <any>
#include <concepts>
#include <string>
#include <string_view>
#include <typeinfo>

// Boost header files go here
#include <boost/core/demangle.hpp>

// Geneva header files go here
#include "common/GCommonHelperFunctions.hpp" // timeAndPlace()
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"

namespace Gem::Courtier {

/******************************************************************************/
/** @brief The mnemonic of the consumer a process falls back to when none was chosen -- the local
 *  thread-pool consumer (GStdThreadConsumerConfig::MNEMONIC). Stated once, here: it is both the
 *  command-line default (Go2's GO2_DEF_CONSUMER binds to it) and the consumer
 *  ensureProcessConsumer() builds for a standalone algorithm that was never given one. */
inline constexpr std::string_view GDEFAULTCONSUMERMNEMONIC = "stc";

/******************************************************************************/
/**
 * @brief What a consumer is to be built from: its mnemonic and the configuration of its family.
 *
 * @c config holds the configuration struct of the family the mnemonic names (for example a
 * GWebsocketConsumerConfig for "beast"), or nothing, which stands for that family's defaults. A
 * program that knows its consumer when it is compiled does not write a spec: it passes the struct
 * to buildConsumer() or buildConsumerClient() (the overloads for a ConsumerConfig), which cannot
 * pair a struct with the wrong mnemonic. The spec is what a caller keeps when the mnemonic is known
 * only at run time (Go2, from --consumer). A provider reads @c config with configOf(), never with
 * std::any_cast. The header names no item type and no consumer.
 */
struct ConsumerSpec {
    std::string mnemonic; ///< The consumer, as chosen with --consumer ("stc", "beast", ...)
    std::any config;      ///< The family's configuration struct; empty: the family's defaults
};

/******************************************************************************/
/**
 * @brief A configuration struct of a consumer family: it names the mnemonic of its family and can
 * be copied (std::any holds copies).
 */
template <typename T>
concept ConsumerConfig = std::copy_constructible<T> && requires {
    { T::MNEMONIC } -> std::convertible_to<std::string_view>;
};

/******************************************************************************/
/**
 * @brief The configuration a spec carries, as the struct of the family that reads it.
 *
 * @tparam config_type The configuration struct the caller's family reads
 * @param spec The consumer specification
 * @return A copy of @c spec.config, or a default-constructed @p config_type if the spec carries none
 * @throws gem_exception if @c spec.config holds another type; the message names the mnemonic,
 *         the expected and the found type
 */
template <typename config_type>
[[nodiscard]] config_type configOf(const ConsumerSpec &spec) {
    if(not spec.config.has_value()) {
        return config_type{};
    }
    if(const auto *config = std::any_cast<config_type>(&spec.config)) {
        return *config;
    }
    throw gem_exception(
        g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
        << "In configOf(): Error!" << '\n'
        << "The consumer \"" << spec.mnemonic << "\" is configured with a "
        << boost::core::demangle(typeid(config_type).name()) << "," << '\n'
        << "but its specification carries a " << boost::core::demangle(spec.config.type().name())
        << "." << '\n'
    );
}

/******************************************************************************/

} /* namespace Gem::Courtier */
