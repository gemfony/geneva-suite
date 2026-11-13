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
 * @file nonintrusive.cpp
 * @brief Serializing a type you cannot modify — the non-intrusive path.
 *
 * When a type is third-party (no access to add a member @c serialize), Weft finds
 * a free function @c gem_archive_serialize(Archive&, T&) by argument-dependent
 * lookup in the type's own namespace — the analogue of a Boost.Serialization
 * non-intrusive free @c serialize. Nested members (here a std::vector of another
 * such type) compose naturally: each element is dispatched the same way.
 */

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "weft/GBinaryArchive.hpp"
#include "weft/GJsonArchive.hpp"

// Pretend this namespace belongs to a third-party library we cannot edit.
namespace vendor {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    bool operator==(const Vec3 &) const = default;
};

struct Trajectory {
    std::string name;
    std::vector<Vec3> points;
    bool operator==(const Trajectory &) const = default;
};

// Non-intrusive hooks, found by ADL because they live in `vendor` alongside the
// types. They never touch the class definitions.
template <typename Archive>
void gem_archive_serialize(Archive &ar, Vec3 &v) {
    ar &Gem::Weft::make_nvp("x", v.x);
    ar &Gem::Weft::make_nvp("y", v.y);
    ar &Gem::Weft::make_nvp("z", v.z);
}

template <typename Archive>
void gem_archive_serialize(Archive &ar, Trajectory &t) {
    ar &Gem::Weft::make_nvp("name", t.name);
    ar &Gem::Weft::make_nvp("points", t.points); // std::vector<Vec3> composes automatically
}

} // namespace vendor

int main() {
    const vendor::Trajectory original{
        .name = "orbit",
        .points = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {-1.0, 0.0, 0.5}}};

    // Binary round trip.
    std::string bytes;
    {
        Gem::Weft::GBinaryOArchive oa;
        oa &original;
        bytes = oa.str();
    }
    vendor::Trajectory from_binary;
    {
        Gem::Weft::GBinaryIArchive ia(bytes);
        ia &from_binary;
    }
    assert(from_binary == original);

    // JSON round trip — same non-intrusive hooks, different codec.
    std::string text;
    {
        Gem::Weft::GJsonOArchive oa;
        oa &original;
        text = oa.str();
    }
    vendor::Trajectory from_json;
    {
        Gem::Weft::GJsonIArchive ia(text);
        ia &from_json;
    }
    assert(from_json == original);

    std::cout << "non-intrusive: round-tripped " << original.points.size()
              << " points through both codecs\n";
    std::cout << "json: " << text << '\n';
    std::cout << "nonintrusive: OK\n";
    return 0;
}
