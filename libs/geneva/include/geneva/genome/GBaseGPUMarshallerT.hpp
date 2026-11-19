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
#include <cstddef>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

// Geneva headers go here
#include "common/GCommonHelperFunctions.hpp"
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "courtier/gpu/GGPUEvaluableI.hpp"
#include "geneva/genome/GGenome.hpp"

namespace Gem::Geneva {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief The device scalar (flat-buffer element) a GPU marshaller was built for.
 *
 * The scalar stays a compile-time template parameter of the marshaller / consumer (never type-erased);
 * this enum is only its runtime witness, so a scalar-agnostic store can hold a marshaller of either
 * precision and the GPU consumer provider can recover the matching typed pointer.
 */
enum class GPUScalarKind {
    Float,
    Double
};

/******************************************************************************/
/**
 * @brief A scalar-agnostic handle to a GPU marshaller.
 *
 * @c GBaseGPUMarshallerT<scalar_type> is templated on the device scalar, so a float marshaller and a
 * double marshaller share no common @c GGPUEvaluableI base. This tiny non-templated base gives the
 * marshaller-provider store a single type to hold, and lets the GPU consumer provider read a marshaller's
 * scalar kind and then @c dynamic_cast it back to the matching @c GGPUEvaluableI<..,scalar_type> to build
 * the correctly-typed @c GGPUConsumerT.
 */
class GGPUMarshallerHandle { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /** @brief The (defaulted) destructor */
    virtual ~GGPUMarshallerHandle() = default;
    /**
     * @brief The device scalar this marshaller flattens / scatters in.
     * @return @c GPUScalarKind::Float or @c GPUScalarKind::Double
     */
    [[nodiscard]] virtual GPUScalarKind scalarKind() const = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * @brief Base scaffolding for a GPU marshaller of flat-genome individuals.
 *
 * The marshaller is the accelerator ADAPTER: it prepares a batch of individuals for the device (flatten)
 * and reconciles the device results back (scatter); it never runs the evaluation itself (the backend/kernel
 * does). This base supplies the parts of that adapter that are identical for every flat-genome problem, so a
 * concrete problem marshaller adds only the genuinely device-specific pieces:
 *
 *  - @c problemConstants() / @c problemConstantsStatic() -- the opaque blob the kernel also needs (default:
 *    none, inherited);
 *  - @c parallelWorkPerItem() -- intra-item device parallelism (default: 1, inherited).
 *
 * The GPU consumer is device-only: it never evaluates on the host. A CPU run uses the individual's own
 * @c evaluate() through a CPU consumer (e.g. --consumer stc), not this marshaller.
 *
 * The generic plumbing provided here implements the processing rule of the interface (stated on
 * @c GGPUEvaluableI: begin in flatten(), complete in scatter(), a failing item fails alone):
 *  - @c itemDimension() -- the flattened scalar count of one individual (its parameter count);
 *  - @c flatten() -- begins each individual's processing, then streamlines its EXTERNAL
 *    (range-folded) values row-major straight into the device buffer. Because the buffer element is
 *    @c scalar_type, the individual's @c long @c double / @c double genome is converted to
 *    @c scalar_type here (the FP conversion), so the author's @c evaluate() stays in full precision
 *    while the device runs at @c scalar_type;
 *  - @c scatter() -- completes each individual's processing with its device-computed fitness
 *    (@c GGenome::completeProcessing(): feasibility gate, policy transform, PROCESSED,
 *    post-processor).
 *
 * A derived marshaller that overrides @c flatten() or @c scatter() delegates to these.
 *
 * This sits in @c geneva (not @c courtier) because the generic flatten/scatter necessarily know the geneva
 * individual model (@c GGenome, @c individual_processing_result); it derives from courtier's
 * genome-agnostic @c GGPUEvaluableI, which stays free of the geneva layer.
 *
 * @tparam scalar_type The device ABI / flat-buffer element type (double for full parity, float for FP32 speed)
 */
template <typename scalar_type = double>
class GBaseGPUMarshallerT
  : public Gem::Courtier::GPU::GGPUEvaluableI<Gem::Geneva::Genome::GGenome, scalar_type>
  , public GGPUMarshallerHandle {
    using base_type = Gem::Courtier::GPU::GGPUEvaluableI<Gem::Geneva::Genome::GGenome, scalar_type>;
    static_assert(
        std::is_same_v<scalar_type, float> || std::is_same_v<scalar_type, double>,
        "GBaseGPUMarshallerT supports only float or double device scalars."
    );

public:
    using item_ptr = typename base_type::item_ptr;

    /***************************************************************************/
    /**
     * @brief The device scalar this marshaller was instantiated for (its runtime witness).
     * @return @c GPUScalarKind::Float for a @c float marshaller, @c GPUScalarKind::Double for @c double
     */
    [[nodiscard]] GPUScalarKind scalarKind() const override {
        if constexpr(std::is_same_v<scalar_type, float>) {
            return GPUScalarKind::Float;
        }
        else {
            return GPUScalarKind::Double;
        }
    }

    /***************************************************************************/
    /**
     * @brief The flattened scalar count of one individual (its parameter count in @c scalar_type).
     * @param item The individual whose flattened dimension is queried
     * @return The number of @c scalar_type values @c flatten() writes for this item
     */
    [[nodiscard]] std::size_t itemDimension(const item_ptr &item) const override {
        return item->template countParameters<scalar_type>();
    }

    /***************************************************************************/
    /**
     * @brief Begins every item's processing (its pre-processor runs), then streamlines its external
     * values row-major into @p params_out (@c scalar_type each). An item whose processing fails
     * here keeps its failure (EXCEPTION_CAUGHT) and is still flattened; scatter() leaves it alone.
     *
     * @param items The batch to flatten (every item must be a due GGenome of uniform dimension)
     * @param params_out Filled with @c items.size() * itemDimension() scalars (parameter j of item i at i*dim + j)
     * @throws gem_exception if an item is not a GGenome, or if its pre-processor changed its
     *         dimension (its values would not fit its row of the buffer)
     */
    void
    flatten(std::span<const item_ptr> items, std::vector<scalar_type> &params_out) const override {
        if(items.empty()) {
            params_out.clear();
            return;
        }
        const std::size_t dim = this->itemDimension(items.front());
        params_out.resize(items.size() * dim);
        for(std::size_t i = 0; i < items.size(); ++i) {
            const auto *flat = dynamic_cast<const Gem::Geneva::Genome::GGenome *>(items[i].get());
            if(flat == nullptr) {
                throw gem_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GBaseGPUMarshallerT::flatten(): Error!" << '\n'
                    << "A work item is not a GGenome; a GPU marshaller requires flat-genome"
                    << " individuals." << '\n'
                );
            }
            try {
                // The pre-processor runs before the device sees the values
                items[i]->beginProcessing();
            }
            catch(const Gem::Courtier::g_processing_exception &) {
                // Recorded on the item (EXCEPTION_CAUGHT), as on the CPU path; it fails alone.
            }
            // The consumer checked the uniform dimension before the pre-processors ran; a
            // pre-processor that changed it would make streamlineInto() write past the item's row.
            if(const std::size_t item_dim = this->itemDimension(items[i]); item_dim != dim) {
                throw gem_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GBaseGPUMarshallerT::flatten(): Error!" << '\n'
                    << "Work item " << i << " has " << item_dim << " parameters after its"
                    << " pre-processing, the batch " << dim << "." << '\n'
                    << "A pre-processor must not change the number of parameters of an item"
                    << " evaluated on a device." << '\n'
                );
            }
            // streamlineInto() writes the external (range-folded) values as scalar_type -- no per-item
            // temporary, and the double-to-scalar_type conversion happens here.
            flat->streamlineInto(params_out.data() + i * dim);
        }
    }

    /***************************************************************************/
    /**
     * @brief Completes each item's processing with its device-computed fitness (the item's
     * post-processor runs). An item whose processing failed in flatten() is left alone; one whose
     * completion fails keeps its failure, as on the CPU path, and the others are completed.
     * @param items The batch the fitness values belong to (in order)
     * @param fitness One fitness scalar per item, produced by the device/host evaluation
     * @param evaluation_time The time in seconds the evaluation of the whole batch took
     */
    void scatter(
        std::span<const item_ptr> items,
        const std::vector<scalar_type> &fitness,
        const double evaluation_time
    ) const override {
        for(auto const &[item, fit] : std::views::zip(items, fitness)) {
            if(not item->is_due_for_processing()) {
                continue; // its processing failed in flatten(); the failure is recorded on it
            }
            try {
                item->completeProcessing(
                    std::vector<Gem::Geneva::Genome::individual_processing_result>(
                        1,
                        Gem::Geneva::Genome::individual_processing_result(fit)
                    ),
                    evaluation_time
                );
            }
            catch(const Gem::Courtier::g_processing_exception &) {
                // Recorded on the item (EXCEPTION_CAUGHT / ERROR_FLAGGED), as on the CPU path.
            }
        }
    }
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Geneva */
