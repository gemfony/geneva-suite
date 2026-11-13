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
 * @file polymorphic.cpp
 * @brief Serializing through a base pointer and reconstructing the dynamic type.
 *
 * A @c shared_ptr / @c unique_ptr to a hierarchy root round-trips as the exact
 * derived type. The plumbing:
 *
 *   - the root exposes @c gemfony_common_root_t (deducing the hierarchy) and is
 *     polymorphic (a virtual), so the dynamic type is recoverable;
 *   - each concrete type has a @c serialize reachable through @c Gem::Weft::access;
 *   - one @c GEM_REGISTER_ARCHIVABLE(Type) at namespace scope wires the type into
 *     both the content-addressed identity registry (tag <-> factory) and the codec
 *     dispatch table.
 *
 * On save Weft writes the dynamic type's wire tag then its members; on load it
 * reads the tag, constructs that type from the registry and fills it. This is the
 * capability behind Boost.Serialization's BOOST_CLASS_EXPORT — here with a JSON
 * codec as well, and a self-contained, dependency-free engine.
 */

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#include "weft/GArchivePolymorphic.hpp"
#include "weft/GBinaryArchive.hpp"
#include "weft/GJsonArchive.hpp"

namespace shapes {

// The hierarchy root: carries the gemfony_common_root_t marker and a virtual, so
// the dynamic type of a Shape* is recoverable. It has no serializable state of its
// own, so it needs no serialize member.
struct Shape {
    using gemfony_common_root_t = Shape;
    virtual ~Shape() = default;
    [[nodiscard]] virtual double area() const = 0;
};

class Circle : public Shape {
public:
    Circle() = default; // public default ctor: the registry factory reconstructs through it
    explicit Circle(double radius) : radius_(radius) {}
    [[nodiscard]] double area() const override { return 3.14159265358979 * radius_ * radius_; }

private:
    // Encapsulated serialize, reached only through the befriended access shim.
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, unsigned /*version*/) {
        ar &Gem::Weft::make_nvp("radius", radius_);
    }
    double radius_ = 0.0;
};

class Rectangle : public Shape {
public:
    Rectangle() = default;
    Rectangle(double w, double h) : w_(w), h_(h) {}
    [[nodiscard]] double area() const override { return w_ * h_; }

private:
    friend struct Gem::Weft::access;
    template <typename Archive>
    void serialize(Archive &ar, unsigned /*version*/) {
        ar &Gem::Weft::make_nvp("w", w_);
        ar &Gem::Weft::make_nvp("h", h_);
    }
    double w_ = 0.0, h_ = 0.0;
};

} // namespace shapes

// One line per concrete type, at namespace scope. The wire tag is the stringized
// fully-qualified type name, exactly as BOOST_CLASS_EXPORT_IMPLEMENT's GUID.
GEM_REGISTER_ARCHIVABLE(shapes::Circle)
GEM_REGISTER_ARCHIVABLE(shapes::Rectangle)

namespace {

// Serialize a base pointer through OArchive and read it back through IArchive.
template <typename OArchive, typename IArchive>
std::shared_ptr<shapes::Shape> roundtrip(const std::shared_ptr<shapes::Shape> &p) {
    std::string blob;
    {
        OArchive oa;
        auto q = p; // operator& takes a non-const lvalue
        oa &q;
        blob = oa.str();
    }
    std::shared_ptr<shapes::Shape> loaded;
    {
        IArchive ia(blob);
        ia &loaded;
    }
    return loaded;
}

} // namespace

int main() {
    using namespace Gem::Weft;

    std::shared_ptr<shapes::Shape> circle = std::make_shared<shapes::Circle>(2.0);
    std::shared_ptr<shapes::Shape> rect = std::make_shared<shapes::Rectangle>(3.0, 4.0);

    // Binary: the dynamic type is reconstructed and its area matches.
    auto c_bin = roundtrip<GBinaryOArchive, GBinaryIArchive>(circle);
    auto r_bin = roundtrip<GBinaryOArchive, GBinaryIArchive>(rect);
    assert(c_bin && std::abs(c_bin->area() - circle->area()) < 1e-9);
    assert(r_bin && std::abs(r_bin->area() - rect->area()) < 1e-9);

    // JSON: same registrations, human-readable payload.
    std::string json;
    {
        GJsonOArchive oa;
        auto q = circle;
        oa &q;
        json = oa.str();
    }
    auto c_json = roundtrip<GJsonOArchive, GJsonIArchive>(circle);
    assert(c_json && std::abs(c_json->area() - circle->area()) < 1e-9);

    std::cout << "circle area  = " << c_bin->area() << '\n';
    std::cout << "rect area    = " << r_bin->area() << '\n';
    std::cout << "circle json  = " << json << '\n';

    // The boot-time completeness check: every identity-registered type is also
    // archive-dispatchable, so this passes silently (it would throw otherwise).
    verifyArchiveRegistrations();
    std::cout << archiveRegisteredHierarchyCount()
              << " hierarchy(ies) fully archive-dispatchable\n";

    std::cout << "polymorphic: OK\n";
    return 0;
}
