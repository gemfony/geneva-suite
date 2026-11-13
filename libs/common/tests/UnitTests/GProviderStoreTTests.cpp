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

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <utility>

#include <boost/program_options/options_description.hpp>

#include "common/GProviderStoreT.hpp"

using namespace Gem::Common;

namespace {

// A minimal provider that hands out a fixed int -- exercises the GProviderT contract the store keys on.
class IntProvider : public GProviderT<int> {
public:
    IntProvider(std::string mnemonic, int value)
      : mnemonic_(std::move(mnemonic)), value_(value) {}

    std::shared_ptr<int> provide() override { return std::make_shared<int>(value_); }
    [[nodiscard]] std::string getMnemonic() const override { return mnemonic_; }
    [[nodiscard]] std::string getName() const override { return "IntProvider(" + mnemonic_ + ")"; }
    void addCLOptions(
        boost::program_options::options_description & /*visible*/,
        boost::program_options::options_description & /*hidden*/
    ) override { /* no options */ }

private:
    std::string mnemonic_;
    int value_;
};

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("GProviderStoreT: registers, retrieves and hands out providers by mnemonic", "[common][providerstore]") {
    auto store = providerStore<int>();
    REQUIRE(store);

    CHECK(store->setOnce("a", std::make_shared<IntProvider>("a", 1)));
    CHECK(store->setOnce("b", std::make_shared<IntProvider>("b", 2)));

    CHECK(store->exists("a"));
    CHECK(store->exists("b"));
    CHECK_FALSE(store->exists("c"));

    std::shared_ptr<GProviderT<int>> p;
    REQUIRE(store->get("a", p));
    REQUIRE(p);
    CHECK(p->getMnemonic() == "a");
    CHECK(*p->provide() == 1);
}

TEST_CASE("GProviderStoreT: the store is a process-global singleton", "[common][providerstore]") {
    // Two accessor calls hand back the same underlying store, so a registration through one is visible
    // through the other -- the property Go2 relies on when a provider self-registers at static init.
    auto first = providerStore<int>();
    first->setOnce("singleton_key", std::make_shared<IntProvider>("singleton_key", 42));

    auto second = providerStore<int>();
    REQUIRE(second->exists("singleton_key"));
    std::shared_ptr<GProviderT<int>> q;
    REQUIRE(second->get("singleton_key", q));
    CHECK(*q->provide() == 42);
}
