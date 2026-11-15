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

// Boost headers go here
#include <boost/asio/io_context.hpp>

namespace Gem::Courtier::Consumers {

/******************************************************************************/
/**
 * An io context whose pending handlers can be destroyed before the context itself is.
 *
 * A connection that does its I/O on one context and runs its role's handlers on another refers to
 * both: its socket belongs to the first, the strand of its handlers to the second, and pending
 * handlers of either context may own it. Neither context may be destroyed while such a connection
 * lives. So the owner of the two, once their threads are joined, shuts the I/O context down first
 * (shutdown(): its pending handlers are destroyed, and with them the connections they alone held,
 * while both contexts still exist) and then destroys the handler context (whose pending handlers
 * own the remaining connections; their sockets' services still exist, because shutdown() does not
 * destroy services). The I/O context itself is destroyed last.
 */
class GIoContext : public boost::asio::io_context {
public:
    using boost::asio::io_context::io_context;

    /** @brief Destroys every pending handler; the context's services stay. Only when no thread
     *  runs the context. May be repeated; the destructor calls it once more. */
    using boost::asio::io_context::shutdown;
};

/******************************************************************************/

} /* namespace Gem::Courtier::Consumers */
