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
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>

// Boost header files go here
#include <boost/core/demangle.hpp>

// Geneva header files go here
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "courtier/GConsumerProviderT.hpp"
#include "courtier/gpu/GGPUConsumer.hpp"
#include "courtier/gpu/GGPUEvaluableI.hpp"
#include "courtier/gpu/GGPUMarshallerHandle.hpp"

namespace Gem::Courtier::GPU {

/******************************************************************************/
/**
 * @brief The provider of the GPU consumer ("gpu"): local, device-only, no clients. It is the one
 * consumer that needs something of the problem -- a marshaller for its item type, registered in
 * marshallerProviderStore() under the device target "cuda" -- and says so in
 * missingPrerequisite(), which the launcher asks when the consumer is chosen, after all modules
 * are loaded. Its configuration is the file the marshaller names, not the command line.
 * @tparam processable_type The item type
 */
template <ProcessableItem processable_type>
class GGPUConsumerProviderT final : public GConsumerProviderT<processable_type> {
    using base_type = GConsumerProviderT<processable_type>;

public:
    /** @brief The mnemonic */
    static constexpr std::string_view MNEMONIC = "gpu";
    /** @brief The device target the marshaller is registered under */
    static constexpr const char *DEVICE_TARGET = "cuda";

    [[nodiscard]] std::string getMnemonic() const override {
        return std::string(MNEMONIC);
    }
    [[nodiscard]] std::string getName() const override {
        return "GGPUConsumerT";
    }
    /** @brief The GPU consumer has no command-line options @param gpb Unused */
    void registerOptions(Gem::Common::GParserBuilder &gpb) override {
        (void)gpb;
    }
    /** @brief No configuration of the command line @return Empty */
    [[nodiscard]] std::any
    configuration(const Gem::Common::GParserBuilder &, const std::any &) const override {
        return {};
    }
    [[nodiscard]] bool needsClient() const override {
        return false;
    }

    /** @brief A marshaller for the item type, or what is missing @return Empty if there is one */
    [[nodiscard]] std::string missingPrerequisite() const override {
        const std::string item = boost::core::demangle(typeid(processable_type).name());
        const auto provider = marshaller_();
        if(not provider) {
            return "no GPU marshaller is registered for the device target \"" +
                   std::string(DEVICE_TARGET) + "\". The problem registers one for its item type " +
                   item +
                   " (a program before its launcher runs, or a module that contributes one).";
        }
        const auto handle = provider->provide();
        const bool fits = handle->scalarKind() == GPUScalarKind::Double
                            ? static_cast<bool>(typed_<double>(handle))
                            : static_cast<bool>(typed_<float>(handle));
        if(not fits) {
            return "the GPU marshaller registered for \"" + std::string(DEVICE_TARGET) +
                   "\" is not one for the item type " + item + ".";
        }
        return {};
    }

    /** @brief Builds the GPU consumer around the registered marshaller @return It */
    typename base_type::consumer_ptr setup(const ConsumerSpec &) override {
        const auto provider = marshaller_();
        if(not provider) {
            throw gem_exception(
                g_error_streamer() << "In GGPUConsumerProviderT::setup(): Error!" << '\n'
                                   << missingPrerequisite() << '\n'
            );
        }
        const auto handle = provider->provide();
        if(handle->scalarKind() == GPUScalarKind::Double) {
            return build_<double>(handle, provider->gpuConfigFile());
        }
        return build_<float>(handle, provider->gpuConfigFile());
    }
    /** @brief There is no client @return nullptr */
    [[nodiscard]] typename base_type::client_ptr buildClient(const ConsumerSpec &) const override {
        return nullptr;
    }

private:
    /** @brief The registered marshaller's provider, or null @return It */
    static std::shared_ptr<GGPUMarshallerProviderBase> marshaller_() {
        std::shared_ptr<Gem::Common::GProviderT<GGPUMarshallerHandle>> base;
        if(not marshallerProviderStore()->get(DEVICE_TARGET, base) || not base) {
            return nullptr;
        }
        return std::static_pointer_cast<GGPUMarshallerProviderBase>(base);
    }
    /** @brief The marshaller as one of the item type, or null @return It */
    template <typename scalar_type>
    static std::shared_ptr<GGPUEvaluableI<processable_type, scalar_type>>
    typed_(const std::shared_ptr<GGPUMarshallerHandle> &handle) {
        return std::dynamic_pointer_cast<GGPUEvaluableI<processable_type, scalar_type>>(handle);
    }
    /** @brief The consumer around a marshaller of the item type @return It */
    template <typename scalar_type>
    static typename base_type::consumer_ptr
    build_(const std::shared_ptr<GGPUMarshallerHandle> &handle, const std::string &config_file) {
        auto marshaller = typed_<scalar_type>(handle);
        if(not marshaller) {
            throw gem_exception(
                g_error_streamer() << "In GGPUConsumerProviderT::setup(): Error!" << '\n'
                                   << "The registered GPU marshaller is not one for the item type "
                                   << boost::core::demangle(typeid(processable_type).name()) << '\n'
            );
        }
        return std::make_shared<GGPUConsumerT<processable_type, scalar_type>>(
            config_file,
            marshaller
        );
    }
};

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
