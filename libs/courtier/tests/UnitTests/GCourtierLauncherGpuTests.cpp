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
 * The GPU consumer on the launcher: it needs a marshaller for the item type, and the launcher asks
 * for it when the consumer is chosen, before anything runs. Refused without one and with one for
 * another item type; accepted with one for the item type (prepare() ends the program before the
 * consumer is built, so no device is needed).
 */

// Standard header files go here
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Catch2 header files go here
#include <catch2/catch_test_macros.hpp>

// Geneva header files go here
#include "GDemoProcessingContainers.hpp"
#include "courtier/GLauncherT.hpp"
#include "courtier/gpu/GGPUEvaluableI.hpp"
#include "courtier/gpu/GGPUMarshallerHandle.hpp"

using namespace Gem::Courtier;
using namespace Gem::Courtier::GPU;

namespace {

/** @brief A marshaller for the item type @p item_type that is never used on a device */
template <typename item_type>
class GIdleMarshaller final
  : public GGPUEvaluableI<item_type, double>
  , public GGPUMarshallerHandle {
    using item_ptr = typename GGPUEvaluableI<item_type, double>::item_ptr;

public:
    [[nodiscard]] GPUScalarKind scalarKind() const override {
        return GPUScalarKind::Double;
    }
    [[nodiscard]] std::size_t itemDimension(const item_ptr &) const override {
        return 1;
    }
    void flatten(std::span<const item_ptr>, std::vector<double> &) const override {
        throw std::logic_error("no device in this test");
    }
    void scatter(std::span<const item_ptr>, const std::vector<double> &, double) const override {
        throw std::logic_error("no device in this test");
    }
};

/** @brief Hands out a GIdleMarshaller of @p item_type */
template <typename item_type>
class GIdleMarshallerProvider final : public GGPUMarshallerProviderBase {
public:
    std::shared_ptr<GGPUMarshallerHandle> provide() override {
        return std::make_shared<GIdleMarshaller<item_type>>();
    }
    [[nodiscard]] std::string getMnemonic() const override {
        return "cuda";
    }
    [[nodiscard]] std::string getName() const override {
        return "idle marshaller";
    }
    [[nodiscard]] const std::string &gpuConfigFile() const override {
        return file_;
    }
    void addCLOptions(
        boost::program_options::options_description &,
        boost::program_options::options_description &
    ) override {
    }

private:
    std::string file_ = "./config/GGPUConsumer.json";
};

/** @brief Runs --consumer gpu up to prepare() @return What it throws; empty if nothing */
std::string runGpu(bool &prepared) {
    std::vector<std::string> words{"GCourtierLauncherGpuTests", "--consumer", "gpu"};
    std::vector<char *> argv;
    for(auto &w : words) {
        argv.push_back(w.data());
    }
    argv.push_back(nullptr);
    GLauncherT<GSimpleContainer> launcher(static_cast<int>(words.size()), argv.data());
    launcher.setConsumerModules(
        GConsumerModules{
            .directory = GEM_LAUNCHER_TEST_MODULE_DIR,
            .prefix = "libgem_launcher_gpu_"
        }
    );
    try {
        (void)launcher.run({.prepare = [&](processRole) -> std::optional<int> {
            prepared = true;
            return 0;
        }});
    }
    catch(const std::exception &e) {
        return e.what();
    }
    return {};
}

} // namespace

TEST_CASE("Launcher: the GPU consumer needs a marshaller for the item type", "[launcher][gpu]") {
    auto store = marshallerProviderStore();
    store->remove("cuda");
    bool prepared = false;

    // None registered
    std::string msg = runGpu(prepared);
    CHECK(
        msg.find("no GPU marshaller is registered for the device target \"cuda\"") !=
        std::string::npos
    );
    CHECK_FALSE(prepared);

    // One for another item type
    REQUIRE(store->setOnce("cuda", std::make_shared<GIdleMarshallerProvider<GFaultyContainer>>()));
    msg = runGpu(prepared);
    CHECK(
        msg.find("is not one for the item type Gem::Courtier::GSimpleContainer") !=
        std::string::npos
    );
    CHECK_FALSE(prepared);

    // One for the item type: the program starts
    store->remove("cuda");
    REQUIRE(store->setOnce("cuda", std::make_shared<GIdleMarshallerProvider<GSimpleContainer>>()));
    msg = runGpu(prepared);
    CHECK(msg.empty());
    CHECK(prepared);
    store->remove("cuda");
}
