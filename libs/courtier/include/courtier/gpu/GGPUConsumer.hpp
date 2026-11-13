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
#include <string>
#include <utility>
#include <vector>

// Geneva headers
#include "common/GErrorStreamer.hpp"
#include "common/GExceptions.hpp"
#include "common/GLogger.hpp"
#include "common/concurrency/GThreadPool.hpp" // the one device thread
#include "courtier/GBaseConsumerT.hpp"
#include "courtier/gpu/GGPUBackendFactory.hpp"
#include "courtier/gpu/GGPUConsumerConfig.hpp"
#include "courtier/gpu/GGPUDeviceBackendI.hpp"
#include "courtier/gpu/GGPUEvaluableI.hpp"

namespace Gem::Courtier::GPU {

/******************************************************************************/
/**
 * A unified, device-model-agnostic GPU consumer (part of the Courtier consumer family).
 *
 * It is an ordinary courtier LOCAL consumer (it derives from the same Gem::Courtier::GBaseConsumerT
 * that every other consumer -- serial, multi-threaded, asio, beast, mpi -- derives from), so it plugs
 * into the existing span+policy submission path unchanged. It is mnemonic-selectable like every other
 * consumer (`--consumer gpu`): a Go2 program contributes its device marshaller into the marshaller store
 * (Gem::Geneva::registerGPUMarshaller(...)) and Go2 builds + registers this consumer through the normal
 * consumer setup; a Go2-less program can still construct it directly and register it in GConsumerRegistry.
 * courtier hands dispatch_() the WHOLE round's batch at once, which this consumer evaluates in a single
 * bulk kernel launch.
 *
 * The two things that used to be hard-wired per CUDA consumer are now decoupled and configurable:
 *   - the device-programming model (CUDA) -- chosen at run time from the config file, served by a
 *     swappable GGPUDeviceBackendI; the GPU consumer is DEVICE-ONLY (a CPU run uses a CPU consumer
 *     such as --consumer stc, which evaluates via the individual's own evaluate());
 *   - the kernel code -- a path in the config file, runtime-compiled (NVRTC) or loaded as a prebuilt
 *     module.
 * The problem-specific marshalling (how a batch of individuals becomes flat device buffers and how
 * results are written back) is supplied as a GGPUEvaluableI, so one consumer serves any problem.
 *
 * GPU evaluation is deterministic and all-or-nothing, so a batch should be submitted under
 * GSubmissionPolicy::full_success_or_fatal.
 *
 * Several algorithms may submit to the one process consumer at the same time (the fan-in case, e.g.
 * a meta-optimization). Every submitter may call in; ONE thread owns the device. A batch is queued
 * to that device thread and its submitter waits for it, so batches reach the device one after
 * another, in the order they were submitted, and always from the same thread -- whatever a
 * backend's rules about threads and device contexts are. The host buffers and the backend are
 * touched by the device thread only and need no lock.
 *
 * @tparam processable_type The work-item type evaluated on the device
 * @tparam scalar_type The genome/fitness flat-buffer element type and device ABI type (default double)
 */
template <typename processable_type, typename scalar_type = double>
class GGPUConsumerT final : public Gem::Courtier::GBaseConsumerT<processable_type> {
public:
    using item_ptr = typename Gem::Courtier::GBaseConsumerT<processable_type>::item_ptr;

    /** @brief Builds the consumer from @p configFile (backend + kernel selection) and the
     *  problem-specific @p marshaller. The backend is created and the kernel acquired lazily, on the
     *  first dispatch_, so construction is cheap and device errors surface at run time.
     *
     *  @param configFile Path to the config file selecting the backend and the kernel
     *  @param marshaller The problem-specific flatten/scatter helper (shared ownership) */
    GGPUConsumerT(const std::string &configFile,
                  std::shared_ptr<GGPUEvaluableI<processable_type, scalar_type>> marshaller)
        : marshaller_(std::move(marshaller)) {
        cfg_.load(configFile);
    }

    /** @brief Builds the consumer around a device backend the caller supplies, instead of one
     *  chosen from a config file: for a backend of the caller's own, and for tests that need no
     *  device. The backend must be ready for evaluate() (initialized by the caller).
     *
     *  @param backend The ready device backend (ownership taken by move)
     *  @param marshaller The problem-specific flatten/scatter helper (shared ownership) */
    GGPUConsumerT(
        std::unique_ptr<GGPUDeviceBackendI<scalar_type>> backend,
        std::shared_ptr<GGPUEvaluableI<processable_type, scalar_type>> marshaller
    )
      : marshaller_(std::move(marshaller))
      , backend_(std::move(backend)) { /* nothing */
    }

    /** @brief The destructor. */
    ~GGPUConsumerT() override = default;

    GGPUConsumerT(const GGPUConsumerT &) = delete;
    GGPUConsumerT &operator=(const GGPUConsumerT &) = delete;

    /** @brief The backend actually in use (after the first dispatch_), e.g. "cuda".
     *  @return The backend name, or "(uninitialised)" before the first dispatch_ */
    [[nodiscard]] std::string activeBackendName() const {
        // Asked on the device thread, like everything that reads the backend.
        return device_thread_
            .async_schedule([this] {
                return backend_ ? backend_->name() : std::string("(uninitialised)");
            })
            .get();
    }

protected:
    /***************************************************************************/
    /** @brief Queues the round's batch to the device thread and waits until it has been evaluated.
     *  Safe to call from several threads at once; an exception thrown while the batch is evaluated
     *  is rethrown here, in the submitter.
     *
     *  @param items The whole round's batch span of work items, evaluated in place (fitness written back) */
    void dispatch_(std::span<item_ptr> items) override {
        if(items.empty()) {
            return;
        }
        device_thread_.async_schedule([this, items] { this->evaluateBatch_(items); }).get();
    }

private:
    /***************************************************************************/
    /** @brief Evaluates the whole round's batch in one bulk launch: flatten -> backend -> scatter.
     *  Runs on the device thread. Requires a uniform genome geometry across the batch (rejects
     *  mixed geometries loudly). The GPU consumer is non-networked and evaluates a batch
     *  all-or-nothing, so a round is always a full, uniform-geometry batch of DO_PROCESS items (no
     *  MISSING/partial re-dispatch, no gaps).
     *
     *  @param items The whole round's batch span of work items, evaluated in place (fitness written back) */
    void evaluateBatch_(std::span<item_ptr> items) {
        ensureBackend_();

        const int n = static_cast<int>(items.size());

        // Enforce a UNIFORM genome geometry across the batch. The GPU evaluates the batch as a single
        // [n_items * dim] grid in one launch, so every individual must contribute the same number of
        // parameters. This keeps the bulk-batch capability while making the (previously implicit)
        // uniform-geometry assumption an explicit, checked contract: a batch mixing individuals of
        // different geometries (e.g. different problems sharing one GPU consumer) is rejected loudly
        // instead of silently corrupting the flattened buffer.
        const std::size_t dim_sz = marshaller_->itemDimension(items.front());
        for(std::size_t i = 1; i < items.size(); ++i) {
            if(marshaller_->itemDimension(items[i]) != dim_sz) {
                throw geneva_exception(
                    g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                    << "In GGPUConsumerT::dispatch_(): Error!" << '\n'
                    << "The GPU consumer requires a UNIFORM item geometry across a batch, but item "
                    << i << " has " << marshaller_->itemDimension(items[i]) << " parameters while item 0"
                    << " has " << dim_sz << "." << '\n'
                    << "Submit work items of a single geometry to the GPU consumer (one problem /"
                    << " item geometry per consumer)." << '\n'
                );
            }
        }

        marshaller_->flatten(items, params_);
        const int dim = static_cast<int>(dim_sz);

        // Build the problem-constant blob ONCE when it is static (e.g. a fixed target image), rather
        // than rebuilding it every generation. The backend likewise skips re-uploading an unchanged
        // blob (it sees the same stable pointer).
        if(not pconst_built_ || not marshaller_->problemConstantsStatic()) {
            pconst_ = marshaller_->problemConstants();
            pconst_built_ = true;
        }
        fitness_.assign(static_cast<std::size_t>(n), scalar_type(0));

        backend_->evaluate(
            params_.data(), n, dim, pconst_.data(), pconst_.size(), fitness_.data(),
            marshaller_->parallelWorkPerItem());

        marshaller_->scatter(items, fitness_);
    }

    /***************************************************************************/
    /** @brief Lazily builds the backend and acquires the kernel, unless the caller supplied a
     *  backend at construction. Runs on the device thread. The GPU consumer is device-only, so a
     *  configured backend that was not compiled into this build is a hard error (there is no CPU
     *  fallback -- a CPU run uses a CPU consumer such as --consumer stc). */
    void ensureBackend_() {
        if(backend_) {
            return;
        }
        BackendKind const kind = cfg_.backendKind();
        if(not backendAvailable(kind)) {
            throw geneva_exception(
                g_error_streamer(DO_LOG, Gem::Common::timeAndPlace())
                << "In Gem::Courtier::GPU::GGPUConsumerT::ensureBackend_(): Error!" << '\n'
                << "The '" << toString(kind) << "' backend was not compiled into this build (its"
                << " toolkit was not found at configure time)." << '\n'
                << "The GPU consumer is device-only; to run on the CPU use a CPU consumer instead"
                << " (e.g. --consumer stc), which evaluates via the individual's own evaluate()." << '\n');
        }
        backend_ = makeBackend<scalar_type>(kind);
        backend_->initialize(cfg_.kernelSpec());
        glogger << "Gem::Courtier::GPU::GGPUConsumer using the '" << backend_->name()
                << "' backend (kernel: " << cfg_.kernel_path << ")" << '\n'
                << GLOGGING;
    }

    /***************************************************************************/
    std::shared_ptr<GGPUEvaluableI<processable_type, scalar_type>> marshaller_; ///< Problem-specific flatten/scatter
    GGPUConsumerConfig cfg_;                          ///< backend + kernel selection
    std::unique_ptr<GGPUDeviceBackendI<scalar_type>> backend_;    ///< The device backend (lazy)

    std::vector<scalar_type> params_;     ///< Reused host parameter buffer (avoids per-round reallocation)
    std::vector<scalar_type> fitness_;    ///< Reused host fitness buffer
    // A value plus a "has it been built" flag, kept as two members rather than folded into a
    // std::optional or a load-once cell. Neither fits: the blob is REBUILT every round when the
    // marshaller reports it as non-static, so this is not a load-once constant, and the flag is
    // read together with problemConstantsStatic() rather than on its own. The pair also lets the
    // vector keep its capacity across rounds, which an optional's reset would throw away.
    std::vector<std::byte> pconst_;  ///< Cached problem-constant blob (built once when static)
    bool pconst_built_ = false;      ///< Whether pconst_ has been built

    /// The one thread that owns the device: every batch is queued to it (see the class comment),
    /// and it alone touches the members above after construction. Declared last, so it is joined
    /// before they are destroyed.
    mutable Gem::Common::Concurrency::GThreadPool device_thread_{
        "consumer:gpu",
        1,
        Gem::Common::Concurrency::ThreadElasticity::Fixed
    };
};

/******************************************************************************/

} /* namespace Gem::Courtier::GPU */
