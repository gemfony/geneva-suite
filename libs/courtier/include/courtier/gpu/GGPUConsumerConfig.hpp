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

// Standard headers
#include <string>

// Geneva headers
#include "common/GParserBuilder.hpp"
#include "courtier/gpu/GGPUKernelSpec.hpp"

namespace Gem::Courtier::GPU {

/******************************************************************************/
/**
 * The GPU consumer's configuration, read from a Geneva-style JSON config file (GParserBuilder). One
 * file selects the backend AND points at the kernel, so the same program can switch device model and
 * kernel without recompiling.
 */
struct GGPUConsumerConfig {
    std::string backend = "cuda";             ///< "cuda" -- the GPU consumer is device-only; any other
                                              ///< value (including the retired "cpu") is rejected by
                                              ///< backendKindFromString(). To run on the CPU, use a CPU
                                              ///< consumer instead (e.g. --consumer stc)
    std::string kernel_path;                  ///< path to the kernel source / prebuilt module
    std::string kernel_entry = "evaluate";    ///< kernel entry-point name
    int device_id = 0;                        ///< which device
    unsigned int block_x = 256;               ///< launch block size (x)
    unsigned int block_y = 1;
    unsigned int block_z = 1;
    unsigned int grid_x = 0;                  ///< launch grid (0 == auto)
    unsigned int grid_y = 0;
    unsigned int grid_z = 0;

    /**
     * @brief Reads the configuration from @p configFile, writing fresh defaults if it is absent.
     * @param configFile Path to the Geneva-style JSON config file to read (created with defaults if missing)
     */
    void load(const std::string &configFile) {
        Gem::Common::GParserBuilder gpb;
        gpb.registerFileParameter<std::string>(
            "backend", backend, backend, Gem::Common::VAR_IS_ESSENTIAL,
            "GPU backend: cuda (device-only; use a CPU consumer such as --consumer stc for CPU runs)");
        gpb.registerFileParameter<std::string>(
            "kernel_path", kernel_path, kernel_path, Gem::Common::VAR_IS_ESSENTIAL,
            "Path to the kernel source (.cu, runtime-compiled) or prebuilt module (.ptx/.cubin)");
        gpb.registerFileParameter<std::string>(
            "kernel_entry", kernel_entry, kernel_entry, Gem::Common::VAR_IS_ESSENTIAL,
            "Kernel entry-point name (must be un-mangled, e.g. extern \"C\")");
        gpb.registerFileParameter<int>(
            "device_id", device_id, device_id, Gem::Common::VAR_IS_ESSENTIAL,
            "Which GPU/device to use");
        gpb.registerFileParameter<unsigned int>(
            "block_x", block_x, block_x, Gem::Common::VAR_IS_ESSENTIAL, "Launch block size (x)");
        gpb.registerFileParameter<unsigned int>(
            "block_y", block_y, block_y, Gem::Common::VAR_IS_ESSENTIAL, "Launch block size (y)");
        gpb.registerFileParameter<unsigned int>(
            "block_z", block_z, block_z, Gem::Common::VAR_IS_ESSENTIAL, "Launch block size (z)");
        gpb.registerFileParameter<unsigned int>(
            "grid_x", grid_x, grid_x, Gem::Common::VAR_IS_ESSENTIAL, "Launch grid size (x; 0 == auto)");
        gpb.registerFileParameter<unsigned int>(
            "grid_y", grid_y, grid_y, Gem::Common::VAR_IS_ESSENTIAL, "Launch grid size (y; 0 == auto)");
        gpb.registerFileParameter<unsigned int>(
            "grid_z", grid_z, grid_z, Gem::Common::VAR_IS_ESSENTIAL, "Launch grid size (z; 0 == auto)");
        gpb.parseConfigFile(configFile);
    }

    /**
     * @brief The kernel spec described by this config.
     * @return A KernelSpec populated from the kernel path, entry point, device id and launch sizes
     */
    [[nodiscard]] KernelSpec kernelSpec() const {
        KernelSpec s;
        s.path = kernel_path;
        s.entry = kernel_entry;
        s.device_id = device_id;
        s.launch = LaunchConfig{block_x, block_y, block_z, grid_x, grid_y, grid_z};
        return s;
    }

    /**
     * @brief The selected backend kind.
     * @return The BackendKind parsed from the "backend" string ("cuda"); throws on any other value
     */
    [[nodiscard]] BackendKind backendKind() const { return backendKindFromString(backend); }
};

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
