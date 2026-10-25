/**
 * @file minimal_consumer.cpp
 */

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
 * The whole of courtier's consumer contract in one program: a consumer implements dispatch_() and
 * a caller submits a batch through processBatch(). Everything else -- how many threads are started,
 * which port is bound, whether the work travels over a network -- is the concrete consumer's
 * business and invisible here.
 *
 * The demo work item is courtier's own GSimpleContainer, so the program depends on nothing but the
 * courtier library itself. It is registered as a test, because a documentation example that no
 * longer compiles (or no longer does what the surrounding text says) is worse than none.
 */

#include <cstddef>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

#include "courtier/GBaseConsumerT.hpp"
#include "courtier/GDemoProcessingContainers.hpp"
#include "courtier/GSubmissionPolicy.hpp"

using namespace Gem::Courtier;

//! [courtier-consumer-surface#1]
/** A consumer of one's own: evaluate this round's DO_PROCESS slots in place, let no exception
 *  escape, and leave every other part of the contract to the base class. This one runs the
 *  evaluations serially in the calling thread; GStdThreadConsumerT spreads the same loop over a
 *  thread pool, and a networked consumer sends the slots to remote clients. */
class GSerialDocConsumer final : public GBaseConsumerT<GSimpleContainer> {
protected:
    void dispatch_(std::span<item_ptr> items) override {
        for(auto &item : items) {
            if(item && item->getProcessingStatus() == processingStatus::DO_PROCESS) {
                item->process(); // records PROCESSED, or EXCEPTION_CAUGHT if the evaluation throws
            }
        }
    }
};
//! [courtier-consumer-surface#1]

int main() {
    std::vector<GSerialDocConsumer::item_ptr> batch;
    for(std::size_t i = 0; i < 8; ++i) {
        batch.push_back(std::make_unique<GSimpleContainer>(i));
    }

    //! [courtier-consumer-surface#2]
    // The single submission entry point. On return every slot of the span holds a successfully
    // evaluated item -- here under the strictest policy, which terminates rather than hand back a
    // batch with a hole in it.
    GSerialDocConsumer consumer;
    consumer.processBatch(std::span<GSerialDocConsumer::item_ptr>(batch.data(), batch.size()),
                          GSubmissionPolicy::full_success_or_fatal());
    //! [courtier-consumer-surface#2]

    std::size_t processed = 0;
    for(const auto &item : batch) {
        if(item && item->is_processed()) {
            ++processed;
        }
    }

    std::cout << "minimal_consumer: " << processed << " of " << batch.size()
              << " work items came back successfully evaluated\n";

    // A full batch is what processBatch() guarantees, so anything else is a failure of the library,
    // not of this program's expectations.
    return processed == batch.size() ? 0 : 1;
}
