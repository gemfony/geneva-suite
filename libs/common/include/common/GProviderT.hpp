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
#include <memory>
#include <string>

// Boost header files go here
#include <boost/program_options.hpp>

namespace Gem::Common {

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/
/**
 * A common provisioning interface for the mnemonic-keyed global stores. Both
 * optimization algorithms and consumers are looked up by mnemonic and then
 * asked to hand out a usable object via provide(). The two flavours differ only
 * in how provide() is implemented:
 *
 *  - optimization algorithms wrap a config-file-driven factory, so provide()
 *    produces a freshly configured object on every call (many instances per run);
 *  - consumers wrap a single prototype instance, so provide() returns that same
 *    instance (one instance per run, used in place).
 *
 * The remaining methods (getMnemonic / getName / addCLOptions) expose metadata
 * and command-line options without producing an object, which is what the help
 * output and option parsing need.
 *
 * @tparam T The type of object handed out by provide()
 */
template <typename T>
class GProviderT { // NOLINT(cppcoreguidelines-special-member-functions)
public:
    /** @brief The default constructor */
    GProviderT() = default;
    /** @brief The (defaulted) destructor */
    virtual ~GProviderT() = default;

    /**
     * @brief Hands out a usable object (produced or prototype, depending on the flavour)
     * @return A usable object of type T (a freshly produced instance or a shared prototype)
     */
    virtual std::shared_ptr<T> provide() = 0;
    /**
     * @brief The mnemonic this provider is registered under
     * @return The short mnemonic key used to look this provider up in the global store
     */
    [[nodiscard]] virtual std::string getMnemonic() const = 0;
    /**
     * @brief A human-readable name, for help output (produces no object)
     * @return The provider's descriptive name
     */
    [[nodiscard]] virtual std::string getName() const = 0;
    /**
     * @brief Adds the provided object's command-line options (produces no object)
     * @param visible The options description collecting user-facing (documented) options
     * @param hidden The options description collecting internal / undocumented options
     */
    virtual void addCLOptions(
        boost::program_options::options_description &visible,
        boost::program_options::options_description &hidden
    ) = 0;
};

/******************************************************************************/
////////////////////////////////////////////////////////////////////////////////
/******************************************************************************/

} /* namespace Gem::Common */
