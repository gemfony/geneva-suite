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

// Standard headers
#include <format>
#include <memory>
#include <string>
#include <utility>

// Boost headers
#include <boost/program_options/options_description.hpp>

// Geneva headers
#include "common/GProviderStoreT.hpp"
#include "common/GProviderT.hpp"
#include "geneva/genome/GBaseGPUMarshallerT.hpp"

namespace Gem::Geneva {

/******************************************************************************/
/**
 * @brief The process-global store of GPU marshaller providers, keyed by device target.
 *
 * The marshaller is the problem's accelerator adapter (flatten / scatter + kernel); it is the ONE piece a
 * GPU problem contributes to make @c --consumer @c gpu work. A problem registers its marshaller under a
 * device target (today "cuda") here; the GPU consumer provider (see GConsumerSetup.cpp) looks it up and
 * builds the generic @c GGPUConsumerT around it. Selection stays consumer-based: the user picks a consumer
 * mnemonic, never a marshaller, and one-problem-per-process means the store holds at most one entry per
 * target.
 *
 * This is an instantiation of the shared @c Gem::Common::GProviderStoreT template (not a bespoke store),
 * the same map abstraction the OA and consumer catalogs use. The stored provide()-type is the
 * scalar-agnostic @c GGPUMarshallerHandle, so the store holds a float or a double marshaller uniformly.
 *
 * @return The shared marshaller-provider store singleton (never nullptr).
 */
[[nodiscard]] inline auto marshallerProviderStore() {
    return Gem::Common::providerStore<GGPUMarshallerHandle>();
}

/******************************************************************************/
/**
 * @brief A provider for one GPU marshaller, registered under a device target ("cuda").
 *
 * Extends the shared @c Gem::Common::GProviderT with the GPU-consumer config path the problem's kernel
 * needs, mirroring how @c GConsumerProviderT extends it with @c setup(). @c provide() hands out the
 * problem's marshaller (as the scalar-agnostic handle); the GPU consumer provider reads its scalar kind
 * and downcasts to the matching @c GGPUEvaluableI<..,scalar_type> to build @c GGPUConsumerT.
 */
class GGPUMarshallerProviderBase
  : public Gem::Common::GProviderT<GGPUMarshallerHandle> {
public:
    /**
     * @brief The GPU-consumer config file (backend + kernel selection) this marshaller's kernel needs.
     * @return The config-file path passed at registration
     */
    [[nodiscard]] virtual const std::string &gpuConfigFile() const = 0;
};

/******************************************************************************/
/**
 * @brief The concrete marshaller provider for a specific marshaller type.
 *
 * @c provide() default-constructs a fresh @p marshaller_type each call (a single one is used per run);
 * the device target doubles as the store key / mnemonic, and the GPU-consumer config path is captured at
 * registration.
 *
 * @tparam marshaller_type A @c GBaseGPUMarshallerT<scalar_type> subclass (so it IS a @c GGPUMarshallerHandle)
 */
template <typename marshaller_type>
class GGPUMarshallerProviderT final : public GGPUMarshallerProviderBase {
public:
    /**
     * @brief Builds a provider for @p marshaller_type under @p device_target with @p gpu_config_file.
     * @param device_target The device target this marshaller serves (store key / mnemonic, e.g. "cuda")
     * @param gpu_config_file The GPU-consumer config file (backend + kernel selection) for this marshaller
     */
    GGPUMarshallerProviderT(std::string device_target, std::string gpu_config_file)
      : device_target_(std::move(device_target)), gpu_config_file_(std::move(gpu_config_file)) {}

    /** @brief Hands out a fresh marshaller (as the scalar-agnostic handle). @return The marshaller. */
    std::shared_ptr<GGPUMarshallerHandle> provide() override {
        return std::make_shared<marshaller_type>();
    }
    /** @brief The device target this marshaller is registered under. @return The store key / mnemonic. */
    [[nodiscard]] std::string getMnemonic() const override { return device_target_; }
    /** @brief A human-readable name, for help output. @return The provider's descriptive name. */
    [[nodiscard]] std::string getName() const override { return std::format("GPU marshaller [{}]", device_target_); }
    /** @brief The GPU-consumer config path the kernel needs. @return The registered config-file path. */
    [[nodiscard]] const std::string &gpuConfigFile() const override { return gpu_config_file_; }

    /** @brief A marshaller has no command-line options of its own. */
    void addCLOptions(
        boost::program_options::options_description & /*visible*/,
        boost::program_options::options_description & /*hidden*/) override {}

private:
    std::string device_target_;   ///< the store key / device mnemonic ("cuda")
    std::string gpu_config_file_; ///< the GPU-consumer config (backend + kernel) path for this marshaller
};

/******************************************************************************/
/**
 * @brief Registers a GPU marshaller for a device target, so @c --consumer @c gpu can build a consumer.
 *
 * A GPU problem calls this once (compiled-in: before constructing @c Go2, so the consumer is built at
 * construction like every other mnemonic; a loaded individual module would call it at module load). It is
 * the sole GPU-specific contribution a problem makes -- Go2 owns consumer selection and lifecycle.
 *
 * @tparam marshaller_type The problem's @c GBaseGPUMarshallerT<scalar_type> subclass
 * @param device_target The device target (store key / mnemonic, e.g. "cuda")
 * @param gpu_config_file The GPU-consumer config file (backend + kernel selection) for this marshaller
 */
template <typename marshaller_type>
void registerGPUMarshaller(const std::string &device_target, const std::string &gpu_config_file) {
    marshallerProviderStore()->setOnce(
        device_target,
        std::make_shared<GGPUMarshallerProviderT<marshaller_type>>(device_target, gpu_config_file));
}

/******************************************************************************/

} /* namespace Gem::Geneva */
