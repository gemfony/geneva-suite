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
#include <cstddef>
#include <cstring>
#include <ranges>
#include <span>
#include <vector>

// Geneva headers
#include "geneva/genome/GBaseGPUMarshallerT.hpp" // generic flatten/scatter/itemDimension scaffolding
#include "GImageScalar.hpp"
#include "GMonaLisaProblem.hpp"

namespace gen = Gem::Geneva::Genome;

namespace Gem::Geneva::MonaLisa {

/******************************************************************************/
/**
 * GPU marshaller for the Mona-Lisa problem of example 14.
 *
 * The generic prepare/reconcile plumbing (itemDimension / flatten / scatter) is inherited from
 * GBaseGPUMarshallerT; this class supplies only the problem-specific device pieces:
 *
 * problemConstants()  packs, as scalars, [W, H, target(W*H*3)] -- the loaded target image; uploaded
 *                     once per launch. NT and the background come from the per-item parameters.
 * parallelWorkPerItem() 256 cooperating threads per individual (pixel-stripe parallelism).
 *
 * The GPU consumer is device-only; a CPU run uses the individual's own evaluate() through a CPU
 * consumer (e.g. --consumer stc), which shares the SAME score() math the device kernel replicates.
 *
 * One kernel launch per batch: minimal kernels, full bulk.
 *
 * The scalar type (gimage_fp_t, see GImageScalar.hpp) is selected at COMPILE TIME: DOUBLE by default,
 * or FLOAT when the example is built with GIMAGE_USE_FLOAT. The marshaller is that scalar end-to-end:
 * GBaseGPUMarshallerT<gimage_fp_t>, so the genome and fitness flat buffers, the device ABI and the CUDA
 * kernel all use it -- no widening/narrowing. The matching device kernel is selected through the default
 * GPU-consumer config.
 */
class GMonaLisaGPUMarshaller final
  : public Gem::Geneva::GBaseGPUMarshallerT<gimage_fp_t> {
public:
    [[nodiscard]] std::vector<std::byte> problemConstants() const override {
        const Target &t = target();
        std::vector<gimage_fp_t> blob;
        blob.reserve(2 + t.rgb.size());
        blob.push_back(static_cast<gimage_fp_t>(t.width));
        blob.push_back(static_cast<gimage_fp_t>(t.height));
        // The target image already uses the selected scalar -- pack it natively.
        blob.insert(blob.end(), t.rgb.begin(), t.rgb.end());

        std::vector<std::byte> bytes(blob.size() * sizeof(gimage_fp_t));
        std::memcpy(bytes.data(), blob.data(), bytes.size());
        return bytes;
    }

    /** @brief Use intra-item (pixel-stripe) parallelism: 256 cooperating threads per individual, so a
     *  small population still fills the GPU. The CUDA backend launches n_items*256 threads and the
     *  kernel atomic-accumulates each item's fitness. */
    [[nodiscard]] int parallelWorkPerItem() const override { return 256; }
};

/******************************************************************************/

} /* namespace Gem::Geneva::MonaLisa */
