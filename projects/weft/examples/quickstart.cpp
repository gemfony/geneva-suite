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
 * @file quickstart.cpp
 * @brief The shortest possible Weft program: serialize a value type through both
 * codecs and read it back.
 *
 * A type opts into serialization the Boost.Serialization way — a member
 * @c serialize(Archive&, unsigned) that streams each field with @c make_nvp. The
 * SAME method drives every codec; here the binary codec (compact, for the wire or
 * a checkpoint) and the JSON codec (human-readable, which Boost.Serialization has
 * no equivalent of). This program links only against gemfony-weft + Boost.JSON.
 */

#include <cassert>
#include <iostream>
#include <string>

#include "weft/GBinaryArchive.hpp"
#include "weft/GJsonArchive.hpp"

// A plain value type. The serialize member is public here for brevity; see
// polymorphic.cpp for the encapsulated (private + friend access) variant.
//! [weft-intrusive-serialize]
struct Particle {
    int id = 0;
    double mass = 0.0;
    std::string label;

    bool operator==(const Particle &) const = default;

    template <typename Archive>
    void serialize(Archive &ar, unsigned /*version*/) {
        ar &Gem::Weft::make_nvp("id", id);
        ar &Gem::Weft::make_nvp("mass", mass);
        ar &Gem::Weft::make_nvp("label", label);
    }
};
//! [weft-intrusive-serialize]

int main() {
    const Particle original{.id = 42, .mass = 9.10938e-31, .label = "electron"};

    // --- Binary codec: compact bytes for the wire / a checkpoint ----------------
    std::string bytes;
    {
        Gem::Weft::GBinaryOArchive oa;
        oa &original;
        bytes = oa.str();
    }
    Particle from_binary;
    {
        Gem::Weft::GBinaryIArchive ia(bytes);
        ia &from_binary;
    }
    assert(from_binary == original);
    std::cout << "binary: round-tripped " << bytes.size() << " bytes\n";

    // --- JSON codec: the same object, human-readable ----------------------------
    std::string text;
    {
        Gem::Weft::GJsonOArchive oa;
        oa &original;
        text = oa.str();
    }
    Particle from_json;
    {
        Gem::Weft::GJsonIArchive ia(text);
        ia &from_json;
    }
    assert(from_json == original);
    std::cout << "json:   " << text << '\n';

    std::cout << "quickstart: OK\n";
    return 0;
}
