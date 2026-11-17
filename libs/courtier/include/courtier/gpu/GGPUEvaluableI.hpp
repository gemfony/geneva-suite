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
#include <memory>
#include <span>
#include <vector>

namespace Gem::Courtier::GPU {

/******************************************************************************/
/**
 * The problem-specific marshalling interface, generic in the processable type. Logically a
 * Gem::Courtier consumer helper (the GPU consumer derives from GBaseConsumerT), so it lives in
 * Gem::Courtier::GPU with NO dependency on the Gem::Geneva layer (which sits above courtier). The
 * concrete, genome-aware marshallers live with the problems (the demos), not here.
 *
 * The GPU consumer owns the device plumbing; the problem owns how a batch of items becomes flat device
 * buffers and how device results are written back. The GPU consumer is DEVICE-ONLY: it never evaluates
 * on the host -- a CPU run uses the individual's own evaluate() through a CPU consumer (e.g.
 * --consumer stc), not this marshaller.
 *
 * ABI: parameters are row-major, one row of `dim` scalar_type per item (params[i*dim + j] = parameter
 * j of item i); the kernel writes one fitness scalar_type per item. problemConstants() is an opaque
 * byte blob the kernel also receives (function id, target image, ...), uploaded once per launch.
 *
 * scalar_type defaults to double, so GGPUEvaluableI<MyProblem> is unchanged from before; pass float as
 * the second argument (GGPUEvaluableI<MyProblem, float>) for an FP32 device path.
 *
 * The processing of an item on this path is the item's processing lifecycle split in two around the
 * device evaluation, so the device path keeps the contract of a CPU consumer's process():
 *  - flatten() BEGINS each item's processing (item->beginProcessing(): the DO_PROCESS check and the
 *    item's pre-processor) before it reads the item's values, so the device evaluates the
 *    pre-processed values. A pre-processor must not change the item's dimension.
 *  - scatter() COMPLETES each item still due with the results computed for it
 *    (item->completeProcessing(results, evaluation_time): adoption of the results, PROCESSED, the
 *    post-processor). It does not write results back through process(results): that would run the
 *    pre-processor a second time, and process() refuses an item whose processing was begun.
 *  - An item whose processing fails (beginProcessing() or completeProcessing() throws
 *    g_processing_exception) keeps the failure recorded on it and fails alone; the others of the
 *    batch are completed, and the consumer's reconciliation applies the submission policy.
 * Gem::Geneva::GBaseGPUMarshallerT implements this for geneva's individuals; a marshaller that
 * overrides its flatten() or scatter() delegates to them.
 *
 * @tparam processable_type The work-item type a batch is composed of
 * @tparam scalar_type The flat-buffer element type for parameters and fitness (double for parity, float for FP32); defaults to double
 */
template <typename processable_type, typename scalar_type = double>
class GGPUEvaluableI {
public:
    using item_ptr = std::unique_ptr<processable_type>;

    /** @brief The virtual destructor. */
    virtual ~GGPUEvaluableI() = default;

    /** @brief The number of scalar_type values one item flattens to -- i.e. its genome geometry. The GPU
     *  consumer evaluates a batch as a uniform row-major [n_items * dim] grid, so every item in a batch
     *  MUST report the same dimension; the consumer validates this before flatten() and rejects a
     *  non-uniform batch. (The GPU consumer keeps its bulk-batch capability and simply enforces a uniform
     *  geometry rather than coping with mixed geometries in one launch.)
     *  @param item The item whose flattened parameter count is queried
     *  @return The number of scalar_type values this item flattens to */
    [[nodiscard]] virtual std::size_t itemDimension(const item_ptr &item) const = 0;

    /** @brief Begins every item's processing and flattens its (pre-processed) parameters into a
     *  row-major buffer of n_items * dim scalar_type, where dim == itemDimension(item) is the same
     *  for every item (the consumer has validated this before; see the class comment for the
     *  processing rule).
     *  @param items The batch span of items to flatten
     *  @param params_out Output buffer filled row-major with n_items * dim scalar_type (parameter j of item i at index i*dim + j) */
    virtual void flatten(std::span<const item_ptr> items, std::vector<scalar_type> &params_out) const = 0;

    /** @brief Optional opaque constants the kernel needs. Default: none.
     *  @return The problem-constant byte blob handed to the kernel (empty by default) */
    [[nodiscard]] virtual std::vector<std::byte> problemConstants() const { return {}; }

    /** @brief Whether problemConstants() is the same on every call (e.g. a fixed target image). When
     *  true (the default) the consumer builds the blob ONCE and the backend uploads it to the device
     *  ONCE, instead of rebuilding/re-uploading it every generation. Override to false only if the
     *  constants genuinely change between batches.
     *  @return true if the problem constants are invariant across batches; false to rebuild/re-upload each batch */
    [[nodiscard]] virtual bool problemConstantsStatic() const { return true; }

    /** @brief Completes every item's processing with its fitness, leaving it PROCESSED -- or
     *  failed, with the failure recorded on it -- for the courtier reconciliation (see the class
     *  comment for the processing rule).
     *  @param items The batch span of items to write results into
     *  @param fitness The per-item fitness values produced by the device evaluation (one per item, in batch order)
     *  @param evaluation_time The time in seconds the device took to evaluate the whole batch; each
     *         item reports it as its processing time (plus the time of its own completion) */
    virtual void scatter(
        std::span<const item_ptr> items,
        const std::vector<scalar_type> &fitness,
        double evaluation_time
    ) const = 0;

    /** @brief How many GPU threads should cooperate on ONE item (intra-item / pixel-level parallelism).
     *  The default 1 means one thread per item (good when the population is large -- thousands of items
     *  already saturate the GPU). A value > 1 adds parallelism WITHIN each item (e.g. one thread per
     *  pixel-stripe of an image), which the CUDA backend serves by launching n_items * this threads and
     *  atomic-accumulating each item's fitness -- essential when the population is small but each item
     *  is heavy. The kernel must be written to match (accumulate, not overwrite); see the Mona-Lisa
     *  demo. Backends without double atomics clamp this to 1.
     *  @return The number of GPU threads to cooperate on a single item (>= 1) */
    [[nodiscard]] virtual int parallelWorkPerItem() const { return 1; }
};

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
